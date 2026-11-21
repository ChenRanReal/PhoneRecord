#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <string>

/// GitHub rejects a release asset at or above 2 GiB. Comparisons use this exclusive bound.
inline constexpr std::uint64_t kGitHubReleaseAssetMaxBytes = 2147483648ULL;

/// Where an installed model may come from. Download is the historical on-demand path.
enum class SpeechSupply {
    /// No usable bundled copy. The explicit installer may fetch it.
    Download = 0,
    /// Install-directory bytes matched the compiled size and SHA-256.
    Bundled = 1,
    /// A bundle claims the model, or some of its files exist, but the pin check failed.
    /// Callers must not replace that copy by downloading.
    Blocked = 2,
};

/// One file observed beside the executable. An empty digest means the hash was not computed yet.
struct SpeechFileFact {
    /// The path exists and is a regular file.
    bool present = false;
    /// Exact byte length, meaningful only when present is true.
    std::uint64_t size = 0;
    /// Lowercase SHA-256 hex, or empty when only the size was collected.
    std::string sha256;
};

/// Which model trees bundle.txt says this directory is supposed to contain.
struct SpeechBundleClaim {
    /// SenseVoice INT8, tokens and Silero VAD.
    bool sense = false;
    /// Qwen3-ASR 0.6B.
    bool qwen = false;
    /// Qwen3-ASR 1.7B.
    bool qwen17 = false;

    /// Return the flag for a stable model id. Unknown ids are unclaimed.
    bool claimed(const char* model) const {
        if (!model) return false;
        if (std::strcmp(model, "sense") == 0) return sense;
        if (std::strcmp(model, "qwen") == 0) return qwen;
        if (std::strcmp(model, "qwen17") == 0) return qwen17;
        return false;
    }
};

/// Compiled identity of one file that must match before a bundle is trusted.
struct SpeechPin {
    /// Stable model id: sense, qwen or qwen17.
    const char* model;
    /// Forward-slash path relative to bundled-speech.
    const char* relative;
    /// Exact byte length published with the pin.
    std::uint64_t size;
    /// Lowercase SHA-256 of those bytes.
    const char* sha256;
};

/// Runtime file that must exist for offline inference. Its bytes are not pinned in the executable.
struct SpeechRuntimeFile {
    /// Stable model id that needs this file.
    const char* model;
    /// Forward-slash path relative to bundled-speech.
    const char* relative;
};

/// SenseVoice and Qwen pins. These are the same identities install.ps1 and qwen-assets.json download.
inline constexpr SpeechPin kSpeechPins[] = {
    {"sense", "models/model.int8.onnx", 239233841ULL, "c71f0ce00bec95b07744e116345e33d8cbbe08cef896382cf907bf4b51a2cd51"},
    {"sense", "models/tokens.txt", 315894ULL, "f449eb28dc567533d7fa59be34e2abca8784f771850c78a47fb731a31429a1dc"},
    {"sense", "models/silero_vad.onnx", 1807522ULL, "a35ebf52fd3ce5f1469b2a36158dba761bc47b973ea3382b3186ca15b1f5af28"},
    {"qwen", "qwen/models/chat_template.jinja", 1434ULL, "f50e6b694fbf4a683206e37869990d68333fe95d285730f084c838a34b0d98c2"},
    {"qwen", "qwen/models/config.json", 2398ULL, "9eecf6f1b383e343889c2e6010e632590fa57d4bc678e151c7d6a160a0dfb04a"},
    {"qwen", "qwen/models/generation_config.json", 165ULL, "9939fc9388b79bd70757f938b87381e817173d6a6158f5af6506c0b73e775c3c"},
    {"qwen", "qwen/models/model.safetensors", 1564928088ULL, "d3f212dd20abecd315d830bc54ae3865e56ebfc3276484e57b771288ba27fd35"},
    {"qwen", "qwen/models/processor_config.json", 487ULL, "bc0b230081b44e629dd5b9045b78495615c1831b4b9f4cffe97bd37e82a6156a"},
    {"qwen", "qwen/models/tokenizer.json", 11429653ULL, "fe1fad59be22a41ee293363fcf95fdedbc7c93f3b49270b1d2e18bd1399a7a05"},
    {"qwen", "qwen/models/tokenizer_config.json", 998ULL, "945e980986de2ca7768f3326bfdbb4fbea3406f972b8ae0be233089f2b253c11"},
    {"qwen17", "qwen/models-1.7b/chat_template.jinja", 1434ULL, "f50e6b694fbf4a683206e37869990d68333fe95d285730f084c838a34b0d98c2"},
    {"qwen17", "qwen/models-1.7b/config.json", 2399ULL, "117ac8e63e2af7cae3665e5a632d6eb03f5f384915519ceb6403c15ec6533f63"},
    {"qwen17", "qwen/models-1.7b/generation_config.json", 165ULL, "9939fc9388b79bd70757f938b87381e817173d6a6158f5af6506c0b73e775c3c"},
    {"qwen17", "qwen/models-1.7b/model.safetensors", 4076193080ULL, "2db53c7d81bd9b8cbc6a074e89be2c968a0d373fb4ee68bb1b1e14f7042dfee1"},
    {"qwen17", "qwen/models-1.7b/processor_config.json", 487ULL, "bc0b230081b44e629dd5b9045b78495615c1831b4b9f4cffe97bd37e82a6156a"},
    {"qwen17", "qwen/models-1.7b/tokenizer.json", 11429653ULL, "fe1fad59be22a41ee293363fcf95fdedbc7c93f3b49270b1d2e18bd1399a7a05"},
    {"qwen17", "qwen/models-1.7b/tokenizer_config.json", 998ULL, "945e980986de2ca7768f3326bfdbb4fbea3406f972b8ae0be233089f2b253c11"},
};

/// Files produced by the pinned installers. Presence is required; the model pins carry the hashes.
inline constexpr SpeechRuntimeFile kSpeechRuntimeFiles[] = {
    {"sense", "node-v24.11.0-win-x64/node.exe"},
    {"sense", "runtime/node_modules/sherpa-onnx-node/package.json"},
    {"sense", "installed-v1"},
    {"qwen", "qwen/python/python.exe"},
    {"qwen", "qwen/installed-v1"},
    {"qwen17", "qwen/python/python.exe"},
    {"qwen17", "qwen/installed-1.7b-v1"},
};

/// Result of comparing one bundle directory with the compiled pins.
struct SpeechBundleAssessment {
    /// SenseVoice supply.
    SpeechSupply sense = SpeechSupply::Download;
    /// Qwen 0.6B supply.
    SpeechSupply qwen = SpeechSupply::Download;
    /// Qwen 1.7B supply.
    SpeechSupply qwen17 = SpeechSupply::Download;

    /// Supply for a stable model id. Unknown ids download.
    SpeechSupply supply(const char* model) const {
        if (!model) return SpeechSupply::Download;
        if (std::strcmp(model, "sense") == 0) return sense;
        if (std::strcmp(model, "qwen") == 0) return qwen;
        if (std::strcmp(model, "qwen17") == 0) return qwen17;
        return SpeechSupply::Download;
    }
};

/// Stable name used by tests and diagnostics. Not a localized label.
inline const char* speechSupplyName(SpeechSupply supply) {
    if (supply == SpeechSupply::Bundled) return "bundled";
    if (supply == SpeechSupply::Blocked) return "blocked";
    return "download";
}

/// True when every hex digit matches, ignoring ASCII case on the observed digest.
inline bool speechDigestEquals(const std::string& actual, const char* expected) {
    if (!expected || actual.size() != 64) return false;
    for (size_t index = 0; index < 64; ++index) {
        unsigned char left = static_cast<unsigned char>(actual[index]);
        unsigned char right = static_cast<unsigned char>(expected[index]);
        if (left >= 'A' && left <= 'F') left = static_cast<unsigned char>(left - 'A' + 'a');
        if (right >= 'A' && right <= 'F') right = static_cast<unsigned char>(right - 'A' + 'a');
        if (left != right) return false;
    }
    return true;
}

/// Parse the four-line bundle claim. A missing or malformed file claims nothing.
inline bool speechParseBundleClaim(const std::string& text, SpeechBundleClaim& claim) {
    claim = {};
    if (text.empty() || text.back() != '\n' || text.find('\r') != std::string::npos) return false;
    std::string lines[4];
    size_t count = 0, start = 0;
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '\n') continue;
        if (count >= 4) return false;
        lines[count++] = text.substr(start, index - start);
        start = index + 1;
    }
    if (count != 4 || start != text.size() || lines[0] != "PhoneRecord-bundle-v1") return false;
    auto flag = [](const std::string& line, const char* key, bool& value) {
        const size_t length = std::strlen(key);
        if (line.compare(0, length, key) != 0) return false;
        const std::string body = line.substr(length);
        if (body == "0") { value = false; return true; }
        if (body == "1") { value = true; return true; }
        return false;
    };
    return flag(lines[1], "sense=", claim.sense) && flag(lines[2], "qwen=", claim.qwen) && flag(lines[3], "qwen17=", claim.qwen17);
}

/// Canonical bundle.txt bytes for the packager and tests.
inline std::string speechBundleClaimText(const SpeechBundleClaim& claim) {
    return std::string("PhoneRecord-bundle-v1\nsense=") + (claim.sense ? "1" : "0") + "\nqwen=" + (claim.qwen ? "1" : "0") + "\nqwen17=" + (claim.qwen17 ? "1" : "0") + "\n";
}

/// Find one relative path. Missing keys are absent files.
inline const SpeechFileFact* speechFindFact(const std::map<std::string, SpeechFileFact>& files, const char* relative) {
    const auto found = files.find(relative);
    return found == files.end() ? nullptr : &found->second;
}

/// True when every pin for the model is present with its compiled size and digest.
inline bool speechPinsMatch(const char* model, const std::map<std::string, SpeechFileFact>& files) {
    bool any = false;
    for (const SpeechPin& pin : kSpeechPins) {
        if (std::strcmp(pin.model, model) != 0) continue;
        any = true;
        const SpeechFileFact* fact = speechFindFact(files, pin.relative);
        if (!fact || !fact->present || fact->size != pin.size || !speechDigestEquals(fact->sha256, pin.sha256)) return false;
    }
    return any;
}

/// True when every required runtime file is present. Digests are intentionally ignored.
inline bool speechRuntimePresent(const char* model, const std::map<std::string, SpeechFileFact>& files) {
    bool any = false;
    for (const SpeechRuntimeFile& file : kSpeechRuntimeFiles) {
        if (std::strcmp(file.model, model) != 0) continue;
        any = true;
        const SpeechFileFact* fact = speechFindFact(files, file.relative);
        if (!fact || !fact->present) return false;
    }
    return any;
}

/// True when at least one pinned payload exists, even if it is the wrong size or digest.
inline bool speechAnyPinPresent(const char* model, const std::map<std::string, SpeechFileFact>& files) {
    for (const SpeechPin& pin : kSpeechPins) {
        if (std::strcmp(pin.model, model) != 0) continue;
        const SpeechFileFact* fact = speechFindFact(files, pin.relative);
        if (fact && fact->present) return true;
    }
    return false;
}

/// Size-only health check used before a full digest, including by the updater.
inline bool speechBundleSized(const char* model, bool directoryPresent, const std::map<std::string, SpeechFileFact>& files) {
    if (!directoryPresent || !model || !model[0]) return false;
    bool any = false;
    for (const SpeechPin& pin : kSpeechPins) {
        if (std::strcmp(pin.model, model) != 0) continue;
        any = true;
        const SpeechFileFact* fact = speechFindFact(files, pin.relative);
        if (!fact || !fact->present || fact->size != pin.size) return false;
    }
    return any && speechRuntimePresent(model, files);
}

/// True when the directory is responsible for this model and a download must not replace it.
inline bool speechBundleHolds(const char* model, bool directoryPresent, const SpeechBundleClaim& claim, const std::map<std::string, SpeechFileFact>& files) {
    if (!directoryPresent || !model) return false;
    return claim.claimed(model) || speechAnyPinPresent(model, files);
}

/// Decide one model. A matching pin set wins over the claim; a partial claim blocks download.
inline SpeechSupply speechSupplyFor(const char* model, bool directoryPresent, bool claimed, const std::map<std::string, SpeechFileFact>& files) {
    if (!directoryPresent) return SpeechSupply::Download;
    if (speechPinsMatch(model, files) && speechRuntimePresent(model, files)) return SpeechSupply::Bundled;
    if (claimed || speechAnyPinPresent(model, files)) return SpeechSupply::Blocked;
    return SpeechSupply::Download;
}

/// Assess every model. directoryPresent is false when bundled-speech does not exist.
inline SpeechBundleAssessment speechAssessBundle(bool directoryPresent, const SpeechBundleClaim& claim, const std::map<std::string, SpeechFileFact>& files) {
    SpeechBundleAssessment result;
    result.sense = speechSupplyFor("sense", directoryPresent, claim.sense, files);
    result.qwen = speechSupplyFor("qwen", directoryPresent, claim.qwen, files);
    result.qwen17 = speechSupplyFor("qwen17", directoryPresent, claim.qwen17, files);
    return result;
}

/// Whether the installer is allowed to fetch this model.
inline bool speechForbidsDownload(bool directoryPresent, bool verified, const SpeechBundleClaim& claim, const SpeechBundleAssessment& assessment, const std::map<std::string, SpeechFileFact>& files, const char* model) {
    if (!directoryPresent || !model) return false;
    if (!verified) return speechBundleHolds(model, true, claim, files);
    return assessment.supply(model) != SpeechSupply::Download;
}

/// Sum of pinned bytes for one model. Zero when the model id is unknown.
inline std::uint64_t speechPinnedBytes(const char* model) {
    std::uint64_t total = 0;
    if (!model) return 0;
    for (const SpeechPin& pin : kSpeechPins) {
        if (std::strcmp(pin.model, model) != 0) continue;
        if (pin.size >= kGitHubReleaseAssetMaxBytes || total > kGitHubReleaseAssetMaxBytes - pin.size) return kGitHubReleaseAssetMaxBytes;
        total += pin.size;
    }
    return total;
}

/// Channel recorded beside an executable. Standard keeps today's on-demand models.
enum class UpdateVariant {
    /// No install-variant.txt, or an explicit standard install.
    Standard = 0,
    /// SenseVoice bundle. Live captions and SenseVoice transcription stay offline.
    OfflineSenseVoice = 1,
    /// Qwen3-ASR 0.6B bundle.
    OfflineQwen06 = 2,
    /// Qwen3-ASR 1.7B bundle built for local media. It is not a GitHub Release asset.
    OfflineQwen17 = 3,
};

/// Wire id stored in install-variant.txt. Stable ASCII, not a translated label.
inline const char* updateVariantId(UpdateVariant variant) {
    if (variant == UpdateVariant::OfflineSenseVoice) return "offline-sensevoice";
    if (variant == UpdateVariant::OfflineQwen06) return "offline-qwen06";
    if (variant == UpdateVariant::OfflineQwen17) return "offline-qwen17";
    return "standard";
}

/// Model id maintained by an offline channel. Standard returns an empty string.
inline const char* updateVariantModel(UpdateVariant variant) {
    if (variant == UpdateVariant::OfflineSenseVoice) return "sense";
    if (variant == UpdateVariant::OfflineQwen06) return "qwen";
    if (variant == UpdateVariant::OfflineQwen17) return "qwen17";
    return "";
}

/// Parse install-variant.txt. Any malformed file is the standard channel.
inline UpdateVariant updateParseVariant(const std::string& text) {
    if (text.empty() || text.back() != '\n' || text.find('\r') != std::string::npos) return UpdateVariant::Standard;
    const std::string magic = "PhoneRecord-variant-v1\n";
    const std::string prefix = "variant=";
    if (text.compare(0, magic.size(), magic) != 0) return UpdateVariant::Standard;
    const size_t end = text.find('\n', magic.size());
    if (end == std::string::npos || end + 1 != text.size()) return UpdateVariant::Standard;
    const std::string line = text.substr(magic.size(), end - magic.size());
    if (line.compare(0, prefix.size(), prefix) != 0) return UpdateVariant::Standard;
    const std::string value = line.substr(prefix.size());
    if (value == "offline-sensevoice") return UpdateVariant::OfflineSenseVoice;
    if (value == "offline-qwen06") return UpdateVariant::OfflineQwen06;
    if (value == "offline-qwen17") return UpdateVariant::OfflineQwen17;
    return UpdateVariant::Standard;
}

/// Canonical variant file body.
inline std::string updateVariantText(UpdateVariant variant) {
    return std::string("PhoneRecord-variant-v1\nvariant=") + updateVariantId(variant) + "\n";
}

/// Release file names for one offline channel. Empty strings mean "no model pack".
struct ModelPackAssets {
    /// True only when the local bundle is missing or the wrong size and a pack should be fetched.
    bool download = false;
    /// Zip asset. Empty for the standard channel.
    const char* zip = "";
    /// Signed model-pack manifest asset.
    const char* manifest = "";
    /// Detached signature asset.
    const char* signature = "";
};

/// Zip name published for a variant, or an empty string for the standard channel.
inline const char* offlineZipName(UpdateVariant variant) {
    if (variant == UpdateVariant::OfflineSenseVoice) return "PhoneRecord-x64-offline-sensevoice.zip";
    if (variant == UpdateVariant::OfflineQwen06) return "PhoneRecord-x64-offline-qwen06.zip";
    if (variant == UpdateVariant::OfflineQwen17) return "PhoneRecord-x64-offline-qwen17.zip";
    return "";
}

/// Choose the model-pack assets. A healthy local bundle never requests a download.
inline ModelPackAssets updateDecideModelPack(UpdateVariant variant, bool localBundleHealthy) {
    ModelPackAssets assets;
    assets.zip = offlineZipName(variant);
    if (!assets.zip[0]) return assets;
    if (variant == UpdateVariant::OfflineSenseVoice) {
        assets.manifest = "update-manifest-x64-offline-sensevoice.txt";
        assets.signature = "update-manifest-x64-offline-sensevoice.sig";
    } else if (variant == UpdateVariant::OfflineQwen06) {
        assets.manifest = "update-manifest-x64-offline-qwen06.txt";
        assets.signature = "update-manifest-x64-offline-qwen06.sig";
    } else if (variant == UpdateVariant::OfflineQwen17) {
        assets.manifest = "update-manifest-x64-offline-qwen17.txt";
        assets.signature = "update-manifest-x64-offline-qwen17.sig";
    }
    assets.download = !localBundleHealthy;
    return assets;
}

/// Files the updater may delete inside its own random staging directory.
inline const char* const kUpdateStagedFiles[] = {
    "PhoneRecord.exe",
    "PhoneRecord.exe.part",
    "PhoneRecord-updater.exe",
    "update-manifest.txt",
    "update-manifest.sig",
    "model-pack.zip",
    "model-pack.zip.part",
    "model-pack-manifest.txt",
    "model-pack-manifest.sig",
};

/// True when cleanup of a staging directory is allowed to delete this file name.
inline bool updateMayRemoveStagedFile(const std::string& name) {
    for (const char* staged : kUpdateStagedFiles) if (name == staged) return true;
    return false;
}

/// Three-component version used by model-pack manifests. Same shape as the executable manifest.
inline bool offlineVersion(const std::string& text) {
    size_t start = 0;
    int parts = 0;
    for (size_t index = 0; index <= text.size() && parts < 3; ++index) {
        if (index != text.size() && text[index] != '.') continue;
        const std::string part = text.substr(start, index - start);
        if (part.empty() || part.size() > 5 || (part.size() > 1 && part[0] == '0')) return false;
        unsigned value = 0;
        for (char ch : part) {
            if (ch < '0' || ch > '9') return false;
            value = value * 10 + static_cast<unsigned>(ch - '0');
        }
        if (value > 65535) return false;
        ++parts;
        start = index + 1;
        if (index == text.size()) break;
    }
    return parts == 3 && start == text.size() + 1;
}

/// Signed description of an offline zip. It is not an executable manifest.
struct ModelPackManifest {
    /// Release version, matched to the tag by the caller.
    std::string version;
    /// Always x64. Speech bundles are not built for x86.
    std::string arch;
    /// offline-sensevoice, offline-qwen06 or offline-qwen17.
    std::string variant;
    /// Zip asset name. It must be the name reserved for variant.
    std::string file;
    /// Lowercase SHA-256 of the zip.
    std::string sha256;
    /// Lowercase SHA-256 of bundle-files.txt, recorded for operators.
    std::string fingerprint;
    /// Exact zip length. It must be below the GitHub asset limit.
    std::uint64_t size = 0;

    /// Parse the canonical eight-line LF manifest. Unknown fields and CR are rejected.
    bool parse(const std::string& bytes) {
        *this = {};
        if (bytes.empty() || bytes.back() != '\n' || bytes.find('\r') != std::string::npos) return false;
        std::string lines[8];
        size_t count = 0, start = 0;
        for (size_t index = 0; index < bytes.size(); ++index) {
            if (bytes[index] != '\n') continue;
            if (count >= 8) return false;
            lines[count++] = bytes.substr(start, index - start);
            start = index + 1;
        }
        if (count != 8 || start != bytes.size() || lines[0] != "PhoneRecord-modelpack-v1") return false;
        auto field = [](const std::string& line, const char* key, std::string& value) {
            const size_t length = std::strlen(key);
            if (line.size() <= length || line.compare(0, length, key) != 0) return false;
            value = line.substr(length);
            return !value.empty();
        };
        std::string versionLine, archLine, variantLine, fileLine, sizeLine, hashLine, fingerLine;
        if (!field(lines[1], "version=", versionLine) || !field(lines[2], "arch=", archLine) || !field(lines[3], "variant=", variantLine) ||
            !field(lines[4], "file=", fileLine) || !field(lines[5], "size=", sizeLine) || !field(lines[6], "sha256=", hashLine) ||
            !field(lines[7], "fingerprint=", fingerLine)) return false;
        if (!offlineVersion(versionLine) || archLine != "x64") return false;
        const UpdateVariant parsed = updateParseVariant(std::string("PhoneRecord-variant-v1\nvariant=") + variantLine + "\n");
        if (parsed == UpdateVariant::Standard || fileLine != offlineZipName(parsed)) return false;
        if (sizeLine.size() > 10 || hashLine.size() != 64 || fingerLine.size() != 64) return false;
        std::uint64_t length = 0;
        for (char ch : sizeLine) {
            if (ch < '0' || ch > '9') return false;
            if (length == 0 && ch == '0' && sizeLine.size() > 1) return false;
            length = length * 10 + static_cast<unsigned>(ch - '0');
        }
        auto hex = [](const std::string& value) {
            for (char ch : value) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
            return true;
        };
        if (!length || length >= kGitHubReleaseAssetMaxBytes || !hex(hashLine) || !hex(fingerLine)) return false;
        version = versionLine; arch = archLine; variant = variantLine; file = fileLine; sha256 = hashLine; fingerprint = fingerLine; size = length;
        return true;
    }
};

/// True when this variant's pinned weights fit in one asset and the finished zip is under the limit.
inline bool offlinePackagePublishable(const char* variant, std::uint64_t zipBytes) {
    const char* model = "";
    if (variant && (std::strcmp(variant, "sensevoice") == 0 || std::strcmp(variant, "offline-sensevoice") == 0)) model = "sense";
    else if (variant && (std::strcmp(variant, "qwen06") == 0 || std::strcmp(variant, "offline-qwen06") == 0)) model = "qwen";
    else if (variant && (std::strcmp(variant, "qwen17") == 0 || std::strcmp(variant, "offline-qwen17") == 0)) model = "qwen17";
    else return false;
    const std::uint64_t pinned = speechPinnedBytes(model);
    return pinned > 0 && pinned < kGitHubReleaseAssetMaxBytes && zipBytes > 0 && zipBytes < kGitHubReleaseAssetMaxBytes;
}
