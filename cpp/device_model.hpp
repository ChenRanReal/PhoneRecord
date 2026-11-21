#pragma once

#include "device_model_util.hpp"
#include "models/fanvil_x3s.hpp"
#include "models/sample_template.hpp"

#include <mutex>
#include <regex>
#include <utility>
#include <vector>

/// One loaded component. A null library handle means the component is compiled into the process.
struct DeviceModelEntry {
    /// Model pointer owned by the built-in image or by the loaded library.
    const PrDeviceModelV1* model = nullptr;
    /// OS library handle. Null for a built-in component.
    void* library = nullptr;
    /// Plugin path, or empty for a built-in component.
    std::string path;
};

/// Result of the last selection. Strings are copies, so the caller can log them after the lock is released.
struct DeviceModelSelection {
    /// Selected component id.
    std::string id;
    /// Stable reason: user_override, unknown_override, a match reason, or fallback.
    std::string reason;
    /// Winning score. Zero when the legacy fallback was used.
    int score = 0;
    /// Number of external libraries currently loaded.
    int loaded = 0;
};

/// Process-wide device-model registry. Built-ins and plugins share one match path.
class DeviceModelRegistry {
public:
    /// Register both shipped components and select the legacy fallback.
    void ensure() {
        std::lock_guard<std::mutex> guard(mutex_);
        ensureLocked();
    }

    /// Add a component compiled into this process. A duplicate id is ignored.
    bool addBuiltIn(const PrDeviceModelV1* model) {
        std::lock_guard<std::mutex> guard(mutex_);
        ensureLocked();
        return addLocked(model, nullptr, "");
    }

    /// Remember a library that has already passed ABI checks. The registry closes it on reset.
    bool addLibrary(const PrDeviceModelV1* model, void* library, const std::string& path) {
        std::lock_guard<std::mutex> guard(mutex_);
        ensureLocked();
        if (!addLocked(model, library, path)) return false;
        return true;
    }

    /// Record a plugin that was seen but not installed.
    void noteFailure(const std::string& failure) {
        std::lock_guard<std::mutex> guard(mutex_);
        failures_.push_back(failure);
    }

    /// Failures from the current process. reset clears them.
    std::vector<std::string> failures() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return failures_;
    }

    /// Choose a component. A non-empty override pins that id, or the fallback when the id is absent.
    DeviceModelSelection select(const std::string& userOverride, const std::string& modelText, const std::string& httpBody,
                                const std::string& httpServer, const std::string& mac) {
        std::lock_guard<std::mutex> guard(mutex_);
        ensureLocked();
        return selectLocked(userOverride, modelText, httpBody, httpServer, mac);
    }

    /// Re-run automatic matching with a newly read phone page. A weak page cannot undo a stronger match.
    DeviceModelSelection observeHttpBody(const std::string& httpBody) {
        std::lock_guard<std::mutex> guard(mutex_);
        ensureLocked();
        if (pinned_) return snapshotLocked();
        PrMatchInput input{};
        input.size = sizeof(input);
        input.user_override = userOverride_.c_str();
        input.model_text = modelText_.c_str();
        input.http_body = httpBody.c_str();
        input.http_server = httpServer_.c_str();
        input.mac = mac_.c_str();
        const PrDeviceModelV1* winner = nullptr;
        int bestScore = 0, bestPriority = 0;
        const char* bestReason = "none";
        for (const auto& entry : entries_) {
            PrMatchResult match{};
            if (!entry.model->match(&input, &match) || match.score < PR_DEVICE_MODEL_AUTO_MIN) continue;
            const int priority = entry.model->profile->priority;
            if (!winner || match.score > bestScore || (match.score == bestScore && priority > bestPriority)) {
                winner = entry.model;
                bestScore = match.score;
                bestPriority = priority;
                bestReason = match.reason ? match.reason : "match";
            }
        }
        if (!winner) return snapshotLocked();
        httpBody_ = httpBody;
        active_ = winner;
        reason_ = bestReason;
        score_ = bestScore;
        return snapshotLocked();
    }

    /// Active component. The pointed-to profile remains valid until reset.
    const PrDeviceModelV1* active() {
        std::lock_guard<std::mutex> guard(mutex_);
        ensureLocked();
        return active_;
    }

    /// Copy of the last selection.
    DeviceModelSelection selection() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return snapshotLocked();
    }

    /// Ids in registration order.
    std::vector<std::string> ids() const {
        std::lock_guard<std::mutex> guard(mutex_);
        std::vector<std::string> result;
        for (const auto& entry : entries_) result.push_back(entry.model->profile->id);
        return result;
    }

    /// Drop libraries and return to an empty registry. Tests use this between cases.
    void reset() {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto& entry : entries_) if (entry.library) libraryCloser_(entry.library);
        entries_.clear();
        failures_.clear();
        active_ = nullptr;
        ready_ = false;
        pinned_ = false;
        userOverride_.clear();
        modelText_.clear();
        httpBody_.clear();
        httpServer_.clear();
        mac_.clear();
        reason_ = "fallback";
        score_ = 0;
    }

    /// Install the platform library closer. The product and the test set this before loading files.
    void setLibraryCloser(void (*closer)(void*)) {
        std::lock_guard<std::mutex> guard(mutex_);
        libraryCloser_ = closer;
    }

private:
    /// Register the shipped components once.
    void ensureLocked() {
        if (ready_) return;
        addLocked(fanvilX3sModel(), nullptr, "");
        addLocked(sampleTemplateModel(), nullptr, "");
        ready_ = true;
        selectLocked("", "", "", "", "");
    }

    /// Validate and store one component. The caller holds mutex_.
    bool addLocked(const PrDeviceModelV1* model, void* library, const std::string& path) {
        std::string error;
        if (!acceptable(model, error)) {
            if (library && libraryCloser_) libraryCloser_(library);
            if (!path.empty()) failures_.push_back(path + ": " + error);
            return false;
        }
        for (const auto& entry : entries_) {
            if (std::strcmp(entry.model->profile->id, model->profile->id) == 0) {
                if (library && libraryCloser_) libraryCloser_(library);
                if (!path.empty()) failures_.push_back(path + ": duplicate");
                return false;
            }
        }
        entries_.push_back({model, library, path});
        return true;
    }

    /// Reject a component that the host would have to guess about.
    static bool acceptable(const PrDeviceModelV1* model, std::string& error) {
        if (!model || model->abi != PR_DEVICE_MODEL_ABI || model->size != sizeof(PrDeviceModelV1) ||
            !model->profile || model->profile->size != sizeof(PrDeviceProfile) || !model->match || !model->classify_event) {
            error = "abi"; return false;
        }
        const PrDeviceProfile& profile = *model->profile;
        if (!deviceModelIdAllowed(prText(profile.id)) || !profile.display_name || !profile.auth_scheme) { error = "id"; return false; }
        const char* const required[] = {
            profile.event_path, profile.event_query_key, profile.event_query_alias, profile.record_page, profile.action_page,
            profile.start_path, profile.stop_path, profile.nonce_path, profile.contacts_path, profile.calllog_path,
            profile.cookie_name, profile.login_language, profile.return_page, profile.protected_marker, profile.record_status_regex,
            profile.field_enable, profile.field_type, profile.field_codec, profile.field_address, profile.field_port,
            profile.field_checkbox_manager, profile.field_default_record, profile.value_enabled, profile.value_network,
            profile.value_codec_pcma, profile.value_codec_pcmu, profile.field_established, profile.field_terminated,
            profile.field_setup_completed, profile.field_ip_change, profile.action_body_prefix, profile.ctrl_start_request,
            profile.ctrl_stop_request, profile.ctrl_start_response, profile.ctrl_stop_response, profile.ctrl_codec_pcma,
            profile.ctrl_codec_pcmu, profile.ctrl_field_cmd, profile.ctrl_field_id, profile.ctrl_field_codec,
            profile.ctrl_field_local, profile.ctrl_field_remote, profile.ctrl_field_direction, profile.ctrl_field_name,
            profile.directory_contact_query, profile.directory_history_query, profile.directory_name_field
        };
        for (const char* field : required) if (!field) { error = "profile"; return false; }
        if (profile.record_status_regex[0]) {
            try { std::regex compiled(profile.record_status_regex); }
            catch (const std::regex_error&) { error = "profile"; return false; }
        }
        return true;
    }

    /// Select under mutex_. A positive automatic score must reach the published minimum.
    DeviceModelSelection selectLocked(const std::string& userOverride, const std::string& modelText, const std::string& httpBody,
                                      const std::string& httpServer, const std::string& mac) {
        userOverride_ = userOverride;
        modelText_ = modelText;
        httpBody_ = httpBody;
        httpServer_ = httpServer;
        mac_ = mac;
        pinned_ = !userOverride.empty();
        if (entries_.empty()) return snapshotLocked();
        PrMatchInput input{};
        input.size = sizeof(input);
        input.user_override = userOverride.c_str();
        input.model_text = modelText.c_str();
        input.http_body = httpBody.c_str();
        input.http_server = httpServer.c_str();
        input.mac = mac.c_str();
        const PrDeviceModelV1* fallback = nullptr;
        for (const auto& entry : entries_) if (entry.model->profile->fallback && !fallback) fallback = entry.model;
        if (!userOverride.empty()) {
            for (const auto& entry : entries_) {
                if (!prEqualAscii(entry.model->profile->id, userOverride.c_str())) continue;
                PrMatchResult match{};
                entry.model->match(&input, &match);
                active_ = entry.model;
                reason_ = "user_override";
                score_ = match.score > 0 ? match.score : 1000;
                return snapshotLocked();
            }
            active_ = fallback ? fallback : entries_.front().model;
            reason_ = "unknown_override";
            score_ = 0;
            return snapshotLocked();
        }
        const PrDeviceModelV1* winner = nullptr;
        int bestScore = 0, bestPriority = 0;
        const char* bestReason = "none";
        for (const auto& entry : entries_) {
            PrMatchResult match{};
            if (!entry.model->match(&input, &match) || match.score < PR_DEVICE_MODEL_AUTO_MIN) continue;
            const int priority = entry.model->profile->priority;
            if (!winner || match.score > bestScore || (match.score == bestScore && priority > bestPriority)) {
                winner = entry.model;
                bestScore = match.score;
                bestPriority = priority;
                bestReason = match.reason ? match.reason : "match";
            }
        }
        if (winner) {
            active_ = winner;
            reason_ = bestReason;
            score_ = bestScore;
        } else {
            active_ = fallback ? fallback : entries_.front().model;
            reason_ = "fallback";
            score_ = 0;
        }
        return snapshotLocked();
    }

    /// Copy the current selection. The caller holds mutex_.
    DeviceModelSelection snapshotLocked() const {
        DeviceModelSelection result;
        result.id = active_ && active_->profile ? active_->profile->id : "";
        result.reason = reason_;
        result.score = score_;
        for (const auto& entry : entries_) if (entry.library) ++result.loaded;
        return result;
    }

    /// Registration lock.
    mutable std::mutex mutex_;
    /// Registered components in insertion order.
    std::vector<DeviceModelEntry> entries_;
    /// Plugins that did not become active components.
    std::vector<std::string> failures_;
    /// Selected component. Null only before ensure.
    const PrDeviceModelV1* active_ = nullptr;
    /// True after the shipped components have been inserted.
    bool ready_ = false;
    /// True when config.ini named a device_model, including an unknown id.
    bool pinned_ = false;
    /// Evidence from the last select call.
    std::string userOverride_, modelText_, httpBody_, httpServer_, mac_, reason_ = "fallback";
    /// Score from the last select call.
    int score_ = 0;
    /// Platform-specific release for a library handle. Null skips release.
    void (*libraryCloser_)(void*) = nullptr;
};

/// The one registry for this process.
inline DeviceModelRegistry& deviceModels() {
    static DeviceModelRegistry registry;
    return registry;
}

/// Active profile. The reference stays valid until the registry is reset.
inline const PrDeviceProfile& activeDeviceProfile() { return *deviceModels().active()->profile; }

/// Classify a token with the active component.
inline int activeClassifyEvent(const char* token) {
    const PrDeviceModelV1* model = deviceModels().active();
    return model->classify_event ? model->classify_event(token) : PR_EVENT_UNKNOWN;
}

/// True when the active component treats both tokens as one known event.
inline bool activeEventSame(const char* expected, const char* actual) {
    return prEventSame(activeDeviceProfile(), expected, actual);
}
