// Copyright 2026 Adam DePrince. Licensed under Apache-2.0; see ../LICENSE.
// Transparent streaming IMAP relay. No protocol parser or message-size limits.
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <getopt.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include "../imap/src/c-client/njord_codec.h"
#include "image.h"

namespace {
constexpr const char* version = "0.1.0";
template<class T, auto Destroy> struct Deleter { void operator()(T* p) const { if (p) Destroy(p); } };
template<class T, auto Destroy> using Owned = std::unique_ptr<T, Deleter<T, Destroy>>;
using Event = Owned<event, event_free>;
using Bev = Owned<bufferevent, bufferevent_free>;
using Context = Owned<SSL_CTX, SSL_CTX_free>;
using Addresses = Owned<evutil_addrinfo, evutil_freeaddrinfo>;
struct Bytes : NJORD_BYTES {
    Bytes() : NJORD_BYTES{} {}
    ~Bytes() { std::free(data); }
    Bytes(const Bytes&) = delete;
    Bytes& operator=(const Bytes&) = delete;
    std::size_t remaining() const { return size - position; }
};
struct Socket {
    int fd;
    explicit Socket(int value) : fd(value) {}
    ~Socket() { if (fd >= 0) evutil_closesocket(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    void release() { fd = -1; }
};
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Options {
    ImageOptions image;
    std::string upstream, listen = "127.0.0.1", ca, cert, key;
    int port = 1143, upstream_port = -1;
    double connect_timeout = 30, shutdown_timeout = 30;
    bool plaintext = false, verbose = false, raw = false;
};
int port_number(const char* value) {
    std::string input(value);
    require(!input.empty() && input.find_first_not_of("0123456789") == std::string::npos,
            "port must be an integer between 0 and 65535");
    const auto result = std::stoul(input);
    require(result <= 65535, "port must be between 0 and 65535 (TCP's 16-bit port field)");
    return static_cast<int>(result);
}
double timeout_number(const char* value) {
    std::size_t used = 0;
    const std::string input(value);
    double result = std::stod(input, &used);
    // Libevent stores timer offsets in the OS timeval representation.
    require(used == input.size() && std::isfinite(result) && result > 0 &&
            result < std::nextafter(static_cast<double>(std::numeric_limits<time_t>::max()), 0.0) - static_cast<double>(std::time(nullptr)),
            "timeout must be positive, finite, and representable by the OS timer");
    return result;
}
void help() {
    std::puts("Njord 0.1.0: Skadi's compressed IMAP proxy\n"
        "Usage: njord --upstream HOST [options]\n"
        "  --upstream-port PORT     Provider port (TLS: 993; plaintext: 143)\n"
        "  --upstream-plaintext     Forward raw TCP instead of upstream TLS\n"
        "  --upstream-ca FILE       CA file for verified upstream TLS\n"
        "  --raw                   Ordinary TCP relay without the Njord protocol\n"
        "  --image-width PIXELS    Preview's longest side (1024; WebP maximum 16383)\n"
        "  --image-quality NUMBER  WebP preview quality (65, range 0..100)\n"
        "  --image-timeout SEC     HTTPS transfer deadline (60)\n"
        "  --image-ca FILE         HTTPS image CA bundle (system trust by default)\n"
        "  --image-allow-private   Permit images hosted on private networks\n"
        "  --listen ADDRESS         Numeric IPv4/IPv6 address (127.0.0.1)\n"
        "  --port PORT              Listener port (1143); 0 asks the OS\n"
        "  --certfile FILE          Listener TLS certificate chain\n"
        "  --keyfile FILE           Listener TLS private key\n"
        "  --connect-timeout SEC    DNS/connect/handshake deadline (30)\n"
        "  --shutdown-timeout SEC   Graceful drain/TLS shutdown deadline (30)\n"
        "  --verbose               Connection diagnostics, never mail contents\n"
        "  --version               Print version\n"
        "  --help                  Show this help\n"
        "Skadi connections use checksummed zstd level 22 frames in both directions.\n"
        "Established sessions have no idle timeout. The provider uses ordinary IMAP.");
}
Options parse(int argc, char** argv) {
    Options o;
    static const option options[] = {
        {"upstream", required_argument, nullptr, 'u'}, {"upstream-port", required_argument, nullptr, 'p'},
        {"upstream-plaintext", no_argument, nullptr, 'P'}, {"upstream-ca", required_argument, nullptr, 'a'},
        {"raw", no_argument, nullptr, 'r'},
        {"image-width", required_argument, nullptr, 256},
        {"image-quality", required_argument, nullptr, 257},
        {"image-timeout", required_argument, nullptr, 258},
        {"image-ca", required_argument, nullptr, 259},
        {"image-allow-private", no_argument, nullptr, 260},
        {"listen", required_argument, nullptr, 'l'}, {"port", required_argument, nullptr, 'L'},
        {"certfile", required_argument, nullptr, 'c'}, {"keyfile", required_argument, nullptr, 'k'},
        {"connect-timeout", required_argument, nullptr, 't'}, {"shutdown-timeout", required_argument, nullptr, 's'},
        {"verbose", no_argument, nullptr, 'v'}, {"version", no_argument, nullptr, 'V'},
        {"help", no_argument, nullptr, 'h'}, {nullptr, 0, nullptr, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "", options, nullptr)) != -1) {
        switch (c) {
        case 'u': o.upstream = optarg; break;
        case 'p': o.upstream_port = port_number(optarg); break;
        case 'P': o.plaintext = true; break;
        case 'r': o.raw = true; break;
        case 256: o.image.width = port_number(optarg);
            require(o.image.width > 0 && o.image.width <= 16383,"WebP dimensions must be 1..16383"); break;
        case 257: o.image.quality = static_cast<float>(port_number(optarg));
            require(o.image.quality <= 100,"WebP quality must be 0..100"); break;
        case 258: { const auto value = timeout_number(optarg);
            require(value < static_cast<double>(LONG_MAX) && std::floor(value) == value,"image timeout must be whole seconds representable by libcurl");
            o.image.timeout = static_cast<long>(value); break; }
        case 259: o.image.ca = optarg; break;
        case 260: o.image.allow_private = true; break;
        case 'a': o.ca = optarg; break;
        case 'l': o.listen = optarg; break;
        case 'L': o.port = port_number(optarg); break;
        case 'c': o.cert = optarg; break;
        case 'k': o.key = optarg; break;
        case 't': o.connect_timeout = timeout_number(optarg); break;
        case 's': o.shutdown_timeout = timeout_number(optarg); break;
        case 'v': o.verbose = true; break;
        case 'V': std::printf("njord %s\n", version); std::exit(0);
        case 'h': help(); std::exit(0);
        default: throw std::runtime_error("invalid command-line option; see --help");
        }
    }
    require(optind == argc, "unexpected positional argument");
    require(!o.upstream.empty(), "--upstream HOST is required");
    require(o.cert.empty() == o.key.empty(), "--certfile and --keyfile must be supplied together");
    require(!o.plaintext || o.ca.empty(), "--upstream-ca requires upstream TLS");
    if (o.upstream_port == -1) o.upstream_port = o.plaintext ? 143 : 993;
    require(o.upstream_port != 0, "--upstream-port must be greater than zero");
    return o;
}
Context tls_context(const Options& o, bool server) {
    Context ctx(SSL_CTX_new(server ? TLS_server_method() : TLS_client_method()));
    require(bool(ctx), "could not allocate TLS context");
    require(SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION) == 1, "could not set TLS minimum version");
    if (server) {
        require(SSL_CTX_use_certificate_chain_file(ctx.get(), o.cert.c_str()) == 1, "could not load listener certificate");
        require(SSL_CTX_use_PrivateKey_file(ctx.get(), o.key.c_str(), SSL_FILETYPE_PEM) == 1, "could not load listener key");
        require(SSL_CTX_check_private_key(ctx.get()) == 1, "listener certificate/key do not match");
    } else {
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
        require((o.ca.empty() ? SSL_CTX_set_default_verify_paths(ctx.get()) :
                 SSL_CTX_load_verify_locations(ctx.get(), o.ca.c_str(), nullptr)) == 1,
                "could not load upstream CA certificates");
    }
    return ctx;
}
struct Proxy;
struct Session;
using Sessions = std::list<std::unique_ptr<Session>>;
struct Endpoint {
    Session& session;
    bool provider;
    Bev bev;
    Event shutdown_event;
    SSL* ssl = nullptr; // Owned by bev.
    std::size_t write_budget = 0;
    bool eof = false, write_eof = false, shutdown_done = false;
    Endpoint(Session& s, bool p) : session(s), provider(p) {}
    void initialize(Socket& socket, SSL_CTX* ctx);
    void shutdown_tls();
    Endpoint& peer();
    std::size_t input_size() const { return bev ? evbuffer_get_length(bufferevent_get_input(bev.get())) : 0; }
    std::size_t output_size() const { return bev ? evbuffer_get_length(bufferevent_get_output(bev.get())) : 0; }
    void reset() { shutdown_event.reset(); bev.reset(); ssl = nullptr; }
};
struct Proxy {
    Options options;
    Owned<event_base, event_base_free> base;
    evdns_base* dns = nullptr;
    Context upstream_tls, listener_tls;
    Owned<evconnlistener, evconnlistener_free> listener;
    Event interrupt, terminate, reaper;
    Sessions active, retired;
    bool stopping = false, failed = false;
    explicit Proxy(Options o);
    ~Proxy();
    void accept(int fd);
    void stop();
    void retire(Session& s);
    void run();
};
struct Session {
    Proxy& proxy;
    Endpoint client, provider;
    Event timer;
    Addresses addresses;
    evutil_addrinfo* next_address = nullptr;
    evdns_getaddrinfo_request* lookup = nullptr;
    Sessions::iterator position;
    Owned<ZSTD_CCtx, ZSTD_freeCCtx> encoder;
    Owned<ZSTD_DCtx, ZSTD_freeDCtx> decoder;
    Bytes encoded_input, plain_input, encoded_output;
    bool protocol_ready = false, image_session = false;
    pid_t image_worker = -1;
    bool ready = false, closing = false, shutting_down = false, dead = false;
    Session(Proxy& p, Socket& fd);
    void start();
    void start_protocol();
    void accept_protocol();
    void start_image();
    void resolve();
    void connect_next();
    void connected(Endpoint& endpoint);
    void pump(Endpoint& from);
    void pump_compressed(Endpoint& from);
    bool has_input(const Endpoint& from) const;
    void progress();
    void received_eof(Endpoint& endpoint);
    void arm_timer(double seconds);
    void finish(const char* error = nullptr);
    template<class F> void guarded(F&& action) noexcept {
        if (dead) return;
        try { action(); }
        catch (const std::exception& e) { finish(e.what()); }
        catch (...) { finish("unexpected failure"); }
    }
};
Endpoint& Endpoint::peer() { return provider ? session.client : session.provider; }
std::size_t socket_budget(int fd, int option) {
    int value = 0;
    socklen_t length = sizeof(value);
    require(getsockopt(fd, SOL_SOCKET, option, &value, &length) == 0 && value > 0,
            "could not read OS socket buffer size");
    return static_cast<std::size_t>(value);
}
void Endpoint::initialize(Socket& socket, SSL_CTX* ctx) {
    reset();
    require(evutil_make_socket_nonblocking(socket.fd) == 0, "could not make socket nonblocking");
    require(evutil_make_socket_closeonexec(socket.fd) == 0, "could not mark socket close-on-exec");
    Owned<SSL, SSL_free> new_ssl;
    if (ctx) {
        new_ssl.reset(SSL_new(ctx));
        require(bool(new_ssl), "could not allocate TLS session");
        if (provider) {
            const auto& host = session.proxy.options.upstream;
            // These structures have sizes fixed by the IPv4/IPv6 wire formats.
            in_addr v4{};
            in6_addr v6{};
            if (inet_pton(AF_INET, host.c_str(), &v4) == 1 || inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
                require(X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(new_ssl.get()), host.c_str()) == 1,
                        "could not configure upstream IP verification");
            } else {
                require(SSL_set_tlsext_host_name(new_ssl.get(), host.c_str()) == 1 &&
                        X509_VERIFY_PARAM_set1_host(SSL_get0_param(new_ssl.get()), host.c_str(), 0) == 1,
                        "could not configure upstream hostname verification");
            }
        }
        bev.reset(bufferevent_openssl_socket_new(session.proxy.base.get(), socket.fd, new_ssl.get(),
                  provider ? BUFFEREVENT_SSL_CONNECTING : BUFFEREVENT_SSL_ACCEPTING,
                  BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS));
    } else {
        bev.reset(bufferevent_socket_new(session.proxy.base.get(), socket.fd,
                  BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS));
    }
    require(bool(bev), "could not allocate connection buffers");
    socket.release();
    ssl = new_ssl.release();
    const int fd = bufferevent_getfd(bev.get());
    // Watermarks are streaming backpressure, not message or queue-count limits.
    // Derive them from the actual OS socket windows instead of a fixed buffer.
    write_budget = socket_budget(fd, SO_SNDBUF);
    bufferevent_setwatermark(bev.get(), EV_READ, 0, socket_budget(fd, SO_RCVBUF));
    bufferevent_setcb(bev.get(),
        [](bufferevent*, void* arg) {
            auto& e = *static_cast<Endpoint*>(arg);
            e.session.guarded([&] { e.session.pump(e); e.session.progress(); });
        },
        [](bufferevent*, void* arg) {
            auto& e = *static_cast<Endpoint*>(arg);
            e.session.guarded([&] { e.session.pump(e.peer()); e.session.progress(); });
        },
        [](bufferevent* bev, short events, void* arg) {
            auto& e = *static_cast<Endpoint*>(arg);
            e.session.guarded([&] {
                if (events & BEV_EVENT_CONNECTED) {
                    e.session.connected(e);
                } else if (events & (BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
                    if (e.ssl) {
                        unsigned long error;
                        while ((error = bufferevent_get_openssl_error(bev)) != 0)
                            std::fprintf(stderr, "njord: TLS error: %s\n", ERR_reason_error_string(error));
                    }
                    if (e.provider && !e.session.ready) e.session.connect_next();
                    else e.session.finish("connection failed");
                } else if (events & BEV_EVENT_EOF) {
                    e.session.received_eof(e);
                }
            });
        }, this);
    require(bufferevent_enable(bev.get(), EV_WRITE) == 0, "could not enable connection writes");
}
Session::Session(Proxy& p, Socket& fd) : proxy(p), client(*this, false), provider(*this, true) {
    timer.reset(evtimer_new(p.base.get(), [](evutil_socket_t, short, void* arg) {
        auto& s = *static_cast<Session*>(arg);
        s.finish(s.closing ? "session shutdown timed out" : "connection setup timed out");
    }, this));
    require(bool(timer), "could not allocate connection timer");
    client.initialize(fd, p.listener_tls.get());
}
void Session::arm_timer(double seconds) {
    timeval value{};
    value.tv_sec = static_cast<time_t>(seconds);
    value.tv_usec = static_cast<suseconds_t>((seconds - static_cast<double>(value.tv_sec)) * 1000000);
    require(evtimer_add(timer.get(), &value) == 0, "could not arm connection timer");
}
void Session::start() {
    arm_timer(proxy.options.connect_timeout);
    if (client.ssl) require(bufferevent_enable(client.bev.get(), EV_READ) == 0, "could not start listener TLS");
    else start_protocol();
}
void Session::start_protocol() {
    if (proxy.options.raw) { protocol_ready = true; resolve(); return; }
    require(bufferevent_enable(client.bev.get(), EV_READ) == 0, "could not read Njord preamble");
    accept_protocol();
}
void Session::accept_protocol() {
    if (protocol_ready || client.input_size() < NJORD_PREAMBLE_SIZE) return;
    auto* input = bufferevent_get_input(client.bev.get());
    const auto* preamble = evbuffer_pullup(input, NJORD_PREAMBLE_SIZE);
    const bool image = preamble && !std::memcmp(preamble, NJORD_IMAGE_PREAMBLE, NJORD_PREAMBLE_SIZE);
    require(preamble && (image || !std::memcmp(preamble, NJORD_PREAMBLE, NJORD_PREAMBLE_SIZE)),
            "expected Njord v1 preamble (use --raw for ordinary IMAP clients)");
    require(evbuffer_drain(input, NJORD_PREAMBLE_SIZE) == 0, "could not consume Njord preamble");
    encoder.reset(ZSTD_createCCtx());
    decoder.reset(ZSTD_createDCtx());
    require(bool(encoder) && bool(decoder), "could not allocate zstd contexts");
    require(bufferevent_write(client.bev.get(), image ? NJORD_IMAGE_PREAMBLE : NJORD_PREAMBLE, NJORD_PREAMBLE_SIZE) == 0,
            "could not send Njord preamble");
    protocol_ready = true;
    bufferevent_disable(client.bev.get(), EV_READ);
    if (image) start_image(); else resolve();
}
void Session::start_image() {
    image_session = true;
    int descriptors[2];
    require(socketpair(AF_UNIX,SOCK_STREAM,0,descriptors) == 0,"could not create image worker connection");
    Socket parent(descriptors[0]), child(descriptors[1]);
    image_worker = fork();
    require(image_worker >= 0,"could not start image worker");
    if (!image_worker) njord_image_worker(child.fd,proxy.options.image);
    provider.initialize(parent,nullptr);
    connected(provider);
}
void Session::resolve() {
    arm_timer(proxy.options.connect_timeout);
    evutil_addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    const auto service = std::to_string(proxy.options.upstream_port);
    lookup = evdns_getaddrinfo(proxy.dns, proxy.options.upstream.c_str(), service.c_str(), &hints,
        [](int error, evutil_addrinfo* result, void* arg) {
            auto& s = *static_cast<Session*>(arg);
            s.lookup = nullptr;
            Addresses owned(result);
            if (s.dead) { s.proxy.retire(s); return; }
            s.guarded([&] {
                if (error || !result) { s.finish(error ? evutil_gai_strerror(error) : "no upstream addresses"); return; }
                s.addresses = std::move(owned);
                s.next_address = s.addresses.get();
                s.connect_next();
            });
        }, this);
}
void Session::connect_next() {
    provider.reset();
    while (next_address) {
        const auto* addr = next_address;
        next_address = next_address->ai_next;
        Socket fd(socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol));
        if (fd.fd < 0) continue;
        provider.initialize(fd, proxy.upstream_tls.get());
        require(bufferevent_enable(provider.bev.get(), EV_READ) == 0, "could not start upstream connection");
        if (bufferevent_socket_connect(provider.bev.get(), addr->ai_addr, static_cast<int>(addr->ai_addrlen)) == 0) return;
        provider.reset();
    }
    finish("upstream connection failed");
}
void Session::connected(Endpoint& endpoint) {
    if (!endpoint.provider) {
        start_protocol();
        return;
    }
    if (provider.ssl && SSL_get_verify_result(provider.ssl) != X509_V_OK) {
        finish("upstream certificate verification failed");
        return;
    }
    ready = true;
    evtimer_del(timer.get());
    addresses.reset();
    next_address = nullptr;
    pump(client);
    pump(provider);
    if (proxy.options.verbose) std::fputs("njord: session connected\n", stderr);
}
void Session::pump(Endpoint& from) {
    if (!from.provider && !protocol_ready) { accept_protocol(); return; }
    if (!ready || dead || shutting_down) return;
    if (!proxy.options.raw) { pump_compressed(from); return; }
    auto& to = from.peer();
    if (!from.bev || !to.bev) return;
    const auto queued = to.output_size();
    const auto room = queued < to.write_budget ? to.write_budget - queued : 0;
    // evbuffer_remove_buffer returns int: keep a single call representable.
    const auto amount = std::min({room, from.input_size(), static_cast<std::size_t>(INT_MAX)});
    if (amount) {
        int moved = evbuffer_remove_buffer(bufferevent_get_input(from.bev.get()),
                                          bufferevent_get_output(to.bev.get()), amount);
        require(moved >= 0 && static_cast<std::size_t>(moved) == amount, "could not queue forwarded bytes");
    }
    if (from.eof || closing || from.input_size() || to.output_size() >= to.write_budget)
        bufferevent_disable(from.bev.get(), EV_READ);
    else
        require(bufferevent_enable(from.bev.get(), EV_READ) == 0, "could not resume connection reads");
}
bool Session::has_input(const Endpoint& from) const {
    return from.input_size() || (!from.provider &&
           (encoded_input.size || plain_input.remaining()));
}
void Session::pump_compressed(Endpoint& from) {
    auto& to = from.peer();
    auto* input = bufferevent_get_input(from.bev.get());
    auto* output = bufferevent_get_output(to.bev.get());
    while (to.output_size() < to.write_budget) {
        if (from.provider) {
            auto amount = std::min(from.input_size(), ZSTD_CStreamInSize());
            if (!amount) break;
            const auto* data = evbuffer_pullup(input, static_cast<ev_ssize_t>(amount));
            require(data != nullptr, "could not access provider input");
            const auto* error = njord_encode(encoder.get(), data, amount, &encoded_output);
            if (error) throw std::runtime_error(error);
            require(evbuffer_add(output, encoded_output.data, encoded_output.size) == 0 &&
                    evbuffer_drain(input, amount) == 0, "could not queue compressed bytes");
        } else {
            if (plain_input.remaining()) {
                const auto amount = std::min(plain_input.remaining(), to.write_budget - to.output_size());
                require(evbuffer_add(output, plain_input.data + plain_input.position, amount) == 0,
                        "could not queue decompressed bytes");
                plain_input.position += amount;
                continue;
            }
            if (!encoded_input.size) {
                if (from.input_size() < NJORD_FRAME_HEADER) {
                    require(!from.eof || !from.input_size(), "truncated Njord frame header");
                    break;
                }
                const auto* header = evbuffer_pullup(input, NJORD_FRAME_HEADER);
                require(header != nullptr, "could not access Njord frame header");
                const auto size = njord_frame_size(header);
                require(size != 0, "empty Njord frame");
                require(njord_reserve(&encoded_input, size) != 0, "could not allocate Njord input frame");
                encoded_input.size = size;
                encoded_input.position = 0;
                require(evbuffer_drain(input, NJORD_FRAME_HEADER) == 0, "could not consume frame header");
            }
            // evbuffer_remove returns int, so each copy must fit that API.
            const auto amount = std::min({from.input_size(), encoded_input.remaining(), static_cast<std::size_t>(INT_MAX)});
            if (amount) {
                require(evbuffer_remove(input, encoded_input.data + encoded_input.position, amount) == static_cast<int>(amount),
                        "could not read compressed frame");
                encoded_input.position += amount;
            }
            if (encoded_input.remaining()) {
                require(!from.eof || from.input_size(), "truncated Njord frame");
                if (!from.input_size()) break;
                continue;
            }
            const auto* error = njord_decode(decoder.get(), encoded_input.data, encoded_input.size, &plain_input);
            if (error) throw std::runtime_error(error);
            encoded_input.size = encoded_input.position = 0;
        }
    }
    if (from.eof || closing || to.output_size() >= to.write_budget || (!from.provider && plain_input.remaining()))
        bufferevent_disable(from.bev.get(), EV_READ);
    else
        require(bufferevent_enable(from.bev.get(), EV_READ) == 0, "could not resume compressed reads");
}
void Session::received_eof(Endpoint& endpoint) {
    if (!ready) { finish("connection ended during setup"); return; }
    endpoint.eof = true;
    bufferevent_disable(endpoint.bev.get(), EV_READ);
    if ((client.ssl || provider.ssl || image_session) && !closing) {
        closing = true;
        arm_timer(proxy.options.shutdown_timeout);
        bufferevent_disable(client.bev.get(), EV_READ);
        bufferevent_disable(provider.bev.get(), EV_READ);
    }
    pump(client);
    pump(provider);
    progress();
}
void Session::progress() {
    if (dead || !ready || shutting_down) return;
    if (closing) {
        if (has_input(client) || has_input(provider) || client.output_size() || provider.output_size()) return;
        shutting_down = true;
        client.shutdown_tls();
        if (!dead) provider.shutdown_tls();
        return;
    }
    for (auto* from : {&client, &provider}) {
        auto& to = from->peer();
        if (from->eof && !has_input(*from) && !to.output_size() && !to.write_eof) {
            require(shutdown(bufferevent_getfd(to.bev.get()), SHUT_WR) == 0 || errno == ENOTCONN,
                    "could not forward TCP half-close");
            to.write_eof = true;
        }
    }
    if (client.eof && provider.eof && !has_input(client) && !has_input(provider) &&
        !client.output_size() && !provider.output_size()) finish();
}
void Endpoint::shutdown_tls() {
    if (session.dead || shutdown_done) return;
    bufferevent_disable(bev.get(), EV_READ | EV_WRITE);
    shutdown_event.reset();
    if (!ssl) {
        shutdown_done = true;
        bev.reset();
    } else {
        // Own readiness explicitly while closing: libevent's SSL bufferevent
        // destructor does not send close_notify. Never fake RECEIVED_SHUTDOWN.
        ERR_clear_error();
        const int result = SSL_shutdown(ssl);
        if (result == 1) {
            shutdown_done = true;
        } else {
            const int error = result == 0 ? SSL_ERROR_WANT_READ : SSL_get_error(ssl, result);
            short interest;
            if (error == SSL_ERROR_WANT_READ) interest = EV_READ;
            else if (error == SSL_ERROR_WANT_WRITE) interest = EV_WRITE;
            else { session.finish("TLS shutdown failed"); return; }
            shutdown_event.reset(event_new(session.proxy.base.get(), bufferevent_getfd(bev.get()), interest,
                [](evutil_socket_t, short, void* arg) {
                    auto& e = *static_cast<Endpoint*>(arg);
                    e.session.guarded([&] { e.shutdown_tls(); });
                }, this));
            require(bool(shutdown_event) && event_add(shutdown_event.get(), nullptr) == 0,
                    "could not watch TLS shutdown");
        }
    }
    if (shutdown_done && peer().shutdown_done) session.finish();
}
void Session::finish(const char* error) {
    if (dead) return;
    dead = true;
    if (error) std::fprintf(stderr, "njord: %s\n", error);
    evtimer_del(timer.get());
    client.reset();
    provider.reset();
    if (image_worker > 0) {
        // A dropped client must not leave a downloader or decoder behind.
        kill(image_worker,SIGKILL);
        while (waitpid(image_worker,nullptr,0) < 0 && errno == EINTR) {}
        image_worker = -1;
    }
    if (lookup) {
        // Cancellation callbacks are asynchronous. Keep this session alive
        // until the resolver acknowledges cancellation, then retire it.
        evdns_getaddrinfo_cancel(lookup);
    } else proxy.retire(*this);
}
Proxy::Proxy(Options o) : options(std::move(o)), base(event_base_new()) {
    require(bool(base), "could not allocate event loop");
    if (!options.plaintext) upstream_tls = tls_context(options, false);
    if (!options.cert.empty()) listener_tls = tls_context(options, true);
    reaper.reset(event_new(base.get(), -1, 0, [](evutil_socket_t, short, void* arg) {
        auto& p = *static_cast<Proxy*>(arg);
        if (p.options.verbose)
            for (const auto& ignored : p.retired) { (void)ignored; std::fputs("njord: session closed\n", stderr); }
        p.retired.clear();
        if (p.stopping && p.active.empty()) event_base_loopbreak(p.base.get());
    }, this));
    require(bool(reaper), "could not allocate session cleanup event");
    auto signal_handler = [](evutil_socket_t, short, void* arg) { static_cast<Proxy*>(arg)->stop(); };
    interrupt.reset(evsignal_new(base.get(), SIGINT, signal_handler, this));
    terminate.reset(evsignal_new(base.get(), SIGTERM, signal_handler, this));
    require(bool(interrupt) && bool(terminate) && event_add(interrupt.get(), nullptr) == 0 &&
            event_add(terminate.get(), nullptr) == 0, "could not install signal handlers");
    evutil_addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = EVUTIL_AI_PASSIVE | EVUTIL_AI_NUMERICHOST;
    evutil_addrinfo* result = nullptr;
    const auto service = std::to_string(options.port);
    require(evutil_getaddrinfo(options.listen.c_str(), service.c_str(), &hints, &result) == 0,
            "--listen must be a valid numeric IPv4 or IPv6 address");
    Addresses addresses(result);
    for (auto* addr = result; addr; addr = addr->ai_next) {
        listener.reset(evconnlistener_new_bind(base.get(),
            [](evconnlistener*, evutil_socket_t fd, sockaddr*, int, void* arg) { static_cast<Proxy*>(arg)->accept(fd); },
            this, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC | LEV_OPT_REUSEABLE,
            SOMAXCONN, addr->ai_addr, static_cast<int>(addr->ai_addrlen)));
        if (listener) break;
    }
    require(bool(listener), "could not bind listener");
    evconnlistener_set_error_cb(listener.get(), [](evconnlistener*, void* arg) {
        std::fprintf(stderr, "njord: listener failed: %s\n", evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
        static_cast<Proxy*>(arg)->failed = true;
        static_cast<Proxy*>(arg)->stop();
    });
    sockaddr_storage address{}; // OS-defined socket address representation.
    socklen_t length = sizeof(address);
    require(getsockname(evconnlistener_get_fd(listener.get()), reinterpret_cast<sockaddr*>(&address), &length) == 0,
            "could not read listener port");
    const auto port = address.ss_family == AF_INET ? ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port) :
                     ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    dns = evdns_base_new(base.get(), EVDNS_BASE_INITIALIZE_NAMESERVERS);
    require(dns != nullptr, "could not initialize asynchronous DNS resolver");
    std::fprintf(stderr, "njord: listening on [%s]:%u; upstream %s:%d (%s)\n", options.listen.c_str(), port,
                 options.upstream.c_str(), options.upstream_port, options.plaintext ? "plaintext" : "TLS verified");
}
Proxy::~Proxy() {
    stop();
    // Resolver cancellations queue callbacks; drain those before freeing sessions.
    while (!active.empty()) {
        if (event_base_loop(base.get(), EVLOOP_NONBLOCK) < 0) break;
    }
    retired.clear();
    if (dns) evdns_base_free(dns, 0);
}
void Proxy::accept(int fd) {
    Socket socket(fd);
    if (stopping) return;
    try {
        auto s = std::make_unique<Session>(*this, socket);
        active.push_back(std::move(s));
        auto position = std::prev(active.end());
        (*position)->position = position;
        (*position)->guarded([&] { (*position)->start(); });
    } catch (const std::exception& e) {
        std::fprintf(stderr, "njord: could not accept connection: %s\n", e.what());
    }
}
void Proxy::retire(Session& s) {
    retired.splice(retired.end(), active, s.position);
    event_active(reaper.get(), 0, 0);
}
void Proxy::stop() {
    if (stopping) return;
    stopping = true;
    if (listener) evconnlistener_disable(listener.get());
    for (auto it = active.begin(); it != active.end();) {
        auto& s = **it++;
        s.finish();
    }
    if (active.empty()) event_base_loopbreak(base.get());
}
void Proxy::run() { require(event_base_dispatch(base.get()) >= 0 && !failed, "event loop failed"); }
} // namespace
int main(int argc, char** argv) {
    try {
        auto options = parse(argc, argv);
        std::signal(SIGPIPE, SIG_IGN);
        Proxy proxy(std::move(options));
        proxy.run();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "njord: %s\n", error.what());
        unsigned long tls_error;
        while ((tls_error = ERR_get_error()) != 0)
            std::fprintf(stderr, "njord: TLS error: %s\n", ERR_reason_error_string(tls_error));
        return 1;
    }
}
