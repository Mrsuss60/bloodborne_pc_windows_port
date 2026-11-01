// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>
#include <string>
#include "../bbgpu.h"

namespace BbSettings {

struct ActionInfo {
    int action;
    const char* ini_key;
    const char* label_en;
    const char* label_ru;
    int default_code;
};

const ActionInfo* GetActionInfos();
/// Layout-aware label for the in-game keybind buttons.
const char* KeyCodeToString(int code);
/// Physical SDL scancode name written to bbport.ini (stable across keyboard layouts).
const char* KeyCodeToStorageName(int code);
int StringToKeyCode(const char* name);
void ResetKeybindsToDefaults();

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
/// NVIDIA's DLSS render-preset hint (NVSDK_NGX_DLSS_Hint_Render_Preset): 0 = the driver's
/// choice, 10 = J (less ghosting), 11 = K (the transformer model, recommended).
enum DlssModel : int { DlssModelDefault = 0, DlssModelJ = 10, DlssModelK = 11 };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
enum HudQuadrant : int { HudTopLeft = 0, HudTopRight = 1, HudBottomLeft = 2, HudBottomRight = 3, HudQuadrantCount };
/// Language of the in-game menu (bbport.ini `language=en|ru`, BB_LANG overrides).
enum Language : int { LangEn = 0, LangRu = 1, LanguageCount };

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label (English, Russian), default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label_en;
    const char* label_ru;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Chromatic aberration", "Хроматическая аберрация", true},
    {"effect_dof", "Depth of field (DoF)", "Глубина резкости (DoF)", true},
    {"effect_motion_blur", "Motion blur", "Размытие в движении", true},
    {"effect_ssao", "SSAO shading", "Затенение SSAO", true},
    {"effect_game_aa", "Native Game AA", "Собственное сглаживание игры", true},
    {"effect_dynamic_shadows", "Dynamic light shadows", "Тени от динамических источников", true},
    {"effect_ssr", "SSR reflections (not in the original game)", "Отражения SSR (не было в игре)", false},
    {"skip_intro", "Skip the intro videos at start", "Пропуск заставок при запуске", false},
    {"debug_camera", "Free camera (Cross + L3)", "Свободная камера (Cross + L3)", false},
    {"debug_menu", "Debug menu (needs the font files)", "Debug menu (нужны файлы шрифтов)", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Persistent cheat slots (bbport.ini [Cheats]). The live cheat table uses ids 0..16.
inline constexpr int CheatSlotCount = 32;
/// Live output resolutions: the upscaler's output and the UI host targets.
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160};
inline constexpr int OutputCount = 4;
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size

struct Values {
    std::atomic<int> upscaler{UpscalerOff};
    std::atomic<int> preset{NativeAA};
    std::atomic<int> dlss_preset{DlssModelDefault};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.65f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{true};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.18f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    std::atomic<bool> show_hud{false};
    std::atomic<int> hud_quadrant{HudTopLeft};
    std::atomic<float> hud_opacity{0.75f};
    std::atomic<float> hud_scale{1.0f};
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// Mouse & Keyboard controls settings
    std::atomic<bool> mk_enabled{true};
    std::atomic<bool> mk_native_mouse{true};
    /// The mouse as a pointer in the game's menus (bbport_menu_pointer.h).
    std::atomic<bool> mouse_menu{true};
    std::atomic<float> mk_sens_x{1.5f};
    std::atomic<float> mk_sens_y{1.5f};
    std::atomic<bool> mk_invert_x{false};
    std::atomic<bool> mk_invert_y{false};
    std::atomic<float> mk_smoothing{0.0f};
    std::atomic<float> mk_responsiveness{1.0f};
    std::atomic<float> mk_acceleration{1.0f};
    std::atomic<int> keybinds[BB_ACTION_COUNT];
    Values();
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    /// Live cheat states (F4 menu), persisted under [Cheats].
    std::atomic<bool> cheats[CheatSlotCount]{};
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    std::atomic<int> prep_workers{-1}; ///< -1 default (Available/2), 0 off, 1..16 explicit workers
    std::atomic<int> async_pipelines{-1}; ///< -1 default (auto), 0 off (sync on GPU thread), 1..16 workers
    /// Live engine clock (probe.c hook). live_fps/target_fps/max_dt_hz persist; the rest is
    /// runtime status for the overlay.
    std::atomic<bool> live_fps{true};
    std::atomic<int> target_fps{60}; ///< 30/60/90/120, 0 = uncapped
    std::atomic<int> max_dt_hz{30}; ///< physics upper clamp (1/30 s by default)
    std::atomic<bool> live_clock_supported{false};
    std::atomic<float> live_clock_dt_ms{0.0f};
    std::atomic<float> live_clock_hz{0.0f};
    std::atomic<int> live_clock_target{60};
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Menu language (Language).
    std::atomic<int> language{LangEn};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    /// DLSS (gpu/dlss_bridge, NVIDIA RTX) is ready, or why not (null before the device exists).
    std::atomic<bool> dlss_supported{false};
    std::atomic<const char*> dlss_problem{nullptr};

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_prep_workers = -1;
    int startup_async_pipelines = -1;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
};

Values& Get();

/// The menu text in the selected language: English by default, Russian with language=ru.
inline const char* Tr(const char* en, const char* ru) {
    return Get().language.load() == LangRu ? ru : en;
}

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Re-reads the ini file at run time to apply external changes in real time.
void Reload();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411);
/// The DLSS bridge/NGX state: unsupported DLSS uses FSR 3.1, like unsupported FSR 4.
void ConfigureDlssSupport(bool available, const char* problem);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Live engine clock requested at start (BB_LIVE_FPS / bbport.ini live_fps).
bool LiveFrameLimitActive();
/// Presenter limit while live mode is active: the target rate (240 for uncapped), or the
/// 60 FPS XML fallback until the probe verifies the eboot and installs the hook.
unsigned LiveFrameLimit();
/// Writes the file (menu changes).
void Save();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);

} // namespace BbSettings
