#pragma once

#include "local_ipv4.hpp"

/// Shared IPv4 endpoint; immutable configuration retains the user's original discovery hint.
static std::atomic<ULONG> g_phoneResolved{0};
/// Incremented only when the trusted endpoint changes, to schedule authentication again.
static std::atomic<unsigned> g_phoneAddressRevision{0};

/// Format a network-order IPv4 value without inet_ntoa's thread-local scratch storage.
static string phoneIpv4Text(ULONG value) {
    IN_ADDR address{}; address.s_addr = value;
    char text[INET_ADDRSTRLEN]{};
    return InetNtopA(AF_INET, &address, text, sizeof(text)) ? text : "";
}

/// Snapshot the current target; unresolved DHCP never falls back to an unverified old IP.
static string phoneAddress() {
    if (g_cfg.phoneAddressMode == "fixed") return g_cfg.phoneIp;
    const ULONG value = g_phoneResolved.load();
    return value ? phoneIpv4Text(value) : "";
}

/// Direct-route evidence used only for first-time MAC validation.
struct PhoneSubnet {
    /// Source interface and network-order local address, used to bind discovery traffic.
    ULONG index = 0, source = 0;
};

#ifdef PHONERECORD_ADDRESS_TESTING
/// Native harness-only evidence providers; release builds expose no injection mechanism.
static std::function<bool(const string&, PhoneSubnet&)> g_addressSubnetTest;
static std::function<bool(const string&)> g_addressMacTest;
#endif

/// Detect the actual direct route automatically; routed peers are never subjected to ARP.
static bool phoneSubnet(const string& hint, PhoneSubnet& subnet) {
#ifdef PHONERECORD_ADDRESS_TESTING
    if (g_addressSubnetTest) return g_addressSubnetTest(hint, subnet);
#endif
    if (!usableLocalIpv4(hint)) return false;
    SOCKADDR_INET destination{}, source{}; destination.si_family = AF_INET;
    InetPtonA(AF_INET, hint.c_str(), &destination.Ipv4.sin_addr);
    MIB_IPFORWARD_ROW2 route{};
    if (GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &route, &source) != NO_ERROR ||
        source.si_family != AF_INET || route.NextHop.Ipv4.sin_addr.s_addr != 0 ||
        route.DestinationPrefix.PrefixLength == 0 || route.DestinationPrefix.PrefixLength > 32) return false;
    subnet.index = route.InterfaceIndex; subnet.source = source.Ipv4.sin_addr.s_addr;
    return true;
}

/// Compare one six-byte Ethernet address to the configured normalized identity.
static bool phoneMacMatches(const UCHAR* bytes, ULONG length) {
    if (length != 6) return false;
    char text[13]{};
    for (int i = 0; i < 6; ++i) snprintf(text + i * 2, 3, "%02x", bytes[i]);
    return normalizePhoneMac(g_cfg.phoneMac) == text;
}

/// Revalidate an on-link address before sending cookies or credentials to a DHCP target.
static bool verifyPhoneMac(const string& address) {
#ifdef PHONERECORD_ADDRESS_TESTING
    if (g_addressMacTest) return g_addressMacTest(address);
#endif
    PhoneSubnet subnet;
    if (!phoneSubnet(address, subnet)) return false;
    MIB_IPNET_ROW2 row{}; row.InterfaceIndex = subnet.index; row.Address.si_family = AF_INET;
    InetPtonA(AF_INET, address.c_str(), &row.Address.Ipv4.sin_addr);
    SOCKADDR_INET source{}; source.si_family = AF_INET; source.Ipv4.sin_addr.s_addr = subnet.source;
    return ResolveIpNetEntry2(&row, &source) == NO_ERROR && phoneMacMatches(row.PhysicalAddress, row.PhysicalAddressLength);
}

/// Current-account protected pairing state; the bearer credential never enters ordinary configuration or logs.
struct PhonePairing {
    /// Random 256-bit callback credential, configuration identity and last acknowledged target.
    string token, binding, lastIp;
    /// True only after the phone's complete callback settings have passed readback.
    bool paired = false;
};
/// Protect pairing snapshots and their atomic on-disk replacement.
static std::mutex g_phonePairingMutex;
/// Owned pairing metadata; every read or write holds g_phonePairingMutex.
static PhonePairing g_phonePairing;
/// Latest authenticated callback source awaiting the serialized command worker.
static std::atomic<ULONG> g_phonePendingAddress{0};

/// Bind a pairing to the selected initial phone, account and callback destination; never to a translated label.
static string phonePairingBinding() {
    string result;
    for (const auto& field : {g_cfg.phoneIp, normalizePhoneMac(g_cfg.phoneMac), g_cfg.phoneUser,
        std::to_string(g_cfg.phonePort), g_cfg.advertiseIp, std::to_string(g_cfg.hookPort), std::to_string(g_cfg.controlPort)})
        result += std::to_string(field.size()) + ":" + field;
    return result;
}

/// Validate the generated credential's stable wire format without accepting caller-controlled lengths.
static bool validPairingToken(const string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == string::npos;
}

/// Persist under the pairing mutex; failure leaves the previous protected state intact.
static bool savePhonePairing(const PhonePairing& value) {
    const string plain = "PhoneRecordPairing1\n" + value.binding + "\n" + value.token + "\n" + value.lastIp + "\n" + (value.paired ? "1\n" : "0\n");
    string encrypted;
    if (!protectLocalSecret(plain, encrypted)) return false;
    const wstring path = g_configPath + L".pairing", pending = path + L".pending";
    std::ofstream output(pending, std::ios::binary | std::ios::trunc);
    output << encrypted; output.flush(); const bool ok = static_cast<bool>(output); output.close();
    return ok && MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

/// Restore only matching protected metadata; first setup uses the explicitly supplied current IP.
static void initializePhoneAddress() {
    std::lock_guard<std::mutex> guard(g_phonePairingMutex);
    g_phonePairing = PhonePairing{}; g_phonePendingAddress = 0; g_phoneResolved = 0;
    if (g_cfg.phoneAddressMode != "dhcp" || !g_cfg.errors().empty()) return;
    g_phonePairing.binding = phonePairingBinding(); g_phonePairing.lastIp = g_cfg.phoneIp;
    std::ifstream input(g_configPath + L".pairing", std::ios::binary);
    char bytes[8193]{}; input.read(bytes, sizeof(bytes));
    if (input.gcount() > 0 && input.gcount() <= 8192) {
        const string encoded(bytes, static_cast<size_t>(input.gcount())); string plain;
        if (encoded.compare(0, 6, "dpapi:") == 0 && unprotectLocalSecret(encoded, plain)) {
            std::istringstream reader(plain); string marker, binding, token, lastIp, paired;
            if (std::getline(reader, marker) && std::getline(reader, binding) && std::getline(reader, token) &&
                std::getline(reader, lastIp) && std::getline(reader, paired) && marker == "PhoneRecordPairing1" &&
                binding == g_phonePairing.binding && validPairingToken(token) && validIpv4(lastIp) && (paired == "0" || paired == "1"))
                g_phonePairing = {token, binding, lastIp, paired == "1"};
        }
    }
    IN_ADDR address{}; InetPtonA(AF_INET, g_phonePairing.lastIp.c_str(), &address); g_phoneResolved = address.s_addr;
}

/// Prepare one durable random credential during explicit setup preview; never silently alter the phone.
static bool ensurePhonePairingToken() {
    if (g_cfg.phoneAddressMode != "dhcp") return true;
    std::lock_guard<std::mutex> guard(g_phonePairingMutex);
    if (validPairingToken(g_phonePairing.token) && g_phonePairing.binding == phonePairingBinding()) return true;
    HCRYPTPROV provider = 0; BYTE bytes[32]{};
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return false;
    const bool generated = CryptGenRandom(provider, sizeof(bytes), bytes) != FALSE; CryptReleaseContext(provider, 0);
    if (!generated) return false;
    string token; char hex[3]{};
    for (BYTE byte : bytes) { snprintf(hex, sizeof(hex), "%02x", byte); token += hex; }
    SecureZeroMemory(bytes, sizeof(bytes));
    PhonePairing draft{token, phonePairingBinding(), phoneAddress(), false};
    if (!savePhonePairing(draft)) return false;
    g_phonePairing = draft; return true;
}

/// Return an internal-only callback credential snapshot, never an operator-facing preview or report.
static string phonePairingToken() {
    std::lock_guard<std::mutex> guard(g_phonePairingMutex); return g_phonePairing.token;
}

/// Report whether setup readback has established the pairing, independently of current phone reachability.
static bool phonePaired() {
    std::lock_guard<std::mutex> guard(g_phonePairingMutex); return g_phonePairing.paired;
}

/// Commit or deactivate pairing only after verified setup or restore; persistence failure remains visible.
static bool completePhonePairing(bool paired) {
    if (g_cfg.phoneAddressMode != "dhcp") return true;
    std::lock_guard<std::mutex> guard(g_phonePairingMutex);
    if (!validPairingToken(g_phonePairing.token)) return false;
    PhonePairing next = g_phonePairing; next.paired = paired; next.lastIp = phoneAddress();
    if (!savePhonePairing(next)) return false;
    g_phonePairing = next; return true;
}

/// Constant-work credential comparison; a DHCP callback must authenticate even when its source IP is unchanged.
static bool authorizedPhoneCallback(const string& supplied) {
    std::lock_guard<std::mutex> guard(g_phonePairingMutex);
    if (!g_phonePairing.paired || !validPairingToken(supplied) || !validPairingToken(g_phonePairing.token)) return false;
    unsigned difference = 0;
    for (size_t i = 0; i < 64; ++i) difference |= static_cast<unsigned char>(supplied[i] ^ g_phonePairing.token[i]);
    return difference == 0;
}

/// Stage only an authenticated transport source, never a caller-supplied IP query parameter.
static void stagePhoneAddress(const string& source) {
    IN_ADDR parsed{};
    if (source != phoneAddress() && validIpv4(source) && InetPtonA(AF_INET, source.c_str(), &parsed) == 1)
        g_phonePendingAddress = parsed.s_addr;
}

/// Apply a staged address on the sole HTTP worker; calls/control sessions pin their existing target.
static bool applyPendingPhoneAddress(bool startingCall = false) {
    const ULONG pending = g_phonePendingAddress.load(); if (!pending) return true;
    std::lock_guard<std::mutex> admission(g_updateAdmission);
    const auto state = diagnosticSnapshot();
    if (g_sessionCount > 0 || (state.callActive && !startingCall) || g_updateInstalling || g_stopping) return false;
    std::lock_guard<std::mutex> guard(g_phonePairingMutex);
    PhonePairing next = g_phonePairing; next.lastIp = phoneIpv4Text(pending);
    if (!next.paired || !savePhonePairing(next)) { LOGW("PHONE_PAIRING_SAVE_FAILED"); return false; }
    g_phonePairing = next; g_phoneResolved = pending;
    ULONG expected = pending; g_phonePendingAddress.compare_exchange_strong(expected, 0);
    diagnosticUpdate([](DiagnosticSnapshot& value) { value.authenticated = false; }); ++g_phoneAddressRevision;
    LOGI("PHONE_ADDRESS_CHANGED authenticated_callback=true"); return true;
}
