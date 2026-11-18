// Parked code of the retired Codex bridge's world observer (CS.20, LC.35). Not compiled, not tested.
// Cut verbatim from src/mod-ollama-chat_httpclient.cpp at b81bf8c by LCB0.

// Includes: <filesystem>, <set>; #ifdef _WIN32 <AclAPI.h>, #pragma comment(lib, "advapi32.lib").
// Inside the file's anonymous namespace:
#ifdef _WIN32
struct ObserverHandle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~ObserverHandle()
    {
        if (value != INVALID_HANDLE_VALUE && value != nullptr)
            CloseHandle(value);
    }
};

// Validate permissions on the same handle used for reading, not a second path lookup.
bool ObserverOwnerOnly(HANDLE handle)
{
    ObserverHandle processToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &processToken.value))
        return false;
    DWORD bytes = 0;
    GetTokenInformation(processToken.value, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> user(bytes);
    if (bytes == 0 || !GetTokenInformation(processToken.value, TokenUser, user.data(), bytes, &bytes))
        return false;
    auto const sid = reinterpret_cast<TOKEN_USER const*>(user.data())->User.Sid;
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
                        &acl, nullptr, &descriptor) != ERROR_SUCCESS)
        return false;
    std::unique_ptr<void, decltype(&LocalFree)> security(descriptor, LocalFree);
    if (!owner || !EqualSid(owner, sid) || !acl)
        return false;
    for (DWORD index = 0; index < acl->AceCount; ++index)
    {
        void* entry = nullptr;
        if (!GetAce(acl, index, &entry))
            return false;
        auto const* ace = static_cast<ACCESS_ALLOWED_ACE const*>(entry);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || !EqualSid(const_cast<DWORD*>(&ace->SidStart), sid))
            return false;
    }
    return true;
}

HANDLE OpenObserverComponent(std::filesystem::path const& path, bool directory)
{
    return CreateFileW(path.c_str(), READ_CONTROL | (directory ? FILE_LIST_DIRECTORY : GENERIC_READ), FILE_SHARE_READ,
                       nullptr, OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
}

// The injectable opener observes actual lookups in the standalone production-helper test.
template <typename OpenFile>
bool ReadObserverToken(std::filesystem::path const& path, std::string& data, OpenFile openFile)
{
    // Open/check each component before any descendant lookup. With no write/delete sharing, each retained
    // handle prevents that ancestor from being replaced, renamed or changed into a reparse point until read ends.
    // FILE_LIST_DIRECTORY is needed: metadata-only handles do not enforce these sharing restrictions.
    std::vector<std::unique_ptr<ObserverHandle>> directories;
    auto cursor = path.root_path();
    auto pinDirectory = [&]()
    {
        auto directory = std::make_unique<ObserverHandle>();
        directory->value = openFile(cursor, true);
        BY_HANDLE_FILE_INFORMATION info{};
        if (directory->value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(directory->value, &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        directories.push_back(std::move(directory));
        return true;
    };
    if (!pinDirectory())
        return false;
    for (auto const& component : path.parent_path().relative_path())
    {
        if (component == "." || component == ".." || component.native().find(L':') != std::wstring::npos)
            return false;
        cursor /= component;
        if (!pinDirectory())
            return false;
    }
    if (!ObserverOwnerOnly(directories.back()->value))
        return false;
    cursor /= path.filename();
    ObserverHandle file{openFile(cursor, false)};
    BY_HANDLE_FILE_INFORMATION info{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1 || info.nFileSizeHigh != 0 || info.nFileSizeLow == 0 || info.nFileSizeLow > 65536 ||
        !ObserverOwnerOnly(file.value))
        return false;
    data.resize(info.nFileSizeLow);
    DWORD read = 0;
    return ReadFile(file.value, data.data(), info.nFileSizeLow, &read, nullptr) && read == info.nFileSizeLow;
}

bool ReadObserverToken(std::filesystem::path const& path, std::string& data)
{
    return ReadObserverToken(path, data, OpenObserverComponent);
}

#endif

bool OllamaBridge::ValidObserverSettings(Settings const& settings)
{
    return settings.observerEnabled && settings.provider == "codex" && ValidSettings(settings) &&
           !settings.observerTokenRef.empty() && settings.observerToken != settings.token &&
           settings.observerToken.size() == 64 &&
           std::all_of(settings.observerToken.begin(), settings.observerToken.end(),
                       [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool OllamaBridge::LoadObserverToken(Settings& settings)
{
    settings.observerToken.clear();
    if (!settings.observerEnabled || settings.provider != "codex" || !ValidSettings(settings) ||
        settings.observerTokenRef.find('\0') != std::string::npos)
        return false;
    try
    {
        std::filesystem::path const path{
            std::u8string(settings.observerTokenRef.begin(), settings.observerTokenRef.end())};
        if (!path.is_absolute() || path.filename() != "observer.json")
            return false;
        std::string data;
#ifdef _WIN32
        // A credential reference cannot initiate SMB/network or removable-drive I/O.
        auto const drive = path.root_name().native();
        if (drive.size() != 2 || drive[1] != L':' || GetDriveTypeW(path.root_path().c_str()) != DRIVE_FIXED ||
            !ReadObserverToken(path, data))
            return false;
#else
        // The reviewed native service requires Windows; no weaker file-policy fallback.
        return false;
#endif
        std::set<std::string> keys;
        bool duplicate = false;
        auto const document = nlohmann::json::parse(
            data,
            [&](int, nlohmann::json::parse_event_t event, nlohmann::json& value)
            {
                if (event == nlohmann::json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
                    duplicate = true;
                return true;
            });
        if (duplicate || !document.is_object() || document.size() != 2 || !document.contains("version") ||
            !document["version"].is_number_integer() || document["version"] != 1 || !document.contains("token") ||
            !document["token"].is_string())
            return false;
        settings.observerToken = document["token"].get<std::string>();
        if (ValidObserverSettings(settings))
            return true;
    }
    catch (std::exception const&)
    {
    }
    settings.observerToken.clear();
    return false;
}

OllamaHttpResult OllamaHttp_PostObserver(OllamaBridge::Settings const& settings, OllamaObserverOperation operation,
                                         std::string const& jsonData, int64_t remainingMs)
{
    char const* path = nullptr;
    switch (operation)
    {
        case OllamaObserverOperation::Open:
            path = "/v1/observer/open";
            break;
        case OllamaObserverOperation::Snapshot:
            path = "/v1/observer/snapshot";
            break;
    }
    if (!path || !OllamaBridge::ValidObserverSettings(settings) || remainingMs <= 0 || remainingMs > 5000)
        return {0, {}, "bridge_invalid_observer_input"};
    return PostLocalBridge(settings.endpoint, path, jsonData, {{"Authorization", "Bearer " + settings.observerToken}},
                           remainingMs);
}


// src/mod-ollama-chat_config.cpp, LoadOllamaChatConfig, after Bridge.Token:
    g_OllamaBridgeSettings.observerEnabled = sConfigMgr->GetOption<bool>("OllamaChat.Bridge.ObserverEnabled", false);
    g_OllamaBridgeSettings.observerTokenRef =
        sConfigMgr->GetOption<std::string>("OllamaChat.Bridge.ObserverTokenRef", "");
    if (!OllamaBridge::LoadObserverToken(g_OllamaBridgeSettings) && g_OllamaBridgeSettings.observerEnabled)
        LOG_ERROR("module.ollamachat", "[Ollama Chat] Invalid observer configuration; observation is disabled.");

// conf/mod_ollama_chat.conf.dist:
// # OllamaChat.Bridge.ObserverEnabled / ObserverTokenRef
// #     Description: Default-off Codex world-observer transport (Windows only, like the native service).
// #                  TokenRef is the absolute fixed-local-drive path to the service's protected observer.json,
// #                  never a bearer, subscription credential or a selection/activation grant. The
// #                  exact JSON is {"version":1,"token":"<64 lowercase hexadecimal characters>"}.
// #                  The token must differ from Bridge.Token. Both file and containing directory
// #                  must be owned by and accessible only to this Windows user, without reparse
// #                  paths or file hardlinks. The native service independently checks its copy.
// #                  Missing, malformed, unsafe or equal credentials disable observation. No file
// #                  is created, changed or provisioned. Other providers and generation stay unchanged.
// #                  Config load/reload resolves the bounded file into the existing owned settings
// #                  snapshot. Workers never read config or credentials from disk. Token rotation
// #                  requires owned stop/drain/restart of the native service and world owner; reload
// #                  alone grants no readiness or permission to reuse an epoch or queued observation.
// #                  Only /v1/observer/open and /v1/observer/snapshot are allowed, with a separate
// #                  observer bearer and at most 5000 ms total transport time, without retry/redirect.
// #                  When enabled, selected native X-d runs one dedicated bounded observer worker.
// #                  It sends fresh world snapshots; HTTP stays off the world thread. A challenge
// #                  response alone grants no readiness or game authority.
// #     Default:     0 / empty (no observation)
// OllamaChat.Bridge.ObserverEnabled = 0
// OllamaChat.Bridge.ObserverTokenRef =
