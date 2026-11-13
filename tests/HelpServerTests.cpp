// HelpServer (uw/base/HelpServer.h): it serves only its files, only to a
// request naming its token and Host, only on 127.0.0.1, and stops cleanly.

#include <arpa/inet.h>
#include <signal.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <string>

#include "Check.h"
#include "uw/base/HelpServer.h"

using namespace uw;

namespace {

// One request on a fresh connection; the whole response.
std::string ask(int port, const std::string& request) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    std::string response;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
#ifdef MSG_NOSIGNAL
        (void)!::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
#else
        (void)!::send(fd, request.data(), request.size(), 0);
#endif
        char buffer[4096];
        for (ssize_t n; (n = ::recv(fd, buffer, sizeof buffer, 0)) > 0;) response.append(buffer, n);
    }
    ::close(fd);
    return response;
}

std::string get(int port, const std::string& path, const std::string& host) {
    return ask(port, "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: test\r\n\r\n");
}

bool status(const std::string& response, const char* code) {
    const std::string want = std::string("HTTP/1.1 ") + code;
    return response.compare(0, want.size(), want) == 0;
}

void testServes() {
    HelpServer        server;
    const std::string url = server.serve({{"help.html", "<p>hi</p>"}, {"logo.png", std::string("\x89PNG\0x", 6)}});
    CHECK(server.running());
    const std::string base = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
    CHECK(url.compare(0, base.size(), base) == 0);
    const std::string path = url.substr(base.size() - 1);   // "/<token>/help.html"
    const std::string dir  = path.substr(0, path.size() - std::string("help.html").size());
    CHECK(dir.size() == 1 + 32 + 1);
    const std::string host = "127.0.0.1:" + std::to_string(server.port());

    const std::string page = get(server.port(), path, host);
    CHECK(status(page, "200"));
    CHECK(page.find("Content-Type: text/html; charset=utf-8\r\n") != std::string::npos);
    CHECK(page.find("X-Content-Type-Options: nosniff\r\n") != std::string::npos);
    CHECK(page.find("Access-Control-Allow-Origin") == std::string::npos);
    CHECK(page.size() > 9 && page.compare(page.size() - 9, 9, "<p>hi</p>") == 0);

    const std::string logo = get(server.port(), dir + "logo.png?x=1", host);
    CHECK(status(logo, "200"));
    CHECK(logo.find("Content-Type: image/png\r\n") != std::string::npos);
    CHECK(logo.find("Content-Length: 6\r\n") != std::string::npos);

    const std::string head = ask(server.port(), "HEAD " + path + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n");
    CHECK(status(head, "200"));
    CHECK(head.find("<p>hi</p>") == std::string::npos);
    CHECK(server.served() == 3);

    // Replacing the files keeps the URL working.
    CHECK(server.serve({{"help.html", "<p>new</p>"}}) == url);
    CHECK(get(server.port(), path, host).find("<p>new</p>") != std::string::npos);
    CHECK(status(get(server.port(), dir + "logo.png", host), "404"));
}

void testRefuses() {
    HelpServer        server;
    const std::string url  = server.serve({{"help.html", "secret-free"}});
    const int         port = server.port();
    const std::string host = "127.0.0.1:" + std::to_string(port);
    const std::string path = url.substr(url.find('/', 7));
    const std::string dir  = path.substr(0, path.rfind('/') + 1);

    CHECK(status(get(port, "/help.html", host), "404"));                        // no token
    CHECK(status(get(port, "/0123456789abcdef0123456789abcdef/help.html", host), "404"));
    CHECK(status(get(port, dir + "../help.html", host), "404"));
    CHECK(status(get(port, dir + "%2e%2e/etc/passwd", host), "404"));
    CHECK(status(get(port, dir + "/etc/passwd", host), "404"));
    CHECK(status(get(port, dir, host), "404"));
    CHECK(status(get(port, path, "evil.example:" + std::to_string(port)), "403"));   // DNS rebinding
    CHECK(status(get(port, path, "localhost:" + std::to_string(port)), "403"));
    CHECK(status(ask(port, "GET " + path + " HTTP/1.1\r\n\r\n"), "403"));           // no Host
    CHECK(status(ask(port, "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n"), "405"));
    CHECK(status(ask(port, "nonsense\r\n\r\n"), "400"));
    CHECK(status(ask(port, "GET " + path + " HTTP/1.1\r\nX: " + std::string(20000, 'a') + "\r\n\r\n"), "400"));
    CHECK(server.served() == 0);

    // Still answering after all that.
    CHECK(status(get(port, path, host), "200"));
}

// Bound to loopback only: on Linux, the kernel's socket table lists the
// listening port at 127.0.0.1 (0100007F), not 0.0.0.0.
void testLoopbackOnly() {
#ifdef __linux__
    HelpServer server;
    server.serve({{"help.html", "x"}});
    char want[16];
    std::snprintf(want, sizeof want, ":%04X ", server.port());
    std::ifstream tcp("/proc/net/tcp");
    std::string   line, local;
    while (std::getline(tcp, line)) {
        const std::size_t at = line.find(want);
        if (at != std::string::npos && at > 8 && line.find(" 0A ") != std::string::npos)   // 0A: LISTEN
            local = line.substr(at - 8, 8);
    }
    CHECK(local == "0100007F");
#endif
}

// A client that connects and says nothing holds the server for the timeout,
// no longer; stop() returns promptly and frees the port.
void testSlowClientAndStop() {
    HelpServer        server;
    const std::string url  = server.serve({{"help.html", "x"}});
    const int         port = server.port();
    const int         idle = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in       addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    CHECK(::connect(idle, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    const auto start = std::chrono::steady_clock::now();
    CHECK(status(get(port, url.substr(url.find('/', 7)), "127.0.0.1:" + std::to_string(port)), "200"));
    const auto waited = std::chrono::steady_clock::now() - start;
    CHECK(waited < std::chrono::milliseconds(HelpServer::kIoTimeoutMs + 1500));
    ::close(idle);

    const auto before = std::chrono::steady_clock::now();
    server.stop();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500));
    CHECK(!server.running());
    CHECK(get(port, "/", "127.0.0.1:" + std::to_string(port)).empty());   // nothing listening

    // And serve() starts it again, with a new token.
    const std::string again = server.serve({{"help.html", "x"}});
    CHECK(!again.empty() && again != url);
}

void testReadDirectory() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("uw-help-test-" + std::to_string(::getpid()));
    fs::create_directories(dir / "sub");
    std::ofstream(dir / "help.html") << "page";
    std::ofstream(dir / "logo.png") << "png";
    std::ofstream(dir / "sub" / "deep.txt") << "no";
    const auto files = HelpServer::readDirectory(dir.string(), "help.html");
    CHECK(files.size() == 2);
    CHECK(!files.empty() && files[0].name == "help.html" && files[0].data == "page");
    CHECK(HelpServer::readDirectory(dir.string(), "missing.html").empty());
    fs::remove_all(dir);
}

} // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);   // macOS has no MSG_NOSIGNAL; a refused request may close early
    testServes();
    testRefuses();
    testLoopbackOnly();
    testSlowClientAndStop();
    testReadDirectory();
    return uw::test::finish("help-server");
}
