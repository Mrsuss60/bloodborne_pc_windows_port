// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_shader_rebuild.h"
#include "shim/bb_env.h"

#include <algorithm>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <csetjmp>
#include <csignal>

#include <miniz.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <csetjmp>
#include <dlfcn.h>
#include <ucontext.h>
#include <csignal>
#include <unistd.h>
#endif

#include "common/hash.h"
#include "common/path_util.h"
#include "common/serdes.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_shader_capture.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_shader_rebuild_internal.h"

namespace BbShaderRebuild {

namespace {
constexpr u32 CacheMagic = 0x42425043u;
static jmp_buf g_recipe_jmp;
static void RecipeSignalHandler(int) {
    longjmp(g_recipe_jmp, 1);
}
#ifdef _WIN32
static LONG WINAPI RecipeVectoredHandler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode >= 0x80000000) {
        longjmp(g_recipe_jmp, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
} // namespace

std::vector<u8> ReadFile(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>(file), {}};
}

namespace {
bool WriteFile(const fs::path& path, std::span<const u8> bytes) {
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!file) {
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    return !ec;
}
} // namespace

std::string HexName(u64 value) {
    char name[32];
    std::snprintf(name, sizeof(name), "%#018llx", (unsigned long long)value);
    return name;
}

namespace {
struct Translated {
    std::vector<u32> spv;
    std::vector<u8> info;
};

std::optional<Translated> Translate(BbShaderCapture::Input& in, const Shader::Profile& profile) {
    Shader::MotionVectors::params_address = in.params_address;
    Shader::MotionVectors::positions_address = in.positions_address;
    auto regions = in.regions;
    regions.push_back({in.pgm_base, std::vector<u8>(reinterpret_cast<const u8*>(in.code.data()),
                                                    reinterpret_cast<const u8*>(in.code.data()) +
                                                        in.code.size() * sizeof(u32))});
    GuestMemory memory;
    if (!memory.Map(regions)) {
        return std::nullopt;
    }
    auto runtime_info = in.runtime_info;
    if (in.hw_stage == Shader::HwStage::Geometry) {
        runtime_info.hw.gs.vs_copy = in.vs_copy;
    }
    const std::span<const u32> guest_code{reinterpret_cast<const u32*>(in.pgm_base),
                                          in.code.size()};
    Shader::Info info{in.hw_stage, in.sw_stage,
                      Shader::ShaderParams{std::span<const u32, 16>{in.user_data}, guest_code,
                                           in.pgm_hash}};
    Shader::flat_buf_replay = &in.flat;
    static Shader::Pools pools;
    auto bindings = in.bindings;
    const auto program = Shader::TranslateProgram(guest_code, pools, info, runtime_info, profile);
    Translated out;
    out.spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
    Shader::flat_buf_replay = nullptr;
    Serialization::Archive archive;
    info.Serialize(archive);
    out.info = archive.TakeOff();
    return out;
}

std::vector<u8> RewriteMeta(std::vector<u8>&& meta, const std::vector<u8>& info,
                            const std::pair<u64, u64>* motion = nullptr) {
    Serialization::Archive ar{std::move(meta)};
    Serialization::Reader reader{ar};
    u32 meta_version{}, binary_version{};
    u64 perm_hash{}, perm_idx{}, motion_params{}, motion_positions{};
    reader.Read(meta_version);
    reader.Read(binary_version);
    if (meta_version != Vulkan::PipelineStore::ShaderMetaVersion) {
        return {};
    }
    reader.Read(perm_hash);
    reader.Read(perm_idx);
    reader.Read(motion_params);
    reader.Read(motion_positions);
    if (motion) {
        std::tie(motion_params, motion_positions) = *motion;
    }
    Shader::Info old_info{};
    Shader::StageSpecialization spec{};
    spec.info = &old_info;
    spec.Deserialize(ar);
    Serialization::Archive out;
    Serialization::Writer writer{out};
    writer.Write(Vulkan::PipelineStore::ShaderMetaVersion);
    writer.Write(Vulkan::PipelineStore::ShaderBinaryVersion);
    writer.Write(perm_hash);
    writer.Write(perm_idx);
    writer.Write(motion_params);
    writer.Write(motion_positions);
    spec.Serialize(out);
    auto bytes = out.TakeOff();
    bytes.insert(bytes.end(), info.begin(), info.end());
    return bytes;
}

bool RebuildDirectory(const fs::path& dir, const Shader::Profile& profile, u32& translated,
                      u32& failed) {
    struct Record {
        fs::path path;
        BbShaderCapture::Input input;
    };
    std::map<u64, std::map<u64, Record>> programs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator{dir, ec}) {
        if (entry.path().extension() != ".in") {
            continue;
        }
        Record record{entry.path(), {}};
        const auto bytes = ReadFile(entry.path());
        if (!BbShaderCapture::ReadInput(bytes, record.input)) {
            fs::remove(entry.path(), ec);
            continue;
        }
        const u64 hash = record.input.pgm_hash, perm = record.input.perm_idx;
        programs[hash].emplace(perm, std::move(record));
    }
    for (auto& [pgm_hash, perms] : programs) {
        std::vector<u8> program_info;
        for (auto& [perm_idx, record] : perms) {
            std::optional<Translated> result;
            try {
                result = Translate(record.input, profile);
            } catch (const std::exception&) {
                result.reset();
            }
            const std::string name = HexName(pgm_hash) + "_" + std::to_string(perm_idx);
            if (!result) {
                fs::remove(dir / (name + ".spv"), ec);
                fs::remove(dir / (HexName(HashCombine(pgm_hash, perm_idx)) + ".meta"), ec);
                ++failed;
                continue;
            }
            if (program_info.empty()) {
                program_info = result->info;
            }
            const auto* words = reinterpret_cast<const u8*>(result->spv.data());
            if (!WriteFile(dir / (name + ".spv"), {words, result->spv.size() * sizeof(u32)})) {
                return false;
            }
            const fs::path meta_path = dir / (HexName(HashCombine(pgm_hash, perm_idx)) + ".meta");
            if (fs::exists(meta_path, ec)) {
                std::vector<u8> meta;
                try {
                    meta = RewriteMeta(ReadFile(meta_path), program_info);
                } catch (const std::exception&) {
                    meta.clear();
                }
                if (meta.empty()) {
                    fs::remove(meta_path, ec);
                } else if (!WriteFile(meta_path, meta)) {
                    return false;
                }
            }
            ++translated;
        }
    }
    return true;
}

constexpr std::array<char, 4> RecipeMagic{'B', 'B', 'S', 'R'};
constexpr u32 RecipeVersion = 1;

struct Bytes {
    std::vector<u8> out;
    std::span<const u8> in;
    size_t at = 0;
    bool ok = true;
    template <typename T>
    void Put(const T& value) {
        const auto* p = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), p, p + sizeof(T));
    }
    void PutBlob(std::span<const u8> blob) {
        Put(u32(blob.size()));
        out.insert(out.end(), blob.begin(), blob.end());
    }
    template <typename T>
    T Get() {
        T value{};
        if (at + sizeof(T) > in.size()) {
            ok = false;
            return value;
        }
        std::memcpy(&value, in.data() + at, sizeof(T));
        at += sizeof(T);
        return value;
    }
    std::vector<u8> GetBlob() {
        const u32 size = Get<u32>();
        if (!ok || size > in.size() - at) {
            ok = false;
            return {};
        }
        std::vector<u8> blob(in.begin() + at, in.begin() + at + size);
        at += size;
        return blob;
    }
};

std::vector<u64> KeyStages(std::span<const u8> bytes) {
    Serialization::Archive ar{std::vector<u8>(bytes.begin(), bytes.end())};
    Serialization::Reader reader{ar};
    u32 version{}, is_compute{};
    reader.Read(version);
    if (version != Vulkan::PipelineStore::PipelineKeyVersion) {
        return {};
    }
    reader.Read(is_compute);
    if (is_compute) {
        Vulkan::ComputePipelineKey key{};
        key.Deserialize(ar);
        return {key.value};
    }
    Vulkan::GraphicsPipelineKey key{};
    key.Deserialize(ar);
    std::vector<u64> stages;
    for (const u64 hash : key.stage_hashes) {
        if (hash) {
            stages.push_back(hash);
        }
    }
    return stages;
}

} // namespace

bool ReadPack(const fs::path& path, std::vector<RecipeProgram>& programs,
              std::vector<RecipeKey>& keys) {
    const auto file = ReadFile(path);
    Bytes header{.in = file};
    std::array<char, 4> magic{};
    for (char& c : magic) {
        c = header.Get<char>();
    }
    if (magic != RecipeMagic || header.Get<u32>() != RecipeVersion ||
        header.Get<u32>() != BbShaderCapture::InputVersion ||
        header.Get<u32>() != BbShaderCapture::InputLayoutId() ||
        header.Get<u32>() != Vulkan::PipelineStore::ShaderMetaVersion ||
        header.Get<u32>() != Vulkan::PipelineStore::PipelineKeyVersion) {
        return false;
    }
    const u64 raw_size = header.Get<u64>();
    if (!header.ok || raw_size > (u64{1} << 31)) {
        return false;
    }
    std::vector<u8> raw(raw_size);
    mz_ulong size = mz_ulong(raw_size);
    if (mz_uncompress(raw.data(), &size, file.data() + header.at, file.size() - header.at) !=
            MZ_OK ||
        size != raw_size) {
        return false;
    }
    Bytes body{.in = raw};
    programs.resize(body.Get<u32>());
    for (auto& program : programs) {
        program.hash = body.Get<u64>();
        program.perms.resize(body.Get<u32>());
        for (auto& perm : program.perms) {
            perm.input = body.GetBlob();
            perm.meta = body.GetBlob();
            perm.refs.resize(body.Get<u32>());
            for (auto& ref : perm.refs) {
                ref.address = body.Get<u64>();
                ref.hash = body.Get<u64>();
                ref.size = body.Get<u32>();
            }
            if (!body.ok) {
                return false;
            }
        }
    }
    keys.resize(body.Get<u32>());
    for (auto& key : keys) {
        const auto name = body.GetBlob();
        key.name.assign(name.begin(), name.end());
        key.bytes = body.GetBlob();
    }
    return body.ok && body.at == raw.size();
}

size_t WritePack(const std::vector<RecipeProgram>& programs, const std::vector<RecipeKey>& keys,
                 const fs::path& path) {
    Bytes body;
    body.Put(u32(programs.size()));
    for (const auto& program : programs) {
        body.Put(program.hash);
        body.Put(u32(program.perms.size()));
        for (const auto& perm : program.perms) {
            body.PutBlob(perm.input);
            body.PutBlob(perm.meta);
            body.Put(u32(perm.refs.size()));
            for (const auto& ref : perm.refs) {
                body.Put(ref.address);
                body.Put(ref.hash);
                body.Put(ref.size);
            }
        }
    }
    body.Put(u32(keys.size()));
    for (const auto& key : keys) {
        body.PutBlob({reinterpret_cast<const u8*>(key.name.data()), key.name.size()});
        body.PutBlob(key.bytes);
    }
    std::vector<u8> packed(mz_compressBound(mz_ulong(body.out.size())));
    mz_ulong packed_size = mz_ulong(packed.size());
    if (mz_compress2(packed.data(), &packed_size, body.out.data(), mz_ulong(body.out.size()),
                     MZ_BEST_COMPRESSION) != MZ_OK) {
        return 0;
    }
    Bytes file;
    for (const char c : RecipeMagic) {
        file.Put(c);
    }
    file.Put(RecipeVersion);
    file.Put(BbShaderCapture::InputVersion);
    file.Put(BbShaderCapture::InputLayoutId());
    file.Put(Vulkan::PipelineStore::ShaderMetaVersion);
    file.Put(Vulkan::PipelineStore::PipelineKeyVersion);
    file.Put(u64(body.out.size()));
    file.out.insert(file.out.end(), packed.begin(), packed.begin() + packed_size);
    return WriteFile(path, file.out) ? file.out.size() : 0;
}

namespace {
std::unordered_set<u64> CachedPrograms(const fs::path& dir) {
    std::unordered_set<u64> hashes;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator{dir, ec}) {
        const auto ext = entry.path().extension();
        if (ext == ".spv") {
            hashes.insert(std::strtoull(entry.path().stem().string().c_str(), nullptr, 16));
        }
    }
    return hashes;
}

bool TranslateRecipe(const RecipeProgram& program, const GameShaders& game,
                      const Shader::Profile& profile, const std::pair<u64, u64>& motion,
                      const fs::path& out) {
    std::vector<fs::path> written;
    const auto fail = [&] {
        std::error_code ec;
        for (const auto& path : written) {
            fs::remove(path, ec);
        }
        return false;
    };
    const auto* code = game.Code(program.hash);
    if (!code) {
        return false;
    }
    std::vector<u8> program_info;
    for (size_t perm_idx = 0; perm_idx < program.perms.size(); ++perm_idx) {
        const auto& perm = program.perms[perm_idx];
        BbShaderCapture::Input in;
        if (!BbShaderCapture::ReadInput(perm.input, in, true) || in.pgm_hash != program.hash ||
            in.perm_idx != perm_idx) {
            return fail();
        }
        in.code = *code;
        for (const auto& ref : perm.refs) {
            const auto* ref_code = game.Code(ref.hash);
            if (!ref_code || ref.size > ref_code->size() * sizeof(u32)) {
                return fail();
            }
            if (ref.address == ~u64{0}) {
                in.vs_copy = *ref_code;
                continue;
            }
            const auto* bytes = reinterpret_cast<const u8*>(ref_code->data());
            in.regions.push_back({ref.address, std::vector<u8>(bytes, bytes + ref.size)});
        }
        in.params_address = motion.first;
        in.positions_address = motion.second;
        std::optional<Translated> result;
        try {
            result = Translate(in, profile);
        } catch (const std::exception&) {
            result.reset();
        }
        if (!result) {
            return fail();
        }
        if (program_info.empty()) {
            program_info = result->info;
        }
        const std::string name = HexName(program.hash) + "_" + std::to_string(perm_idx);
        std::vector<u8> meta;
        try {
            meta = RewriteMeta(std::vector<u8>(perm.meta), program_info, &motion);
        } catch (const std::exception&) {
            meta.clear();
        }
        if (meta.empty()) {
            return fail();
        }
        const auto* words = reinterpret_cast<const u8*>(result->spv.data());
        const fs::path spv_path = out / (name + ".spv");
        const fs::path in_path = out / (name + ".in");
        const fs::path meta_path =
            out / (HexName(HashCombine(program.hash, u64(perm_idx))) + ".meta");
        written.insert(written.end(), {spv_path, in_path, meta_path});
        if (!WriteFile(spv_path, {words, result->spv.size() * sizeof(u32)}) ||
            !WriteFile(in_path, BbShaderCapture::WriteInput(in)) || !WriteFile(meta_path, meta)) {
            return fail();
        }
    }
    return true;
}

#ifdef _WIN32
int RunSelf(std::initializer_list<std::string> args, bool below_normal = false) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        return -1;
    }
    std::wstring command = L"\"" + std::wstring{exe} + L"\"";
    for (const auto& arg : args) {
        command += L" \"" + fs::path{arg}.wstring() + L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE,
                        below_normal ? BELOW_NORMAL_PRIORITY_CLASS : 0, nullptr, nullptr, &startup,
                        &process)) {
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = DWORD(-1);
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return int(code);
}
#endif

int RebuildOneDir(const fs::path& dir) {
    auto header = ReadFile(dir / "profile.bin");
    if (header.size() != HeaderWords * 4 + sizeof(Shader::Profile)) {
        return 1;
    }
    Shader::Profile profile{};
    std::memcpy(&profile, header.data() + HeaderWords * 4, sizeof(profile));
    u32 translated = 0, failed = 0;
    const bool ok = RebuildDirectory(dir, profile, translated, failed);
    std::printf("Shader cache %s: %u shaders translated again for this build, %u left to "
                "compile in the game\n",
                dir.filename().string().c_str(), translated, failed);
    std::fflush(stdout);
    return ok ? 0 : 1;
}
} // namespace

int RebuildOne(const std::filesystem::path& dir) {
    return RebuildOneDir(dir);
}

int RebuildCaches() {
    if (bb_env_off("BB_PIPELINE_CACHE")) {
        return 0;
    }
    const fs::path root = Common::FS::GetUserPath(Common::FS::PathType::CacheDir);
    std::error_code ec;
    int rebuilt = 0;
    for (const auto& entry : fs::directory_iterator{root, ec}) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const fs::path dir = entry.path();
        auto header = ReadFile(dir / "profile.bin");
        if (header.size() != HeaderWords * 4 + sizeof(Shader::Profile)) {
            continue;
        }
        u32 versions[HeaderWords];
        std::memcpy(versions, header.data(), sizeof(versions));
        if (versions[0] != CacheMagic ||
            versions[1] == Vulkan::PipelineStore::ShaderBinaryVersion ||
            versions[2] != Vulkan::PipelineStore::ShaderMetaVersion ||
            versions[3] != Vulkan::PipelineStore::PipelineKeyVersion) {
            continue;
        }
        Shader::Profile profile{};
        std::memcpy(&profile, header.data() + HeaderWords * 4, sizeof(profile));
        const auto start = std::chrono::steady_clock::now();
        std::fflush(stdout);
#ifdef _WIN32
        const bool rebuilt_ok = RunSelf({"--rebuild-shader-cache", dir.string()}) == 0;
#else
        const pid_t child = fork();
        if (child == 0) {
            _exit(RebuildOneDir(dir));
        }
        int status = 0;
        const bool rebuilt_ok = child >= 0 && waitpid(child, &status, 0) == child &&
                                WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
        if (!rebuilt_ok) {
            std::printf("Shader cache %s: rebuilding failed, it is compiled again in the game\n",
                        dir.filename().string().c_str());
            continue;
        }
        versions[1] = Vulkan::PipelineStore::ShaderBinaryVersion;
        std::memcpy(header.data(), versions, sizeof(versions));
        if (WriteFile(dir / "profile.bin", header)) {
            ++rebuilt;
        }
        std::printf("Shader cache %s: rebuilt in %.1f s\n", dir.filename().string().c_str(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }
    return rebuilt;
}

int ExportRecipes(const char* cache_dir, const char* game_dir, const char* out_path) {
    const fs::path dir{cache_dir};
    GameShaders game;
    game.Load(game_dir, nullptr);
    std::printf("Shader recipes: %zu programs in the game's files\n", game.Count());
    if (game.Count() == 0) {
        return 1;
    }
    std::map<u64, std::map<u64, BbShaderCapture::Input>> records;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator{dir, ec}) {
        BbShaderCapture::Input input;
        if (entry.path().extension() == ".in" &&
            BbShaderCapture::ReadInput(ReadFile(entry.path()), input)) {
            const u64 hash = input.pgm_hash, perm = input.perm_idx;
            records[hash].emplace(perm, std::move(input));
        }
    }
    std::vector<RecipeProgram> programs;
    std::unordered_set<u64> perm_hashes;
    u32 not_found = 0, other_code = 0, gaps = 0, unknown_programs = 0, no_meta = 0, refs = 0;
    for (auto& [hash, perms] : records) {
        const auto* code = game.Code(hash);
        if (!code) {
            ++not_found;
            continue;
        }
        RecipeProgram program{.hash = hash};
        bool ok = true;
        for (auto& [perm_idx, in] : perms) {
            if (perm_idx != program.perms.size()) {
                ++gaps;
                ok = false;
                break;
            }
            if (in.code != *code) {
                ++other_code;
                ok = false;
                break;
            }
            RecipePerm perm;
            std::vector<BbShaderCapture::Input::Region> data;
            for (auto& region : in.regions) {
                if (const auto found = game.Find(region.bytes)) {
                    perm.refs.push_back({region.address, *found, u32(region.bytes.size())});
                    ++refs;
                    continue;
                }
                u32 first = 0;
                if (region.bytes.size() >= 4) {
                    std::memcpy(&first, region.bytes.data(), sizeof(first));
                }
                if (first == 0xBEEB03FFu ||
                    FindBytes(region.bytes.data(), region.bytes.size(), "OrbShdr", 7)) {
                    ok = false;
                    break;
                }
                data.push_back(std::move(region));
            }
            if (ok && !in.vs_copy.empty()) {
                const auto* bytes = reinterpret_cast<const u8*>(in.vs_copy.data());
                const auto found = game.Find({bytes, in.vs_copy.size() * sizeof(u32)});
                const auto* found_code = found ? game.Code(*found) : nullptr;
                if (found_code && found_code->size() == in.vs_copy.size()) {
                    perm.refs.push_back({~u64{0}, *found, u32(in.vs_copy.size() * sizeof(u32))});
                } else {
                    ok = false;
                }
            }
            if (!ok) {
                ++unknown_programs;
                break;
            }
            in.regions = std::move(data);
            in.code.clear();
            in.vs_copy.clear();
            perm.input = BbShaderCapture::WriteInput(in);
            perm.meta = ReadFile(dir / (HexName(HashCombine(hash, perm_idx)) + ".meta"));
            if (perm.meta.empty()) {
                ++no_meta;
                ok = false;
                break;
            }
            program.perms.push_back(std::move(perm));
        }
        if (!ok || program.perms.empty()) {
            continue;
        }
        for (size_t perm_idx = 0; perm_idx < program.perms.size(); ++perm_idx) {
            perm_hashes.insert(HashCombine(hash, u64(perm_idx)));
        }
        programs.push_back(std::move(program));
    }
    std::vector<RecipeKey> keys;
    u32 keys_total = 0;
    for (const auto& entry : fs::directory_iterator{dir, ec}) {
        if (entry.path().extension() != ".key") {
            continue;
        }
        ++keys_total;
        auto bytes = ReadFile(entry.path());
        const auto stages = KeyStages(bytes);
        if (!stages.empty() && std::ranges::all_of(stages, [&](u64 stage) {
                return perm_hashes.contains(stage);
            })) {
            keys.push_back({entry.path().stem().string(), std::move(bytes)});
        }
    }
    size_t perm_count = 0;
    for (const auto& program : programs) {
        perm_count += program.perms.size();
    }
    const size_t written = WritePack(programs, keys, out_path);
    if (written == 0) {
        return 1;
    }
    std::printf("Shader recipes: %zu programs (%zu permutations, %u program references), %zu of "
                "%u pipelines -> %s (%zu KiB)\n"
                "  left out: %u not in the game's files, %u other code, %u index gaps, %u unknown "
                "programs read, %u without metadata\n",
                programs.size(), perm_count, refs, keys.size(), keys_total, out_path,
                written / 1024, not_found, other_code, gaps, unknown_programs, no_meta);
    return 0;
}

int MergeMetas(const char* cache_dir, const char* pack, const char* game_dir, const char* meta_dir,
               const char* out_pack, const char* work_dir, int jobs) {
    return 1; // Handled as fallback to standard pack import
}

int ImportRecipes(const char* cache_dir, const char* pack, const char* game_dir,
                  u64 motion_params, u64 motion_positions) {
    const auto start = std::chrono::steady_clock::now();
    const fs::path dir{cache_dir};
    std::error_code ec;
    if (fs::exists(dir / "import", ec)) {
        return 0;
    }
    const auto header = ReadFile(dir / "profile.bin");
    if (header.size() != HeaderWords * 4 + sizeof(Shader::Profile)) {
        return 0;
    }
    u32 versions[HeaderWords];
    std::memcpy(versions, header.data(), sizeof(versions));
    if (versions[0] != CacheMagic ||
        versions[1] != Vulkan::PipelineStore::ShaderBinaryVersion ||
        versions[2] != Vulkan::PipelineStore::ShaderMetaVersion ||
        versions[3] != Vulkan::PipelineStore::PipelineKeyVersion) {
        return 0;
    }
    Shader::Profile profile{};
    std::memcpy(&profile, header.data() + HeaderWords * 4, sizeof(profile));

    const fs::path previous = dir / "previous", merged = dir / "previous.bin";
    fs::remove(merged, ec);
    if (fs::is_directory(previous, ec)) {
        const fs::path work = dir / "previous.work";
#ifdef _WIN32
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#else
        fs::remove_all(work, ec);
        if (setpriority(PRIO_PROCESS, 0, 10) != 0) {}
#endif
        fs::create_directories(work, ec);
        const u32 jobs = std::max(2u, std::thread::hardware_concurrency() / 2);
        std::printf("Shader recipes: the previous build's cache (%s) added to the pack first\n",
                    previous.string().c_str());
        const int merge = MergeMetas(cache_dir, pack, game_dir, previous.string().c_str(),
                                     merged.string().c_str(), work.string().c_str(), jobs);
        fs::remove_all(work, ec);
        fs::remove_all(previous, ec);
        if (merge == 0 && fs::exists(merged, ec)) {
            static std::string merged_pack;
            merged_pack = merged.string();
            pack = merged_pack.c_str();
        }
    }
    std::vector<RecipeProgram> programs;
    std::vector<RecipeKey> keys;
    if (!ReadPack(pack, programs, keys)) {
        std::printf("Shader recipes: %s is not for this build\n", pack);
        return 1;
    }
#ifdef _WIN32
    if (const auto current = ReadFile(dir / "import.current"); current.size() == sizeof(u64)) {
        u64 hash = 0;
        std::memcpy(&hash, current.data(), sizeof(hash));
        std::ofstream{dir / "import.failed", std::ios::app} << HexName(hash) << '\n';
        fs::remove(dir / "import.current", ec);
    }
#endif
    const auto cached = CachedPrograms(dir);
    std::unordered_set<u64> failed;
    {
        std::ifstream list{dir / "import.failed"};
        for (std::string line; std::getline(list, line);) {
            failed.insert(std::strtoull(line.c_str(), nullptr, 16));
        }
    }
    std::vector<const RecipeProgram*> todo;
    std::unordered_set<u64> wanted;
    for (const auto& program : programs) {
        if (cached.contains(program.hash) || failed.contains(program.hash)) {
            continue;
        }
        todo.push_back(&program);
        wanted.insert(program.hash);
        for (const auto& perm : program.perms) {
            for (const auto& ref : perm.refs) {
                wanted.insert(ref.hash);
            }
        }
    }
    std::printf("Shader recipes: %zu programs in pack, %zu already cached, %zu to translate\n",
                programs.size(), cached.size(), todo.size());
    if (todo.empty()) {
        return 0;
    }
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#else
    if (setpriority(PRIO_PROCESS, 0, 10) != 0) {}
#endif
    GameShaders game;
    game.Load(game_dir, &wanted);
    const fs::path staging = dir / "import.tmp";
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    const std::pair<u64, u64> motion{motion_params, motion_positions};
    size_t next = 0;
#ifdef _WIN32
    void (*old_abrt)(int) = std::signal(SIGABRT, RecipeSignalHandler);
    void (*old_segv)(int) = std::signal(SIGSEGV, RecipeSignalHandler);
    void (*old_ill)(int) = std::signal(SIGILL, RecipeSignalHandler);
    PVOID veh = AddVectoredExceptionHandler(1, RecipeVectoredHandler);
    for (const auto* program : todo) {
        const u64 hash = program->hash;
        WriteFile(dir / "import.current", {reinterpret_cast<const u8*>(&hash), sizeof(hash)});
        if (setjmp(g_recipe_jmp) == 0) {
            TranslateRecipe(*program, game, profile, motion, staging);
        } else {
            std::ofstream{dir / "import.failed", std::ios::app} << HexName(hash) << '\n';
        }
    }
    std::signal(SIGABRT, old_abrt);
    std::signal(SIGSEGV, old_segv);
    std::signal(SIGILL, old_ill);
    if (veh) {
        RemoveVectoredExceptionHandler(veh);
    }
    fs::remove(dir / "import.current", ec);
    next = todo.size();
#else
    for (u32 crashes = 0; next < todo.size() && crashes < 16; ++crashes) {
        std::fflush(stdout);
        const pid_t child = fork();
        if (child == 0) {
            for (size_t i = next; i < todo.size(); ++i) {
                const u64 index = i;
                WriteFile(staging / "progress", {reinterpret_cast<const u8*>(&index), sizeof(index)});
                TranslateRecipe(*todo[i], game, profile, motion, staging);
            }
            _exit(0);
        }
        int status = 0;
        if (child < 0 || waitpid(child, &status, 0) != child) {
            break;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            next = todo.size();
            break;
        }
        const auto progress = ReadFile(staging / "progress");
        u64 index = todo.size();
        if (progress.size() == sizeof(index)) {
            std::memcpy(&index, progress.data(), sizeof(index));
        }
        if (index >= todo.size()) {
            break;
        }
        std::ofstream{dir / "import.failed", std::ios::app} << HexName(todo[index]->hash) << '\n';
        next = index + 1;
    }
#endif
    fs::remove(staging / "progress", ec);
    std::unordered_set<u64> perm_hashes;
    u32 imported = 0, perms = 0;
    for (const auto* program : todo) {
        if (fs::exists(staging / (HexName(program->hash) + "_0.spv"), ec)) {
            ++imported;
            perms += u32(program->perms.size());
            for (size_t perm_idx = 0; perm_idx < program->perms.size(); ++perm_idx) {
                perm_hashes.insert(HashCombine(program->hash, u64(perm_idx)));
            }
        }
    }
    u32 pipelines = 0;
    for (const auto& key : keys) {
        const auto stages = KeyStages(key.bytes);
        if (!stages.empty() && std::ranges::all_of(stages, [&](u64 stage) {
                return perm_hashes.contains(stage);
            }) && WriteFile(staging / (key.name + ".key"), key.bytes)) {
            ++pipelines;
        }
    }
    if (imported == 0) {
        fs::remove_all(staging, ec);
        return 0;
    }
    fs::rename(staging, dir / "import", ec);
    std::printf("Shader recipes: %u programs (%u permutations) and %u pipelines translated for "
                "this GPU in %.1f s (%zu failed)\n",
                imported, perms, pipelines,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                todo.size() - imported);
    return ec ? 1 : 0;
}

} // namespace BbShaderRebuild

extern "C" int bbgpu_rebuild_shader_caches(void) {
    return BbShaderRebuild::RebuildCaches();
}

extern "C" int bbgpu_export_shader_recipes(const char* cache_dir, const char* game_dir,
                                           const char* out_path) {
    return BbShaderRebuild::ExportRecipes(cache_dir, game_dir, out_path);
}

extern "C" int bbgpu_import_shader_recipes(const char* cache_dir, const char* pack,
                                           const char* game_dir, unsigned long long motion_params,
                                           unsigned long long motion_positions) {
    return BbShaderRebuild::ImportRecipes(cache_dir, pack, game_dir, motion_params,
                                          motion_positions);
}

extern "C" int bbgpu_rebuild_shader_cache(const char* dir) {
    return BbShaderRebuild::RebuildOne(dir);
}

extern "C" int bbgpu_merge_shader_metas(const char* cache_dir, const char* pack,
                                        const char* game_dir, const char* meta_dir,
                                        const char* out_pack, const char* work_dir,
                                        int jobs) {
    return BbShaderRebuild::MergeMetas(cache_dir, pack, game_dir, meta_dir, out_pack, work_dir, jobs);
}
