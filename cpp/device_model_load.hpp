#pragma once

#include "device_model.hpp"

#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#endif
#ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif
#else
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#endif

/// Release one library handle. Null is ignored.
inline void deviceModelCloseLibrary(void* library) {
#if defined(_WIN32)
    if (library) FreeLibrary(static_cast<HMODULE>(library));
#else
    if (library) dlclose(library);
#endif
}

/// True when the file name ends with the platform plugin suffix.
inline bool deviceModelPluginName(const std::string& name) {
#if defined(_WIN32)
    const char* suffix = ".dll";
#else
    const char* suffix = ".so";
#endif
    const size_t width = std::strlen(suffix);
    if (name.size() < width) return false;
    return prEqualAscii(name.c_str() + name.size() - width, suffix);
}

/// Load one plugin file. A rejected file is recorded and does not replace a built-in id.
inline bool deviceModelLoadFile(const std::string& path) {
    deviceModels().setLibraryCloser(&deviceModelCloseLibrary);
#if defined(_WIN32)
    const int chars = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (chars <= 1) { deviceModels().noteFailure(path + ": open"); return false; }
    std::wstring wide(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], chars);
    HMODULE library = LoadLibraryExW(wide.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library) library = LoadLibraryW(wide.c_str());
    if (!library) { deviceModels().noteFailure(path + ": open"); return false; }
    auto entry = reinterpret_cast<const PrDeviceModelV1* (*)()>(GetProcAddress(library, "pr_device_model_v1"));
    if (!entry) { FreeLibrary(library); deviceModels().noteFailure(path + ": symbol"); return false; }
    const PrDeviceModelV1* model = entry();
    if (!model) { FreeLibrary(library); deviceModels().noteFailure(path + ": symbol"); return false; }
    return deviceModels().addLibrary(model, library, path);
#else
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) { deviceModels().noteFailure(path + ": open"); return false; }
    auto entry = reinterpret_cast<const PrDeviceModelV1* (*)()>(dlsym(library, "pr_device_model_v1"));
    if (!entry) { dlclose(library); deviceModels().noteFailure(path + ": symbol"); return false; }
    const PrDeviceModelV1* model = entry();
    if (!model) { dlclose(library); deviceModels().noteFailure(path + ": symbol"); return false; }
    return deviceModels().addLibrary(model, library, path);
#endif
}

/// Load up to 32 plugins from one directory. A missing directory loads nothing.
inline int deviceModelsDiscover(const std::string& directory) {
    deviceModels().ensure();
    if (directory.empty()) return 0;
    int loaded = 0, seen = 0;
#if defined(_WIN32)
    std::wstring pattern;
    const int chars = MultiByteToWideChar(CP_UTF8, 0, directory.c_str(), -1, nullptr, 0);
    if (chars <= 1) return 0;
    pattern.assign(static_cast<size_t>(chars - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, directory.c_str(), -1, &pattern[0], chars);
    const wchar_t last = pattern.empty() ? L'\0' : pattern.back();
    if (last != L'\\' && last != L'/') pattern += L'\\';
    const std::wstring prefix = pattern;
    pattern += L'*';
    WIN32_FIND_DATAW data{};
    HANDLE found = FindFirstFileW(pattern.c_str(), &data);
    if (found == INVALID_HANDLE_VALUE) return 0;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name;
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 1) continue;
        name.assign(static_cast<size_t>(bytes - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, &name[0], bytes, nullptr, nullptr);
        if (!deviceModelPluginName(name) || seen++ >= 32) continue;
        std::wstring full = prefix + data.cFileName;
        const int fullBytes = WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(fullBytes > 0 ? fullBytes - 1 : 0), '\0');
        if (fullBytes > 1) WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, &utf8[0], fullBytes, nullptr, nullptr);
        if (deviceModelLoadFile(utf8)) ++loaded;
    } while (FindNextFileW(found, &data));
    FindClose(found);
#else
    DIR* directoryHandle = opendir(directory.c_str());
    if (!directoryHandle) return 0;
    while (const dirent* entry = readdir(directoryHandle)) {
        if (seen >= 32) break;
        const std::string name = entry->d_name;
        if (!deviceModelPluginName(name)) continue;
        const std::string full = directory + (directory.back() == '/' ? "" : "/") + name;
        struct stat info {};
        if (stat(full.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
        ++seen;
        if (deviceModelLoadFile(full)) ++loaded;
    }
    closedir(directoryHandle);
#endif
    return loaded;
}

/// Discover plugins beside the executable, then apply the saved override or the automatic fallback.
inline DeviceModelSelection deviceModelsStartup(const std::string& pluginDirectory, const std::string& userOverride) {
    deviceModelsDiscover(pluginDirectory);
    return deviceModels().select(userOverride, "", "", "", "");
}
