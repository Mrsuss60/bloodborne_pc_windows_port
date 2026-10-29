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
    if (!CreateProcessW(state.exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                        BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup, &created)) {
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
        MergeShaderImport(true);
        import_ready.store(true, std::memory_order_relaxed);
    }
    SaveProfile();
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
        return;
    }
    const std::string dir = store.Directory().string();
    const std::string params = fmt::format("{:x}", Shader::MotionVectors::params_address);
    const std::string positions = fmt::format("{:x}", Shader::MotionVectors::positions_address);

    if (!import) {
        import = std::make_unique<ShaderImport>();
    } else {
        import->listed = import->done = false;
        import->programs.clear();
        import->keys.clear();
        import->next_program = import->next_key = 0;
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
        state->running = false;
        std::error_code ec;
        if (std::filesystem::exists(Storage::DataBase::Instance().Directory() / "import", ec)) {
            import_ready.store(true, std::memory_order_relaxed);
        } else {
            g_bb_shader_import_active.store(false, std::memory_order_relaxed);
        }
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
        if (version != Vulkan::PipelineStore::PipelineKeyVersion) {
            // another build's
        } else if (is_compute) {
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
    }
    const u32 frame = BbStats::frame_number.load(std::memory_order_relaxed);
    if (!at_start && frame == state.frame) {
        return;
    }
    state.frame = frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    const auto in_budget = [&] {
        return at_start || std::chrono::steady_clock::now() < deadline;
    };
    while (state.next_program < state.programs.size() && in_budget()) {
        MergeImportedProgram(state, state.next_program++);
    }
    while (state.next_program == state.programs.size() && state.next_key < state.keys.size() &&
           in_budget()) {
        const fs::path& path = state.keys[state.next_key++];
        std::vector<u8> bytes;
        {
            const Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read};
            if (!file.IsOpen()) {
                continue;
            }
            bytes.resize(file.GetSize());
            file.Read(bytes);
        }
        if (!state.remap.empty() && !RemapImportedKey(state.remap, bytes, path)) {
            fs::remove(path, ec);
            continue;
        }
        fs::rename(path, dir / path.filename(), ec);
        MergeImportedKey(state, std::move(bytes));
    }
    if (state.next_program == state.programs.size() && state.next_key == state.keys.size()) {
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

void PipelineCache::MergeImportedProgram(ShaderImport& state, size_t index) {
    auto& store = Storage::DataBase::Instance();
    const fs::path dir = store.Directory(), from = dir / "import";
    std::error_code ec;
    const auto& [hash, files] = state.programs[index];
    u64 perms = 0;
    for (const auto& file : files) {
        perms += file.extension() == ".spv" ? 1 : 0;
    }
    if (program_cache.find(hash) != program_cache.end()) {
        AdoptImportedProgram(state, hash, perms);
        RemoveImportedProgram(from, files, hash, perms);
        ++state.skipped;
        return;
    }
    if (fs::exists(dir / (Hex(hash) + "_0.spv"), ec)) {
        RemoveImportedProgram(from, files, hash, perms);
        ++state.skipped;
        return;
    }
    for (const auto& file : files) {
        fs::rename(file, dir / file.filename(), ec);
    }
    const PipelineSelection saved = sel;
    for (u64 perm = 0; perm < perms; ++perm) {
        const std::string meta_name = Hex(HashCombine(hash, perm));
        fs::rename(from / (meta_name + ".meta"), dir / (meta_name + ".meta"), ec);
        std::vector<u8> meta;
        store.Load(Storage::BlobType::ShaderMeta, meta_name, meta);
        try {
            Serialization::Archive ar{std::move(meta)};
            std::unique_lock lk{programs_mutex};
            LoadPipelineStage(ar, 0);
        } catch (const std::exception&) {
        }
    }
    sel = saved;
    ++state.merged;
}

void PipelineCache::AdoptImportedProgram(ShaderImport& state, u64 hash, u64 perms) {
    const fs::path from = Storage::DataBase::Instance().Directory() / "import";
    std::unique_lock lk{programs_mutex};
    for (u64 perm = 0; perm < perms; ++perm) {
        const u64 perm_hash = HashCombine(hash, perm);
        const std::string name = fmt::format("{}_{}", Hex(hash), perm);
        auto meta = ReadImportFile(from / (Hex(perm_hash) + ".meta"));
        const auto bytes = ReadImportFile(from / (name + ".spv"));
        std::vector<u32> spv(bytes.size() / 4);
        std::memcpy(spv.data(), bytes.data(), spv.size() * 4);
        if (const auto idx = AdoptImportedPermutation(std::move(meta), std::move(spv),
                                                      ReadImportFile(from / (name + ".in")))) {
            state.remap[perm_hash] = HashCombine(hash, u64(*idx));
            ++state.adopted;
        } else {
            state.remap[perm_hash] = 0;
        }
    }
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
    GetCompiler().Submit([this, &state, key, info, module, sdata]() mutable {
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
        job->make = std::move(make);
        pending_graphics.emplace(key, job);
        ++state.queued;
        g_bb_background_pipelines_total.fetch_add(1, std::memory_order_relaxed);
        GetCompiler().Submit([job] {
            SCOPE_EXIT {
                g_bb_background_pipelines_done.fetch_add(1, std::memory_order_relaxed);
            };
            if (job->started.exchange(true)) {
                return;
            }
            std::unique_ptr<GraphicsPipeline> pipeline;
            try {
                pipeline = job->make();
            } catch (const std::exception&) {
            }
            std::scoped_lock lk{job->mutex};
            job->pipeline = std::move(pipeline);
            job->done = true;
            job->cv.notify_all();
        });
    }
}

std::optional<size_t> PipelineCache::AdoptImportedPermutation(std::vector<u8>&& meta,
                                                              std::vector<u32>&& spv,
                                                              std::vector<u8>&& input) {
    Shader::Info info{};
    Shader::StageSpecialization spec{};
    spec.info = &info;
    size_t import_idx{};
    try {
        Serialization::Archive ar{std::move(meta)};
        if (!LoadShaderMeta(ar, info, spec, import_idx)) {
            return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    const auto it = program_cache.find(info.pgm_hash);
    if (it == program_cache.end()) {
        return std::nullopt;
    }
    Program& program = *it.value();
    auto& modules = program.modules;
    if (const auto found = std::ranges::find(modules, spec, &Program::Module::spec);
        found != modules.end()) {
        return size_t(std::distance(modules.begin(), found));
    }
    if (spv.size() < 5 || spv[0] != 0x07230203u) {
        return std::nullopt;
    }
    vk::ShaderModule module = CompileSPV(spv, instance.GetDevice());
    if (!module) {
        return std::nullopt;
    }
    const size_t idx = modules.size();
    RegisterShaderMeta(info, spec, HashCombine(info.pgm_hash, idx), idx);
    RegisterShaderBinary(std::move(spv), info.pgm_hash, idx);
    if (!input.empty()) {
        RegisterShaderInput(std::move(input), info.pgm_hash, idx);
    }
    spec.info = &program.info;
    program.InsertPermut(module, std::move(spec), idx);
    return idx;
}

} // namespace Vulkan
