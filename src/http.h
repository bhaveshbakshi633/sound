#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

typedef struct ssl_ctx_st SSL_CTX;   // keep <openssl/*> out of every TU
typedef struct ssl_st     SSL;

namespace uchat {

// One client connection: either a plain fd or an fd wrapped in TLS. Everything above this
// struct is written once and works for both, so "does it have TLS" never leaks into the
// request handling or the SSE broadcast path.
struct Conn {
    int  fd  = -1;
    SSL* ssl = nullptr;

    bool    write_all(const char* p, size_t n);
    bool    write_all(const std::string& s) { return write_all(s.data(), s.size()); }
    ssize_t read_some(char* p, size_t n);
    void    close();
};

// Minimal HTTP/1.1 (+ optional TLS) with Server-Sent Events.
//
// Binds 127.0.0.1 by default: this process can be a live microphone decoder, and that has no
// business being reachable from the network. Binding wider is an explicit, opt-in decision.
//
// TLS exists for one reason: browsers only grant getUserMedia in a secure context, and
// "secure" means HTTPS for anything that is not localhost. A phone loading this page over
// plain http:// will be refused the microphone with no useful error.
class Server {
public:
    using SendFn = std::function<void(const std::string&)>;

    ~Server();
    // cert/key empty → plain HTTP. bind_addr "0.0.0.0" → reachable from the LAN.
    bool start(int port, std::string webroot, SendFn on_send, std::string* err,
               const std::string& bind_addr = "127.0.0.1",
               const std::string& cert = "", const std::string& key = "");
    void broadcast(const std::string& json);   // pushed to every open /events stream
    void stop();
    bool tls() const { return ctx_ != nullptr; }

private:
    void accept_loop();
    void handle(Conn c);

    int                      lfd_ = -1;
    std::thread              th_;
    std::atomic<bool>        run_{false};
    std::vector<Conn>        sse_;
    std::mutex               m_;
    std::string              webroot_;
    SendFn                   on_send_;
    SSL_CTX*                 ctx_ = nullptr;
};

std::string json_escape(const std::string& s);

} // namespace uchat
