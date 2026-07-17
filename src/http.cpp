#include "http.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace uchat {

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    return o;
}

// ── Conn ────────────────────────────────────────────────────────────────────

bool Conn::write_all(const char* p, size_t n) {
    while (n) {
        ssize_t w;
        if (ssl) w = SSL_write(ssl, p, static_cast<int>(n));
        else     w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) { if (!ssl && errno == EINTR) continue; return false; }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

ssize_t Conn::read_some(char* p, size_t n) {
    if (ssl) return SSL_read(ssl, p, static_cast<int>(n));
    return ::recv(fd, p, n, 0);
}

void Conn::close() {
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
    if (fd >= 0) { ::close(fd); fd = -1; }
}

namespace {

void respond(Conn& c, const char* status, const char* ctype, const std::string& body) {
    std::ostringstream h;
    h << "HTTP/1.1 " << status << "\r\n"
      << "Content-Type: " << ctype << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << "Connection: close\r\n\r\n";
    c.write_all(h.str());
    c.write_all(body);
}

const char* mime_for(const std::string& path) {
    auto ends = [&](const char* s) {
        const size_t n = std::strlen(s);
        return path.size() >= n && path.compare(path.size() - n, n, s) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js"))   return "application/javascript";
    if (ends(".wasm")) return "application/wasm";
    if (ends(".css"))  return "text/css";
    if (ends(".json")) return "application/json";
    return "application/octet-stream";
}

// Serve a file from webroot. Rejects anything with ".." — the server is about to be
// reachable from the LAN, and a path traversal here reads arbitrary files off the host.
bool serve_file(Conn& c, const std::string& webroot, const std::string& path) {
    if (path.find("..") != std::string::npos) return false;
    const std::string full = webroot + path;
    std::ifstream f(full, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    respond(c, "200 OK", mime_for(path), ss.str());
    return true;
}

} // namespace

Server::~Server() { stop(); }

bool Server::start(int port, std::string webroot, SendFn on_send, std::string* err,
                   const std::string& bind_addr, const std::string& cert,
                   const std::string& key) {
    webroot_ = std::move(webroot);
    on_send_ = std::move(on_send);

    if (!cert.empty()) {
        SSL_library_init();
        SSL_load_error_strings();
        ctx_ = SSL_CTX_new(TLS_server_method());
        if (!ctx_) { *err = "SSL_CTX_new failed"; return false; }
        if (SSL_CTX_use_certificate_file(ctx_, cert.c_str(), SSL_FILETYPE_PEM) <= 0) {
            *err = "cannot load cert " + cert; SSL_CTX_free(ctx_); ctx_ = nullptr; return false;
        }
        if (SSL_CTX_use_PrivateKey_file(ctx_, key.c_str(), SSL_FILETYPE_PEM) <= 0) {
            *err = "cannot load key " + key; SSL_CTX_free(ctx_); ctx_ = nullptr; return false;
        }
        if (!SSL_CTX_check_private_key(ctx_)) {
            *err = "key does not match cert"; SSL_CTX_free(ctx_); ctx_ = nullptr; return false;
        }
    }

    lfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd_ < 0) { *err = std::string("socket: ") + std::strerror(errno); return false; }
    int one = 1;
    ::setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, bind_addr.c_str(), &a.sin_addr) != 1) {
        *err = "bad bind address: " + bind_addr; return false;
    }

    if (::bind(lfd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
        *err = "bind " + bind_addr + ":" + std::to_string(port) + ": " + std::strerror(errno);
        ::close(lfd_); lfd_ = -1; return false;
    }
    if (::listen(lfd_, 32) < 0) {
        *err = std::string("listen: ") + std::strerror(errno);
        ::close(lfd_); lfd_ = -1; return false;
    }
    run_ = true;
    th_ = std::thread([this] { accept_loop(); });
    return true;
}

void Server::accept_loop() {
    while (run_.load()) {
        const int fd = ::accept(lfd_, nullptr, nullptr);
        if (fd < 0) { if (run_.load() && errno == EINTR) continue; break; }

        Conn c;
        c.fd = fd;
        if (ctx_) {
            c.ssl = SSL_new(ctx_);
            SSL_set_fd(c.ssl, fd);
            if (SSL_accept(c.ssl) <= 0) { c.close(); continue; }   // bad handshake: drop it
        }
        std::thread([this, c] { Conn cc = c; handle(cc); }).detach();
    }
}

void Server::handle(Conn c) {
    std::string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = c.read_some(buf, sizeof buf);
        if (n <= 0) { c.close(); return; }
        req.append(buf, static_cast<size_t>(n));
        if (req.size() > (1u << 20)) { c.close(); return; }
    }

    std::istringstream rl(req.substr(0, req.find("\r\n")));
    std::string method, path;
    rl >> method >> path;
    if (path == "/") path = "/index.html";

    if (method == "GET" && path == "/events") {
        const char* h = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/event-stream\r\n"
                        "Cache-Control: no-store\r\n"
                        "Connection: keep-alive\r\n"
                        "X-Accel-Buffering: no\r\n\r\n";
        if (!c.write_all(h, std::strlen(h))) { c.close(); return; }
        int one = 1;
        ::setsockopt(c.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        { std::lock_guard<std::mutex> lk(m_); sse_.push_back(c); }
        return;                       // stays open, owned by broadcast() from here on
    }

    if (method == "POST" && path == "/send") {
        size_t clen = 0;
        const size_t cl = req.find("Content-Length:");
        if (cl != std::string::npos)
            clen = static_cast<size_t>(std::strtoul(req.c_str() + cl + 15, nullptr, 10));
        const size_t bstart = req.find("\r\n\r\n") + 4;
        std::string body = req.substr(bstart);
        while (body.size() < clen) {
            const ssize_t n = c.read_some(buf, sizeof buf);
            if (n <= 0) break;
            body.append(buf, static_cast<size_t>(n));
        }
        body.resize(std::min(body.size(), clen));
        if (on_send_ && !body.empty()) on_send_(body);
        respond(c, "200 OK", "application/json", "{\"ok\":true}");
        c.close();
        return;
    }

    if (method == "GET" && serve_file(c, webroot_, path)) { c.close(); return; }

    respond(c, "404 Not Found", "text/plain", "no\n");
    c.close();
}

void Server::broadcast(const std::string& json) {
    const std::string msg = "data: " + json + "\n\n";
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = sse_.begin(); it != sse_.end();) {
        if (!it->write_all(msg)) { it->close(); it = sse_.erase(it); }   // client vanished
        else ++it;
    }
}

void Server::stop() {
    if (!run_.exchange(false)) return;
    if (lfd_ >= 0) { ::shutdown(lfd_, SHUT_RDWR); ::close(lfd_); lfd_ = -1; }
    if (th_.joinable()) th_.join();
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& c : sse_) c.close();
        sse_.clear();
    }
    if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
}

} // namespace uchat
