#include "fh2mod.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct HeightVector {
    std::uint32_t* begin;
    std::uint32_t* end;
    std::uint32_t* capacity;
};

struct PatternByte {
    std::uint8_t value;
    bool wildcard;
};

using BuildOptionsFn = HeightVector*(__fastcall*)(HeightVector*, std::uintptr_t, const std::uint32_t*);
using InsertHeightFn = char*(__fastcall*)(const void**, std::uint8_t*, const std::uint32_t*);

constexpr std::uint32_t kDefaultHeight = 540;
constexpr std::uint32_t kMinHeight = 360;
constexpr std::uint32_t kMaxHeight = 4320;
constexpr wchar_t kModFolder[] = L"RenderHeight";
constexpr wchar_t kConfigSection[] = L"RenderHeight";
constexpr wchar_t kStateSection[] = L"State";
constexpr wchar_t kAzimuthTitleSection[] = L"title-1a96545b";

const FH2ModApi* g_api = nullptr;
FH2Mod* g_mod = nullptr;
std::wstring g_config_path;
std::wstring g_azimuth_ini;
std::atomic<bool> g_enabled{false};
std::atomic<std::uint32_t> g_height{kDefaultHeight};
std::atomic<std::uint32_t> g_restore_height{0};
std::atomic<std::uint32_t> g_last_notice{0};
std::atomic<bool> g_hook_ready{false};
std::atomic<bool> g_hook_active{false};
BuildOptionsFn g_build_options = nullptr;
InsertHeightFn g_insert_height = nullptr;
FH2Hook* g_hook = nullptr;

void log_line(const char* text) {
    if (g_api && g_mod && g_api->log)
        g_api->log(g_mod, text);
}

std::uint32_t normalize_height(std::uint32_t value) {
    value = std::clamp(value, kMinHeight, kMaxHeight);
    return (value + 1u) & ~1u;
}

std::uint32_t read_uint(const wchar_t* section, const wchar_t* key, std::uint32_t fallback,
                        const std::wstring& path) {
    return static_cast<std::uint32_t>(GetPrivateProfileIntW(section, key, static_cast<int>(fallback), path.c_str()));
}

bool write_uint(const wchar_t* section, const wchar_t* key, std::uint32_t value,
                const std::wstring& path) {
    const std::wstring text = std::to_wstring(value);
    return WritePrivateProfileStringW(section, key, text.c_str(), path.c_str()) != 0;
}

std::wstring join_path(const wchar_t* left, const wchar_t* right) {
    std::wstring result(left ? left : L"");
    if (!result.empty() && result.back() != L'\\' && result.back() != L'/')
        result.push_back(L'\\');
    result += right;
    return result;
}

void save_mod_value(const wchar_t* key, std::uint32_t value) {
    if (!write_uint(kConfigSection, key, value, g_config_path))
        log_line("could not save a setting in RenderHeight/settings.ini");
}

std::uint32_t current_azimuth_height() {
    return read_uint(kAzimuthTitleSection, L"resolution", 720, g_azimuth_ini);
}

void apply_custom_height() {
    if (g_azimuth_ini.empty())
        return;

    const std::uint32_t height = normalize_height(g_height.load(std::memory_order_relaxed));
    g_height.store(height, std::memory_order_relaxed);
    const std::uint32_t current = current_azimuth_height();
    std::uint32_t restore = g_restore_height.load(std::memory_order_relaxed);
    if (restore == 0 && current != height) {
        restore = current;
        if (!write_uint(kStateSection, L"original_resolution", restore, g_config_path)) {
            log_line("could not save the rollback resolution; Azimuth setting was not changed");
            return;
        }
        g_restore_height.store(restore, std::memory_order_relaxed);
    }
    if (current != height) {
        if (!write_uint(kAzimuthTitleSection, L"resolution", height, g_azimuth_ini)) {
            log_line("could not write the custom height to data/azimuth.ini");
            return;
        }
        char line[128]{};
        std::snprintf(line, sizeof line, "set Azimuth title resolution to %u; previous value %u", height, restore);
        log_line(line);
    }
}

void restore_azimuth_height() {
    if (g_azimuth_ini.empty())
        return;

    const std::uint32_t restore = g_restore_height.load(std::memory_order_relaxed);
    if (restore == 0)
        return;

    const std::uint32_t current = current_azimuth_height();
    const std::uint32_t active = normalize_height(g_height.load(std::memory_order_relaxed));
    if (current == active) {
        if (!write_uint(kAzimuthTitleSection, L"resolution", restore, g_azimuth_ini)) {
            log_line("could not restore the previous Azimuth title resolution; rollback value kept");
            return;
        }
        log_line("restored the previous Azimuth title resolution");
    } else {
        log_line("left Azimuth title resolution unchanged because it no longer matches this mod's value");
    }
    g_restore_height.store(0, std::memory_order_relaxed);
    if (!write_uint(kStateSection, L"original_resolution", 0, g_config_path)) {
        g_restore_height.store(restore, std::memory_order_relaxed);
        log_line("could not clear the saved rollback value in RenderHeight/settings.ini");
    }
}

void fail_enabled_state(const char* reason) {
    const bool was_enabled = g_enabled.exchange(false, std::memory_order_relaxed);
    save_mod_value(L"enabled", 0);
    if (g_api && g_api->api_version >= 4 && g_api->set_setting)
        g_api->set_setting(g_mod, "enabled", 0);
    if (was_enabled || g_restore_height.load(std::memory_order_relaxed) != 0)
        restore_azimuth_height();
    log_line(reason);
}

void setting_changed(void*, const char*, int value) {
    if (value != 0) {
        if (!g_hook_ready.load(std::memory_order_relaxed) || !g_hook || !g_api->enable_hooks) {
            fail_enabled_state("cannot enable: verified Azimuth hook is not ready");
            return;
        }
        if (!g_api->enable_hooks(g_mod, g_hook)) {
            fail_enabled_state("platform could not enable the Azimuth hook");
            return;
        }
        g_hook_active.store(true, std::memory_order_relaxed);
        g_enabled.store(true, std::memory_order_relaxed);
        save_mod_value(L"enabled", 1);
        apply_custom_height();
        return;
    }

    g_enabled.store(false, std::memory_order_relaxed);
    save_mod_value(L"enabled", 0);
    if (g_hook_active.exchange(false, std::memory_order_relaxed) && g_api->disable_hooks &&
        !g_api->disable_hooks(g_mod, g_hook))
        log_line("platform could not disable the Azimuth hook; detour remains a no-op while disabled");
    restore_azimuth_height();
}

void slider_changed(void*, const char*, float value) {
    const auto height = normalize_height(static_cast<std::uint32_t>(value + 0.5f));
    g_height.store(height, std::memory_order_relaxed);
    save_mod_value(L"height", height);
    if (g_enabled.load(std::memory_order_relaxed) && g_hook_active.load(std::memory_order_relaxed))
        apply_custom_height();
}

bool readable_range(const void* address, std::size_t size) {
    if (!address || size == 0)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof info) != sizeof info || info.State != MEM_COMMIT)
        return false;
    if ((info.Protect & PAGE_GUARD) || (info.Protect & PAGE_NOACCESS))
        return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start <= region_end && size <= region_end - start;
}

std::vector<PatternByte> parse_pattern(std::string_view text) {
    std::vector<PatternByte> pattern;
    std::istringstream input{std::string(text)};
    std::string token;
    while (input >> token) {
        if (token == "?" || token == "??") {
            pattern.push_back({0, true});
        } else {
            pattern.push_back({static_cast<std::uint8_t>(std::stoul(token, nullptr, 16)), false});
        }
    }
    return pattern;
}

bool matches_at(const std::uint8_t* bytes, const std::vector<PatternByte>& pattern) {
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (!pattern[i].wildcard && bytes[i] != pattern[i].value)
            return false;
    }
    return true;
}

void* find_unique_executable_match(HMODULE module, std::string_view signature, std::size_t& count) {
    count = 0;
    if (!module)
        return nullptr;
    const auto pattern = parse_pattern(signature);
    const auto base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return nullptr;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;
    const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    void* match = nullptr;

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if ((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;
        const std::size_t rva = section[i].VirtualAddress;
        const std::size_t length = section[i].Misc.VirtualSize ? section[i].Misc.VirtualSize : section[i].SizeOfRawData;
        if (rva >= image_size || length > image_size - rva || length < pattern.size())
            continue;
        auto* start = base + rva;
        for (std::size_t offset = 0; offset <= length - pattern.size(); ++offset) {
            if (matches_at(start + offset, pattern)) {
                ++count;
                match = start + offset;
                if (count > 1)
                    return nullptr;
            }
        }
    }
    return count == 1 ? match : nullptr;
}

bool valid_vector(const HeightVector* vector) {
    if (!readable_range(vector, sizeof *vector))
        return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(vector->begin);
    const auto end = reinterpret_cast<std::uintptr_t>(vector->end);
    const auto capacity = reinterpret_cast<std::uintptr_t>(vector->capacity);
    if (begin == 0 || end < begin || capacity < end || ((end - begin) % sizeof(std::uint32_t)) != 0)
        return false;
    const std::size_t count = (end - begin) / sizeof(std::uint32_t);
    const std::size_t capacity_count = (capacity - begin) / sizeof(std::uint32_t);
    return count > 0 && count <= 64 && capacity_count >= count && capacity_count <= 64 &&
           ((capacity - begin) % sizeof(std::uint32_t)) == 0 &&
           readable_range(vector->begin, capacity - begin);
}

void note_once(std::uint32_t height, const char* message) {
    std::uint32_t previous = g_last_notice.exchange(height, std::memory_order_relaxed);
    if (previous != height)
        log_line(message);
}

HeightVector* __fastcall build_options_detour(HeightVector* output, std::uintptr_t display_context,
                                               const std::uint32_t* native_height) {
    HeightVector* result = g_build_options(output, display_context, native_height);
    if (!result || !g_enabled.load(std::memory_order_relaxed))
        return result;
    if (!valid_vector(result)) {
        note_once(0xFFFFFFFFu, "resolution options vector failed validation; left unchanged");
        return result;
    }

    const std::uint32_t target = normalize_height(g_height.load(std::memory_order_relaxed));
    const std::size_t count = (reinterpret_cast<std::uintptr_t>(result->end) -
                               reinterpret_cast<std::uintptr_t>(result->begin)) / sizeof(std::uint32_t);
    if (count == 0 || target > result->end[-1]) {
        note_once(target, "custom height exceeds Azimuth's display-derived ceiling; no option added");
        return result;
    }

    auto* position = std::lower_bound(result->begin, result->end, target);
    if (position != result->end && *position == target)
        return result;
    if (!g_insert_height) {
        note_once(target, "vector insertion helper unavailable; no option added");
        return result;
    }

    g_insert_height(reinterpret_cast<const void**>(result), reinterpret_cast<std::uint8_t*>(position), &target);
    if (!valid_vector(result)) {
        note_once(target, "Azimuth vector helper returned an invalid vector; review this build");
        return result;
    }
    const std::size_t new_count = (reinterpret_cast<std::uintptr_t>(result->end) -
                                   reinterpret_cast<std::uintptr_t>(result->begin)) / sizeof(std::uint32_t);
    if (new_count == count + 1 && std::binary_search(result->begin, result->end, target)) {
        char line[128]{};
        std::snprintf(line, sizeof line, "added custom resolution height %u to Azimuth's sorted options", target);
        note_once(target, line);
    } else {
        note_once(target, "custom height insertion could not be verified; review this build");
    }
    return result;
}

DWORD WINAPI install_hook_thread(void*) {
    if (!g_api || g_api->api_version < 3 || !g_api->hook_function || !g_api->enable_hooks) {
        fail_enabled_state("platform API 3 hook functions are unavailable; feature disabled");
        return 0;
    }

    HMODULE module = GetModuleHandleW(L"azimuth.exe");
    if (!module) {
        fail_enabled_state("Azimuth module was not found; no hook installed");
        return 0;
    }

    constexpr std::string_view kOptionsSignature =
        "48 89 5c 24 20 56 57 41 56 48 83 ec 60 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 50 "
        "48 8b c2 48 8b d9 48 89 4c 24 30 33 ff 89 7c 24 24 49 8b d0 48 8b c8 e8 ?? ?? ?? ?? "
        "44 8b f0 b8 e0 10 00 00 45 85 f6 74 0b 43 8d 34 36 3b f0 0f 47 f0 eb 05 be 70 08 00 00 "
        "48 89 3b 48 89 7b 08 48 89 7b 10";
    constexpr std::string_view kInsertSignature =
        "48 89 6c 24 20 57 41 54 41 56 48 83 ec 20 48 8b 41 08 48 8b ea 48 8b 11 4c 8b f5 4c 2b f2 "
        "48 2b c2 4d 8b e0 49 c1 fe 02 48 c1 f8 02 49 b8 ff ff ff ff ff ff ff 3f 48 8b f9 49 3b c0 "
        "0f 84 c7 00 00 00 48 8b 49 10 48 2b ca 48 89 5c 24 40 48 c1 f9 02 48 8b d1 4c 89 7c 24 50 "
        "4c 8d 78 01 48 d1 ea 49 8b c0 48 2b c2 48 3b c8 76 38 49 8b d8 48 8d 0c 9d 00 00 00 00 "
        "48 89 74 24 48 e8 ef ae fe ff";

    std::size_t options_count = 0;
    std::size_t insert_count = 0;
    void* options = find_unique_executable_match(module, kOptionsSignature, options_count);
    void* insert = find_unique_executable_match(module, kInsertSignature, insert_count);
    if (!options || !insert) {
        char line[192]{};
        std::snprintf(line, sizeof line, "signature check failed (options=%zu, insert=%zu); no hook installed",
                      options_count, insert_count);
        log_line(line);
        fail_enabled_state("signature mismatch; custom resolution disabled and prior setting restored if available");
        return 0;
    }
    g_insert_height = reinterpret_cast<InsertHeightFn>(insert);
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    char line[160]{};
    std::snprintf(line, sizeof line, "verified Azimuth signatures (options RVA 0x%llX, insert RVA 0x%llX)",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(options) - base),
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(insert) - base));
    log_line(line);

    void* trampoline = nullptr;
    g_hook = g_api->hook_function(g_mod, "Azimuth resolution options", options,
                                  reinterpret_cast<void*>(&build_options_detour), &trampoline);
    if (!g_hook || !trampoline) {
        g_hook_ready.store(false, std::memory_order_relaxed);
        fail_enabled_state("platform refused the Azimuth hook or original trampoline; custom setting disabled");
        return 0;
    }
    g_build_options = reinterpret_cast<BuildOptionsFn>(trampoline);
    g_hook_ready.store(true, std::memory_order_relaxed);
    if (g_enabled.load(std::memory_order_relaxed)) {
        if (!g_api->enable_hooks(g_mod, g_hook)) {
            g_hook_ready.store(false, std::memory_order_relaxed);
            fail_enabled_state("platform could not enable the Azimuth hook; custom setting disabled");
            return 0;
        }
        g_hook_active.store(true, std::memory_order_relaxed);
        apply_custom_height();
        log_line("Azimuth resolution-list hook enabled");
    } else {
        log_line("verified Azimuth hook prepared but left disabled");
    }
    return 0;
}

void initialize_config(const FH2ModApi* api) {
    const std::wstring azimuth_dir = api->azimuth_dir ? api->azimuth_dir : L"";
    const std::wstring mod_dir = join_path(azimuth_dir.c_str(), kModFolder);
    CreateDirectoryW(mod_dir.c_str(), nullptr);
    g_config_path = join_path(mod_dir.c_str(), L"settings.ini");
    g_azimuth_ini = join_path(azimuth_dir.c_str(), L"data\\azimuth.ini");

    if (GetFileAttributesW(g_config_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        write_uint(kConfigSection, L"enabled", 0, g_config_path);
        write_uint(kConfigSection, L"height", kDefaultHeight, g_config_path);
        write_uint(kStateSection, L"original_resolution", 0, g_config_path);
    }
    const bool enabled = read_uint(kConfigSection, L"enabled", 0, g_config_path) != 0;
    const std::uint32_t height = normalize_height(read_uint(kConfigSection, L"height", kDefaultHeight, g_config_path));
    const std::uint32_t restore = read_uint(kStateSection, L"original_resolution", 0, g_config_path);
    g_enabled.store(enabled, std::memory_order_relaxed);
    g_height.store(height, std::memory_order_relaxed);
    g_restore_height.store(restore, std::memory_order_relaxed);

    if (!enabled && restore != 0)
        restore_azimuth_height();
}

void register_settings() {
    if (g_api->api_version < 4 || !g_api->add_setting)
        return;

    FH2Setting enabled{};
    enabled.size = static_cast<std::uint32_t>(offsetof(FH2Setting, choices));
    enabled.key = "enabled";
    enabled.label = "Custom launcher height";
    enabled.description = "Adds one custom height to Azimuth's resolution choices. Takes effect after restart.";
    enabled.value = g_enabled.load(std::memory_order_relaxed) ? 1 : 0;
    enabled.needs_restart = 1;
    enabled.on_change = setting_changed;
    if (!g_api->add_setting(g_mod, &enabled))
        log_line("could not add the enable switch to the MODS page");

    if (g_api->api_version < 5)
        return;
    FH2Setting height{};
    height.size = static_cast<std::uint32_t>(offsetof(FH2Setting, choices));
    height.key = "height";
    height.label = "Launcher height (px)";
    height.description = "Even height from 360 to 4320. Changes the saved Azimuth title resolution; scene-only behavior is unverified.";
    height.needs_restart = 1;
    height.kind = FH2_SETTING_SLIDER;
    height.min = static_cast<float>(kMinHeight);
    height.max = static_cast<float>(kMaxHeight);
    height.step = 2.0f;
    height.slider_value = static_cast<float>(g_height.load(std::memory_order_relaxed));
    height.decimals = 0;
    height.on_slide = slider_changed;
    if (!g_api->add_setting(g_mod, &height))
        log_line("could not add the height slider to the MODS page");
}

} // namespace

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    if (!api || api->api_version < 3 || !api->register_mod || !api->azimuth_dir || !*api->azimuth_dir)
        return 1;
    g_api = api;

    FH2ModInfo info{};
    info.size = static_cast<std::uint32_t>(offsetof(FH2ModInfo, offline_only));
    info.name = "RenderHeight";
    info.version = "0.1.0";
    info.github_repo = nullptr;
    info.platform_version = "2.4.0";
    info.check_updates = 0;
    info.description = "Adds a selectable custom height to Azimuth's launcher resolution choices.";
    g_mod = api->register_mod(&info);
    if (!g_mod)
        return 1;

    initialize_config(api);
    register_settings();
    HANDLE thread = CreateThread(nullptr, 0, install_hook_thread, nullptr, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
        fail_enabled_state("could not start the signature scan thread; custom setting disabled");
    return 0;
}
