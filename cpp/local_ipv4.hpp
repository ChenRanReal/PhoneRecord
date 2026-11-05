#pragma once

#include <iphlpapi.h>
#include <netioapi.h>

#pragma comment(lib, "iphlpapi.lib")

/// Assigned IPv4 and interface evidence used only to suggest a settings draft.
struct LocalIpv4Candidate {
    string address;
    ULONG metric = 0;
    bool gateway = false;
};

/// Reject unspecified, loopback, link-local, multicast and reserved destinations.
static bool usableLocalIpv4(const string& text) {
    IN_ADDR value{};
    if (InetPtonA(AF_INET, text.c_str(), &value) != 1) return false;
    const ULONG host = ntohl(value.s_addr);
    return (host >> 24) != 0 && (host >> 24) != 127 && (host >> 24) < 224 && (host >> 16) != 0xa9fe;
}

/// Prefer the OS route's assigned source; before a phone is known, require a unique best default.
static string selectLocalIpv4(const std::vector<LocalIpv4Candidate>& candidates, const string& routedSource = "", bool phoneKnown = false) {
    std::map<string, LocalIpv4Candidate> unique;
    for (const auto& candidate : candidates) if (usableLocalIpv4(candidate.address)) unique.emplace(candidate.address, candidate);
    if (phoneKnown) return unique.count(routedSource) ? routedSource : "";
    string best; ULONG metric = 0xffffffffUL; bool tied = false;
    for (const auto& item : unique) if (item.second.gateway) {
        if (best.empty() || item.second.metric < metric) { best = item.first; metric = item.second.metric; tied = false; }
        else if (item.second.metric == metric) tied = true;
    }
    if (!best.empty()) return tied ? "" : best;
    return unique.size() == 1 ? unique.begin()->first : "";
}

#ifdef PHONERECORD_NETWORK_TESTING
/// Test-only resolver injection; no environment or product command can enable it.
static std::function<string(const string&)> g_localIpv4TestResolver;
#endif

/// Read Windows adapter/routing tables without probing the phone or an external host.
static string detectLocalIpv4(const string& phone) {
#ifdef PHONERECORD_NETWORK_TESTING
    if (g_localIpv4TestResolver) return g_localIpv4TestResolver(phone);
#endif
    ULONG bytes = 16 * 1024;
    std::vector<unsigned char> buffer(bytes);
    IP_ADAPTER_ADDRESSES* adapters = nullptr;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        if (bytes > 4 * 1024 * 1024) return "";
        buffer.resize(bytes); adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bytes);
    }
    if (result != NO_ERROR) return "";
    std::vector<LocalIpv4Candidate> candidates;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        bool gateway = false;
        for (auto* item = adapter->FirstGatewayAddress; item; item = item->Next)
            if (item->Address.lpSockaddr && item->Address.lpSockaddr->sa_family == AF_INET &&
                reinterpret_cast<sockaddr_in*>(item->Address.lpSockaddr)->sin_addr.s_addr != INADDR_ANY) gateway = true;
        for (auto* item = adapter->FirstUnicastAddress; item; item = item->Next) {
            if (!item->Address.lpSockaddr || item->Address.lpSockaddr->sa_family != AF_INET || item->DadState != IpDadStatePreferred) continue;
            char address[INET_ADDRSTRLEN]{};
            const auto* value = reinterpret_cast<const sockaddr_in*>(item->Address.lpSockaddr);
            if (InetNtopA(AF_INET, const_cast<IN_ADDR*>(&value->sin_addr), address, sizeof(address)))
                candidates.push_back({address, adapter->Ipv4Metric, gateway});
        }
    }
    if (phone.empty()) return selectLocalIpv4(candidates);
    if (!usableLocalIpv4(phone)) return "";
    SOCKADDR_INET destination{}, source{};
    destination.Ipv4.sin_family = AF_INET;
    InetPtonA(AF_INET, phone.c_str(), &destination.Ipv4.sin_addr);
    MIB_IPFORWARD_ROW2 route{};
    if (GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &route, &source) != NO_ERROR || source.si_family != AF_INET) return "";
    char address[INET_ADDRSTRLEN]{};
    if (!InetNtopA(AF_INET, &source.Ipv4.sin_addr, address, sizeof(address))) return "";
    return selectLocalIpv4(candidates, address, true);
}
