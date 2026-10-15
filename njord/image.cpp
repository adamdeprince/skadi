// Copyright 2026 Adam DePrince. Apache-2.0.
#include "image.h"
#include <curl/curl.h>
#include <webp/decode.h>
#include <webp/encode.h>
#include <arpa/inet.h>
#include <sys/resource.h>
#include <unistd.h>
#include <algorithm>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
// stb's decoder uses signed int offsets. This is its representation limit.
#define STBI_MAX_DIMENSIONS INT_MAX
#include "../pith/stb_image.h"
#define STBIR_NO_SIMD
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../pith/stb_image_resize2.h"

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool public_v4(uint32_t address) {
    const auto a = ntohl(address);
    return (a >> 24) != 0 && (a >> 24) != 10 && (a >> 24) != 127 &&
        (a >> 22) != 0x191 && (a >> 16) != 0xa9fe && (a >> 20) != 0xac1 &&
        (a >> 16) != 0xc0a8 && (a >> 24) < 224;
}
curl_socket_t open_public(void* context, curlsocktype, curl_sockaddr* address) {
    const auto& options = *static_cast<const ImageOptions*>(context);
    bool allowed = options.allow_private;
    if (!allowed && address->family == AF_INET)
        allowed = public_v4(reinterpret_cast<sockaddr_in*>(&address->addr)->sin_addr.s_addr);
    if (!allowed && address->family == AF_INET6) {
        const auto& a = reinterpret_cast<sockaddr_in6*>(&address->addr)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(&a)) {
            uint32_t v4; std::memcpy(&v4, a.s6_addr + 12, sizeof(v4)); allowed = public_v4(v4);
        } else allowed = (a.s6_addr[0] & 0xe0) == 0x20; // Global unicast 2000::/3.
    }
    return allowed ? socket(address->family, address->socktype, address->protocol) : CURL_SOCKET_BAD;
}
size_t append(char* data, size_t size, size_t count, void* context) noexcept {
    auto& bytes = *static_cast<std::vector<unsigned char>*>(context);
    if (size && count > std::numeric_limits<size_t>::max() / size) return 0;
    const auto n = size * count;
    try { bytes.insert(bytes.end(), data, data + n); return n; }
    catch (...) { return 0; }
}
void valid_url(const std::string& url) {
    check(url.compare(0,8,"https://") == 0, "Images require HTTPS");
    const auto end = url.find_first_of("/?#",8);
    const auto host = url.substr(8,end == std::string::npos ? end : end - 8);
    check(!host.empty() && host.find('@') == std::string::npos, "Invalid image address");
    for (unsigned char c : url) check(c > 32 && c != 127, "Invalid image address");
}
std::vector<unsigned char> download(std::string url, const ImageOptions& options) {
    check(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "HTTPS initialization failed");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);
    check(bool(curl), "HTTPS allocation failed");
    std::vector<unsigned char> bytes;
    std::set<std::string> visited;
    const auto set = [&](CURLoption key, auto value) {
        check(curl_easy_setopt(curl.get(),key,value) == CURLE_OK, "HTTPS option failed");
    };
    set(CURLOPT_WRITEFUNCTION,append); set(CURLOPT_WRITEDATA,&bytes);
    set(CURLOPT_OPENSOCKETFUNCTION,open_public); set(CURLOPT_OPENSOCKETDATA,&options);
    set(CURLOPT_PROXY,""); // Never inherit ambient proxies or mail credentials.
    set(CURLOPT_NETRC,CURL_NETRC_IGNORED); set(CURLOPT_NOSIGNAL,1L);
    set(CURLOPT_CONNECTTIMEOUT,options.timeout); set(CURLOPT_TIMEOUT,options.timeout);
    set(CURLOPT_SSL_VERIFYPEER,1L); set(CURLOPT_SSL_VERIFYHOST,2L);
    if (!options.ca.empty()) set(CURLOPT_CAINFO,options.ca.c_str());
    // Resolve redirects ourselves: validate every URL and detect cycles, without
    // imposing a fixed redirect count. CURLOPT_REDIRECT_URL resolves relative URLs.
    for (;;) {
        valid_url(url);
        check(visited.insert(url).second, "Image redirect loop");
        bytes.clear(); set(CURLOPT_URL,url.c_str());
        check(curl_easy_perform(curl.get()) == CURLE_OK, "HTTPS image download failed");
        long status = 0; char* redirect = nullptr;
        check(curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status) == CURLE_OK, "Missing HTTPS status");
        if (status >= 300 && status < 400) {
            curl_easy_getinfo(curl.get(),CURLINFO_REDIRECT_URL,&redirect);
            check(redirect != nullptr, "Image redirect has no address"); url = redirect; continue;
        }
        check(status >= 200 && status < 300 && !bytes.empty(), "Image server returned an error or empty image");
        return bytes;
    }
}
std::string mime(const std::vector<unsigned char>& b) {
    if (b.size() >= 8 && !std::memcmp(b.data(),"\x89PNG\r\n\x1a\n",8)) return "image/png";
    if (b.size() >= 3 && b[0] == 255 && b[1] == 216 && b[2] == 255) return "image/jpeg";
    if (b.size() >= 6 && (!std::memcmp(b.data(),"GIF87a",6) || !std::memcmp(b.data(),"GIF89a",6))) return "image/gif";
    if (b.size() >= 12 && !std::memcmp(b.data(),"RIFF",4) && !std::memcmp(b.data()+8,"WEBP",4)) return "image/webp";
    throw std::runtime_error("Supported images are JPEG, PNG, GIF and WebP");
}
std::vector<unsigned char> preview(const std::vector<unsigned char>& bytes, const ImageOptions& options) {
    int width = 0, height = 0, channels = 0;
    std::unique_ptr<unsigned char, decltype(&std::free)> pixels(nullptr,std::free);
    if (WebPGetInfo(bytes.data(),bytes.size(),&width,&height)) {
        check(width > 0 && width <= INT_MAX / 4 && height > 0 &&
              size_t(height) <= SIZE_MAX / (size_t(width)*4), "Image dimensions overflow");
        const auto size = size_t(width)*size_t(height)*4;
        pixels.reset(static_cast<unsigned char*>(std::malloc(size)));
        check(bool(pixels) && WebPDecodeRGBAInto(bytes.data(),bytes.size(),pixels.get(),size,width*4), "WebP decoding failed");
    } else {
        check(bytes.size() <= INT_MAX, "Image exceeds decoder's signed-int input representation; request original");
        pixels.reset(stbi_load_from_memory(bytes.data(),static_cast<int>(bytes.size()),&width,&height,&channels,4));
        check(bool(pixels), "Image decoding failed; request original");
    }
    const double scale = std::min(1.0, double(options.width)/std::max(width,height));
    const int w = std::max(1,int(width*scale)), h = std::max(1,int(height*scale));
    // WebP has a 14-bit dimension field; CLI validates its requested preview size.
    std::vector<unsigned char> resized(size_t(w)*size_t(h)*4);
    check(stbir_resize_uint8_srgb(pixels.get(),width,height,0,resized.data(),w,h,0,STBIR_RGBA) != nullptr,
          "Image resizing failed");
    unsigned char* encoded = nullptr;
    const auto size = WebPEncodeRGBA(resized.data(),w,h,w*4,options.quality,&encoded);
    std::unique_ptr<unsigned char, decltype(&WebPFree)> owned(encoded,WebPFree);
    check(size && encoded, "WebP encoding failed");
    return std::vector<unsigned char>(encoded,encoded+size);
}
}

void njord_image_worker(int fd, const ImageOptions& options) noexcept {
    // No inherited listener, DNS, IMAP or TLS descriptors survive in the worker.
    if (dup2(fd,STDIN_FILENO) < 0 || dup2(fd,STDOUT_FILENO) < 0) _exit(1);
    struct rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE,&limit)) _exit(1);
    for (rlim_t i = 3; i < limit.rlim_cur && i <= INT_MAX; ++i) close(static_cast<int>(i));
    std::signal(SIGINT,SIG_DFL); std::signal(SIGTERM,SIG_DFL); std::signal(SIGPIPE,SIG_IGN);
    try {
        char* line = nullptr; size_t capacity = 0;
        const auto n = getline(&line,&capacity,stdin);
        std::unique_ptr<char, decltype(&std::free)> owned(line,std::free);
        check(n > 2 && line[n-2] == '\r' && line[n-1] == '\n' &&
              !std::memchr(line,0,static_cast<size_t>(n)), "Invalid image request");
        std::string command(line,static_cast<size_t>(n)-2);
        const bool original = command.compare(0,9,"ORIGINAL ") == 0;
        check(original || command.compare(0,8,"PREVIEW ") == 0, "Unknown image request");
        auto bytes = download(command.substr(original ? 9 : 8),options);
        auto type = mime(bytes);
        const auto original_size = bytes.size();
        if (!original) {
            auto smaller = preview(bytes,options);
            if (smaller.size() < bytes.size()) { bytes.swap(smaller); type = "image/webp"; }
        }
        if (std::printf("OK %zu %zu %s\r\n",original_size,bytes.size(),type.c_str()) < 0 ||
            std::fwrite(bytes.data(),1,bytes.size(),stdout) != bytes.size() || std::fflush(stdout)) _exit(1);
    } catch (const std::exception& e) {
        std::printf("ERR %s\r\n",e.what()); std::fflush(stdout); _exit(1);
    } catch (...) { std::fputs("ERR Image processing failed\r\n",stdout); std::fflush(stdout); _exit(1); }
    _exit(0);
}
