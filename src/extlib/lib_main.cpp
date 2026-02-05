#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "lib_recomp.hpp"

extern "C" {
    DLLEXPORT uint32_t recomp_api_version = 1;
}

#if defined(_WIN32)
using SocketHandle = SOCKET;
static const SocketHandle kInvalidSocket = INVALID_SOCKET;
#define CLOSE_SOCKET closesocket
#else
using SocketHandle = int;
static const SocketHandle kInvalidSocket = -1;
#define CLOSE_SOCKET close
#endif

static std::atomic<bool> sRunning{ false };
static std::thread sServerThread;
static std::mutex sStateMutex;
static std::string sSnapshotJson;
static std::string sApiKey;
static std::string sBindAddress;
static int sPort = 0;
static SocketHandle sListenSocket = kInvalidSocket;
static std::mutex sQueueMutex;
static std::deque<std::string> sMessageQueue;
static std::chrono::steady_clock::time_point sStartTime;

static const size_t kMaxRequestSize = 8192;
static const size_t kMaxQueueSize = 16;
static const size_t kMaxMessageSize = 512;

static std::string ToLower(const std::string& input) {
    std::string out = input;
    for (char& c : out) {
        c = (char)std::tolower((unsigned char)c);
    }
    return out;
}

static std::string Trim(const std::string& input) {
    size_t start = 0;
    size_t end = input.size();

    while (start < end && std::isspace((unsigned char)input[start])) {
        start++;
    }
    while (end > start && std::isspace((unsigned char)input[end - 1])) {
        end--;
    }

    return input.substr(start, end - start);
}

static void SendResponse(SocketHandle socket, int code, const std::string& body, const char* content_type) {
    const char* reason = "OK";
    if (code == 400) reason = "Bad Request";
    if (code == 401) reason = "Unauthorized";
    if (code == 404) reason = "Not Found";
    if (code == 500) reason = "Internal Server Error";

    std::ostringstream response;
    response << "HTTP/1.1 " << code << " " << reason << "\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n\r\n";
    response << body;

    std::string payload = response.str();
    const char* data = payload.c_str();
    size_t remaining = payload.size();

    while (remaining > 0) {
        int sent = (int)send(socket, data, (int)remaining, 0);
        if (sent <= 0) {
            break;
        }
        data += sent;
        remaining -= (size_t)sent;
    }
}

static bool ParseRequestLine(const std::string& line, std::string* method, std::string* path) {
    size_t first_space = line.find(' ');
    if (first_space == std::string::npos) {
        return false;
    }
    size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string::npos) {
        return false;
    }

    *method = line.substr(0, first_space);
    *path = line.substr(first_space + 1, second_space - first_space - 1);
    return true;
}

static bool ExtractJsonString(const std::string& body, const std::string& key, std::string* out) {
    std::string token = "\"" + key + "\"";
    size_t key_pos = body.find(token);
    if (key_pos == std::string::npos) {
        return false;
    }
    size_t colon = body.find(':', key_pos + token.size());
    if (colon == std::string::npos) {
        return false;
    }
    size_t first_quote = body.find('"', colon + 1);
    if (first_quote == std::string::npos) {
        return false;
    }

    std::string result;
    result.reserve(128);

    bool escaping = false;
    for (size_t i = first_quote + 1; i < body.size(); ++i) {
        char c = body[i];
        if (escaping) {
            if (c == 'n') {
                result.push_back('\n');
            } else if (c == 't') {
                result.push_back('\t');
            } else {
                result.push_back(c);
            }
            escaping = false;
            continue;
        }

        if (c == '\\') {
            escaping = true;
            continue;
        }

        if (c == '"') {
            *out = result;
            return true;
        }

        result.push_back(c);
        if (result.size() >= kMaxMessageSize) {
            break;
        }
    }

    if (!result.empty()) {
        *out = result;
        return true;
    }

    return false;
}

static void HandleClient(SocketHandle client) {
    std::string buffer;
    buffer.reserve(1024);

    char temp[1024];
    bool headers_complete = false;
    size_t header_end = std::string::npos;
    size_t content_length = 0;

    while (buffer.size() < kMaxRequestSize) {
        int received = (int)recv(client, temp, sizeof(temp), 0);
        if (received <= 0) {
            break;
        }
        buffer.append(temp, (size_t)received);

        if (!headers_complete) {
            header_end = buffer.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                headers_complete = true;

                std::string header_block = buffer.substr(0, header_end + 2);
                std::istringstream header_stream(header_block);
                std::string line;
                while (std::getline(header_stream, line)) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    size_t sep = line.find(':');
                    if (sep != std::string::npos) {
                        std::string name = ToLower(Trim(line.substr(0, sep)));
                        std::string value = Trim(line.substr(sep + 1));
                        if (name == "content-length") {
                            content_length = (size_t)std::strtoul(value.c_str(), nullptr, 10);
                        }
                    }
                }
            }
        }

        if (headers_complete) {
            size_t body_start = header_end + 4;
            if (buffer.size() >= body_start + content_length) {
                break;
            }
        }
    }

    if (buffer.empty()) {
        CLOSE_SOCKET(client);
        return;
    }

    header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        SendResponse(client, 400, "{\"error\":\"bad request\"}", "application/json");
        CLOSE_SOCKET(client);
        return;
    }

    std::string header_block = buffer.substr(0, header_end);
    std::string body;
    if (header_end + 4 <= buffer.size()) {
        body = buffer.substr(header_end + 4);
    }

    std::istringstream header_stream(header_block);
    std::string request_line;
    std::getline(header_stream, request_line);
    if (!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }

    std::string method;
    std::string path;
    if (!ParseRequestLine(request_line, &method, &path)) {
        SendResponse(client, 400, "{\"error\":\"bad request\"}", "application/json");
        CLOSE_SOCKET(client);
        return;
    }

    std::string api_key_header;
    std::string line;
    while (std::getline(header_stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        size_t sep = line.find(':');
        if (sep != std::string::npos) {
            std::string name = ToLower(Trim(line.substr(0, sep)));
            std::string value = Trim(line.substr(sep + 1));
            if (name == "x-api-key") {
                api_key_header = value;
            }
        }
    }

    if (!sApiKey.empty() && api_key_header != sApiKey) {
        SendResponse(client, 401, "{\"error\":\"unauthorized\"}", "application/json");
        CLOSE_SOCKET(client);
        return;
    }

    if (method == "GET" && path == "/v1/health") {
        auto now = std::chrono::steady_clock::now();
        auto uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - sStartTime).count();
        std::ostringstream json;
        json << "{\"ok\":true,\"uptimeMs\":" << uptime_ms << "}";
        SendResponse(client, 200, json.str(), "application/json");
        CLOSE_SOCKET(client);
        return;
    }

    if (method == "GET" && path == "/v1/state") {
        std::string snapshot;
        {
            std::lock_guard<std::mutex> lock(sStateMutex);
            snapshot = sSnapshotJson.empty() ? "{}" : sSnapshotJson;
        }
        SendResponse(client, 200, snapshot, "application/json");
        CLOSE_SOCKET(client);
        return;
    }

    if (method == "POST" && path == "/v1/message") {
        std::string message;
        if (!ExtractJsonString(body, "text", &message)) {
            SendResponse(client, 400, "{\"error\":\"missing text\"}", "application/json");
            CLOSE_SOCKET(client);
            return;
        }

        if (message.size() > kMaxMessageSize) {
            message.resize(kMaxMessageSize);
        }

        {
            std::lock_guard<std::mutex> lock(sQueueMutex);
            if (sMessageQueue.size() >= kMaxQueueSize) {
                sMessageQueue.pop_front();
            }
            sMessageQueue.push_back(message);
        }

        SendResponse(client, 202, "{\"ok\":true}", "application/json");
        CLOSE_SOCKET(client);
        return;
    }

    SendResponse(client, 404, "{\"error\":\"not found\"}", "application/json");
    CLOSE_SOCKET(client);
}

static void ServerLoop() {
    sStartTime = std::chrono::steady_clock::now();

    while (sRunning.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sListenSocket, &read_fds);

        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;

        int result = select((int)(sListenSocket + 1), &read_fds, nullptr, nullptr, &timeout);
        if (result > 0 && FD_ISSET(sListenSocket, &read_fds)) {
            SocketHandle client = accept(sListenSocket, nullptr, nullptr);
            if (client != kInvalidSocket) {
                HandleClient(client);
            }
        }
    }
}

static bool StartServerInternal(const std::string& bind_address, int port) {
    if (sRunning.load()) {
        return true;
    }

#if defined(_WIN32)
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return false;
    }
#endif

    SocketHandle socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == kInvalidSocket) {
        return false;
    }

    int opt_val = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt_val, sizeof(opt_val));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
        CLOSE_SOCKET(socket_fd);
        return false;
    }

    if (bind(socket_fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        CLOSE_SOCKET(socket_fd);
        return false;
    }

    if (listen(socket_fd, 8) != 0) {
        CLOSE_SOCKET(socket_fd);
        return false;
    }

    sListenSocket = socket_fd;
    sRunning.store(true);
    sServerThread = std::thread(ServerLoop);
    return true;
}

static void StopServerInternal() {
    if (!sRunning.load()) {
        return;
    }

    sRunning.store(false);
    if (sServerThread.joinable()) {
        sServerThread.join();
    }

    if (sListenSocket != kInvalidSocket) {
        CLOSE_SOCKET(sListenSocket);
        sListenSocket = kInvalidSocket;
    }

#if defined(_WIN32)
    WSACleanup();
#endif
}

RECOMP_DLL_FUNC(http_server_start) {
    std::string bind_address = RECOMP_ARG_STR(0);
    int port = (int)RECOMP_ARG(s32, 1);
    std::string api_key = RECOMP_ARG_STR(2);

    if (port <= 0 || port > 65535) {
        RECOMP_RETURN(int, 0);
    }

    {
        std::lock_guard<std::mutex> lock(sStateMutex);
        sBindAddress = bind_address;
        sApiKey = api_key;
        sPort = port;
    }

    bool ok = StartServerInternal(bind_address, port);
    RECOMP_RETURN(int, ok ? 1 : 0);
}

RECOMP_DLL_FUNC(http_server_stop) {
    StopServerInternal();
    return;
}

RECOMP_DLL_FUNC(http_server_set_snapshot) {
    std::string json = RECOMP_ARG_STR(0);
    {
        std::lock_guard<std::mutex> lock(sStateMutex);
        sSnapshotJson = json;
    }
    return;
}

RECOMP_DLL_FUNC(http_server_pop_message) {
    PTR(char) out_buf_addr = RECOMP_ARG(PTR(char), 0);
    int max_len = (int)RECOMP_ARG(s32, 1);
    if (out_buf_addr == 0 || max_len <= 0) {
        RECOMP_RETURN(int, 0);
    }

    char* out_buf = TO_PTR(char, out_buf_addr);

    std::string message;
    {
        std::lock_guard<std::mutex> lock(sQueueMutex);
        if (sMessageQueue.empty()) {
            RECOMP_RETURN(int, 0);
        }
        message = sMessageQueue.front();
        sMessageQueue.pop_front();
    }

    if ((int)message.size() >= max_len) {
        message.resize((size_t)(max_len - 1));
    }

    PTR(char) out_ptr = out_buf_addr;
    for (size_t i = 0; i < message.size(); ++i) {
        MEM_B(out_ptr, i) = (uint8_t)message[i];
    }
    MEM_B(out_ptr, message.size()) = 0x00;
    RECOMP_RETURN(int, (int)message.size());
}
