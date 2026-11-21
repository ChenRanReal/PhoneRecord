#include "../cpp/offline_rules.hpp"

#include <iostream>
#include <string>

/// Count a failed expectation without aborting the rest of the table.
static int g_failures = 0;

/// Record one expectation. The label is a stable technical id.
static void expect(bool value, const char* label) {
    if (value) return;
    ++g_failures;
    std::cerr << "FAIL " << label << "\n";
}

/// Build a complete, hash-valid fact set for one model.
static std::map<std::string, SpeechFileFact> validFacts(const char* model) {
    std::map<std::string, SpeechFileFact> files;
    for (const SpeechPin& pin : kSpeechPins) {
        if (std::strcmp(pin.model, model) != 0) continue;
        files[pin.relative] = SpeechFileFact{true, pin.size, pin.sha256};
    }
    for (const SpeechRuntimeFile& file : kSpeechRuntimeFiles) {
        if (std::strcmp(file.model, model) != 0) continue;
        files[file.relative] = SpeechFileFact{true, 1, ""};
    }
    return files;
}

/// Rules for bundled models, update channels and the GitHub asset ceiling.
int main() {
    SpeechBundleClaim empty;
    const auto missing = speechAssessBundle(false, empty, {});
    expect(missing.sense == SpeechSupply::Download && missing.qwen == SpeechSupply::Download && missing.qwen17 == SpeechSupply::Download, "absent directory downloads");
    expect(!speechForbidsDownload(false, false, empty, missing, {}, "sense"), "absent directory may download");

    SpeechBundleClaim senseClaim;
    senseClaim.sense = true;
    const auto senseFiles = validFacts("sense");
    const auto bundled = speechAssessBundle(true, senseClaim, senseFiles);
    expect(bundled.supply("sense") == SpeechSupply::Bundled, "valid sense bundle");
    expect(bundled.supply("qwen") == SpeechSupply::Download && bundled.supply("qwen17") == SpeechSupply::Download, "other models stay on demand");
    expect(speechForbidsDownload(true, true, senseClaim, bundled, senseFiles, "sense"), "bundled sense is not downloaded");
    expect(!speechForbidsDownload(true, true, senseClaim, bundled, senseFiles, "qwen"), "unbundled qwen may download");
    expect(std::string(speechBundleClaimText(senseClaim)).find("sense=1\nqwen=0\nqwen17=0\n") != std::string::npos, "claim text roundtrip");
    SpeechBundleClaim parsed;
    expect(speechParseBundleClaim(speechBundleClaimText(senseClaim), parsed) && parsed.sense && !parsed.qwen, "claim parser");

    auto corrupt = senseFiles;
    corrupt["models/model.int8.onnx"].sha256 = std::string(64, '0');
    const auto blocked = speechAssessBundle(true, senseClaim, corrupt);
    expect(blocked.supply("sense") == SpeechSupply::Blocked, "bad digest blocks");
    expect(speechForbidsDownload(true, true, senseClaim, blocked, corrupt, "sense"), "bad digest does not fall through to download");
    expect(speechBundleSized("sense", true, corrupt), "size can still match when the digest does not");
    expect(speechBundleSized("sense", true, senseFiles), "valid sizes report healthy");

    auto partial = senseFiles;
    partial.erase("models/tokens.txt");
    const auto claimedPartial = speechAssessBundle(true, senseClaim, partial);
    expect(claimedPartial.supply("sense") == SpeechSupply::Blocked, "claimed partial bundle blocks");
    expect(speechForbidsDownload(true, false, senseClaim, claimedPartial, partial, "sense"), "unverified claim forbids download");

    SpeechBundleClaim none;
    const auto unclaimedPartial = speechAssessBundle(true, none, partial);
    expect(unclaimedPartial.supply("sense") == SpeechSupply::Blocked, "stray model bytes are not a download source");
    const auto unclaimedValid = speechAssessBundle(true, none, senseFiles);
    expect(unclaimedValid.supply("sense") == SpeechSupply::Bundled, "matching bytes are used without a claim");

    const auto qwenFiles = validFacts("qwen");
    const auto qwen = speechAssessBundle(true, none, qwenFiles);
    expect(qwen.supply("qwen") == SpeechSupply::Bundled && qwen.supply("qwen17") == SpeechSupply::Download, "qwen sizes are independent");
    auto qwen17Files = validFacts("qwen17");
    qwen17Files["qwen/python/python.exe"] = SpeechFileFact{true, 1, ""};
    const auto both = speechAssessBundle(true, none, qwen17Files);
    expect(both.supply("qwen17") == SpeechSupply::Bundled && both.supply("qwen") == SpeechSupply::Download, "1.7B does not satisfy 0.6B");

    expect(updateParseVariant("") == UpdateVariant::Standard, "missing variant file is standard");
    expect(updateParseVariant("variant=offline-sensevoice\n") == UpdateVariant::Standard, "variant file without magic is standard");
    expect(updateParseVariant(updateVariantText(UpdateVariant::OfflineSenseVoice)) == UpdateVariant::OfflineSenseVoice, "sensevoice variant");
    expect(updateParseVariant(updateVariantText(UpdateVariant::OfflineQwen06)) == UpdateVariant::OfflineQwen06, "qwen06 variant");
    expect(updateParseVariant(updateVariantText(UpdateVariant::Standard)) == UpdateVariant::Standard, "explicit standard");
    expect(updateParseVariant(updateVariantText(UpdateVariant::OfflineSenseVoice) + "extra\n") == UpdateVariant::Standard, "trailing variant line rejected");

    const ModelPackAssets standard = updateDecideModelPack(UpdateVariant::Standard, false);
    expect(!standard.download && standard.zip[0] == '\0', "standard channel has no model pack");
    const ModelPackAssets healthy = updateDecideModelPack(UpdateVariant::OfflineSenseVoice, true);
    expect(!healthy.download && std::string(healthy.zip) == "PhoneRecord-x64-offline-sensevoice.zip", "healthy offline install skips the pack");
    expect(std::string(healthy.manifest) == "update-manifest-x64-offline-sensevoice.txt" && std::string(healthy.signature) == "update-manifest-x64-offline-sensevoice.sig", "sensevoice pack names");
    const ModelPackAssets repair = updateDecideModelPack(UpdateVariant::OfflineQwen06, false);
    expect(repair.download && std::string(repair.zip) == "PhoneRecord-x64-offline-qwen06.zip", "unhealthy qwen06 install selects its pack");
    const ModelPackAssets local17 = updateDecideModelPack(UpdateVariant::OfflineQwen17, true);
    expect(!local17.download && std::string(local17.zip) == "PhoneRecord-x64-offline-qwen17.zip", "healthy 1.7B install is not forced to download");

    expect(!updateMayRemoveStagedFile("bundled-speech") && !updateMayRemoveStagedFile("install-variant.txt"), "install siblings are not staging files");
    expect(updateMayRemoveStagedFile("PhoneRecord.exe") && updateMayRemoveStagedFile("model-pack.zip") && updateMayRemoveStagedFile("update-manifest.sig"), "staging names stay removable");

    const std::string pack = "PhoneRecord-modelpack-v1\nversion=9.0.0\narch=x64\nvariant=offline-sensevoice\nfile=PhoneRecord-x64-offline-sensevoice.zip\nsize=3\nsha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\nfingerprint=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";
    ModelPackManifest manifest;
    expect(manifest.parse(pack) && manifest.size == 3 && manifest.file == "PhoneRecord-x64-offline-sensevoice.zip", "model pack manifest");
    expect(!manifest.parse(pack + "extra\n") && !manifest.parse(std::string("\r\n") + pack), "model pack rejects extra lines and CR");
    std::string wrongFile = pack;
    const std::string original = "file=PhoneRecord-x64-offline-sensevoice.zip";
    wrongFile.replace(wrongFile.find(original), original.size(), "file=PhoneRecord-x64-offline-qwen06.zip");
    expect(!ModelPackManifest{}.parse(wrongFile), "pack file name must match the variant");
    std::string huge = pack;
    huge.replace(huge.find("size=3"), 6, "size=2147483648");
    expect(!ModelPackManifest{}.parse(huge), "pack at the asset ceiling is rejected");
    expect(!ModelPackManifest{}.parse("PhoneRecord-update-v1\nversion=9.0.0\narch=x64\nfile=PhoneRecord.exe\nsize=3\nsha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"), "executable manifest is not a model pack");

    expect(speechPinnedBytes("qwen17") >= kGitHubReleaseAssetMaxBytes, "1.7B weights do not fit one asset");
    expect(speechPinnedBytes("sense") < kGitHubReleaseAssetMaxBytes && speechPinnedBytes("qwen") < kGitHubReleaseAssetMaxBytes, "sense and 0.6B weights fit one asset");
    expect(!offlinePackagePublishable("qwen17", 1024), "1.7B package is never a release asset");
    expect(offlinePackagePublishable("sensevoice", 1024) && offlinePackagePublishable("qwen06", speechPinnedBytes("qwen")), "smaller packages publish when the zip fits");
    expect(!offlinePackagePublishable("sensevoice", kGitHubReleaseAssetMaxBytes) && !offlinePackagePublishable("qwen06", 0), "empty or ceiling zips are not uploaded");
    expect(offlineVersion("2.7.7") && offlineVersion("0.0.0") && !offlineVersion("02.7.7") && !offlineVersion("2.7") && !offlineVersion("2.7.7.1"), "model pack versions");

    if (g_failures) {
        std::cerr << g_failures << " failed\n";
        return 1;
    }
    std::cout << "OFFLINE_RULES_OK\n";
    return 0;
}
