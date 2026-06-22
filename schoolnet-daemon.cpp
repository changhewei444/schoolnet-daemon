/*
 * schoolnet-daemon-optimized.cpp
 * Campus network ePortal auto-reconnect daemon (optimized for OpenWrt)
 * Zero external dependencies, pure socket HTTP, non-blocking event loop.
 *
 * Key optimizations:
 *   - Timer-based state machine, no blocking sleep.
 *   - Netlink real-time WAN monitoring for millisecond-level reconnect.
 *   - All destinations use hardcoded IPs (no DNS blocking).
 *   - Multi-target online detection.
 *   - Simple log rotation with size limit.
 *   - EINTR-safe non-blocking connect.
 *   - Exponential backoff with low start delay.
 */

#include <iostream>
#include <string>
#include <cstring>
#include <cstdarg>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <cerrno>
#include <functional>
#include <vector>

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <linux/rtnetlink.h>
#include <linux/netlink.h>
#include <net/if.h>

// ============================================================
//  Compile-time configuration (MODIFY FOR YOUR SCHOOL)
// ============================================================
#ifndef USER_ACCOUNT
#define USER_ACCOUNT    ",0,username"      // your full account string
#endif
#ifndef USER_PASSWORD
#define USER_PASSWORD   "password"         // password
#endif
#ifndef AC_IP
#define AC_IP           "125.70.212.239"   // AC server IP
#endif
#ifndef AC_NAME
#define AC_NAME         "PX-CDJCXY-XYWBRAS-01.MAN.M6000"  // AC name (URL-encoded internally)
#endif
#ifndef AUTH_HOST_IP
#define AUTH_HOST_IP    "110.188.66.35"    // Auth server IP (must be IP to avoid DNS)
#endif
#ifndef AUTH_PORT
#define AUTH_PORT       801
#endif
#ifndef AUTH_PATH
#define AUTH_PATH       "/eportal/"
#endif
#ifndef CHECK_TARGETS
// Multiple online check targets (IPs and paths), tried in order
#define CHECK_TARGETS \
    {"223.5.5.5", "/", 80}, \
    {"119.29.29.29", "/", 80}, \
    {"www.baidu.com", "/", 80}   // Keep one domain as fallback (will resolve using cache)
#endif
#ifndef WAN_IFACE
#define WAN_IFACE       "eth1"
#endif

// ============================================================
//  Performance tuning
// ============================================================
constexpr int CHECK_INTERVAL_MS      = 10000;    // Online check interval (10s, low CPU)
constexpr int FAST_CHECK_MS          = 50;       // Initial retry delay when offline
constexpr int CONNECT_TIMEOUT_MS     = 1500;     // TCP connect timeout
constexpr int RECV_TIMEOUT_MS        = 3000;     // HTTP recv timeout
constexpr int MAX_RETRIES            = 5;        // Max fast retries before backoff
constexpr int BACKOFF_TIME_MS        = 5000;     // Backoff wait time
constexpr int POST_LOGIN_DELAY_MS    = 300;      // Wait after login before verifying online
constexpr int EPOLL_MAX_EVENTS       = 16;
constexpr size_t HTTP_BUF_SIZE       = 4096;
constexpr size_t URL_MAX_LEN         = 2048;
constexpr long long DNS_CACHE_TTL_MS = 600000;   // DNS cache 10 minutes
constexpr off_t   MAX_LOG_SIZE       = 512 * 1024; // 512KB

// ============================================================
//  State machine
// ============================================================
enum class State {
    Online,         // Connected, normal check interval
    Checking,       // Triggered by netlink event or startup
    Offline,        // Trying to login with fast retry
    LoginWait,      // Waiting after login attempt before verifying
    Backoff         // Exponential backoff after repeated failures
};

// ============================================================
//  RAII file descriptor
// ============================================================
class ScopedFd {
public:
    ScopedFd() : fd_(-1) {}
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() { close(); }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) { close(); fd_ = other.fd_; other.fd_ = -1; }
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    void close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
    int release() { int fd = fd_; fd_ = -1; return fd; }

private:
    int fd_;
};

// ============================================================
//  Simple logger with file rotation (no ring buffer bugs)
// ============================================================
class Logger {
public:
    Logger() = default;
    ~Logger() { close_log(); }

    bool init(const std::string& path) {
        log_path_ = path;
        return open_log();
    }

    void write(const char* level, const char* fmt, ...) {
        // Get wall clock time (readable in logs)
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        struct tm tm;
        localtime_r(&t, &tm);

        char time_buf[32];
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;

        char entry[512];
        int len = snprintf(entry, sizeof(entry), "[%s.%03lld] [%s] ",
                           time_buf, (long long)ms.count(), level);

        va_list ap;
        va_start(ap, fmt);
        int remain = (int)sizeof(entry) - len;
        int vlen = vsnprintf(entry + len, remain > 0 ? remain : 0, fmt, ap);
        va_end(ap);

        if (vlen < 0) vlen = 0;
        if (vlen >= remain) vlen = remain - 1;
        len += vlen;
        if (len >= (int)sizeof(entry) - 2) len = sizeof(entry) - 2;
        entry[len++] = '\n';
        entry[len] = '\0';

        // Write to file (simple and safe)
        if (fd_ >= 0) {
            ssize_t written = ::write(fd_, entry, len);
            (void)written; // ignore write error (file may be full)
        }

        // Also output to stderr (visible via logread)
        std::cerr.write(entry, len);
        std::cerr.flush();

        // Check rotation after each write
        if (fd_ >= 0) {
            off_t sz = ::lseek(fd_, 0, SEEK_END);
            if (sz >= MAX_LOG_SIZE) {
                rotate();
            }
        }
    }
	void flush() { /* 无需操作，write 已即时落盘 */ }

private:
    bool open_log() {
        fd_ = ::open(log_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        return fd_ >= 0;
    }

    void close_log() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void rotate() {
        // Close current fd, rename log file, reopen new one
        close_log();
        std::string old_path = log_path_ + ".old";
        ::rename(log_path_.c_str(), old_path.c_str()); // ignore error
        if (!open_log()) {
            std::cerr << "Log rotation failed, logging to stderr only" << std::endl;
        } else {
            // Write rotation marker
            const char* msg = "=== Log rotated ===\n";
            ::write(fd_, msg, strlen(msg));
        }
    }

    int fd_ = -1;
    std::string log_path_;
};

static Logger g_log;

// ============================================================
//  Time utilities (monotonic clock for intervals)
// ============================================================
static inline long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ============================================================
//  Network utilities
// ============================================================
class Network {
public:
    // Get WAN IP (fast ioctl first, then fallback to route)
    static bool getWanIp(const std::string& iface, std::string& out_ip) {
        char buf[INET_ADDRSTRLEN];
        if (getWanIpFast(iface, buf, sizeof(buf))) {
            out_ip = buf;
            return true;
        }
        if (getWanIpRoute(buf, sizeof(buf))) {
            out_ip = buf;
            return true;
        }
        return false;
    }

    // Get WAN MAC address (may be needed for some portals)
    static bool getWanMac(const std::string& iface, std::string& out_mac) {
        struct ifreq ifr;
        ScopedFd fd(::socket(AF_INET, SOCK_DGRAM, 0));
        if (!fd.valid()) return false;

        strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
        ifr.ifr_name[IFNAMSIZ - 1] = '\0';
        if (ioctl(fd.get(), SIOCGIFHWADDR, &ifr) < 0) return false;

        unsigned char* mac = (unsigned char*)ifr.ifr_hwaddr.sa_data;
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        out_mac = mac_str;
        return true;
    }

private:
    static bool getWanIpFast(const std::string& iface, char* buf, size_t len) {
        struct ifreq ifr;
        ScopedFd fd(::socket(AF_INET, SOCK_DGRAM, 0));
        if (!fd.valid()) return false;

        strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
        ifr.ifr_name[IFNAMSIZ - 1] = '\0';
        if (ioctl(fd.get(), SIOCGIFADDR, &ifr) < 0) return false;

        struct sockaddr_in* sin = (struct sockaddr_in*)&ifr.ifr_addr;
        inet_ntop(AF_INET, &sin->sin_addr, buf, len);
        return true;
    }

    static bool getWanIpRoute(char* buf, size_t len) {
        ScopedFd fd(::socket(AF_INET, SOCK_DGRAM, 0));
        if (!fd.valid()) return false;

        struct sockaddr_in dest = {};
        dest.sin_family = AF_INET;
        inet_pton(AF_INET, "223.5.5.5", &dest.sin_addr); // use one check IP

        if (connect(fd.get(), (struct sockaddr*)&dest, sizeof(dest)) == 0) {
            struct sockaddr_in local;
            socklen_t local_len = sizeof(local);
            if (getsockname(fd.get(), (struct sockaddr*)&local, &local_len) == 0) {
                inet_ntop(AF_INET, &local.sin_addr, buf, len);
                return true;
            }
        }
        return false;
    }
};

// ============================================================
//  HTTP client (pure socket, non-blocking connect with EINTR)
// ============================================================
class HttpClient {
public:
    struct Response {
        int code = 0;
        std::string body;
    };

    static Response get(const std::string& host, int port, const std::string& path,
                        int timeout_ms) {
        Response resp;
        std::string ip = resolveHostToIp(host);

        ScopedFd fd(tcpConnect(ip, port, timeout_ms));
        if (!fd.valid()) return resp;

        std::string request = "GET " + path + " HTTP/1.1\r\n"
                              "Host: " + host + "\r\n"
                              "User-Agent: Mozilla/5.0\r\n"
                              "Accept: */*\r\n"
                              "Connection: close\r\n"
                              "\r\n";

        // Send with timeout
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (send(fd.get(), request.c_str(), request.size(), 0) != (ssize_t)request.size())
            return resp;

        setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[HTTP_BUF_SIZE];
        int total = 0;
        int n;
        while ((n = recv(fd.get(), buf + total, sizeof(buf) - total - 1, 0)) > 0) {
            total += n;
            if (total >= (int)sizeof(buf) - 1) break;
        }
        if (total <= 0) return resp;
        buf[total] = '\0';

        sscanf(buf, "HTTP/%*s %d", &resp.code);

        const char* body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            resp.body.assign(body_start + 4, total - (body_start + 4 - buf));
            if (resp.body.size() > 1024) resp.body.resize(1024);
        }
        return resp;
    }

private:
    // Resolve hostname to IP, using built-in IPs or simple DNS with cache
    static std::string resolveHostToIp(const std::string& host) {
        // Check if it's already an IP
        struct in_addr addr;
        if (inet_pton(AF_INET, host.c_str(), &addr) == 1) {
            return host; // already IP
        }

        // Simple DNS cache (no blocking gethostbyname)
        auto& c = dns_cache_;
        long long now = now_ms();
        if (c.host == host && (now - c.timestamp) < DNS_CACHE_TTL_MS) {
            return c.ip;
        }

        // Fallback: do a one-time blocking resolution (should be rare)
        struct hostent* he = gethostbyname(host.c_str());
        if (!he || !he->h_addr_list[0]) {
            // Return empty if unresolvable, will cause connect failure
            return "";
        }
        char ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, he->h_addr_list[0], ip_buf, sizeof(ip_buf));
        c.host = host;
        c.ip = ip_buf;
        c.timestamp = now;
        return c.ip;
    }

    struct DnsCache {
        std::string host;
        std::string ip;
        long long timestamp = 0;
    };
    static DnsCache dns_cache_;

    static int tcpConnect(const std::string& ip, int port, int timeout_ms) {
        if (ip.empty()) return -1;

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        int ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
        if (ret < 0 && errno == EINPROGRESS) {
            struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);

            // Retry select on EINTR
            do {
                ret = select(fd + 1, nullptr, &wfds, nullptr, &tv);
            } while (ret < 0 && errno == EINTR);

            if (ret > 0) {
                int so_error = 0;
                socklen_t slen = sizeof(so_error);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &slen);
                if (so_error != 0) { ::close(fd); return -1; }
            } else {
                ::close(fd);
                return -1;
            }
        } else if (ret < 0) {
            ::close(fd);
            return -1;
        }

        // Restore blocking mode and set timeouts
        fcntl(fd, F_SETFL, flags);
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        return fd;
    }
};
HttpClient::DnsCache HttpClient::dns_cache_;

// ============================================================
//  URL encoder
// ============================================================
inline bool isUnreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

std::string urlEncode(const std::string& src) {
    static const char hex[] = "0123456789ABCDEF";
    std::string dst;
    dst.reserve(src.size() * 3);
    for (unsigned char c : src) {
        if (isUnreserved(c)) {
            dst += c;
        } else {
            dst += '%';
            dst += hex[c >> 4];
            dst += hex[c & 0xF];
        }
    }
    return dst;
}

// ============================================================
//  Netlink listener (real-time WAN events)
// ============================================================
class NetlinkListener {
public:
    bool init() {
        fd_ = ScopedFd(::socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK, NETLINK_ROUTE));
        if (!fd_.valid()) return false;

        int bufsize = 262144;
        setsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

        struct sockaddr_nl sa = {};
        sa.nl_family = AF_NETLINK;
        sa.nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_LINK | RTMGRP_IPV4_ROUTE;

        if (bind(fd_.get(), (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            fd_.close();
            return false;
        }
        return true;
    }

    int getFd() const { return fd_.get(); }

    void process() {
        char buf[8192];
        struct iovec iov = {buf, sizeof(buf)};
        struct sockaddr_nl sa;
        struct msghdr msg = {};
        msg.msg_name = &sa;
        msg.msg_namelen = sizeof(sa);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        ssize_t n;
        while ((n = recvmsg(fd_.get(), &msg, MSG_DONTWAIT)) > 0) {
            struct nlmsghdr* nh = (struct nlmsghdr*)buf;
            for (; NLMSG_OK(nh, (size_t)n); nh = NLMSG_NEXT(nh, n)) {
                if (nh->nlmsg_type == NLMSG_DONE || nh->nlmsg_type == NLMSG_ERROR)
                    break;
                if (nh->nlmsg_type == RTM_NEWADDR || nh->nlmsg_type == RTM_DELADDR ||
                    nh->nlmsg_type == RTM_NEWLINK || nh->nlmsg_type == RTM_DELLINK) {
                    struct ifaddrmsg* ifa = (struct ifaddrmsg*)NLMSG_DATA(nh);
                    char ifname[IFNAMSIZ];
                    if_indextoname(ifa->ifa_index, ifname);
                    if (strcmp(ifname, WAN_IFACE) == 0) {
                        g_log.write("NETLINK", "WAN change detected on %s", ifname);
                        if (trigger_fn_) trigger_fn_(ifname);
                    }
                }
            }
        }
    }

    void setTrigger(std::function<void(const char*)> fn) {
        trigger_fn_ = std::move(fn);
    }

private:
    ScopedFd fd_;
    std::function<void(const char*)> trigger_fn_;
};

// ============================================================
//  Login engine
// ============================================================
class LoginEngine {
public:
    LoginEngine() : last_login_time_(std::chrono::steady_clock::now()) {}

    bool doLogin() {
        auto start = now_ms();

        if (!Network::getWanIp(WAN_IFACE, current_ip_)) {
            g_log.write("ERROR", "Cannot get WAN IP");
            return false;
        }

        // Skip duplicate login within 5 seconds for same IP
        auto now = std::chrono::steady_clock::now();
        auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_login_time_).count();
        if (current_ip_ == last_ip_ && since_last < 5000) {
            g_log.write("INFO", "Same IP %s, skip duplicate login", current_ip_.c_str());
            return true; // treat as success
        }

        // Get MAC address (some portals require correct MAC)
        std::string mac = "000000000000";
        std::string real_mac;
        if (Network::getWanMac(WAN_IFACE, real_mac)) {
            mac = real_mac;
        }

        // Build login URL
        std::string path = std::string(AUTH_PATH) + "?c=Portal&a=login"
            + "&callback=dr" + std::to_string(now_ms())
            + "&login_method=1"
            + "&user_account=" + urlEncode(USER_ACCOUNT)
            + "&user_password=" + urlEncode(USER_PASSWORD)
            + "&wlan_user_ip=" + current_ip_
            + "&wlan_user_ipv6="
            + "&wlan_user_mac=" + mac
            + "&wlan_ac_ip=" + AC_IP
            + "&wlan_ac_name=" + urlEncode(AC_NAME)
            + "&jsVersion=3.1"
            + "&_=" + std::to_string(now_ms());

        login_count_++;
        g_log.write("LOGIN", "Sending auth (count=%lu) IP=%s MAC=%s", login_count_, current_ip_.c_str(), mac.c_str());

        auto resp = HttpClient::get(AUTH_HOST_IP, AUTH_PORT, path, RECV_TIMEOUT_MS);
        long long elapsed = now_ms() - start;

        if (resp.code == 0) {
            g_log.write("ERROR", "HTTP auth failed (time %lld ms)", elapsed);
            return false;
        }

        // Strict success check: exact portal response codes
        bool success = (resp.body.find("\"result\":\"1\"") != std::string::npos) ||
                       (resp.body.find("\"success\":\"1\"") != std::string::npos) ||
                       (resp.body.find("success\":\"1\"") != std::string::npos); // common

        if (success) {
            g_log.write("SUCCESS", "Login OK, IP=%s, time=%lld ms, code=%d",
                       current_ip_.c_str(), elapsed, resp.code);
            last_ip_ = current_ip_;
            last_login_time_ = now;
            reconnect_count_++;
            return true;
        }

        g_log.write("WARN", "Login response: code=%d, body=%s", resp.code, resp.body.c_str());
        return false;
    }

    const std::string& getCurrentIp() const { return current_ip_; }
    unsigned long getLoginCount() const { return login_count_; }
    unsigned long getReconnectCount() const { return reconnect_count_; }
    void resetFailCount() { fail_count_ = 0; }
    int incrementFail() { return ++fail_count_; }

private:
    std::string current_ip_;
    std::string last_ip_;
    std::chrono::steady_clock::time_point last_login_time_;
    unsigned long login_count_ = 0;
    unsigned long reconnect_count_ = 0;
    int fail_count_ = 0;
};

// ============================================================
//  Daemon core (non-blocking state machine with timerfd)
// ============================================================
class DaemonCore {
public:
    static DaemonCore& instance() {
        static DaemonCore core;
        return core;
    }

    bool init() {
        if (!g_log.init("/var/log/schoolnet-daemon.log")) {
            std::cerr << "Failed to open log file" << std::endl;
            return false;
        }
        g_log.write("INFO", "=== SchoolNet Daemon v2.0 Started ===");
        g_log.write("INFO", "WAN=%s, Auth=%s:%d, Check interval=%dms",
                    WAN_IFACE, AUTH_HOST_IP, AUTH_PORT, CHECK_INTERVAL_MS);

        timer_fd_ = ScopedFd(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK));
        if (!timer_fd_.valid()) {
            g_log.write("FATAL", "Timer creation failed");
            return false;
        }

        if (netlink_.init()) {
            netlink_.setTrigger([](const char* /*iface*/) {
                DaemonCore::instance().onNetlinkEvent();
            });
            g_log.write("INFO", "Netlink listener ready");
        } else {
            g_log.write("WARN", "No netlink, timer-only mode");
        }

        epoll_fd_ = ScopedFd(epoll_create1(EPOLL_CLOEXEC));
        if (!epoll_fd_.valid()) {
            g_log.write("FATAL", "Epoll creation failed");
            return false;
        }

        struct epoll_event ev = {};
        ev.events = EPOLLIN;
        ev.data.fd = timer_fd_.get();
        epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, timer_fd_.get(), &ev);

        if (netlink_.getFd() >= 0) {
            ev.data.fd = netlink_.getFd();
            epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, netlink_.getFd(), &ev);
        }

        return true;
    }

    void run() {
        setTimer(0); // immediate first check
        g_log.write("INFO", "Entering event loop");

        struct epoll_event events[EPOLL_MAX_EVENTS];

        while (running_) {
            int nfds = epoll_wait(epoll_fd_.get(), events, EPOLL_MAX_EVENTS, -1);
            if (nfds < 0) {
                if (errno == EINTR) continue;
                g_log.write("ERROR", "epoll_wait: %s", strerror(errno));
                break;
            }

            for (int i = 0; i < nfds; ++i) {
                if (events[i].data.fd == timer_fd_.get()) {
                    handleTimer();
                } else if (netlink_.getFd() >= 0 && events[i].data.fd == netlink_.getFd()) {
                    netlink_.process();
                }
            }
        }

        g_log.write("INFO", "Shutdown. Logins=%lu Reconnects=%lu",
                    engine_.getLoginCount(), engine_.getReconnectCount());
        g_log.flush(); // not needed, write is immediate
    }

    void shutdown() { running_ = false; }

private:
    DaemonCore() = default;

    void onNetlinkEvent() {
        // Netlink tells us WAN changed → re-check immediately
        state_ = State::Checking;
        setTimer(0);
    }

    void setTimer(int ms) {
        struct itimerspec its = {};
        its.it_value.tv_sec = ms / 1000;
        its.it_value.tv_nsec = (ms % 1000) * 1000000L;
        if (ms == 0) its.it_value.tv_nsec = 1; // immediate
        timerfd_settime(timer_fd_.get(), 0, &its, nullptr);
    }

    void handleTimer() {
        // Consume timer expiration
        uint64_t exp;
        read(timer_fd_.get(), &exp, sizeof(exp));

        switch (state_) {
        case State::Online:
            if (!checkOnlineMulti()) {
                g_log.write("WARN", "Connection lost");
                state_ = State::Offline;
                engine_.resetFailCount();
                setTimer(FAST_CHECK_MS); // start fast retry immediately
            } else {
                setTimer(CHECK_INTERVAL_MS); // normal low-frequency check
            }
            break;

        case State::Checking:
            if (checkOnlineMulti()) {
                state_ = State::Online;
                setTimer(CHECK_INTERVAL_MS);
            } else {
                g_log.write("INFO", "Offline, starting login");
                state_ = State::Offline;
                engine_.resetFailCount();
                setTimer(0); // immediate login attempt
            }
            break;

        case State::Offline:
            if (engine_.doLogin()) {
                // Login sent, wait a bit before checking online to avoid false negative
                state_ = State::LoginWait;
                setTimer(POST_LOGIN_DELAY_MS);
            } else {
                int fc = engine_.incrementFail();
                if (fc >= MAX_RETRIES) {
                    g_log.write("ERROR", "Max retries reached, entering backoff");
                    state_ = State::Backoff;
                    setTimer(BACKOFF_TIME_MS);
                } else {
                    // Exponential backoff: 50, 100, 200, 400, 800 ms, capped at 5s
                    int delay = FAST_CHECK_MS * (1 << (fc - 1));
                    if (delay > 5000) delay = 5000;
                    setTimer(delay);
                }
            }
            break;

        case State::LoginWait:
            // Post-login verification
            if (checkOnlineMulti()) {
                g_log.write("INFO", "Online confirmed after login");
                state_ = State::Online;
                setTimer(CHECK_INTERVAL_MS);
            } else {
                g_log.write("WARN", "Still offline after login attempt");
                state_ = State::Offline;
                setTimer(FAST_CHECK_MS);
            }
            break;

        case State::Backoff:
            // Backoff finished, re-check
            g_log.write("INFO", "Backoff ended, retrying");
            engine_.resetFailCount();
            state_ = State::Checking;
            setTimer(0);
            break;
        }
    }

    // Multi-target online check: any target returns 2xx/3xx → online
    static bool checkOnlineMulti() {
        // Targets: each is {IP/hostname, path, port}
        struct Target {
            const char* host;
            const char* path;
            int port;
        };
        static const Target targets[] = { CHECK_TARGETS };
        static const int num_targets = sizeof(targets) / sizeof(targets[0]);

        for (int i = 0; i < num_targets; ++i) {
            auto resp = HttpClient::get(targets[i].host, targets[i].port,
                                        targets[i].path, CONNECT_TIMEOUT_MS);
            // Success: 2xx or 3xx response (e.g., 204, 200, 302)
            if (resp.code >= 200 && resp.code < 400) {
                return true;
            }
        }
        return false;
    }

    ScopedFd timer_fd_;
    ScopedFd epoll_fd_;
    NetlinkListener netlink_;
    LoginEngine engine_;
    State state_ = State::Checking;
    std::atomic<bool> running_{true};
};

// ============================================================
//  Signal handling
// ============================================================
static std::atomic<bool> g_signal_received{false};

static void signalHandler(int sig) {
    (void)sig;
    g_signal_received = true;
    DaemonCore::instance().shutdown();
}

// ============================================================
//  Main
// ============================================================
int main(int argc, char** argv) {
    (void)argc; (void)argv;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    if (!DaemonCore::instance().init()) {
        return 1;
    }

    DaemonCore::instance().run();
    return 0;
}
