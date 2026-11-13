#pragma once

// HelpServer: serves a help page, and the files it shows, to the browser over
// loopback HTTP, so a sandboxed browser can read it. Ubuntu's Firefox is a
// snap: it can't read the real /tmp or hidden folders in the home directory
// (~/.local), where help pages are written or installed, but it can load
// http://127.0.0.1. Header-only, with no dependencies beyond POSIX, so every
// plugin can use it without linking anything.
//
// The server runs inside a host's process (a DAW, for a plugin), so it's kept
// small and refuses anything it doesn't expect:
//   - It listens on 127.0.0.1 only, never on another interface, on a port the
//     kernel picks.
//   - Files are in memory and found by exact name: no request path reaches
//     the filesystem, so there's no traversal to get wrong.
//   - Every URL carries a random 128-bit token, so a web page probing local
//     ports can't fetch anything; the Host header must be 127.0.0.1:<port>,
//     which a DNS-rebinding page can't send. No CORS headers, so other
//     origins can't read a response either.
//   - Requests are GET or HEAD, at most 8 KiB, read with a 2 s timeout, one
//     connection at a time, on the server's own thread (never the caller's,
//     never audio). Nothing it does can throw out of the thread, and writes
//     can't raise SIGPIPE in the host.
//   - Sockets and pipes are close-on-exec, so the browser launch doesn't
//     inherit them, and the browser is launched without a shell parsing
//     the URL.
//   - It stops after 15 minutes without a request, and when destroyed: the
//     destructor joins the thread, so a plugin's code isn't unloaded while it
//     runs. Another click on Help starts it again (with a new port and token).

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

namespace uw {

class HelpServer {
public:
    struct File {
        std::string name;   // as the page refers to it, e.g. "help.html", "Unlikelyware.png"
        std::string data;
    };

    static constexpr std::size_t kMaxRequest   = 8192;
    static constexpr int         kIoTimeoutMs  = 2000;
    static constexpr int         kIdleSeconds  = 15 * 60;

    HelpServer() = default;
    ~HelpServer() { stop(); }
    HelpServer(const HelpServer&)            = delete;
    HelpServer& operator=(const HelpServer&) = delete;

    // Serves `files` (files[0] is the page) and returns the page's URL, or ""
    // if no socket could be opened. If already running, the files are replaced
    // and the port and token kept, so an open tab keeps working.
    std::string serve(std::vector<File> files) {
        if (files.empty()) return {};
        const std::string page = files.front().name;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            files_.clear();
            for (File& f : files) files_[f.name] = std::move(f.data);
        }
        lastRequest_.store(nowSeconds());   // before the check, so an idle stop can't race it
        if (!running_.load()) {
            stop();
            if (!start()) return {};
        }
        return "http://127.0.0.1:" + std::to_string(port_) + "/" + token_ + "/" + page;
    }

    // serve(), then the page in the default browser. False if either fails.
    bool open(std::vector<File> files) {
        const std::string url = serve(std::move(files));
        return !url.empty() && openUrl(url);
    }

    void stop() {
        if (thread_.joinable()) {
            stopping_.store(true);
            if (wake_[1] >= 0) {
                const char c = 0;
                (void)!::write(wake_[1], &c, 1);
            }
            thread_.join();
        }
        closeFd(listener_);
        closeFd(wake_[0]);
        closeFd(wake_[1]);
        running_.store(false);
        stopping_.store(false);
    }

    bool          running() const { return running_.load(); }
    int           port() const { return port_; }
    std::uint64_t served() const { return served_.load(); }   // 200 responses, for tests

    // The regular files directly in `dir` (an installed help folder), page
    // first; empty if the page isn't there. At most 32 MiB in all.
    static std::vector<File> readDirectory(const std::string& dir, const std::string& page) {
        namespace fs = std::filesystem;
        std::vector<File> files;
        std::error_code   ec;
        std::size_t       total = 0;
        auto read = [&](const fs::path& p) {
            std::ifstream in(p, std::ios::binary);
            if (!in) return false;
            std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            total += data.size();
            if (total > (32u << 20)) return false;
            files.push_back(File{p.filename().string(), std::move(data)});
            return true;
        };
        if (!fs::is_regular_file(fs::path(dir) / page, ec) || !read(fs::path(dir) / page)) return {};
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().filename() == page || !it->is_regular_file(ec)) continue;
            if (!read(it->path())) break;
        }
        return files;
    }

    // The URL in the default browser, launched without a shell: on Linux
    // through `sh -c 'xdg-open "$1" &'` with the URL as an argument (so
    // xdg-open, which may wait for the browser, doesn't block or leave a zombie).
    static bool openUrl(const std::string& url) {
#ifdef __APPLE__
        const char* argv[] = {"open", url.c_str(), nullptr};
#else
        const char* argv[] = {"sh", "-c", "xdg-open \"$1\" >/dev/null 2>&1 </dev/null &", "sh", url.c_str(),
                              nullptr};
#endif
        pid_t pid = 0;
        if (posix_spawnp(&pid, argv[0], nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0)
            return false;
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

private:
    static std::int64_t nowSeconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    static void closeFd(int& fd) {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    static void setCloexec(int fd) { ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC); }

    static std::string randomToken() {
        std::random_device rd;   // /dev/urandom on Linux and macOS
        static const char hex[] = "0123456789abcdef";
        std::string       token;
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t v = rd();
            for (int b = 0; b < 8; ++b) token += hex[(v >> (b * 4)) & 0xf];
        }
        return token;
    }

    bool start() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) return false;
        setCloexec(listener_);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        socklen_t len        = sizeof addr;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 ||
            ::listen(listener_, 16) < 0 ||
            ::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) < 0 || ::pipe(wake_) < 0) {
            stop();
            return false;
        }
        setCloexec(wake_[0]);
        setCloexec(wake_[1]);
        port_  = ntohs(addr.sin_port);
        token_ = randomToken();
        running_.store(true);
        lastRequest_.store(nowSeconds());
        try {
            thread_ = std::thread([this] { run(); });
        } catch (...) {
            stop();
            return false;
        }
        return true;
    }

    void run() noexcept {
        try {
            while (!stopping_.load()) {
                pollfd fds[2] = {{listener_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
                const int n   = ::poll(fds, 2, 1000);
                if (n < 0 && errno != EINTR) break;
                if (fds[1].revents != 0) break;
                if (n > 0 && (fds[0].revents & POLLIN)) {
                    const int client = ::accept(listener_, nullptr, nullptr);
                    if (client >= 0) {
                        setCloexec(client);
                        handle(client);
                        ::close(client);
                    }
                }
                if (nowSeconds() - lastRequest_.load() > kIdleSeconds) break;
            }
        } catch (...) {
        }
        running_.store(false);
    }

    static bool sendAll(int fd, const char* p, std::size_t size) {
        while (size > 0) {
#ifdef MSG_NOSIGNAL
            const ssize_t n = ::send(fd, p, size, MSG_NOSIGNAL);
#else
            const ssize_t n = ::send(fd, p, size, 0);
#endif
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            p += n;
            size -= static_cast<std::size_t>(n);
        }
        return true;
    }

    static const char* contentType(const std::string& name) {
        const std::size_t dot = name.rfind('.');
        const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
        if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
        if (ext == "css") return "text/css; charset=utf-8";
        if (ext == "js") return "text/javascript; charset=utf-8";
        if (ext == "png") return "image/png";
        if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
        if (ext == "svg") return "image/svg+xml";
        if (ext == "gif") return "image/gif";
        if (ext == "webp") return "image/webp";
        if (ext == "woff2") return "font/woff2";
        if (ext == "ttf") return "font/ttf";
        return "application/octet-stream";
    }

    static void respond(int fd, int status, const char* reason, const char* type, const std::string* body,
                        bool head) {
        const std::size_t size   = body != nullptr ? body->size() : 0;
        std::string       header = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
        header += "Content-Type: " + std::string(type) + "\r\n";
        header += "Content-Length: " + std::to_string(size) + "\r\n";
        header += "Cache-Control: no-store\r\n"
                  "X-Content-Type-Options: nosniff\r\n"
                  "Referrer-Policy: no-referrer\r\n"
                  "Connection: close\r\n\r\n";
        if (sendAll(fd, header.data(), header.size()) && body != nullptr && !head)
            sendAll(fd, body->data(), body->size());
    }

    static bool equalsIgnoreCase(const std::string& a, const char* b) {
        const std::size_t n = std::strlen(b);
        if (a.size() != n) return false;
        for (std::size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    void handle(int fd) {
        timeval tv{kIoTimeoutMs / 1000, (kIoTimeoutMs % 1000) * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        // The request head, up to its blank line.
        std::string request;
        char        buffer[2048];
        const auto  deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kIoTimeoutMs);
        while (request.find("\r\n\r\n") == std::string::npos) {
            if (request.size() >= kMaxRequest || std::chrono::steady_clock::now() > deadline) {
                respond(fd, 400, "Bad Request", "text/plain", nullptr, false);
                return;
            }
            const ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return;
            request.append(buffer, static_cast<std::size_t>(n));
        }
        request.resize(request.find("\r\n\r\n") + 2);

        // "GET /<token>/<name> HTTP/1.x"
        const std::size_t lineEnd = request.find("\r\n");
        const std::string line    = request.substr(0, lineEnd);
        const std::size_t sp1     = line.find(' ');
        const std::size_t sp2     = sp1 == std::string::npos ? sp1 : line.find(' ', sp1 + 1);
        if (sp2 == std::string::npos || line.compare(sp2 + 1, 7, "HTTP/1.") != 0) {
            respond(fd, 400, "Bad Request", "text/plain", nullptr, false);
            return;
        }
        const std::string method = line.substr(0, sp1);
        std::string       target = line.substr(sp1 + 1, sp2 - sp1 - 1);
        const bool        head   = method == "HEAD";
        if (method != "GET" && !head) {
            respond(fd, 405, "Method Not Allowed", "text/plain", nullptr, false);
            return;
        }

        // Host must be this server, as typed into the URL (DNS rebinding).
        std::string host;
        for (std::size_t at = lineEnd + 2; at < request.size();) {
            const std::size_t end   = request.find("\r\n", at);
            const std::string field = request.substr(at, end - at);
            at                      = end + 2;
            const std::size_t colon = field.find(':');
            if (colon == std::string::npos) continue;
            if (equalsIgnoreCase(field.substr(0, colon), "host")) {
                std::size_t b = colon + 1, e = field.size();
                while (b < e && (field[b] == ' ' || field[b] == '\t')) ++b;
                while (e > b && (field[e - 1] == ' ' || field[e - 1] == '\t')) --e;
                host = field.substr(b, e - b);
            }
        }
        if (host != "127.0.0.1:" + std::to_string(port_)) {
            respond(fd, 403, "Forbidden", "text/plain", nullptr, head);
            return;
        }

        const std::size_t query = target.find_first_of("?#");
        if (query != std::string::npos) target.resize(query);
        const std::string prefix = "/" + token_ + "/";
        if (target.compare(0, prefix.size(), prefix) != 0) {
            respond(fd, 404, "Not Found", "text/plain", nullptr, head);
            return;
        }
        const std::string name = target.substr(prefix.size());

        std::string body;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = files_.find(name);
            if (it == files_.end()) {
                respond(fd, 404, "Not Found", "text/plain", nullptr, head);
                return;
            }
            body = it->second;
        }
        lastRequest_.store(nowSeconds());
        respond(fd, 200, "OK", contentType(name), &body, head);
        served_.fetch_add(1);
    }

    std::mutex                         mutex_;
    std::map<std::string, std::string> files_;   // guarded by mutex_
    std::thread                        thread_;
    std::atomic<bool>                  running_{false};
    std::atomic<bool>                  stopping_{false};
    std::atomic<std::int64_t>          lastRequest_{0};
    std::atomic<std::uint64_t>         served_{0};
    int                                listener_ = -1;
    int                                wake_[2]  = {-1, -1};
    int                                port_     = 0;
    std::string                        token_;
};

} // namespace uw
