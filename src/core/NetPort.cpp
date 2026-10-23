#include "NetPort.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef __linux__
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace bk {

#ifdef __linux__
namespace {

std::string sysValue(const std::string& ifname, const char* key) {
    std::ifstream f("/sys/class/net/" + ifname + "/" + key);
    std::string s;
    std::getline(f, s);
    return s;
}

class TapPort : public EthPort {
public:
    TapPort(int fd, std::string ifname, std::string what) : fd_(fd), if_(std::move(ifname)), what_(std::move(what)) {}
    ~TapPort() override { if (fd_ >= 0) ::close(fd_); }
    size_t recv(uint8_t* buf, size_t cap) override {
        for (;;) {
            const ssize_t n = ::read(fd_, buf, cap);
            if (n > 0) return static_cast<size_t>(n);
            if (n < 0 && errno == EINTR) continue;
            return 0;
        }
    }
    void send(const uint8_t* frame, size_t len) override {
        ssize_t n;
        do n = ::write(fd_, frame, len); while (n < 0 && errno == EINTR);
    }
    // Несущая — по sysfs (у tap она есть, пока интерфейс поднят и устройство открыто).
    bool link() const override {
        const std::string c = sysValue(if_, "carrier");
        return c.empty() || c == "1";
    }
    std::string describe() const override { return what_; }
private:
    int fd_;
    std::string if_, what_;
};

std::string macText(const uint8_t m[6]) {
    char b[24];
    std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

// Запрос режима: кадр без заголовка PI и без virtio_net_hdr (у macvtap он по умолчанию включён).
bool setTapFlags(int fd, const char* name, std::string* err) {
    struct ifreq ifr {};
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (name) std::strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (::ioctl(fd, TUNSETIFF, &ifr) < 0) {
        if (err) *err = std::string("TUNSETIFF: ") + std::strerror(errno);
        return false;
    }
    return true;
}

// Имя интерфейса macvtap по описанию кабеля (macvtap:ИМЯ или /dev/tapN), иначе пусто.
std::string macvtapIfname(const std::string& spec) {
    if (spec.rfind("macvtap:", 0) == 0) return spec.substr(8);
    if (spec.rfind("/dev/tap", 0) != 0) return {};
    const std::string idx = spec.substr(8);
    std::ifstream l("/proc/net/dev");
    std::string line;
    while (std::getline(l, line)) {
        const size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string n = line.substr(0, c);
        n.erase(0, n.find_first_not_of(' '));
        if (sysValue(n, "ifindex") == idx) return n;
    }
    return {};
}

} // namespace

// macvtap принимает только кадры на свой MAC (и широковещательные): MAC интерфейса должен
// совпадать с MAC платы из AZ.INI.
std::string ethPortMacWarning(const std::string& spec, const uint8_t mac[6]) {
    const std::string ifname = macvtapIfname(spec);
    if (ifname.empty()) return {};
    const std::string have = sysValue(ifname, "address");
    if (have.empty() || have == macText(mac)) return {};
    return "у " + ifname + " MAC " + have + ", у платы " + macText(mac) +
           " — ответы на MAC платы не дойдут: sudo ip link set " + ifname + " address " + macText(mac);
}

std::unique_ptr<EthPort> openEthPort(const std::string& spec, const uint8_t mac[6], std::string* err, std::string* warn) {
    auto fail = [&](const std::string& e) { if (err) *err = e; return nullptr; };
    if (spec.rfind("tap:", 0) == 0) {
        const std::string name = spec.substr(4);
        if (name.empty() || name.size() >= IFNAMSIZ) return fail("tap: нужно имя интерфейса");
        const int fd = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return fail(std::string("/dev/net/tun: ") + std::strerror(errno));
        std::string e;
        if (!setTapFlags(fd, name.c_str(), &e)) {
            ::close(fd);
            return fail("tap " + name + ": " + e + " — создайте его заранее: sudo ip tuntap add " + name +
                        " mode tap user $USER && sudo ip link set " + name + " up");
        }
        return std::make_unique<TapPort>(fd, name, "tap " + name);
    }
    std::string dev = spec;
    const std::string ifname = macvtapIfname(spec);
    if (spec.rfind("macvtap:", 0) == 0) {
        const std::string idx = sysValue(ifname, "ifindex");
        if (idx.empty()) return fail("macvtap: нет интерфейса " + ifname);
        dev = "/dev/tap" + idx;
    } else if (spec.rfind("/dev/tap", 0) != 0) {
        return fail("ожидается tap:ИМЯ, macvtap:ИМЯ или /dev/tapN, а не «" + spec + "»");
    }
    const int fd = ::open(dev.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return fail(dev + ": " + std::strerror(errno) + (errno == EACCES ? " — отдайте устройство пользователю: sudo chown $USER " + dev : ""));
    std::string e;
    if (!setTapFlags(fd, nullptr, &e)) { ::close(fd); return fail(dev + ": " + e); }
    if (warn) *warn = ethPortMacWarning(spec, mac);
    return std::make_unique<TapPort>(fd, ifname.empty() ? std::string() : ifname,
                                     "macvtap " + (ifname.empty() ? dev : ifname + " (" + dev + ")"));
}

#else

std::unique_ptr<EthPort> openEthPort(const std::string&, const uint8_t*, std::string* err, std::string*) {
    if (err) *err = "сеть AZБК на этой платформе не поддерживается (нужен Linux: tap/macvtap)";
    return nullptr;
}

std::string ethPortMacWarning(const std::string&, const uint8_t*) { return {}; }

#endif

} // namespace bk
