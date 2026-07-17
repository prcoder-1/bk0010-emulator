#include "HotPathWidget.h"
#include "Board.h"
#include "Disasm.h"
#include <QPainter>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <unordered_map>

using bk::Board;

// A control-transfer instruction ends a basic block (PDP-11/ВМ1, octal ranges).
static bool isTerminator(uint16_t ir) {
    if (ir >= 0000400 && ir <= 0003477) return true; // conditional branches / BR
    if (ir >= 0100000 && ir <= 0103777) return true; // BPL..BCS
    if (ir >= 0000100 && ir <= 0000177) return true; // JMP
    if (ir >= 0004000 && ir <= 0004777) return true; // JSR
    if (ir >= 0000200 && ir <= 0000207) return true; // RTS
    if (ir == 0000002 || ir == 0000006) return true; // RTI / RTT
    if (ir >= 0104000 && ir <= 0104777) return true; // EMT / TRAP
    if (ir >= 0077000 && ir <= 0077777) return true; // SOB
    if (ir == 0000000) return true;                  // HALT
    return false;
}

// True for transfers that do NOT fall through to the next instruction.
static bool isUncond(uint16_t ir) {
    if (ir >= 0000400 && ir <= 0000777) return true; // unconditional BR
    if (ir >= 0000100 && ir <= 0000177) return true; // JMP
    if (ir >= 0000200 && ir <= 0000207) return true; // RTS
    if (ir == 0000002 || ir == 0000006) return true; // RTI / RTT
    if (ir == 0000000) return true;                  // HALT
    return false;
}

static QString oct6(uint16_t v) { return QString("%1").arg(v, 6, 8, QChar('0')); }

static QString cnt(uint32_t n) {
    if (n >= 1000000) return QString::number(n / 1e6, 'f', 1) + "M";
    if (n >= 1000)    return QString::number(n / 1e3, 'f', 1) + "K";
    return QString::number(n);
}

HotPathWidget::HotPathWidget(Board* board, QWidget* parent)
    : QWidget(parent), board_(board) {
    setMinimumSize(360, 260);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);   // hover → linked highlighting
}

void HotPathWidget::refresh() {
    uint32_t now = board_->trace().now();
    // Repaint only when the profile is re-snapshotted — layout/labels change only
    // then. Interactive changes (scroll/expand/hover/highlight) repaint on their
    // own. Repainting at the full 50 Hz otherwise redrew an identical image (with a
    // disasm() per visible row) many times per update, stealing time from the
    // emulation in this same GUI thread.
    if (instrs_.empty() || now - lastBuild_ >= 50) { rebuild(); lastBuild_ = now; update(); }
}

// ---------------------------------------------------------------------------
// Build basic blocks and the top-N hottest paths (traces) from the trace.
// ---------------------------------------------------------------------------
void HotPathWidget::rebuild() {
    bk::Trace& tr = board_->trace();
    tr.setEnabled(true);
    const bk::Memory& mem = board_->memory();
    bk::Cpu& cpu = board_->cpu();

    // 1. Executed instructions in address order (skip user-hidden ones).
    instrs_.clear();
    totalCost_ = 0; maxCount_ = 1;
    for (int a = 0; a < 0x10000; a += 2) {
        uint32_t c = tr.execCount((uint16_t)a);
        if (!c) continue;
        totalCost_ += (uint64_t)c * cpu.instrTicks(mem.peekWord((uint16_t)a));
        if (excluded_.count((uint16_t)a)) continue;
        instrs_.push_back({ (uint16_t)a, c });
        maxCount_ = std::max(maxCount_, c);
    }
    if (totalCost_ == 0) totalCost_ = 1;
    const int NI = (int)instrs_.size();
    blocks_.clear(); paths_.clear();
    if (!NI) return;

    std::unordered_map<uint16_t, int> idxOf;
    for (int i = 0; i < NI; ++i) idxOf[instrs_[i].addr] = i;

    // 2. Block leaders: first instr, branch targets, instr after a terminator/gap.
    std::set<uint16_t> leaders;
    leaders.insert(instrs_[0].addr);
    for (auto& e : tr.edges()) {
        uint16_t to = e.first & 0xFFFF;
        if (idxOf.count(to)) leaders.insert(to);
    }
    for (int i = 0; i + 1 < NI; ++i) {
        uint16_t a = instrs_[i].addr, ir = mem.peekWord(a);
        uint16_t nextSeq = a + (uint16_t)(bk::disasm(mem, a).words * 2);
        uint16_t b = instrs_[i + 1].addr;
        if (b != nextSeq || isTerminator(ir)) leaders.insert(b);
    }

    // 3. Build blocks (each a maximal run up to the next leader).
    std::unordered_map<uint16_t, int> blockOfLeader;
    for (int i = 0; i < NI; ) {
        int j = i + 1;
        while (j < NI && !leaders.count(instrs_[j].addr)) ++j;
        Block blk;
        blk.leader = instrs_[i].addr; blk.first = i; blk.last = j - 1;
        blk.count = instrs_[i].count;
        uint16_t lastA = instrs_[j - 1].addr;
        blk.endAddr = lastA + (uint16_t)(bk::disasm(mem, lastA).words * 2);
        blockOfLeader[blk.leader] = (int)blocks_.size();
        blocks_.push_back(blk);
        i = j;
    }
    const int NB = (int)blocks_.size();

    std::unordered_map<uint16_t, int> blockByLast;
    for (int b = 0; b < NB; ++b) blockByLast[instrs_[blocks_[b].last].addr] = b;

    // 4. Block-level successor/predecessor weights: taken branches + fall-through.
    std::vector<std::map<int, uint64_t>> succ(NB), pred(NB);
    for (auto& e : tr.edges()) {
        uint16_t from = e.first >> 16, to = e.first & 0xFFFF;
        auto f = blockByLast.find(from); auto t = blockOfLeader.find(to);
        if (f == blockByLast.end() || t == blockOfLeader.end() || f->second == t->second) continue;
        succ[f->second][t->second] += e.second;
        pred[t->second][f->second] += e.second;
    }
    for (int b = 0; b < NB; ++b) {
        uint16_t lastIr = mem.peekWord(instrs_[blocks_[b].last].addr);
        auto nxt = blockOfLeader.find(blocks_[b].endAddr);
        if (isUncond(lastIr) || nxt == blockOfLeader.end()) continue;
        uint64_t taken = 0; for (auto& kv : succ[b]) taken += kv.second;
        uint64_t fw = blocks_[b].count > taken ? blocks_[b].count - taken : 1;
        succ[b][nxt->second] += fw;
        pred[nxt->second][b] += fw;
    }

    // 5. Trace formation: seed from the hottest unused block, then greedily grow
    //    forward/backward along the heaviest edges into unused blocks.
    std::vector<int> order(NB);
    for (int b = 0; b < NB; ++b) order[b] = b;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return blocks_[a].count > blocks_[b].count; });
    std::vector<char> used(NB, 0);
    auto heaviest = [&](const std::map<int, uint64_t>& m) {
        int best = -1; uint64_t bw = 0;
        for (auto& kv : m) if (!used[kv.first] && kv.second > bw) { bw = kv.second; best = kv.first; }
        return best;
    };
    for (int seed : order) {
        if (used[seed]) continue;
        std::deque<int> dq; dq.push_back(seed); used[seed] = 1;
        for (int cur = seed; ; ) { int nx = heaviest(succ[cur]); if (nx < 0) break; dq.push_back(nx); used[nx] = 1; cur = nx; }
        for (int cur = seed; ; ) { int pv = heaviest(pred[cur]); if (pv < 0) break; dq.push_front(pv); used[pv] = 1; cur = pv; }
        Path pth; pth.blocks.assign(dq.begin(), dq.end());
        pth.entry = blocks_[pth.blocks.front()].leader;
        pth.cost = 0;
        for (int b : pth.blocks)
            for (int k = blocks_[b].first; k <= blocks_[b].last; ++k)
                pth.cost += (uint64_t)instrs_[k].count * cpu.instrTicks(mem.peekWord(instrs_[k].addr));
        paths_.push_back(std::move(pth));
        if ((int)paths_.size() >= topN_) break;
    }
    std::sort(paths_.begin(), paths_.end(), [](const Path& a, const Path& b) { return a.cost > b.cost; });
}

// Warm heat colour for a 0..1 fraction (transparent → orange → red).
static QColor heat(double f, int alpha) {
    f = std::clamp(f, 0.0, 1.0);
    QColor c = QColor::fromHsvF(float((28.0 - 28.0 * f) / 360.0), 0.85f, 0.60f);
    c.setAlpha(alpha);
    return c;
}

void HotPathWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(16, 18, 26));
    QFont mono("monospace"); mono.setStyleHint(QFont::TypeWriter); mono.setPixelSize(12);
    p.setFont(mono);
    QFontMetrics fm(mono);
    rowH_ = fm.height() + 4;
    const int cw = fm.horizontalAdvance('0');
    const bk::Memory& mem = board_->memory();

    p.setPen(QColor(200, 210, 255));
    p.drawText(10, fm.ascent() + 4, QString::fromUtf8(
        "Горячий путь — самые исполняемые цепочки блоков (доля времени CPU); ▸ развернуть до инструкций"));
    const int top = rowH_ + 8;

    if (paths_.empty()) {
        p.setPen(QColor(140, 150, 170));
        p.drawText(10, top + fm.ascent(), QString::fromUtf8("нет данных трассировки — запустите игру"));
        return;
    }

    rows_.clear();
    double y = top - scroll_;
    const int bottom = height() - rowH_ - 6;
    const double logMax = std::log(double(maxCount_) + 1.0);
    auto hfrac = [&](uint32_t c) { return c ? std::log(double(c) + 1.0) / (logMax + 1e-9) : 0.0; };

    auto rowVisible = [&](double yy) { return yy + rowH_ >= top && yy <= height(); };
    // Linked highlight: mark the row for the routine hovered in another window.
    auto hlRow = [&](const QRect& rr, uint16_t a) {
        if (link_ >= 0 && (int)a == link_) {
            p.fillRect(rr, QColor(255, 245, 150, 45));
            p.fillRect(QRect(rr.x(), rr.y(), 3, rr.height()), QColor(255, 245, 150));
        }
    };

    int rank = 1;
    for (auto& path : paths_) {
        // ---- Path row ----
        QRect pr(0, (int)y, width(), rowH_);
        bool pOpen = pathOpen_.count(path.entry);
        rows_.push_back({ pr, 0, K_PATH, path.entry, path.entry, true });
        if (rowVisible(y)) {
            double pct = 100.0 * double(path.cost) / double(totalCost_);
            p.fillRect(pr, heat(pct / 100.0, 70));
            hlRow(pr, path.entry);
            int x = 8;
            p.setPen(QColor(230, 200, 120));
            p.drawText(x, (int)y + fm.ascent() + 2, pOpen ? "▾" : "▸");
            x += 2 * cw;
            p.setPen(QColor(235, 240, 255));
            p.drawText(x, (int)y + fm.ascent() + 2,
                       QString("#%1  %2  %3  %4 бл  %5%")
                           .arg(rank).arg(oct6(path.entry)).arg("×" + cnt(blocks_[path.blocks.front()].count), -8)
                           .arg((int)path.blocks.size(), 2).arg(pct, 4, 'f', 1));
            // heat bar (proportional to % CPU) at the right edge
            int barW = width() / 4;
            QRect bar(width() - barW - 10, (int)y + 4, barW, rowH_ - 8);
            p.fillRect(bar, QColor(255, 255, 255, 18));
            p.fillRect(QRect(bar.x(), bar.y(), int(bar.width() * pct / 100.0), bar.height()),
                       heat(pct / 100.0, 220));
        }
        y += rowH_;

        if (!pOpen) { ++rank; continue; }

        // ---- Block rows ----
        for (size_t bi = 0; bi < path.blocks.size(); ++bi) {
            int b = path.blocks[bi];
            const Block& blk = blocks_[b];
            QRect br(0, (int)y, width(), rowH_);
            bool bOpen = blockOpen_.count(blk.leader);
            rows_.push_back({ br, 1, K_BLOCK, blk.leader, blk.leader, true });
            if (rowVisible(y)) {
                p.fillRect(br, heat(hfrac(blk.count), 55));
                hlRow(br, blk.leader);
                int x = 8 + 2 * cw;
                p.setPen(QColor(210, 200, 150));
                p.drawText(x, (int)y + fm.ascent() + 2, bOpen ? "▾" : "▸");
                x += 2 * cw;
                int ni = blk.last - blk.first + 1;
                QString mnem = QString::fromStdString(bk::disasm(mem, blk.leader).text);
                QString nextTxt;
                if (bi + 1 < path.blocks.size())
                    nextTxt = "→ " + oct6(blocks_[path.blocks[bi + 1]].leader);
                p.setPen(QColor(225, 232, 246));
                QString s = QString("%1  %2  ×%3  %4 инстр  %5")
                                .arg(oct6(blk.leader)).arg(mnem, -18).arg(cnt(blk.count), -6)
                                .arg(ni, 2).arg(nextTxt);
                p.drawText(QRect(x, (int)y, width() - x - 10, rowH_),
                           Qt::AlignVCenter | Qt::AlignLeft,
                           fm.elidedText(s, Qt::ElideRight, width() - x - 12));
            }
            y += rowH_;

            if (!bOpen) continue;

            // ---- Instruction rows ----
            for (int k = blk.first; k <= blk.last; ++k) {
                QRect ir(0, (int)y, width(), rowH_);
                rows_.push_back({ ir, 2, K_INSTR, instrs_[k].addr, 0, false });
                if (rowVisible(y)) {
                    p.fillRect(ir, heat(hfrac(instrs_[k].count), 45));
                    hlRow(ir, instrs_[k].addr);
                    bk::DisasmLine d = bk::disasm(mem, instrs_[k].addr);
                    QString raw;
                    for (int w = 0; w < d.words; ++w) raw += oct6(mem.peekWord(instrs_[k].addr + w * 2)) + " ";
                    int x = 8 + 4 * cw;
                    p.setPen(QColor(150, 160, 200));
                    p.drawText(x, (int)y + fm.ascent() + 2, oct6(instrs_[k].addr));
                    p.setPen(QColor(120, 135, 170));
                    p.drawText(x + 8 * cw, (int)y + fm.ascent() + 2, raw);
                    p.setPen(QColor(220, 228, 244));
                    p.drawText(x + 8 * cw + 8 * 3 * cw, (int)y + fm.ascent() + 2,
                               QString::fromStdString(d.text));
                    p.setPen(QColor(230, 170, 120));
                    p.drawText(QRect(0, (int)y, width() - 10, rowH_),
                               Qt::AlignVCenter | Qt::AlignRight, "×" + cnt(instrs_[k].count));
                }
                y += rowH_;
            }
        }
        ++rank;
    }
    contentH_ = y + scroll_ - top;

    p.setPen(QColor(120, 130, 150));
    p.drawText(10, height() - 6, QString::fromUtf8(
        "колесо/тащить — прокрутка · ▸ — свернуть/развернуть · клик — в дизасм · ПКМ — скрыть · +/- — число путей"));
}

void HotPathWidget::wheelEvent(QWheelEvent* e) {
    scroll_ -= e->angleDelta().y() * 0.6;
    scroll_ = std::clamp(scroll_, 0.0, std::max(0.0, contentH_ - (height() - 2.0 * rowH_)));
    update();
}

void HotPathWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) {
        // Hide a swamping address (e.g. a hot delay loop). Handled on press — a
        // press-only-on-left setup could otherwise never see the right release.
        for (const Row& r : rows_)
            if (r.rect.contains(e->pos())) { excluded_.insert(r.addr); rebuild(); update(); break; }
        return;
    }
    if (e->button() == Qt::LeftButton) { dragging_ = true; lastDrag_ = pressPos_ = e->pos(); }
}

void HotPathWidget::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_) {
        scroll_ -= e->pos().y() - lastDrag_.y();
        scroll_ = std::clamp(scroll_, 0.0, std::max(0.0, contentH_ - (height() - 2.0 * rowH_)));
        lastDrag_ = e->pos();
        update();
        return;
    }
    // Hover → broadcast the address under the cursor for linked highlighting.
    int found = -1;
    for (const Row& r : rows_) if (r.rect.contains(e->pos())) { found = r.addr; break; }
    if (found != hoverEmit_) { hoverEmit_ = found; setHighlight(found); emit hoverAddress(found); }
}

void HotPathWidget::leaveEvent(QEvent*) {
    if (hoverEmit_ != -1) { hoverEmit_ = -1; emit hoverAddress(-1); }
    setHighlight(-1);
}

void HotPathWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;   // right-click handled on press
    dragging_ = false;
    if ((e->pos() - pressPos_).manhattanLength() > 4) return;   // was a scroll-drag
    for (const Row& r : rows_) {
        if (!r.rect.contains(e->pos())) continue;
        // Triangle zone (indent + a couple of chars) folds/unfolds; the body jumps.
        int cw = QFontMetrics(font()).horizontalAdvance('0');
        int triX = 8 + r.level * 2 * cw + 3 * cw;
        if (r.expandable && e->pos().x() < triX) {
            std::set<uint16_t>& s = (r.kind == K_PATH) ? pathOpen_ : blockOpen_;
            if (s.count(r.key)) s.erase(r.key); else s.insert(r.key);
            update();
        } else {
            emit addressPicked(r.addr);
        }
        return;
    }
}

void HotPathWidget::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
    case Qt::Key_Plus: case Qt::Key_Equal:  topN_ = std::min(topN_ + 4, 60); rebuild(); update(); break;
    case Qt::Key_Minus: case Qt::Key_Underscore: topN_ = std::max(topN_ - 4, 4); rebuild(); update(); break;
    case Qt::Key_0: excluded_.clear(); rebuild(); update(); break;   // un-hide all
    default: QWidget::keyPressEvent(e);
    }
}
