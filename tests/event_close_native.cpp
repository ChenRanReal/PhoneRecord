/// Verify that a phone can observe response EOF before reverse HTTP work begins.
#define WinMain PhoneRecordProductMain
#include "../cpp/phonerecord.cpp"
#undef WinMain
#include <stdexcept>

/// Fail with a stable label without logging device or caller data.
static void require(bool condition, const char* label) {
    if (!condition) throw std::runtime_error(label);
}

/// Own one synthetic loopback client socket, including assertion failure paths.
struct TestSocket {
    SOCKET value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    /// Close the socket exactly once.
    ~TestSocket() { if (value != INVALID_SOCKET) closesocket(value); }
};

/// Exercise response/close/callback ordering with the real event listener.
static void checkCloseBeforeCallback() {
    int port = 0;
    {
        TestSocket reservation;
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(reservation.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "reserve port");
        int length = sizeof(address);
        require(getsockname(reservation.value, reinterpret_cast<sockaddr*>(&address), &length) == 0, "read port");
        port = ntohs(address.sin_port);
    }
    std::atomic<bool> readerSawEof{false}, callbackEntered{false}, eofDuringCallback{false};
    HttpServer server(port, [&](HttpRequest&, HttpResponse& response) {
        response.body = "OK";
        response.afterSend = [&]() {
            callbackEntered = true;
            // Keep this callback running so EOF cannot come from its eventual return.
            const ULONGLONG deadline = GetTickCount64() + 1500;
            while (!readerSawEof && GetTickCount64() < deadline) Sleep(5);
            eofDuringCallback = readerSawEof.load();
        };
    });
    require(server.start(), "start event listener");
    TestSocket client;
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(static_cast<u_short>(port));
    require(connect(client.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect event client");
    const DWORD timeout = 3000;
    setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    require(sendComplete(client.value, "GET /event?ev=est HTTP/1.1\r\nHost: localhost\r\n\r\n"), "send event");
    string response;
    char buffer[1024];
    int received = 0;
    while ((received = recv(client.value, buffer, sizeof(buffer), 0)) > 0) response.append(buffer, received);
    readerSawEof = received == 0;
    server.shutdown();
    require(callbackEntered, "callback executed");
    require(response.find("HTTP/1.1 200 OK\r\n") == 0 && response.size() >= 2 && response.substr(response.size() - 2) == "OK", "complete HTTP acknowledgement");
    require(eofDuringCallback, "response EOF must precede callback completion");
}

/// Run only synthetic local networking; never start product services or contact a phone.
int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 2;
    int result = 0;
    try { checkCloseBeforeCallback(); puts("PASSED response_close_before_callback"); }
    catch (const std::exception& error) { fprintf(stderr, "FAILED %s\n", error.what()); result = 1; }
    WSACleanup();
    return result;
}
