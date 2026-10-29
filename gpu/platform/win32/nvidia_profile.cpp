// SPDX-License-Identifier: GPL-2.0-or-later
// bbport Windows: "Power management mode" of this executable in the NVIDIA driver's profiles.

#include "platform/win32/nvidia_profile.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <type_traits>
#include <vector>

#include "shim/bb_env.h"

namespace BbNvidiaProfile {

namespace {

using NvStatus = int;
using Session = void*;
using Profile = void*;
constexpr NvStatus Ok = 0, SettingNotFound = -160, ProfileNotFound = -163,
                   ExecutableNotFound = -166;
constexpr std::uint32_t PreferredPstateId = 0x1057EB71;
constexpr std::uint32_t PreferMax = 1;
constexpr int StringMax = 2048;
using UnicodeString = std::uint16_t[StringMax];

#pragma pack(push, 4)
struct Setting {
    std::uint32_t version;
    UnicodeString name;
    std::uint32_t id;
    std::uint32_t type;     // 0: DWORD
    std::uint32_t location; // 0: this profile, 1: global, 2: base, 3: default
    std::uint32_t is_current_predefined;
    std::uint32_t is_predefined_valid;
    union {
        std::uint32_t u32;
        std::uint8_t binary[4 + 4096];
        UnicodeString wide;
        std::uint64_t u64;
    } predefined;
    union {
        std::uint32_t u32;
        std::uint8_t binary[4 + 4096];
        UnicodeString wide;
        std::uint64_t u64;
    } current;
};
#pragma pack(pop)

struct Application {
    std::uint32_t version;
    std::uint32_t is_predefined;
    UnicodeString app_name;
    UnicodeString friendly_name;
    UnicodeString launcher;
    UnicodeString file_in_folder;
    std::uint32_t flags; // isMetro:1, isCommandLine:1
    UnicodeString command_line;
};

struct ProfileInfo {
    std::uint32_t version;
    UnicodeString name;
    std::uint32_t gpu_support;
    std::uint32_t is_predefined;
    std::uint32_t num_apps;
    std::uint32_t num_settings;
};

template <typename T>
constexpr std::uint32_t Version(std::uint32_t v) {
    return std::uint32_t(sizeof(T)) | (v << 16);
}

void Copy(std::uint16_t* dst, const wchar_t* src) {
    std::size_t i = 0;
    for (; src[i] && i + 1 < StringMax; ++i) {
        dst[i] = std::uint16_t(src[i]);
    }
    dst[i] = 0;
}

struct Api {
    using Query = void*(__cdecl*)(std::uint32_t);
    NvStatus(__cdecl* initialize)();
    NvStatus(__cdecl* create_session)(Session*);
    NvStatus(__cdecl* destroy_session)(Session);
    NvStatus(__cdecl* load_settings)(Session);
    NvStatus(__cdecl* save_settings)(Session);
    NvStatus(__cdecl* find_application)(Session, UnicodeString, Profile*, Application*);
    NvStatus(__cdecl* find_profile)(Session, UnicodeString, Profile*);
    NvStatus(__cdecl* create_profile)(Session, ProfileInfo*, Profile*);
    NvStatus(__cdecl* create_application)(Session, Profile, Application*);
    NvStatus(__cdecl* get_setting)(Session, Profile, std::uint32_t, Setting*);
    NvStatus(__cdecl* set_setting)(Session, Profile, Setting*);

    bool Load() {
        HMODULE module = LoadLibraryA("nvapi64.dll");
        if (!module) {
            return false;
        }
        const auto query = reinterpret_cast<Query>(GetProcAddress(module, "nvapi_QueryInterface"));
        if (!query) {
            return false;
        }
        const auto get = [&](auto& fn, std::uint32_t id) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(query(id));
            return fn != nullptr;
        };
        return get(initialize, 0x0150E828) && get(create_session, 0x0694D52E) &&
               get(destroy_session, 0xDAD9CFF8) && get(load_settings, 0x375DBD6B) &&
               get(save_settings, 0xFCBC7E14) && get(find_application, 0xEEE566B2) &&
               get(find_profile, 0x7E4A9A0B) && get(create_profile, 0xCC176068) &&
               get(create_application, 0x4347A9DE) && get(get_setting, 0x73BF8338) &&
               get(set_setting, 0x577DD202);
    }
};

} // namespace

void Apply() {
    if (bb_env_off("BB_NV_POWER")) {
        return;
    }
    Api api{};
    if (!api.Load() || api.initialize() != Ok) {
        return; // no NVIDIA driver
    }
    Session session = nullptr;
    if (api.create_session(&session) != Ok) {
        return;
    }
    const auto done = [&](const char* what, NvStatus status) {
        if (what) {
            std::printf("NVIDIA profile: %s (NVAPI status %d)\n", what, status);
        }
        api.destroy_session(session);
    };
    NvStatus status = api.load_settings(session);
    if (status != Ok) {
        return done("could not read the driver settings", status);
    }
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return done("no executable name", 0);
    }
    const wchar_t* exe = std::wcsrchr(path, L'\\') ? std::wcsrchr(path, L'\\') + 1 : path;

    auto app = std::make_unique<Application>();
    std::vector<std::uint16_t> name(StringMax);
    Profile profile = nullptr;
    std::memset(app.get(), 0, sizeof(Application));
    app->version = Version<Application>(4);
    Copy(name.data(), exe);
    status = api.find_application(session, name.data(), &profile, app.get());
    if (status == ExecutableNotFound) {
        std::vector<std::uint16_t> profile_name(StringMax);
        Copy(profile_name.data(), L"bbport");
        status = api.find_profile(session, profile_name.data(), &profile);
        if (status == ProfileNotFound) {
            auto info = std::make_unique<ProfileInfo>();
            std::memset(info.get(), 0, sizeof(ProfileInfo));
            info->version = Version<ProfileInfo>(1);
            Copy(info->name, L"bbport");
            status = api.create_profile(session, info.get(), &profile);
        }
        if (status != Ok) {
            return done("could not make the bbport profile", status);
        }
        std::memset(app.get(), 0, sizeof(Application));
        app->version = Version<Application>(4);
        Copy(app->app_name, exe);
        Copy(app->friendly_name, L"bbport");
        status = api.create_application(session, profile, app.get());
        if (status != Ok) {
            return done("could not add the executable to the bbport profile", status);
        }
    } else if (status != Ok) {
        return done("could not look up the executable's profile", status);
    }

    auto setting = std::make_unique<Setting>();
    std::memset(setting.get(), 0, sizeof(Setting));
    setting->version = Version<Setting>(1);
    status = api.get_setting(session, profile, PreferredPstateId, setting.get());
    const bool user_set = status == Ok && setting->location == 0 && !setting->is_current_predefined;
    if (status == Ok && setting->current.u32 == PreferMax) {
        std::printf("NVIDIA profile: power management already prefers maximum performance\n");
        return done(nullptr, 0);
    }
    if (user_set) {
        std::printf("NVIDIA profile: power management mode %u chosen by the user for %ls, kept\n",
                    setting->current.u32, exe);
        return done(nullptr, 0);
    }
    if (status != Ok && status != SettingNotFound) {
        return done("could not read the power management mode", status);
    }
    const std::uint32_t was = status == Ok ? setting->current.u32 : 5;
    std::memset(setting.get(), 0, sizeof(Setting));
    setting->version = Version<Setting>(1);
    setting->id = PreferredPstateId;
    setting->type = 0;
    setting->current.u32 = PreferMax;
    status = api.set_setting(session, profile, setting.get());
    if (status == Ok) {
        status = api.save_settings(session);
    }
    if (status != Ok) {
        return done("could not set prefer maximum performance", status);
    }
    std::printf("NVIDIA profile: power management for %ls: mode %u -> prefer maximum performance "
                "(BB_NV_POWER=0: leave it)\n",
                exe, was);
    done(nullptr, 0);
}

} // namespace BbNvidiaProfile
