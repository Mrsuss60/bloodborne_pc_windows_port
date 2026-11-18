// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport render: shader recipes imported while the game runs (a bb-probe --import-shaders
// process translating them) and merged into the cache as they come. Split out of
// pipeline_cache.cpp (2026-10-26).

#include <algorithm>
#include "shim/bb_env.h"
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <fstream>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "bbport_toggles.h"
#include "common/hash.h"
#include "common/scope_exit.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/serdes.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_capture.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan {

#ifdef _WIN32
bool PipelineCache::LaunchShaderImport(ShaderImport& state) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION created{};
    std::wstring command = state.command;
    const bool foreground = !bb_env_off("BB_BOOT_PRECOMPILE");
    const DWORD prio = foreground ? NORMAL_PRIORITY_CLASS
                                  : (std::thread::hardware_concurrency() <= 4
                                         ? IDLE_PRIORITY_CLASS
                                         : BELOW_NORMAL_PRIORITY_CLASS);
    if (foreground) {
        SetEnvironmentVariableA("BB_IMPORT_FOREGROUND", "1");
    }
    const BOOL ok = CreateProcessW(state.exe.c_str(), command.data(), nullptr, nullptr, TRUE, prio,
                                   nullptr, nullptr, &startup, &created);
    if (foreground) {
        SetEnvironmentVariableA("BB_IMPORT_FOREGROUND", nullptr);
    }
    if (!ok) {
        return false;
    }
    CloseHandle(created.hThread);
    state.process = created.hProcess;
    return true;
}
#endif

void PipelineCache::StartShaderImport() {
    auto& store = Storage::DataBase::Instance();
    if (!store.IsOpened()) {
        return;
    }
    std::error_code ec;
    if (std::filesystem::exists(store.Directory() / "import", ec)) {
        if (bb_env_off("BB_BOOT_PRECOMPILE")) {
            MergeShaderImport(true);
        }
        import_ready.store(true, std::memory_order_release);
    }
    SaveProfile();
    // bbport: the import child validates <cache>/profile.bin before it imports any recipe, so the
    // profile has to be on disk (not just queued) before CreateProcess runs.
    store.Flush();
    std::string pack_str;
    const char* pack = bb_env("BB_SHADER_RECIPES");
    if (!pack || !*pack || !std::filesystem::exists(pack, ec)) {
        if (std::filesystem::exists("shader_recipes.bin", ec)) {
            pack_str = "shader_recipes.bin";
            pack = pack_str.c_str();
        } else {
#ifdef _WIN32
            wchar_t exe_buf[MAX_PATH];
            if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH)) {
                auto p1 = std::filesystem::path(exe_buf).parent_path() / "shader_recipes.bin";
                auto p2 = std::filesystem::path(exe_buf).parent_path().parent_path() / "shader_recipes.bin";
                if (std::filesystem::exists(p1, ec)) {
                    pack_str = p1.string();
                    pack = pack_str.c_str();
                } else if (std::filesystem::exists(p2, ec)) {
                    pack_str = p2.string();
                    pack = pack_str.c_str();
                }
            }
#endif
        }
    }
    std::string game_str;
    const char* game = bb_env("BB_SHADER_GAME_DIR");
    if (!game || !*game || !std::filesystem::exists(game, ec)) {
        if (std::filesystem::exists("../CUSA03173", ec)) {
            game_str = "../CUSA03173";
            game = game_str.c_str();
        } else if (std::filesystem::exists("CUSA03173", ec)) {
            game_str = "CUSA03173";
            game = game_str.c_str();
        }
    }
    if (!pack || !*pack || !game || !*game || !std::filesystem::exists(pack, ec) || !std::filesystem::exists(game, ec)) {
        std::printf("Shader recipes: skipped (pack=%s, game=%s)\n",
                    pack && *pack ? pack : "<none>", game && *game ? game : "<none>");
        return;
    }
    const std::string dir = store.Directory().string();
    const std::string params = fmt::format("{:x}", Shader::MotionVectors::params_address);
    const std::string positions = fmt::format("{:x}", Shader::MotionVectors::positions_address);

    if (!import) {
        import = std::make_unique<ShaderImport>();
    } else {
        import->listed = import->done = import->adopting_program = false;
        import->programs.clear();
        import->keys.clear();
        import->next_program = import->next_perm = import->next_key = 0;
        import->precompiled.reset();
    }
#ifdef _WIN32
    {
        wchar_t exe[MAX_PATH];
        std::wstring command;
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
            command = L"\"" + std::wstring{exe} + L"\" --import-shaders";
            for (const std::string& arg : {dir, std::string{pack}, std::string{game}, params, positions}) {
                command += L" \"" + std::filesystem::path{arg}.wstring() + L"\"";
            }
        }
        import->exe = exe;
        import->command = command;
        if (command.empty() || !LaunchShaderImport(*import)) {
            import->done = true;
            return;
        }
    }
#else
    const char* argv[] = {"bb-probe", "--import-shaders", dir.c_str(), pack, game,
                          params.c_str(), positions.c_str(), nullptr};
    std::vector<char*> env;
    for (char** e = environ; *e; ++e) {
        if (std::strncmp(*e, "LD_PRELOAD=", 11) != 0) {
            env.push_back(*e);
        }
    }
    env.push_back(nullptr);
    if (posix_spawn(&import->pid, "/proc/self/exe", nullptr, nullptr, const_cast<char**>(argv),
                    env.data()) != 0) {
        import->done = true;
        return;
    }
#endif
    import->running = true;
    g_bb_shader_import_active.store(true, std::memory_order_relaxed);
    import->watcher = std::thread([this, state = import.get()] {
        Common::SetCurrentThreadName("bb:ShaderImport");
#ifdef _WIN32
        for (int restarts = 0;; ++restarts) {
            void* handle = state->process.load();
            WaitForSingleObject(handle, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(handle, &code);
            CloseHandle(handle);
            state->process = nullptr;
            if (code == 0 || state->stopping || restarts == 16) {
                break;
            }
            std::printf("Shader recipes: the import process ended (code %lu), started again\n",
                        static_cast<unsigned long>(code));
            if (!LaunchShaderImport(*state)) {
                break;
            }
        }
#else
        int status = 0;
        while (waitpid(state->pid, &status, 0) < 0 && errno == EINTR) {
        }
#endif
        std::error_code ec;
        if (std::filesystem::exists(Storage::DataBase::Instance().Directory() / "import", ec)) {
            import_ready.store(true, std::memory_order_release);
        } else {
            g_bb_shader_import_active.store(false, std::memory_order_relaxed);
        }
        state->running.store(false, std::memory_order_release);
    });
}

namespace {

namespace fs = std::filesystem;

std::string Hex(u64 value) {
    return fmt::format("{:#018x}", value);
}

std::vector<u8> ReadImportFile(const fs::path& path) {
    std::vector<u8> bytes;
    const Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read};
    if (file.IsOpen()) {
        bytes.resize(file.GetSize());
        file.Read(bytes);
    }
    return bytes;
}

void RemoveImportedProgram(const fs::path& from, const std::vector<fs::path>& files, u64 hash,
                           u64 perms) {
    std::error_code ec;
    for (const auto& file : files) {
        fs::remove(file, ec);
    }
    for (u64 perm = 0; perm < perms; ++perm) {
        fs::remove(from / (Hex(HashCombine(hash, perm)) + ".meta"), ec);
    }
}

bool RemapImportedKey(const std::unordered_map<u64, u64>& remap, std::vector<u8>& bytes,
                      const fs::path& path) {
    bool drop = false, changed = false;
    try {
        Serialization::Archive in{std::vector<u8>{bytes}};
        Serialization::Reader reader{in};
        u32 version{}, is_compute{};
        reader.Read(version);
        if (version != Vulkan::PipelineStore::PipelineKeyVersion) {
            return false;
        }
        if (remap.empty()) {
            return true;
        }
        reader.Read(is_compute);
        const auto map = [&](auto& value) {
            if (const auto it = remap.find(u64(value)); it != remap.end()) {
                drop |= it->second == 0;
                changed |= it->second != u64(value);
                value = static_cast<std::remove_reference_t<decltype(value)>>(it->second);
            }
        };
        Serialization::Archive out{};
        Serialization::Writer writer{out};
        writer.Write(version);
        writer.Write(is_compute);
        if (is_compute) {
            ComputePipelineKey key{};
            key.Deserialize(in);
            ComputePipeline::SerializationSupport sdata{};
            sdata.Deserialize(in);
            map(key.value);
            key.Serialize(out);
            sdata.Serialize(out);
        } else {
            GraphicsPipelineKey key{};
            key.Deserialize(in);
            GraphicsPipeline::SerializationSupport sdata{};
            sdata.Deserialize(in);
            for (auto& stage : key.stage_hashes) {
                map(stage);
            }
            key.Serialize(out);
            sdata.Serialize(out);
        }
        if (changed && !drop) {
            bytes = std::move(out.TakeOff());
            const Common::FS::IOFile file{path, Common::FS::FileAccessMode::Write};
            if (file.IsOpen()) {
                file.Write(bytes);
            }
        }
    } catch (const std::exception&) {
        drop = true;
    }
    return !drop;
}

} // namespace

void PipelineCache::StartImportPrecompile(ShaderImport& state) {
    auto pre = std::make_shared<ShaderImport::Precompiled>();
    pre->device = instance.GetDevice();
    state.precompiled = pre;
    for (const auto& [hash, files] : state.programs) {
        for (const auto& file : files) {
            if (file.extension() != ".spv") {
                continue;
            }
            const std::string stem = file.stem().string();
            const size_t sep = stem.rfind('_');
            if (sep == std::string::npos) {
                continue;
            }
            const u64 perm = std::strtoull(stem.c_str() + sep + 1, nullptr, 10);
            const u64 perm_hash = HashCombine(hash, perm);
            pre->pending.fetch_add(1, std::memory_order_relaxed);
            GetCompiler().SubmitLow([pre, file, perm_hash]() {
                SCOPE_EXIT {
                    if (pre->pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        std::scoped_lock lk{pre->mutex};
                        pre->cv.notify_all();
                    }
                };
                const auto bytes = ReadImportFile(file);
                if (bytes.size() < 20 || (bytes.size() & 3) != 0) {
                    return;
                }
                std::vector<u32> spv(bytes.size() / 4);
                std::memcpy(spv.data(), bytes.data(), bytes.size());
                if (spv[0] != 0x07230203u) {
                    return;
                }
                if (vk::ShaderModule mod = CompileSPV(spv, pre->device)) {
                    std::scoped_lock lk{pre->mutex};
                    pre->modules.emplace(perm_hash, mod);
                }
            });
        }
    }
}

void PipelineCache::MergeShaderImport(bool at_start) {
    auto& store = Storage::DataBase::Instance();
    if (!import) {
        import = std::make_unique<ShaderImport>();
    }
    auto& state = *import;
    if (state.compute_queued.load(std::memory_order_relaxed) != 0) {
        std::scoped_lock lk{state.compute_mutex};
        for (auto& [key, pipeline] : state.compute_made) {
            compute_pipelines.try_emplace(key, std::move(pipeline));
            state.compute_queued.fetch_sub(1, std::memory_order_relaxed);
        }
        state.compute_made.clear();
    }
    if (!at_start && state.running.load(std::memory_order_relaxed)) {
        return;
    }
    if (state.done || !store.IsOpened()) {
        if (state.compute_queued.load(std::memory_order_relaxed) == 0) {
            import_ready.store(false, std::memory_order_relaxed);
        }
        return;
    }
    const fs::path dir = store.Directory(), from = dir / "import";
    std::error_code ec;
    const bool boot_mode = g_bb_boot_precompile_active.load(std::memory_order_relaxed);
    if (!state.listed) {
        state.listed = true;
        state.start = std::chrono::steady_clock::now();
        std::map<u64, std::vector<fs::path>> by_program;
        for (const auto& entry : fs::directory_iterator{from, ec}) {
            const auto ext = entry.path().extension();
            if (ext == ".spv" || ext == ".in") {
                const u64 hash = std::strtoull(entry.path().stem().string().c_str(), nullptr, 16);
                by_program[hash].push_back(entry.path());
            } else if (ext == ".key") {
                state.keys.push_back(entry.path());
            }
        }
        state.programs.assign(by_program.begin(), by_program.end());
        if (boot_mode && !state.keys.empty()) {
            g_bb_boot_compile_done.store(0, std::memory_order_relaxed);
            g_bb_boot_compile_total.store(u32(state.keys.size()), std::memory_order_release);
        }
        StartImportPrecompile(state);
    }
    if (state.precompiled && state.precompiled->pending.load(std::memory_order_acquire) > 0) {
        if (!at_start || boot_mode) {
            return;
        }
        std::unique_lock lk{state.precompiled->mutex};
        state.precompiled->cv.wait(
            lk, [&] { return state.precompiled->pending.load(std::memory_order_acquire) == 0; });
    }
    const u32 frame = BbStats::frame_number.load(std::memory_order_relaxed);
    if (!at_start && !boot_mode && frame == state.frame) {
        return;
    }
    state.frame = frame;
    const auto slice_ms = boot_mode ? 40 : (at_start ? 5 : 2);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(slice_ms);
    const auto in_budget = [&] {
        return std::chrono::steady_clock::now() < deadline;
    };
    {
        const PipelineSelection saved = sel;
        SCOPE_EXIT {
            sel = saved;
        };
        while (state.next_program < state.programs.size() && in_budget()) {
            if (!MergeImportedProgram(state, state.next_program, in_budget)) {
                break;
            }
            ++state.next_program;
        }
    }
    while (state.next_program == state.programs.size() && state.next_key < state.keys.size() &&
           in_budget()) {
        const fs::path& path = state.keys[state.next_key++];
        std::vector<u8> bytes;
        {
            const Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read};
            if (!file.IsOpen()) {
                if (boot_mode) {
                    g_bb_boot_compile_done.fetch_add(1, std::memory_order_relaxed);
                }
                continue;
            }
            bytes.resize(file.GetSize());
            file.Read(bytes);
        }
        if (!RemapImportedKey(state.remap, bytes, path)) {
            fs::remove(path, ec);
            if (boot_mode) {
                g_bb_boot_compile_done.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        fs::rename(path, dir / path.filename(), ec);
        const u32 jobs_before = state.queued + state.compute;
        MergeImportedKey(state, std::move(bytes));
        if (boot_mode) {
            const u32 jobs_added = (state.queued + state.compute) - jobs_before;
            if (jobs_added == 0) {
                g_bb_boot_compile_done.fetch_add(1, std::memory_order_relaxed);
            } else if (jobs_added > 1) {
                g_bb_boot_compile_total.fetch_add(jobs_added - 1, std::memory_order_relaxed);
            }
        }
    }
    if (state.next_program == state.programs.size() && state.next_key == state.keys.size()) {
        state.precompiled.reset();
        fs::remove_all(from, ec);
        state.done = true;
        g_bb_shader_import_active.store(false, std::memory_order_relaxed);
        std::printf("Shader recipes: %u programs merged into the pipeline cache (%u translated by "
                    "the game meanwhile, %u of their permutations joined theirs), %u graphics and "
                    "%u compute pipelines compiling in the background (%.0f ms)\n",
                    state.merged, state.skipped, state.adopted, state.queued, state.compute,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              state.start)
                        .count());
    }
}

bool PipelineCache::MergeImportedProgram(ShaderImport& state, size_t index,
                                         const std::function<bool()>& in_budget) {
    auto& store = Storage::DataBase::Instance();
    const fs::path dir = store.Directory(), from = dir / "import";
    std::error_code ec;
    const auto& [hash, files] = state.programs[index];
    u64 perms = 0;
    for (const auto& file : files) {
        perms += file.extension() == ".spv" ? 1 : 0;
    }
    if (state.next_perm == 0) {
        state.adopting_program = (program_cache.find(hash) != program_cache.end());
    }
    if (state.adopting_program) {
        if (!AdoptImportedProgram(state, hash, perms, in_budget)) {
            return false;
        }
        RemoveImportedProgram(from, files, hash, perms);
        ++state.skipped;
        state.next_perm = 0;
        state.adopting_program = false;
        return true;
    }
    if (state.next_perm == 0) {
        for (const auto& file : files) {
            const fs::path dst = dir / file.filename();
            if (!fs::exists(dst, ec)) {
                fs::rename(file, dst, ec);
            } else {
                fs::remove(file, ec);
            }
        }
    }
    for (u64 perm = state.next_perm; perm < perms; ++perm) {
        if (perm > state.next_perm && !in_budget()) {
            state.next_perm = perm;
            return false;
        }
        const u64 perm_hash = HashCombine(hash, perm);
        const std::string meta_name = Hex(perm_hash);
        const fs::path meta_dst = dir / (meta_name + ".meta");
        const fs::path meta_src = from / (meta_name + ".meta");
        if (!fs::exists(meta_dst, ec)) {
            fs::rename(meta_src, meta_dst, ec);
        } else {
            fs::remove(meta_src, ec);
        }
        vk::ShaderModule precompiled = state.TakePrecompiled(perm_hash);
        std::vector<u8> meta;
        store.Load(Storage::BlobType::ShaderMeta, meta_name, meta);
        try {
            Serialization::Archive ar{std::move(meta)};
            std::unique_lock lk{programs_mutex};
            LoadPipelineStage(ar, 0, precompiled);
        } catch (const Serialization::CorruptData& e) {
            LOG_WARNING(Render_Vulkan, "Imported shader meta {} is corrupt ({}); removing it",
                        meta_name, e.what());
            fs::remove(meta_dst, ec);
        } catch (const std::exception&) {
        }
    }
    state.next_perm = 0;
    ++state.merged;
    return true;
}

bool PipelineCache::AdoptImportedProgram(ShaderImport& state, u64 hash, u64 perms,
                                         const std::function<bool()>& in_budget) {
    const fs::path from = Storage::DataBase::Instance().Directory() / "import";
    for (u64 perm = state.next_perm; perm < perms; ++perm) {
        if (perm > state.next_perm && !in_budget()) {
            state.next_perm = perm;
            return false;
        }
        const u64 perm_hash = HashCombine(hash, perm);
        const std::string name = fmt::format("{}_{}", Hex(hash), perm);
        auto meta = ReadImportFile(from / (Hex(perm_hash) + ".meta"));
        const auto bytes = ReadImportFile(from / (name + ".spv"));
        std::vector<u32> spv(bytes.size() / 4);
        std::memcpy(spv.data(), bytes.data(), spv.size() * 4);
        vk::ShaderModule precompiled = state.TakePrecompiled(perm_hash);
        std::unique_lock lk{programs_mutex};
        if (const auto idx = AdoptImportedPermutation(std::move(meta), std::move(spv),
                                                      ReadImportFile(from / (name + ".in")),
                                                      precompiled)) {
            state.remap[perm_hash] = HashCombine(hash, u64(*idx));
            ++state.adopted;
        } else {
            state.remap[perm_hash] = 0;
        }
    }
    state.next_perm = 0;
    return true;
}

void PipelineCache::MergeImportedKey(ShaderImport& state, std::vector<u8>&& bytes) {
    const PipelineSelection saved = sel;
    try {
        Serialization::Archive ar{std::move(bytes)};
        Serialization::Reader reader{ar};
        u32 version{}, is_compute{};
        reader.Read(version);
        reader.Read(is_compute);
        if (version != Vulkan::PipelineStore::PipelineKeyVersion) {
            // another build's
        } else if (is_compute) {
            QueueImportedCompute(state, ar);
        } else {
            QueueImportedGraphics(state, ar);
        }
    } catch (const std::exception&) {
    }
    sel = saved;
}

void PipelineCache::QueueImportedCompute(ShaderImport& state, Serialization::Archive& ar) {
    ComputePipelineKey key{};
    key.Deserialize(ar);
    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);
    std::vector<u8> meta;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta, Hex(key.value), meta);
    bool loaded = false;
    if (!meta.empty() && compute_pipelines.find(key) == compute_pipelines.end()) {
        Serialization::Archive meta_ar{std::move(meta)};
        std::unique_lock lk{programs_mutex};
        loaded = LoadPipelineStage(meta_ar, 0);
    }
    if (!loaded) {
        return;
    }
    const Shader::Info* info = sel.infos[0];
    const vk::ShaderModule module = sel.modules[0];
    ++state.compute;
    state.compute_queued.fetch_add(1, std::memory_order_relaxed);
    const bool track_boot = g_bb_boot_precompile_active.load(std::memory_order_relaxed);
    GetCompiler().SubmitLow([this, &state, key, info, module, sdata, track_boot]() mutable {
        SCOPE_EXIT {
            if (track_boot) {
                g_bb_boot_compile_done.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::unique_ptr<ComputePipeline> pipeline;
        try {
            pipeline = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                         *pipeline_cache, key, *info, module,
                                                         sdata, true);
        } catch (const std::exception&) {
        }
        std::scoped_lock lk{state.compute_mutex};
        if (pipeline) {
            state.compute_made.emplace_back(key, std::move(pipeline));
        } else {
            state.compute_queued.fetch_sub(1, std::memory_order_relaxed);
        }
    });
}

void PipelineCache::QueueImportedGraphics(ShaderImport& state, Serialization::Archive& ar) {
    std::vector<PreloadJob> jobs;
    {
        std::unique_lock lk{programs_mutex};
        LoadGraphicsPipeline(ar, jobs);
    }
    for (auto& [key, make] : jobs) {
        if (graphics_pipelines.find(key) != graphics_pipelines.end() ||
            pending_graphics.find(key) != pending_graphics.end()) {
            continue;
        }
        auto job = std::make_shared<PendingPipeline>();
        job->saved = true;
        job->from_cache = true;
        job->make = std::move(make);
        pending_graphics.emplace(key, job);
        ++state.queued;
        g_bb_background_pipelines_total.fetch_add(1, std::memory_order_relaxed);
        const bool track_boot = g_bb_boot_precompile_active.load(std::memory_order_relaxed);
        GetCompiler().SubmitLow([job, key, track_boot] {
            SCOPE_EXIT {
                g_bb_background_pipelines_done.fetch_add(1, std::memory_order_relaxed);
                if (track_boot) {
                    g_bb_boot_compile_done.fetch_add(1, std::memory_order_relaxed);
                }
            };
            if (job->started.exchange(true)) {
                return;
            }
            std::unique_ptr<GraphicsPipeline> pipeline;
            try {
                pipeline = job->make();
            } catch (const std::exception& e) {
                LOG_WARNING(Render_Vulkan, "Imported graphics pipeline {:#x} failed: {}",
                            std::hash<GraphicsPipelineKey>{}(key), e.what());
            }
            std::scoped_lock lk{job->mutex};
            job->pipeline = std::move(pipeline);
            job->make = {};
            job->done = true;
            job->ready.store(true, std::memory_order_release);
            job->cv.notify_all();
        });
    }
}

std::optional<size_t> PipelineCache::AdoptImportedPermutation(std::vector<u8>&& meta,
                                                              std::vector<u32>&& spv,
                                                              std::vector<u8>&& input,
                                                              vk::ShaderModule precompiled) {
    auto discard_precompiled = [&] {
        if (precompiled) {
            instance.GetDevice().destroyShaderModule(precompiled);
        }
    };
    Shader::Info info{};
    Shader::StageSpecialization spec{};
    spec.info = &info;
    size_t import_idx{};
    try {
        Serialization::Archive ar{std::move(meta)};
        if (!LoadShaderMeta(ar, info, spec, import_idx)) {
            discard_precompiled();
            return std::nullopt;
        }
    } catch (const std::exception&) {
        discard_precompiled();
        return std::nullopt;
    }
    const auto it = program_cache.find(info.pgm_hash);
    if (it == program_cache.end()) {
        discard_precompiled();
        return std::nullopt;
    }
    Program& program = *it.value();
    auto& modules = program.modules;
    if (const auto found = std::ranges::find(modules, spec, &Program::Module::spec);
        found != modules.end()) {
        discard_precompiled();
        return size_t(std::distance(modules.begin(), found));
    }
    if (spv.size() < 5 || spv[0] != 0x07230203u) {
        discard_precompiled();
        return std::nullopt;
    }
    vk::ShaderModule module = precompiled ? precompiled : CompileSPV(spv, instance.GetDevice());
    if (!module) {
        return std::nullopt;
    }
    size_t idx = modules.size();
    if (import_idx < modules.size() && !modules[import_idx].spec.Valid()) {
        idx = import_idx;
    }
    RegisterShaderMeta(info, spec, HashCombine(info.pgm_hash, idx), idx);
    RegisterShaderBinary(std::move(spv), info.pgm_hash, idx);
    if (!input.empty()) {
        RegisterShaderInput(std::move(input), info.pgm_hash, idx);
    }
    if (!program.info_template) {
        program.info_template = std::make_unique<Shader::Info>(program.info);
    }
    const u64 flat_hash = XXH3_64bits(info.flattened_ud_buf.data(),
                                      info.flattened_ud_buf.size() * sizeof(u32));
    spec.info = &program.info;
    program.InsertPermut(module, std::move(spec), idx, flat_hash);
    return idx;
}

void PipelineCache::FinishBootPrecompile(const std::function<void()>& tick_ui) {
    auto& store = Storage::DataBase::Instance();
    if (!store.IsOpened()) {
        return;
    }
    const auto has_compute_pending = [&] {
        return import && import->compute_queued.load(std::memory_order_relaxed) > 0;
    };
    const auto drain_compute = [&] {
        if (!import || import->compute_queued.load(std::memory_order_relaxed) == 0) {
            return;
        }
        std::scoped_lock lk{import->compute_mutex};
        for (auto& [key, pipeline] : import->compute_made) {
            compute_pipelines.try_emplace(key, std::move(pipeline));
            import->compute_queued.fetch_sub(1, std::memory_order_relaxed);
        }
        import->compute_made.clear();
    };

    std::error_code ec;
    const bool has_import =
        (import && import->running.load(std::memory_order_acquire)) ||
        import_ready.load(std::memory_order_acquire) ||
        fs::exists(store.Directory() / "import", ec);
    const bool has_pending = !pending_graphics.empty() || has_compute_pending();
    if (!has_import && !has_pending) {
        return;
    }

    const auto boot_start = std::chrono::steady_clock::now();
    const size_t initial_graphics = graphics_pipelines.size();
    const size_t initial_compute = compute_pipelines.size();
    g_bb_recipe_import_done.store(0, std::memory_order_relaxed);
    g_bb_recipe_import_total.store(0, std::memory_order_relaxed);
    g_bb_boot_compile_done.store(0, std::memory_order_relaxed);
    g_bb_boot_compile_total.store(0, std::memory_order_relaxed);
    g_bb_boot_precompile_active.store(true, std::memory_order_release);
    SCOPE_EXIT {
        g_bb_boot_precompile_active.store(false, std::memory_order_release);
        g_bb_recipe_import_done.store(0, std::memory_order_relaxed);
        g_bb_recipe_import_total.store(0, std::memory_order_relaxed);
        g_bb_boot_compile_done.store(0, std::memory_order_relaxed);
        g_bb_boot_compile_total.store(0, std::memory_order_relaxed);
    };

    auto last_ui = boot_start;
    bool ui_shown = !pending_graphics.empty();
    const auto maybe_tick_ui = [&](bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!ui_shown) {
            if (now - boot_start < std::chrono::milliseconds(150)) {
                return;
            }
            ui_shown = true;
            force = true;
        }
        if (tick_ui && (force || now - last_ui >= std::chrono::milliseconds(33))) {
            tick_ui();
            last_ui = std::chrono::steady_clock::now();
        }
    };

    // Phase 1: wait for --import-shaders child (running at NORMAL priority across shards).
    if (import && import->running.load(std::memory_order_acquire)) {
        const fs::path staging = store.Directory() / "import.tmp";
        auto last_scan = boot_start - std::chrono::seconds(1);
        while (import->running.load(std::memory_order_acquire)) {
            RetirePendingGraphics(true);
            drain_compute();
            const auto now = std::chrono::steady_clock::now();
            if (now - last_scan >= std::chrono::milliseconds(250)) {
                last_scan = now;
                if (g_bb_recipe_import_total.load(std::memory_order_relaxed) == 0) {
                    std::ifstream tf{staging / "total.txt"};
                    u32 tot = 0;
                    if (tf >> tot && tot > 0) {
                        g_bb_recipe_import_total.store(tot, std::memory_order_relaxed);
                    }
                }
                if (fs::exists(staging, ec)) {
                    u32 done = 0;
                    for (const auto& entry : fs::directory_iterator{staging, ec}) {
                        const std::string fname = entry.path().filename().string();
                        if (fname.ends_with("_0.spv")) {
                            ++done;
                        }
                    }
                    g_bb_recipe_import_done.store(done, std::memory_order_relaxed);
                }
            }
            maybe_tick_ui();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    // Phase 2: merge all translated shader recipes and queue their Vulkan pipelines.
    g_bb_recipe_import_done.store(0, std::memory_order_relaxed);
    g_bb_recipe_import_total.store(0, std::memory_order_relaxed);
    if (import_ready.load(std::memory_order_acquire) ||
        fs::exists(store.Directory() / "import", ec)) {
        import_ready.store(true, std::memory_order_release);
        if (import) {
            import->done = false;
        }
        while (import_ready.load(std::memory_order_acquire) && (!import || !import->done)) {
            MergeShaderImport(true);
            RetirePendingGraphics(true);
            drain_compute();
            maybe_tick_ui();
            if (import && import->precompiled &&
                import->precompiled->pending.load(std::memory_order_acquire) > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    // Phase 3: drain all queued graphics & compute Vulkan pipelines before gameplay starts.
    while (!pending_graphics.empty() || has_compute_pending()) {
        RetirePendingGraphics(true);
        drain_compute();
        maybe_tick_ui();
        if (!pending_graphics.empty() || has_compute_pending()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    const bool did_work = graphics_pipelines.size() > initial_graphics ||
                          compute_pipelines.size() > initial_compute;
    if (did_work) {
        SaveDriverPipelineCache();
        store.Flush();
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - boot_start).count();
        std::printf("Shader cache: boot precompile complete in %.1f s (%zu graphics, %zu compute "
                    "pipelines ready)\n",
                    secs, graphics_pipelines.size(), compute_pipelines.size());
        std::fflush(stdout);
    }
}

} // namespace Vulkan
