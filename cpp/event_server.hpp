#pragma once

/// Bounded, owned HTTP event listener; callback work begins after response socket closure.
class HttpServer {
public:
    /// Store the listening port and request handler without starting background work.
    HttpServer(int port, std::function<void(HttpRequest&, HttpResponse&)> handler) : port_(port), handler_(handler) {}
    /// Join every client before releasing the server object.
    ~HttpServer() { shutdown(); }

    /// Bind exclusively; a port conflict remains an explicit product error.
    bool start() {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) return failure();
        BOOL exclusive = TRUE; setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = INADDR_ANY; address.sin_port = htons(static_cast<u_short>(port_));
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener_, 16) != 0) return failure();
        running_ = true;
        diagnosticUpdate([](DiagnosticSnapshot& state) { state.eventListening = true; });
        LOGI("EVENT_LISTENING port=%d", port_);
        acceptWorker_ = std::thread(&HttpServer::acceptLoop, this);
        return true;
    }

    /// Stop accepts, interrupt blocked clients, and join all owned worker handles.
    void shutdown() {
        if (!running_.exchange(false)) return;
        if (listener_ != INVALID_SOCKET) closesocket(listener_);
        if (acceptWorker_.joinable()) acceptWorker_.join();
        listener_ = INVALID_SOCKET;
        { std::lock_guard<std::mutex> guard(mutex_); for (SOCKET client : clients_) ::shutdown(client, SD_BOTH); }
        for (auto& worker : workers_) if (worker.thread.joinable()) worker.thread.join();
        workers_.clear();
        diagnosticUpdate([](DiagnosticSnapshot& state) { state.eventListening = false; });
        LOGI("EVENT_STOPPED workers_joined=true");
    }

private:
    /// One joinable HTTP worker and its completion marker.
    struct Worker {
        /// Actual bounded worker thread.
        std::thread thread;
        /// Signals that socket cleanup is complete.
        std::shared_ptr<std::atomic<bool>> done;
    };

    /// Release a failed listener and retain an actionable port/error diagnostic.
    bool failure() {
        const int error = WSAGetLastError();
        if (listener_ != INVALID_SOCKET) { closesocket(listener_); listener_ = INVALID_SOCKET; }
        diagnosticStage(RecordingStage::Failed, "EVENT_BIND_FAILED", "port=" + std::to_string(port_) + " wsa=" + std::to_string(error));
        LOGE("EVENT_BIND_FAILED port=%d wsa=%d", port_, error); return false;
    }

    /// Accept a bounded number of clients, reclaiming finished thread resources.
    void acceptLoop() {
        while (running_) {
            for (auto it = workers_.begin(); it != workers_.end();) {
                if (*it->done) { it->thread.join(); it = workers_.erase(it); } else ++it;
            }
            fd_set readable; FD_ZERO(&readable); FD_SET(listener_, &readable);
            timeval wait{0, 200000};
            if (select(0, &readable, NULL, NULL, &wait) <= 0) continue;
            SOCKET client = accept(listener_, NULL, NULL);
            if (client == INVALID_SOCKET) continue;
            { std::lock_guard<std::mutex> guard(mutex_); if (clients_.size() >= 16) { closesocket(client); LOGW("EVENT_CLIENT_LIMIT"); continue; } clients_.insert(client); }
            auto done = std::make_shared<std::atomic<bool>>(false);
            Worker worker; worker.done = done;
            worker.thread = std::thread([this, client, done]() {
                std::function<void()> afterClose;
                handle(client, afterClose);
                { std::lock_guard<std::mutex> guard(mutex_); clients_.erase(client); }
                closesocket(client);
                // Release the Action URL connection before issuing reverse phone HTTP.
                // Sending the body alone leaves a race with the phone waiting for EOF.
                if (afterClose) afterClose();
                *done = true;
            });
            workers_.push_back(std::move(worker));
        }
    }

    /// Send a bounded acknowledgement and transfer successful work to the socket owner.
    void handle(SOCKET client, std::function<void()>& afterClose) {
        u_long nonblocking = 1; ioctlsocket(client, FIONBIO, &nonblocking);
        string raw;
        size_t headerEnd = string::npos, need = 0;
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (running_ && !g_stopping && GetTickCount64() < deadline) {
            fd_set readable; FD_ZERO(&readable); FD_SET(client, &readable);
            timeval wait{0, 200000};
            if (select(0, &readable, NULL, NULL, &wait) <= 0) continue;
            char chunk[4096];
            int received = recv(client, chunk, sizeof(chunk), 0);
            if (received <= 0) return;
            raw.append(chunk, received);
            if (raw.size() > 16384) { LOGW("EVENT_REQUEST_TOO_LARGE"); return; }
            if (headerEnd == string::npos) {
                headerEnd = raw.find("\r\n\r\n");
                if (headerEnd != string::npos) {
                    const string head = toLower(raw.substr(0, headerEnd));
                    const size_t length = head.find("content-length:");
                    if (length != string::npos) {
                        const size_t end = head.find("\r\n", length);
                        const int parsed = configurationInteger(trim(head.substr(length + 15, end - length - 15)));
                        if (parsed < 0 || parsed > 8192) { LOGW("EVENT_BAD_CONTENT_LENGTH"); return; }
                        need = parsed;
                    }
                }
            }
            if (headerEnd != string::npos && raw.size() >= headerEnd + 4 + need) break;
        }
        if (headerEnd == string::npos || raw.size() < headerEnd + 4 + need) { LOGW("EVENT_REQUEST_INCOMPLETE"); return; }
        HttpRequest request;
        sockaddr_in peer{}; int length = sizeof(peer);
        getpeername(client, reinterpret_cast<sockaddr*>(&peer), &length);
        request.sourceIp = inet_ntoa(peer.sin_addr);
        std::istringstream firstLine(raw.substr(0, raw.find("\r\n")));
        string target, version;
        firstLine >> request.method >> target >> version;
        if (target.empty() || version.compare(0, 5, "HTTP/") != 0) return;
        const size_t query = target.find('?');
        request.path = urlDecode(target.substr(0, query));
        if (query != string::npos) request.query = parseQuery(target.substr(query + 1));
        request.body = raw.substr(headerEnd + 4, need);
        HttpResponse response;
        try { handler_(request, response); }
        catch (...) { response.status = 500; response.body = "internal error"; response.afterSend = nullptr; LOGE("EVENT_HANDLER_EXCEPTION"); }
        std::ostringstream output;
        output << "HTTP/1.1 " << response.status << (response.status == 200 ? " OK" : " ERROR")
               << "\r\nContent-Type: " << response.contentType << "\r\nContent-Length: " << response.body.size()
               << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" << response.body;
        const bool sent = sendComplete(client, output.str());
        if (request.path != "/health") LOGI("EVENT_HTTP_ACK path=%s status=%d sent=%d", request.path == "/event" ? "/event" : "other", response.status, sent);
        if (sent) afterClose = std::move(response.afterSend);
    }

    /// Configured TCP port.
    int port_;
    /// Actual listener, closed before worker joins.
    SOCKET listener_ = INVALID_SOCKET;
    /// Listener lifetime flag.
    std::atomic<bool> running_{false};
    /// Owned accept worker.
    std::thread acceptWorker_;
    /// Owned client workers, modified only by acceptLoop until it joins.
    std::vector<Worker> workers_;
    /// Live sockets used to interrupt shutdown waits.
    std::set<SOCKET> clients_;
    /// Protect the socket set.
    std::mutex mutex_;
    /// Established in-process route handler.
    std::function<void(HttpRequest&, HttpResponse&)> handler_;
};
