#pragma once
#ifdef _WIN64

/// Official Windows CLI distribution, separate from saved account/model profiles.
struct AiCliTool {
    /// Stable adapter identity, product caption and executable command.
    const char* id;
    const wchar_t* name;
    const wchar_t* command;
    /// Official installation documentation; the embedded worker owns the installer allowlist.
    const wchar_t* documentation;
};

/// Curated tools with official native Windows installation and noninteractive interfaces.
static const AiCliTool kAiCliTools[] = {
    {"claude",L"Claude Code",L"claude.exe",L"https://code.claude.com/docs/en/setup"},
    {"codex",L"ChatGPT / Codex CLI",L"codex.exe",L"https://learn.chatgpt.com/docs/codex/cli"},
    {"cursor",L"Cursor Agent",L"agent.ps1",L"https://cursor.com/docs/cli/installation"},
    {"antigravity",L"Antigravity",L"agy.exe",L"https://antigravity.google/docs/cli/install"},
    {"codebuddy",L"CodeBuddy",L"codebuddy.exe",L"https://www.codebuddy.cn/docs/cli/installation"},
    {"qwen",L"Qwen Code",L"qwen.cmd",L"https://qwenlm.github.io/qwen-code-docs/en/users/quickstart/"},
    {"kimi",L"Kimi Code",L"kimi.exe",L"https://www.kimi.com/code/docs/en/kimi-code-cli/guides/getting-started.html"},
    {"qoder",L"Qoder",L"qoder.exe",L"https://docs.qoder.com/cli/installation"},
    {"trae",L"TRAE CLI",L"traecli.exe",L"https://docs.trae.cn/cli_get-started-with-trae-cli-2"}
};

/// Resolve one known provider without making a model or network request.
static const AiCliTool* aiCliTool(const string& id) {
    for(const auto& tool:kAiCliTools)if(id==tool.id)return &tool;
    return nullptr;
}

/// Detect known user installations, including commands whose PATH was added after app startup.
static wstring aiCliPath(const string& type,const string& configured="") {
    namespace fs=std::filesystem;
    const auto* tool=aiCliTool(type);
    const auto ordinary=[](const fs::path& path) {
        const DWORD attributes=GetFileAttributesW(path.c_str());
        std::error_code error;return attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_DIRECTORY) && std::filesystem::is_regular_file(path,error);
    };
    if(!configured.empty()) {
        const fs::path path=utf8ToWide(configured);
        if(path.is_absolute())return ordinary(path)?path.native():L"";
        wchar_t found[32768]{};if(SearchPathW(NULL,path.c_str(),L".exe",32768,found,NULL) && ordinary(found))return found;
        return L"";
    }
    if(!tool)return L"";
    wchar_t found[32768]{};
    if(SearchPathW(NULL,tool->command,NULL,32768,found,NULL) && ordinary(found))return found;
    wchar_t local[32768]{},user[32768]{};GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);GetEnvironmentVariableW(L"USERPROFILE",user,32768);
    const std::vector<fs::path> folders={fs::path(user)/L".local/bin",fs::path(local)/L"codebuddy/bin",fs::path(local)/L"cursor-agent",fs::path(local)/L"agy/bin",fs::path(local)/L"Programs/OpenAI/Codex/bin",fs::path(local)/L"qwen-code/bin",fs::path(user)/L".qwen/bin",fs::path(user)/L".kimi-code/bin",fs::path(local)/L"kimi-code/bin",fs::path(user)/L".qoder/bin",fs::path(local)/L"qoder/bin",fs::path(user)/L".trae/bin",fs::path(local)/L"trae-cli"};
    for(const auto& folder:folders)if(ordinary(folder/tool->command))return (folder/tool->command).native();
    return L"";
}

/// Last management observation is separate from analysis results and never implies model entitlement.
struct AiCliObservation {
    /// Detected command, normalized model IDs and their documented provenance.
    string path,models,source,step,version;
    uint64_t revision=0;
    /// Cached lists are explicitly stale until the user refreshes them.
    bool cached=false;
};
#endif
