// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <SDL3/SDL.h>

namespace BbSettings {

static const ActionInfo s_action_infos[BB_ACTION_COUNT] = {
    {BB_ACTION_ATTACK, "key.attack", "Light Attack (R1)", "Обычная атака (R1)", BB_KEY_MOUSE_LEFT},
    {BB_ACTION_HEAVY_ATTACK, "key.heavy_attack", "Heavy Attack (R2)", "Сильная атака (R2)", BB_KEY_CHORD_SHIFT_LMB},
    {BB_ACTION_FIREARM, "key.firearm", "Firearm / Gun (L2)", "Выстрел / оружие (L2)", BB_KEY_MOUSE_RIGHT},
    {BB_ACTION_TRANSFORM, "key.transform", "Weapon Transform (L1)", "Трансформация оружия (L1)", SDL_SCANCODE_TAB},
    {BB_ACTION_LOCK_ON, "key.lock_on", "Lock-On / Camera Reset (R3)", "Захват цели (R3)", BB_KEY_MOUSE_MIDDLE},
    {BB_ACTION_MOVE_FORWARD, "key.move_forward", "Move Forward", "Движение вперёд", SDL_SCANCODE_W},
    {BB_ACTION_MOVE_BACKWARD, "key.move_backward", "Move Backward", "Движение назад", SDL_SCANCODE_S},
    {BB_ACTION_MOVE_LEFT, "key.move_left", "Move Left", "Движение влево", SDL_SCANCODE_A},
    {BB_ACTION_MOVE_RIGHT, "key.move_right", "Move Right", "Движение вправо", SDL_SCANCODE_D},
    {BB_ACTION_DODGE, "key.dodge", "Dodge / Roll / Sprint (Circle)", "Уворот / рывок / прыжок (Circle)", SDL_SCANCODE_SPACE},
    {BB_ACTION_INTERACT, "key.interact", "Interact / Talk (Cross)", "Взаимодействие (Cross)", SDL_SCANCODE_E},
    {BB_ACTION_USE_ITEM, "key.use_item", "Use Item (Square)", "Использовать предмет (Square)", SDL_SCANCODE_R},
    {BB_ACTION_HEAL, "key.heal", "Heal / Blood Vial (Triangle)", "Лечение (Triangle)", SDL_SCANCODE_F},
    {BB_ACTION_DPAD_UP, "key.dpad_up", "Blood Bullets (Up)", "Кровавые пули (Вверх)", SDL_SCANCODE_1},
    {BB_ACTION_DPAD_DOWN, "key.dpad_down", "Switch Item (Down)", "Сменить предмет (Вниз)", SDL_SCANCODE_2},
    {BB_ACTION_DPAD_LEFT, "key.dpad_left", "Switch Left Weapon (Left)", "Сменить левое оружие (Влево)", SDL_SCANCODE_3},
    {BB_ACTION_DPAD_RIGHT, "key.dpad_right", "Switch Right Weapon (Right)", "Сменить правое оружие (Вправо)", SDL_SCANCODE_4},
    {BB_ACTION_GESTURES, "key.gestures", "Gestures Menu (Touchpad L)", "Меню жестов (Touchpad L)", SDL_SCANCODE_G},
    {BB_ACTION_PERSONAL_EFFECTS, "key.personal_effects", "Personal Effects (Touchpad R)", "Личные вещи (Touchpad R)", SDL_SCANCODE_BACKSPACE},
    {BB_ACTION_MENU, "key.menu", "System Menu (Options)", "Меню игры (Options)", SDL_SCANCODE_ESCAPE},
    {BB_ACTION_WALK, "key.walk", "Walk (hold)", "Шаг (удерживать)", SDL_SCANCODE_LALT},
    {BB_ACTION_OVERLAY_TOGGLE, "key.overlay_toggle", "Toggle In-Game Overlay",
     "Открыть/закрыть оверлей", SDL_SCANCODE_INSERT},
    {BB_ACTION_HUD_TOGGLE, "key.hud_toggle", "Toggle Performance HUD",
     "Оверлей производительности", SDL_SCANCODE_F11},
    {BB_ACTION_CHEAT_TOGGLE, "key.cheat_toggle", "Toggle Cheat Menu", "Меню читов",
     SDL_SCANCODE_F4},
    {BB_ACTION_MOUSE_CAPTURE_TOGGLE, "key.mouse_capture_toggle", "Toggle Mouse Capture",
     "Захват/освобождение мыши", SDL_SCANCODE_F1},
};

const ActionInfo* GetActionInfos() {
    return s_action_infos;
}

namespace {

const char* SpecialKeyName(int code) {
    if (code <= 0) return "None";
    if (code == BB_KEY_MOUSE_LEFT) return "Mouse Left";
    if (code == BB_KEY_MOUSE_RIGHT) return "Mouse Right";
    if (code == BB_KEY_MOUSE_MIDDLE) return "Mouse Middle";
    if (code == BB_KEY_MOUSE_X1) return "Mouse X1";
    if (code == BB_KEY_MOUSE_X2) return "Mouse X2";
    if (code == BB_KEY_CHORD_SHIFT_LMB) return "Shift + Mouse Left";
    if (code == BB_KEY_CHORD_SHIFT_RMB) return "Shift + Mouse Right";
    return nullptr;
}

// Letters and punctuation are the keys whose SDL scancode names differ from the
// character printed on a non-US layout.  Digits stay as "1".."0" because their
// unshifted AZERTY glyphs (&, é, ...) are less useful as control labels.
bool IsLayoutLocalized(int code) {
    return (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) ||
           (code >= SDL_SCANCODE_MINUS && code <= SDL_SCANCODE_SLASH);
}

} // namespace

const char* KeyCodeToString(int code) {
    if (const char* special = SpecialKeyName(code)) return special;
    if (code < SDL_SCANCODE_COUNT) {
        if (IsLayoutLocalized(code)) {
            const SDL_Keycode key =
                SDL_GetKeyFromScancode((SDL_Scancode)code, SDL_KMOD_NONE, false);
            const char* name = SDL_GetKeyName(key);
            if (name && name[0] && SDL_strcasecmp(name, "Unknown") != 0) return name;
        }
        const char* name = SDL_GetScancodeName((SDL_Scancode)code);
        if (name && name[0]) return name;
    }
    return "Unknown";
}

const char* KeyCodeToStorageName(int code) {
    if (const char* special = SpecialKeyName(code)) return special;
    if (code < SDL_SCANCODE_COUNT) {
        const char* name = SDL_GetScancodeName((SDL_Scancode)code);
        if (name && name[0]) return name;
    }
    return "Unknown";
}

int StringToKeyCode(const char* name) {
    if (!name || !name[0] || !SDL_strcasecmp(name, "None") || !SDL_strcasecmp(name, "Unknown")) return BB_KEY_NONE;
    if (!SDL_strcasecmp(name, "Mouse Left") || !SDL_strcasecmp(name, "LMB")) return BB_KEY_MOUSE_LEFT;
    if (!SDL_strcasecmp(name, "Mouse Right") || !SDL_strcasecmp(name, "RMB")) return BB_KEY_MOUSE_RIGHT;
    if (!SDL_strcasecmp(name, "Mouse Middle") || !SDL_strcasecmp(name, "MMB")) return BB_KEY_MOUSE_MIDDLE;
    if (!SDL_strcasecmp(name, "Mouse X1") || !SDL_strcasecmp(name, "Mouse 4")) return BB_KEY_MOUSE_X1;
    if (!SDL_strcasecmp(name, "Mouse X2") || !SDL_strcasecmp(name, "Mouse 5")) return BB_KEY_MOUSE_X2;
    if (!SDL_strcasecmp(name, "Shift + Mouse Left") || !SDL_strcasecmp(name, "Shift + LMB")) return BB_KEY_CHORD_SHIFT_LMB;
    if (!SDL_strcasecmp(name, "Shift + Mouse Right") || !SDL_strcasecmp(name, "Shift + RMB")) return BB_KEY_CHORD_SHIFT_RMB;

    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc != SDL_SCANCODE_UNKNOWN) return (int)sc;
    return BB_KEY_NONE;
}

Values::Values() {
    for (int i = 0; i < BB_ACTION_COUNT; ++i) {
        keybinds[i] = s_action_infos[i].default_code;
    }
}

void ResetKeybindsToDefaults() {
    auto& v = Get();
    for (int i = 0; i < BB_ACTION_COUNT; ++i) {
        v.keybinds[i] = s_action_infos[i].default_code;
    }
    Save();
}

namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u) ||
                (u == UpscalerFsr3 && (value == "fsr31" || value == "fsr3")) ||
                (u == UpscalerFsr4 && value == "fsr4")) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "dlss_preset") {
        v.dlss_preset = i == DlssModelJ || i == DlssModelK ? i : DlssModelDefault;
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "show_hud") {
        v.show_hud = i != 0;
    } else if (key == "hud_quadrant") {
        v.hud_quadrant = std::clamp(i, 0, HudQuadrantCount - 1);
    } else if (key == "hud_opacity") {
        v.hud_opacity = Clamp(f, 0.1f, 1.0f);
    } else if (key == "hud_scale") {
        v.hud_scale = Clamp(f, 0.5f, 2.5f);
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "live_fps") {
        v.live_fps = i != 0;
    } else if (key == "target_fps") {
        v.target_fps = (value == "uncap" || i == 0) ? 0 : std::clamp(i, 30, 240);
    } else if (key == "max_dt_hz") {
        v.max_dt_hz = std::clamp(i, 10, 240);
    } else if (key == "prep_workers") {
        v.prep_workers = value == "auto" ? -1 : std::clamp(i, 0, 16);
    } else if (key == "async_pipelines") {
        v.async_pipelines = value == "auto" ? -1 : std::clamp(i, 0, 16);
    } else if (key == "texture_quality") {
        for (int q = 0; q < TqCount; ++q) {
            if (value == TextureQualityName(q)) v.texture_quality = q;
        }
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "language") {
        v.language = value == "ru" ? LangRu : LangEn;
    } else if (key == "mk_enabled") {
        v.mk_enabled = i != 0;
    } else if (key == "mk_native_mouse") {
        v.mk_native_mouse = i != 0;
    } else if (key == "mouse_menu") {
        v.mouse_menu = i != 0;
    } else if (key == "mk_sens_x") {
        v.mk_sens_x = Clamp(f, 0.1f, 10.0f);
    } else if (key == "mk_sens_y") {
        v.mk_sens_y = Clamp(f, 0.1f, 10.0f);
    } else if (key == "mk_invert_x") {
        v.mk_invert_x = i != 0;
    } else if (key == "mk_invert_y") {
        v.mk_invert_y = i != 0;
    } else if (key == "mk_smoothing") {
        v.mk_smoothing = Clamp(f, 0.0f, 0.80f);
    } else if (key == "mk_responsiveness") {
        v.mk_responsiveness = Clamp(f, 0.5f, 2.5f);
    } else if (key == "mk_acceleration") {
        v.mk_acceleration = Clamp(f, 1.0f, 2.0f);
    } else if (key.rfind("key.", 0) == 0) {
        for (int a = 0; a < BB_ACTION_COUNT; ++a) {
            if (key == s_action_infos[a].ini_key) {
                v.keybinds[a] = StringToKeyCode(value.c_str());
                break;
            }
        }
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else if (key.rfind("cheat.", 0) == 0) {
        const int index = std::atoi(key.c_str() + 6);
        if (index >= 0 && index < CheatSlotCount) {
            v.cheats[index] = i != 0;
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    if (const char* env = std::getenv("BB_SHARPNESS_DEFAULT")) {
        v.sharpness = Clamp(std::strtof(env, nullptr), 0.0f, 2.0f);
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0 ||
                (u == UpscalerFsr3 && (std::strcmp(env, "fsr31") == 0 || std::strcmp(env, "fsr3") == 0)) ||
                (u == UpscalerFsr4 && std::strcmp(env, "fsr4") == 0)) {
                v.upscaler = u;
            }
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
        {"BB_DLSS_PRESET", "dlss_preset"},
        {"BB_HUD", "show_hud"},                     {"BB_HUD_QUADRANT", "hud_quadrant"},
        {"BB_HUD_OPACITY", "hud_opacity"},          {"BB_HUD_SCALE", "hud_scale"},
        {"BB_PREP_WORKERS", "prep_workers"},
        {"BB_ASYNC_PIPELINES", "async_pipelines"},
        {"BB_TEXTURE_QUALITY", "texture_quality"},
        {"BB_FPS", "target_fps"},                   {"BB_LIVE_FPS", "live_fps"},
        {"BB_TARGET_FPS", "target_fps"},            {"BB_MAX_DT_HZ", "max_dt_hz"},
        {"BB_LANG", "language"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_prep_workers = v.prep_workers;
    v.startup_async_pipelines = v.async_pipelines;
    v.startup_texture_quality = v.texture_quality;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
}

void Reload() {
    auto& v = Get();
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
    }
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "the DLSS bridge or NVIDIA's DLSS library is missing"
                                 : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

bool LiveFrameLimitActive() {
    return Get().live_fps.load(std::memory_order_relaxed);
}

unsigned LiveFrameLimit() {
    const auto& v = Get();
    if (!v.live_clock_supported.load(std::memory_order_relaxed)) {
        // The launcher's live base is 60 FPS++: until the probe verifies the eboot and installs
        // the hook (or after a signature rejection), the engine runs at that static rate.
        return 60u;
    }
    const int target = v.target_fps.load(std::memory_order_relaxed);
    return target <= 0 ? 240u : unsigned(std::clamp(target, 1, 480));
}

void Save() {
    static std::mutex save_mutex;
    std::scoped_lock lock{save_mutex};
    const auto& v = Get();
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    std::fprintf(file,
                 "# bbport settings (in-game menu: Insert / L3+R3)\n"
                 "# upscaler: off | fsr31 | fsr4 | fsr411 | taa | dlss"
                 " (dlss needs bbport_dlss.dll and nvngx_dlss.dll, else FSR 3.1)\n"
                 "# dlss_preset: 0 = driver default, 10 = J (less ghosting), 11 = K (transformer)\n"
                 "upscaler=%s\npreset=%d\ndlss_preset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nshow_hud=%d\nhud_quadrant=%d\nhud_opacity=%.2f\nhud_scale=%.2f\n"
                 "fsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\n"
                 "mk_enabled=%d\nmk_sens_x=%.2f\nmk_sens_y=%.2f\nmk_invert_x=%d\nmk_invert_y=%d\nmk_smoothing=%.2f\nmk_responsiveness=%.2f\nmk_acceleration=%.2f\n"
                 "mouse_menu=%d\n"
                 "live_fps=%d\ntarget_fps=%d\nmax_dt_hz=%d\n",
                 UpscalerName(v.upscaler), v.preset.load(), v.dlss_preset.load(),
                 int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()),
                 int(v.show_hud.load()), v.hud_quadrant.load(),
                 v.hud_opacity.load(), v.hud_scale.load(),
                 int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()),
                 int(v.mk_enabled.load()), v.mk_sens_x.load(), v.mk_sens_y.load(),
                 int(v.mk_invert_x.load()), int(v.mk_invert_y.load()),
                 v.mk_smoothing.load(), v.mk_responsiveness.load(), v.mk_acceleration.load(),
                 int(v.mouse_menu.load()), int(v.live_fps.load()), v.target_fps.load(),
                 v.max_dt_hz.load());
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        std::fprintf(file, "%s=%d\n", Effects[e].key, int(v.effects[e].load()));
    }
    std::fprintf(file, "model_lod=%d\noutput_res=%dx%d\n", v.model_lod.load(),
                 OutputWidths[v.output_res], OutputHeights[v.output_res]);
    if (v.prep_workers < 0) {
        std::fprintf(file, "prep_workers=auto\n");
    } else {
        std::fprintf(file, "prep_workers=%d\n", v.prep_workers.load());
    }
    if (v.async_pipelines < 0) {
        std::fprintf(file, "async_pipelines=auto\n");
    } else {
        std::fprintf(file, "async_pipelines=%d\n", v.async_pipelines.load());
    }
    std::fprintf(file, "texture_quality=%s\n", TextureQualityName(v.texture_quality.load()));
    // Read by run.sh at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    std::fprintf(file, "language=%s\n", v.language == LangRu ? "ru" : "en");
    // Live cheat states (F4 menu).  Section headers are comments for the loader above.
    std::fprintf(file, "\n[Cheats]\n");
    for (int c = 0; c < CheatSlotCount; ++c) {
        std::fprintf(file, "cheat.%d=%d\n", c, int(v.cheats[c].load()));
    }
    // Action keybindings
    for (int a = 0; a < BB_ACTION_COUNT; ++a) {
        std::fprintf(file, "%s=%s\n", s_action_infos[a].ini_key,
                     KeyCodeToStorageName(v.keybinds[a].load()));
    }
    std::fclose(file);
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr31", "fsr4", "fsr411", "taa",
                                                         "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace BbSettings
