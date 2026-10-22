#pragma once
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>

// Куда AZБК пишет то, что пишет БК (EEPROM, AZ.INI из Setup, файлы, снимки экрана): с
// записью на карту — сама карта, иначе свой каталог данных для каждой карты SD
// (<AppData>/azbk/<имя>-<хеш пути>), чтобы шаблон roms/azbk не менялся, а настройки одной
// карты не попадали на другую.
inline QString azWriteDirFor(const QString& sdDir, bool rw) {
    if (rw) return sdDir;
    const QString abs = QDir(sdDir).absolutePath();
    const QString tag = QString::fromLatin1(QCryptographicHash::hash(abs.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/azbk/"
           + QFileInfo(abs).fileName() + "-" + tag;
}
