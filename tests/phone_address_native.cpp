#define PHONERECORD_ADDRESS_TESTING
#define PHONERECORD_NETWORK_TESTING
#include "../cpp/phonerecord.cpp"
#include <iostream>
#include <stdexcept>

/// Assert behavior without reporting credentials, personal configuration or real network addresses.
static void requireAddress(bool value, const char* label) {
    if (!value) throw std::runtime_error(label);
}

/// Invoke the actual handler and its post-close callback using synthetic transport metadata.
static void eventAddress(const string& source, const string& token, int expected, const char* event = "ip") {
    HttpRequest request; request.path = "/event"; request.method = "GET"; request.sourceIp = source;
    request.query["ev"] = event; request.query["pair"] = token; request.query["ip"] = "203.0.113.99";
    HttpResponse response; hookHandler(request, response);
    requireAddress(response.status == expected, "callback authorization");
    requireAddress(g_phonePendingAddress == 0, "no address change before acknowledgement closes");
    if (response.afterSend) response.afterSend();
}

/// Exercise real HTTP on a paired target; post-pairing login must never query MAC or routing again.
static void pairedHttp() {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    requireAddress(bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0 && listen(listener, 2) == 0, "loopback listener");
    int length = sizeof(local); getsockname(listener, reinterpret_cast<sockaddr*>(&local), &length);
    const int saved = g_cfg.phonePort; g_cfg.phonePort = ntohs(local.sin_port);
    g_addressSubnetTest = [](const string&, PhoneSubnet&) -> bool { throw std::runtime_error("repeated route/MAC verification after pairing"); };
    int received = 0; bool safe = true;
    std::thread peer([&] {
        for (int i = 0; i < 2; ++i) {
            fd_set ready; FD_ZERO(&ready); FD_SET(listener, &ready); timeval timeout{5, 0};
            if (select(0, &ready, nullptr, nullptr, &timeout) <= 0) break;
            SOCKET client = accept(listener, nullptr, nullptr); if (client == INVALID_SOCKET) break;
            DWORD wait = 3000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&wait), sizeof(wait));
            char buffer[4096]{}; const int count = recv(client, buffer, sizeof(buffer), 0);
            const string request(buffer, count > 0 ? count : 0);
            safe = safe && request.find("GET ") == 0 && request.find("Host: 127.0.0.1\r\n") != string::npos && request.find("Cookie:") == string::npos;
            const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nxyz";
            send(client, response, sizeof(response)-1, 0); closesocket(client); ++received;
        }
    });
    bool authenticated = false;
    try { authenticated = g_phone.login(); } catch (...) { peer.join(); closesocket(listener); throw; }
    peer.join(); closesocket(listener); g_cfg.phonePort = saved; g_addressSubnetTest = {};
    requireAddress(!authenticated && safe && received == 2, "paired HTTP uses target without stale cookies or MAC checks");
}

/// Validate first setup, protected persistence, authenticated handoff, source pinning and reset boundaries.
int main() {
    try {
        WSADATA data{}; requireAddress(WSAStartup(MAKEWORD(2,2), &data) == 0, "winsock");
        requireAddress(normalizePhoneMac("02-AA-bb-CC-dd-01") == "02aabbccdd01", "MAC normalization");
        for (const char* mac : {"", "00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff", "02:aa:bb:cc:dd", "02:aa:bb:cc:dd:zz"})
            requireAddress(normalizePhoneMac(mac).empty(), "MAC format rejection");
        g_cfg = Config{}; g_cfg.phoneIp = "127.0.0.1"; g_cfg.advertiseIp = "127.0.0.1"; g_cfg.phoneUser = "synthetic";
        requireAddress(g_cfg.errors().empty() && phoneAddress() == "127.0.0.1", "fixed default unchanged");
        eventAddress("127.0.0.2", "", 403, "est");
        g_cfg.phoneAddressMode = "dhcp"; requireAddress(!g_cfg.errors().empty(), "initial DHCP MAC required");
        g_cfg.phoneMac = "02-AA-BB-CC-DD-01"; requireAddress(g_cfg.errors().empty(), "DHCP config accepted");
        g_cfg.phoneIp.clear(); requireAddress(!g_cfg.errors().empty(), "initial DHCP IP required"); g_cfg.phoneIp = "127.0.0.1";
        g_configPath = L"pairing-native.ini"; initializePhoneAddress();
        requireAddress(phoneAddress() == "127.0.0.1" && !phonePaired(), "bootstrap from explicit IP without DNS or scanning");
        eventAddress("127.0.0.1", string(64,'a'), 403, "est");
        requireAddress(ensurePhonePairingToken(), "durable draft credential");
        const string token = phonePairingToken(); requireAddress(validPairingToken(token), "random credential format");
        string url; requireAddress(setupCallback("", "est", url) && compatibleCallback(url,"est"), "setup and inspection agree on credential");
        eventAddress("127.0.0.1", token, 403, "est");
        requireAddress(completePhonePairing(true), "pairing completes only explicitly after setup readback");
        std::ifstream stored(g_configPath + L".pairing",std::ios::binary); const string encrypted((std::istreambuf_iterator<char>(stored)),{}); stored.close();
        requireAddress(encrypted.compare(0,6,"dpapi:") == 0 && encrypted.find(token) == string::npos, "pairing is protected on disk");
        initializePhoneAddress(); requireAddress(phonePaired() && phonePairingToken() == token, "pairing reload requires matching config");
        eventAddress("127.0.0.2", string(64,'b'), 403); requireAddress(phoneAddress() == "127.0.0.1", "forged callback cannot rebind");
        eventAddress("127.0.0.2", token, 200);
        diagnosticUpdate([](DiagnosticSnapshot& state) { state.callActive = true; });
        requireAddress(!applyPendingPhoneAddress() && phoneAddress() == "127.0.0.1", "active call pins target");
        diagnosticUpdate([](DiagnosticSnapshot& state) { state.callActive = false; });
        g_sessionCount = 1; requireAddress(!applyPendingPhoneAddress(true), "control session pins target"); g_sessionCount = 0;
        requireAddress(applyPendingPhoneAddress() && phoneAddress() == "127.0.0.2", "source IP wins over supplied IP query");
        initializePhoneAddress(); requireAddress(phoneAddress() == "127.0.0.2", "last IP restored after process restart");
        eventAddress("127.0.0.1", token, 200); requireAddress(applyPendingPhoneAddress(), "return to loopback peer");
        pairedHttp();
        requireAddress(completePhonePairing(false) && !authorizedPhoneCallback(token), "restore deactivates pairing");
        g_cfg.phoneMac = "02-AA-BB-CC-DD-02"; initializePhoneAddress();
        requireAddress(!phonePaired() && phonePairingToken().empty(), "different device requires fresh explicit pairing");
        g_addressSubnetTest = [](const string&, PhoneSubnet& subnet) { subnet.index = 1; return true; };
        bool checked = false; g_addressMacTest = [&](const string&) { checked = true; return false; };
        requireAddress(!g_phone.login() && checked, "first on-link login checks supplied MAC");
        g_addressSubnetTest = {}; g_addressMacTest = {};
        WSACleanup();
        std::cout << "PHONE_PAIRING_NATIVE_OK fixed_default=true initial_ip_mac=true protected_reload=true authenticated_handoff=true call_pinning=true no_repeated_mac=true\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
