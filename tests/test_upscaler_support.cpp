// SPDX-License-Identifier: GPL-2.0-or-later
// Saved FSR 4 choices must be safe before the first rendered frame on older GPUs.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
static inline int setenv(const char *name, const char *value, int overwrite) {
    (void)overwrite;
    return _putenv_s(name, value);
}
static inline int unsetenv(const char *name) {
    return _putenv_s(name, "");
}
#endif
#include "bbport_settings.h"

int main() {
    using namespace BbSettings;
#ifdef _WIN32
    char temp_dir[MAX_PATH];
    GetTempPathA(sizeof(temp_dir), temp_dir);
    char path[512];
    snprintf(path, sizeof(path), "%sbbport-upscaler-test-%lu", temp_dir, (unsigned long)GetCurrentProcessId());
    FILE *init_f = fopen(path, "w");
    if (init_f) fclose(init_f);
#else
    char path[] = "/tmp/bbport-upscaler-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
#endif
    setenv("BB_CONFIG", path, 1);
    unsetenv("BB_UPSCALER");
    unsetenv("BB_UPSCALE_PRESET");
    unsetenv("BB_RENDER_RES");
    auto& s = Get();
    s.upscaler = UpscalerTaa;
    s.preset = Performance;
    assert(RenderPreset() == NativeAA && s.preset == Performance);
    Save();
    s.upscaler = UpscalerOff;
    Load();
    assert(s.upscaler == UpscalerTaa && s.preset == Performance);
    assert(RenderPreset() == NativeAA && !ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr3;
    assert(RenderPreset() == Performance);
    for (bool fsr4 : {false, true}) {
        for (bool fsr411 : {false, true}) {
            for (bool dlss : {false, true}) {
                for (bool fsr_sdk : {false, true}) {
                    for (bool xess : {false, true}) {
                        for (int requested = 0; requested < UpscalerCount; ++requested) {
                            FILE* config = std::fopen(path, "w");
                            assert(config);
                            std::fprintf(config, "upscaler=%s\npreset=3\noutput_res=2560x1440\n",
                                         UpscalerName(requested));
                            std::fclose(config);
                            s.fsr4_problem = nullptr;
                            s.dlss_problem = nullptr;
                            Load();
                            assert(s.upscaler == requested);
                            ConfigureUpscalerSupport(fsr4, fsr411, dlss, fsr_sdk, false, xess);
                            const bool unsupported = (requested == UpscalerFsr4 && !fsr4) ||
                                (requested == UpscalerFsr411 && !(fsr4 && fsr411)) ||
                                (requested == UpscalerDlss && !dlss) ||
                                (requested == UpscalerFsr && !fsr_sdk) ||
                                (requested == UpscalerXess && !xess);
                            const int expected = requested == UpscalerFsr411 && !(fsr4 && fsr411)
                                ? (fsr4 ? UpscalerFsr4 : UpscalerFsr3)
                                : (unsupported ? UpscalerFsr3 : requested);
                            assert(s.upscaler == expected);
                            assert(s.fsr4_supported == fsr4);
                            assert(s.fsr411_supported == (fsr4 && fsr411));
                            assert(s.dlss_supported == dlss);
                            assert(s.fsr_sdk_supported == fsr_sdk);
                            assert(s.xess_supported == xess);
                            assert(bool(s.fsr4_problem.load()) == unsupported);
                            assert(s.preset == Performance && s.output_res == 2);
                            // Startup patch settings still describe the already applied guest patches.
                            assert(s.startup_upscaler == requested);
                            ConfigureUpscalerSupport(fsr4, fsr411, dlss, fsr_sdk, false, xess);
                            assert(s.upscaler == expected);
                        }
                    }
                }
            }
        }
    }
    // FSR4 runs through AMD's FSR SDK when requested via fsr4_to_sdk and fsr_sdk is available.
    s.upscaler = UpscalerFsr4;
    ConfigureUpscalerSupport(true, false, false, true, true, false);
    assert(s.upscaler == UpscalerFsr && !s.fsr4_supported.load());
    // DLSS preset text parsing & formatting checks.
    assert(DlssPresetFromText("auto") == DlssPresetAuto);
    assert(DlssPresetFromText("k") == DlssPresetK);
    assert(DlssPresetFromText("f") == 6);
    assert(std::string(DlssPresetText(DlssPresetAuto)) == "auto");
    assert(std::string(DlssPresetText(DlssPresetK)) == "k");
    // A saved DLSS choice must not survive a machine without the bridge or an RTX GPU.
    s.upscaler = UpscalerDlss;
    ConfigureDlssSupport(false, "test: no DLSS here");
    assert(s.upscaler == UpscalerFsr3 && !s.dlss_supported.load());
    assert(s.dlss_problem.load() && std::string(s.dlss_problem.load()) == "test: no DLSS here");
    s.upscaler = UpscalerDlss;
    ConfigureDlssSupport(true, nullptr);
    assert(s.upscaler == UpscalerDlss && s.dlss_supported.load());
    assert(s.dlss_problem.load() == nullptr);
    s.startup_preset = Quality;
    s.startup_upscaler = UpscalerFsr3;
    s.upscaler = UpscalerFsr3;
    s.preset = Performance;
    assert(RenderPreset() == Performance && !ResolutionNeedsRestart());
    setenv("BB_RENDER_RES", "1706x960", 1);
    assert(RenderPreset() == Quality && ResolutionNeedsRestart());
    s.preset = Quality;
    assert(!ResolutionNeedsRestart());
    s.upscaler = UpscalerOff;
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    assert(!ResolutionNeedsRestart()); // provider change at the same patched size is live
    s.upscaler = UpscalerTaa;          // TAA needs native guest targets
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    const int output = s.startup_output_res;
    s.output_res = (output + 1) % OutputCount;
    assert(ResolutionNeedsRestart());
    s.output_res = output;
    setenv("BB_RENDER_RES", "", 1);
    assert(!FixedRenderSession() && !ResolutionNeedsRestart());
    unsetenv("BB_RENDER_RES");
    std::remove(path);
    std::puts("Upscaler support: saved choices and FSR 3.1 fallback PASS");
}
