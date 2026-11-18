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
#include <memory>
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
#include "common/scope_exit.h"
#include "common/serdes.h"
#include "core/signals.h"
#include "video_core/cache_storage.h"
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
constexpr const char* ImportFailedHeader = "# bbport-import-failed v2";
thread_local bool g_recipe_jmp_active = false;
thread_local std::unique_ptr<Shader::Pools> g_translate_pools;

#if defined(_WIN32) && defined(__x86_64__)
struct RecipeRecoverBuf {
    u64 rbx, rbp, rdi, rsi, r12, r13, r14, r15, rsp, rip;
    u8 xmm[10 * 16];
    u32 mxcsr;
    u16 fcw;
    u16 pad;
};
thread_local RecipeRecoverBuf g_recipe_jmp{};

extern "C" int bb_recipe_setjmp(RecipeRecoverBuf* buf) __attribute__((returns_twice));
extern "C" void bb_recipe_longjmp(RecipeRecoverBuf* buf) __attribute__((noreturn));
__asm__(".text\n"
        ".globl bb_recipe_setjmp\n"
        ".def bb_recipe_setjmp; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "bb_recipe_setjmp:\n"
        "  movq %rbx, 0(%rcx)\n  movq %rbp, 8(%rcx)\n  movq %rdi, 16(%rcx)\n  movq %rsi, 24(%rcx)\n"
        "  movq %r12, 32(%rcx)\n  movq %r13, 40(%rcx)\n  movq %r14, 48(%rcx)\n  movq %r15, 56(%rcx)\n"
        "  leaq 8(%rsp), %rdx\n  movq %rdx, 64(%rcx)\n  movq (%rsp), %rdx\n  movq %rdx, 72(%rcx)\n"
        "  movups %xmm6, 80(%rcx)\n  movups %xmm7, 96(%rcx)\n  movups %xmm8, 112(%rcx)\n"
        "  movups %xmm9, 128(%rcx)\n  movups %xmm10, 144(%rcx)\n  movups %xmm11, 160(%rcx)\n"
        "  movups %xmm12, 176(%rcx)\n  movups %xmm13, 192(%rcx)\n  movups %xmm14, 208(%rcx)\n"
        "  movups %xmm15, 224(%rcx)\n  stmxcsr 240(%rcx)\n  fnstcw 244(%rcx)\n"
        "  xorl %eax, %eax\n  ret\n"
        ".globl bb_recipe_longjmp\n"
        ".def bb_recipe_longjmp; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "bb_recipe_longjmp:\n"
        "  movq 0(%rcx), %rbx\n  movq 8(%rcx), %rbp\n  movq 16(%rcx), %rdi\n  movq 24(%rcx), %rsi\n"
        "  movq 32(%rcx), %r12\n  movq 40(%rcx), %r13\n  movq 48(%rcx), %r14\n  movq 56(%rcx), %r15\n"
        "  movups 80(%rcx), %xmm6\n  movups 96(%rcx), %xmm7\n  movups 112(%rcx), %xmm8\n"
        "  movups 128(%rcx), %xmm9\n  movups 144(%rcx), %xmm10\n  movups 160(%rcx), %xmm11\n"
        "  movups 176(%rcx), %xmm12\n  movups 192(%rcx), %xmm13\n  movups 208(%rcx), %xmm14\n"
        "  movups 224(%rcx), %xmm15\n  ldmxcsr 240(%rcx)\n  fldcw 244(%rcx)\n"
        "  movq 64(%rcx), %rsp\n  movl $1, %eax\n  jmpq *72(%rcx)\n");

#define BB_RECIPE_SETJMP() bb_recipe_setjmp(&g_recipe_jmp)

static void RecipeSignalHandler(int) {
    if (g_recipe_jmp_active) {
        g_recipe_jmp_active = false;
        bb_recipe_longjmp(&g_recipe_jmp);
    }
}

static LONG WINAPI RecipeVectoredHandler(PEXCEPTION_POINTERS ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
        void* fault_addr = ep->ExceptionRecord->NumberParameters >= 2
                               ? reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1])
                               : nullptr;
        if (Core::Signals::Instance()->DispatchAccessViolation(ep, fault_addr)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (g_recipe_jmp_active) {
            g_recipe_jmp_active = false;
            // Restore the recovery frame directly into ContextRecord instead of calling CRT
            // longjmp (RtlUnwindEx crashes inside Windows VEH / un-unwound JIT frames).
            auto* ctx = ep->ContextRecord;
            ctx->Rbx = g_recipe_jmp.rbx;
            ctx->Rbp = g_recipe_jmp.rbp;
            ctx->Rdi = g_recipe_jmp.rdi;
            ctx->Rsi = g_recipe_jmp.rsi;
            ctx->R12 = g_recipe_jmp.r12;
            ctx->R13 = g_recipe_jmp.r13;
            ctx->R14 = g_recipe_jmp.r14;
            ctx->R15 = g_recipe_jmp.r15;
            ctx->Rsp = g_recipe_jmp.rsp;
            ctx->Rip = g_recipe_jmp.rip;
            ctx->Rax = 1;
            std::memcpy(&ctx->Xmm6, g_recipe_jmp.xmm, sizeof(g_recipe_jmp.xmm));
            ctx->MxCsr = g_recipe_jmp.mxcsr;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
thread_local jmp_buf g_recipe_jmp;
#define BB_RECIPE_SETJMP() setjmp(g_recipe_jmp)
static void RecipeSignalHandler(int) {
    if (g_recipe_jmp_active) {
        g_recipe_jmp_active = false;
        longjmp(g_recipe_jmp, 1);
    }
}
#ifdef _WIN32
static LONG WINAPI RecipeVectoredHandler(PEXCEPTION_POINTERS ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (g_recipe_jmp_active &&
        (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)) {
        g_recipe_jmp_active = false;
        longjmp(g_recipe_jmp, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
#endif

void AppendImportFailed(const fs::path& path, u64 hash, bool with_header) {
    std::error_code ec;
    const bool need_header = with_header && (!fs::exists(path, ec) || fs::file_size(path, ec) == 0);
    std::ofstream out{path, std::ios::app};
    if (!out) {
        return;
    }
    if (need_header) {
        out << ImportFailedHeader << '\n';
    }
    out << HexName(hash) << '\n' << std::flush;
}

bool LoadImportFailed(const fs::path& path, std::unordered_set<u64>& out_failed,
                      bool require_header) {
    std::ifstream list{path};
    if (!list.is_open()) {
        return true;
    }
    std::string line;
    if (!std::getline(list, line)) {
        return true;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (require_header) {
        if (line != ImportFailedHeader) {
            return false; // legacy pre-fix blacklist; caller should clear and retry
        }
    } else if (!line.empty() && line[0] != '#') {
        out_failed.insert(std::strtoull(line.c_str(), nullptr, 16));
    }
    while (std::getline(list, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && line[0] != '#') {
            out_failed.insert(std::strtoull(line.c_str(), nullptr, 16));
        }
    }
    return true;
}
} // namespace

std::vector<u8> ReadFile(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>(file), {}};
}

namespace {
bool WriteFile(const fs::path& path, std::span<const u8> bytes) {
    fs::path temp = path;
    temp += ".tmp";
    std::error_code ec;
    {
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        file.flush();
        if (!file) {
            file.close();
            fs::remove(temp, ec);
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
#else
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
#endif
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

enum class RecipeFailReason {
    None,
    MissingCode,
    ReadInput,
    MissingRef,
    MapMemory,
    Translate,
    RewriteMeta,
    WriteFile,
    Fault,
};

struct RecipeFailStats {
    u32 missing_code = 0;
    u32 read_input = 0;
    u32 missing_ref = 0;
    u32 map_memory = 0;
    u32 translate = 0;
    u32 rewrite_meta = 0;
    u32 write_file = 0;
    u32 fault = 0;
    void Add(RecipeFailReason r) {
        switch (r) {
        case RecipeFailReason::MissingCode: ++missing_code; break;
        case RecipeFailReason::ReadInput: ++read_input; break;
        case RecipeFailReason::MissingRef: ++missing_ref; break;
        case RecipeFailReason::MapMemory: ++map_memory; break;
        case RecipeFailReason::Translate: ++translate; break;
        case RecipeFailReason::RewriteMeta: ++rewrite_meta; break;
        case RecipeFailReason::WriteFile: ++write_file; break;
        case RecipeFailReason::Fault: ++fault; break;
        case RecipeFailReason::None: break;
        }
    }
    u32 Total() const {
        return missing_code + read_input + missing_ref + map_memory + translate + rewrite_meta +
               write_file + fault;
    }
};

struct ReplayRegionView {
    VAddr address;
    const u8* data;
    size_t size;
};

thread_local const std::vector<ReplayRegionView>* g_replay_regions = nullptr;

const void* TranslateGuestReplayAddr(VAddr address, size_t size) {
    if (!g_replay_regions) {
        return nullptr;
    }
    for (const auto& r : *g_replay_regions) {
        if (address >= r.address && address - r.address <= r.size &&
            size <= r.size - (address - r.address)) {
            return r.data + (address - r.address);
        }
    }
    return nullptr;
}

std::optional<Translated> Translate(BbShaderCapture::Input& in, const Shader::Profile& profile,
                                    RecipeFailReason* out_reason = nullptr) {
    (void)out_reason;
    Shader::MotionVectors::params_address = in.params_address;
    Shader::MotionVectors::positions_address = in.positions_address;
    std::vector<ReplayRegionView> views;
    views.reserve(in.regions.size() + 1);
    for (const auto& region : in.regions) {
        views.push_back({region.address, region.bytes.data(), region.bytes.size()});
    }
    views.push_back({in.pgm_base, reinterpret_cast<const u8*>(in.code.data()),
                     in.code.size() * sizeof(u32)});
    auto runtime_info = in.runtime_info;
    if (in.hw_stage == Shader::HwStage::Geometry) {
        runtime_info.hw.gs.vs_copy = in.vs_copy;
    }
    const std::span<const u32> guest_code{in.code.data(), in.code.size()};
    Shader::Info info{in.hw_stage, in.sw_stage,
                      Shader::ShaderParams{std::span<const u32, 16>{in.user_data}, guest_code,
                                           in.pgm_hash}};
    info.pgm_base = in.pgm_base;
    g_replay_regions = &views;
    Shader::guest_mem_translate = TranslateGuestReplayAddr;
    Shader::flat_buf_replay = &in.flat;
    SCOPE_EXIT {
        Shader::flat_buf_replay = nullptr;
        Shader::guest_mem_translate = nullptr;
        g_replay_regions = nullptr;
    };
    if (!g_translate_pools) {
        g_translate_pools = std::make_unique<Shader::Pools>();
    }
    auto bindings = in.bindings;
    const auto program =
        Shader::TranslateProgram(guest_code, *g_translate_pools, info, runtime_info, profile);
    Translated out;
    out.spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
    Serialization::Archive archive;
    info.Serialize(archive);
    out.info = archive.TakeOff();
    return out;
}

std::vector<u8> RewriteMeta(std::vector<u8>&& meta, const std::vector<u8>& info,
                            std::optional<Shader::HwStage> expected_hw = std::nullopt,
                            std::optional<Shader::SwStage> expected_sw = std::nullopt) {
    /* bbport: on-disk metas written by current builds store (meta_version, binary_version,
     * perm_hash, perm_idx, StageSpecialization, Info). Earlier pre-ce83b80 metas inside mixed
     * shader_recipes.bin packs also included two u64 motion-buffer addresses (16 bytes) right
     * after perm_idx under the same ShaderMetaVersion=8. Try the current layout first and fall
     * back to skipping the 16-byte legacy header, validating bitset length (128) and stage enums
     * so a shifted meta can never be silently accepted. Output is always normalized to current. */
    auto try_parse = [&](bool skip_legacy_motion, u64& perm_hash, u64& perm_idx,
                         Shader::StageSpecialization& spec) -> bool {
        try {
            Serialization::Archive ar{std::vector<u8>{meta}};
            Serialization::Reader reader{ar};
            u32 meta_version{}, binary_version{};
            reader.Read(meta_version);
            reader.Read(binary_version);
            if (meta_version != Vulkan::PipelineStore::ShaderMetaVersion) {
                return false;
            }
            (void)binary_version; // the pack may predate the current compiler; it is re-emitted below
            reader.Read(perm_hash);
            reader.Read(perm_idx);
            if (skip_legacy_motion) {
                u64 motion_params{}, motion_positions{};
                reader.Read(motion_params);
                reader.Read(motion_positions);
            }
            {
                Serialization::Archive probe{std::vector<u8>{meta}};
                Serialization::Reader pr{probe};
                probe.Advance(sizeof(u32) * 2 + sizeof(u64) * 2 +
                              (skip_legacy_motion ? sizeof(u64) * 2 : 0) +
                              sizeof(Shader::Backend::Bindings) + sizeof(Shader::RuntimeInfo));
                size_t bits_len{};
                pr.Read(bits_len);
                if (bits_len != Shader::StageSpecialization::MaxStageResources) {
                    return false;
                }
            }
            Shader::Info old_info{};
            spec = {};
            spec.info = &old_info;
            spec.Deserialize(ar);
            if (static_cast<u32>(spec.runtime_info.hw_stage) >
                    static_cast<u32>(Shader::HwStage::Compute) ||
                static_cast<u32>(spec.runtime_info.sw_stage) >=
                    static_cast<u32>(Shader::SwStage::NumLogicalStages)) {
                return false;
            }
            if (expected_hw && spec.runtime_info.hw_stage != *expected_hw) {
                return false;
            }
            if (expected_sw && spec.runtime_info.sw_stage != *expected_sw) {
                return false;
            }
            return !ar.IsEoS();
        } catch (const std::exception&) {
            return false;
        }
    };

    u64 perm_hash{}, perm_idx{};
    Shader::StageSpecialization spec{};
    if (!try_parse(false, perm_hash, perm_idx, spec) &&
        !try_parse(true, perm_hash, perm_idx, spec)) {
        return {};
    }
    Serialization::Archive out;
    Serialization::Writer writer{out};
    writer.Write(Vulkan::PipelineStore::ShaderMetaVersion);
    writer.Write(Vulkan::PipelineStore::ShaderBinaryVersion);
    writer.Write(perm_hash);
    writer.Write(perm_idx);
    spec.Serialize(out);
    if (!info.empty()) {
        writer.Write(info.data(), info.size());
    }
    return out.TakeOff();
}

bool RebuildDirectory(const fs::path& dir, const Shader::Profile& profile, u32& translated,
                      u32& failed) {
#ifdef _WIN32
    void (*old_abrt)(int) = std::signal(SIGABRT, RecipeSignalHandler);
    void (*old_segv)(int) = std::signal(SIGSEGV, RecipeSignalHandler);
    void (*old_ill)(int) = std::signal(SIGILL, RecipeSignalHandler);
    PVOID veh = AddVectoredExceptionHandler(1, RecipeVectoredHandler);
#endif
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
        for (auto& [perm_idx, record] : perms) {
            std::optional<Translated> result;
#ifdef _WIN32
            if (BB_RECIPE_SETJMP() == 0) {
                g_recipe_jmp_active = true;
                try {
                    result = Translate(record.input, profile);
                } catch (const std::exception&) {
                    result.reset();
                }
                g_recipe_jmp_active = false;
            } else {
                g_recipe_jmp_active = false;
                GuestMemory::ReleaseActive();
                Shader::flat_buf_replay = nullptr;
                Shader::guest_mem_translate = nullptr;
                g_replay_regions = nullptr;
                g_translate_pools.reset();
                result.reset();
            }
#else
            try {
                result = Translate(record.input, profile);
            } catch (const std::exception&) {
                result.reset();
            }
#endif
            const std::string name = HexName(pgm_hash) + "_" + std::to_string(perm_idx);
            if (!result) {
                fs::remove(dir / (name + ".spv"), ec);
                fs::remove(dir / (HexName(HashCombine(pgm_hash, perm_idx)) + ".meta"), ec);
                ++failed;
                continue;
            }
            const auto* words = reinterpret_cast<const u8*>(result->spv.data());
            if (!WriteFile(dir / (name + ".spv"), {words, result->spv.size() * sizeof(u32)})) {
                return false;
            }
            const fs::path meta_path = dir / (HexName(HashCombine(pgm_hash, perm_idx)) + ".meta");
            if (fs::exists(meta_path, ec)) {
                std::vector<u8> meta;
                try {
                    meta = RewriteMeta(ReadFile(meta_path), result->info, record.input.hw_stage,
                                       record.input.sw_stage);
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
#ifdef _WIN32
    std::signal(SIGABRT, old_abrt);
    std::signal(SIGSEGV, old_segv);
    std::signal(SIGILL, old_ill);
    if (veh) {
        RemoveVectoredExceptionHandler(veh);
    }
#endif
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

std::vector<u64> KeyStages(std::span<const u8> bytes, bool skip_motion_vectors = false) {
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
    if (skip_motion_vectors && key.motion_vectors) {
        return {};
    }
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
    std::unordered_set<u64> complete;
    std::unordered_set<u64> incomplete;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator{dir, ec}) {
        if (entry.path().extension() != ".spv") {
            continue;
        }
        const std::string stem = entry.path().stem().string();
        const size_t sep = stem.rfind('_');
        if (sep == std::string::npos) {
            continue;
        }
        const u64 pgm_hash = std::strtoull(stem.substr(0, sep).c_str(), nullptr, 16);
        const u64 perm_idx = std::strtoull(stem.substr(sep + 1).c_str(), nullptr, 10);
        const fs::path meta = dir / (HexName(HashCombine(pgm_hash, perm_idx)) + ".meta");
        if (fs::exists(meta, ec)) {
            complete.insert(pgm_hash);
        } else {
            incomplete.insert(pgm_hash);
        }
    }
    for (const u64 hash : incomplete) {
        complete.erase(hash);
    }
    return complete;
}

bool TranslateRecipe(const RecipeProgram& program, const GameShaders& game,
                      const Shader::Profile& profile, const std::pair<u64, u64>& motion,
                      const fs::path& out, RecipeFailReason* out_reason = nullptr) {
    std::vector<fs::path> written;
    const auto fail = [&](RecipeFailReason reason) {
        if (out_reason) {
            *out_reason = reason;
        }
        std::error_code ec;
        for (const auto& path : written) {
            fs::remove(path, ec);
        }
        return false;
    };
    const auto* code = game.Code(program.hash);
    if (!code) {
        return fail(RecipeFailReason::MissingCode);
    }
    for (size_t perm_idx = 0; perm_idx < program.perms.size(); ++perm_idx) {
        const auto& perm = program.perms[perm_idx];
        BbShaderCapture::Input in;
        if (!BbShaderCapture::ReadInput(perm.input, in, true) || in.pgm_hash != program.hash ||
            in.perm_idx != perm_idx) {
            return fail(RecipeFailReason::ReadInput);
        }
        in.code = *code;
        for (const auto& ref : perm.refs) {
            const auto* ref_code = game.Code(ref.hash);
            if (!ref_code || ref.size > ref_code->size() * sizeof(u32)) {
                return fail(RecipeFailReason::MissingRef);
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
        RecipeFailReason tr_reason = RecipeFailReason::Translate;
        try {
            result = Translate(in, profile, &tr_reason);
        } catch (const std::exception&) {
            result.reset();
        }
        if (!result) {
            return fail(tr_reason);
        }
        const std::string name = HexName(program.hash) + "_" + std::to_string(perm_idx);
        std::vector<u8> meta;
        try {
            meta = RewriteMeta(std::vector<u8>(perm.meta), result->info, in.hw_stage, in.sw_stage);
        } catch (const std::exception&) {
            meta.clear();
        }
        if (meta.empty()) {
            return fail(RecipeFailReason::RewriteMeta);
        }
        const auto* words = reinterpret_cast<const u8*>(result->spv.data());
        const fs::path spv_path = out / (name + ".spv");
        const fs::path in_path = out / (name + ".in");
        const fs::path meta_path =
            out / (HexName(HashCombine(program.hash, u64(perm_idx))) + ".meta");
        written.insert(written.end(), {spv_path, in_path, meta_path});
        if (!WriteFile(spv_path, {words, result->spv.size() * sizeof(u32)}) ||
            !WriteFile(in_path, BbShaderCapture::WriteInput(in)) || !WriteFile(meta_path, meta)) {
            return fail(RecipeFailReason::WriteFile);
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
        if (!Storage::IsCacheEpochCurrent(dir, Vulkan::PipelineStore::CacheEpoch)) {
            continue;
        }
        auto header = ReadFile(dir / "profile.bin");
        if (header.size() != HeaderWords * 4 + sizeof(Shader::Profile)) {
            continue;
        }
        u32 versions[HeaderWords];
        std::memcpy(versions, header.data(), sizeof(versions));
        if (versions[0] != CacheMagic ||
            versions[1] == Vulkan::PipelineStore::ShaderBinaryVersion) {
            continue;
        }
        /* bbport: before the versions were unified, the rebuild tool stamped profile.bin with its
         * own stale copy of the binary version (24) after translating every shader with the
         * current compiler. When the meta and key versions are current too, the shaders on disk
         * are current as well and LoadShaderMeta checks each meta on its own, so the header is
         * corrected here instead of re-translating the whole cache on the next launch. Any other
         * mismatch keeps the old rebuild path. */
        constexpr u32 StaleToolBinaryVersion = 24u;
        if (versions[1] == StaleToolBinaryVersion &&
            versions[2] == Vulkan::PipelineStore::ShaderMetaVersion &&
            versions[3] == Vulkan::PipelineStore::PipelineKeyVersion) {
            versions[1] = Vulkan::PipelineStore::ShaderBinaryVersion;
            std::memcpy(header.data(), versions, sizeof(versions));
            if (WriteFile(dir / "profile.bin", header)) {
                std::printf("Shader cache %s: version header updated to %u without "
                            "re-translating\n",
                            dir.filename().string().c_str(),
                            Vulkan::PipelineStore::ShaderBinaryVersion);
                std::fflush(stdout);
            }
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
            versions[1] = Vulkan::PipelineStore::ShaderBinaryVersion;
            std::memcpy(header.data(), versions, sizeof(versions));
            WriteFile(dir / "profile.bin", header);
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

/// True when every piece of guest code a recipe needs is present in this game build (program and
/// all referenced shaders). TranslateRecipe() fails immediately when something is missing, and the
/// answer cannot change until either the pack or the game build does.
bool RecipeCodeAvailable(const RecipeProgram& program, const GameShaders& game) {
    if (!game.Code(program.hash)) {
        return false;
    }
    for (const auto& perm : program.perms) {
        for (const auto& ref : perm.refs) {
            const auto* ref_code = game.Code(ref.hash);
            if (!ref_code || ref.size > ref_code->size() * sizeof(u32)) {
                return false;
            }
        }
    }
    return true;
}

/// True when every permutation of `program` has its complete .spv, .in, and .meta files in `staging`.
bool ProgramStagedComplete(const RecipeProgram& program, const fs::path& staging) {
    if (program.perms.empty()) {
        return false;
    }
    std::error_code ec;
    const std::string hex = HexName(program.hash);
    for (size_t perm_idx = 0; perm_idx < program.perms.size(); ++perm_idx) {
        const std::string stem = hex + "_" + std::to_string(perm_idx);
        const std::string meta = HexName(HashCombine(program.hash, u64(perm_idx))) + ".meta";
        if (!fs::exists(staging / (stem + ".spv"), ec) ||
            !fs::exists(staging / (stem + ".in"), ec) || !fs::exists(staging / meta, ec)) {
            return false;
        }
    }
    return true;
}

/// Validates an existing staging directory against `profile_header`. Preserves already-translated
/// shaders across interrupted launches, removes only partial `.tmp` write artifacts, and wipes the
/// directory only if the GPU/driver/build signature changed or staged metas have mismatched versions.
void PrepareStagingDir(const fs::path& staging, const std::vector<u8>& profile_header) {
    std::error_code ec;
    const fs::path sig_path = staging / "profile.sig";
    bool keep = false;
    if (fs::exists(staging, ec)) {
        if (fs::exists(sig_path, ec)) {
            keep = (ReadFile(sig_path) == profile_header);
        } else {
            keep = true;
            for (const auto& entry : fs::directory_iterator{staging, ec}) {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".meta") {
                    const auto bytes = ReadFile(entry.path());
                    if (bytes.size() < 8) {
                        keep = false;
                        break;
                    }
                    u32 meta_ver = 0, bin_ver = 0;
                    std::memcpy(&meta_ver, bytes.data(), sizeof(meta_ver));
                    std::memcpy(&bin_ver, bytes.data() + 4, sizeof(bin_ver));
                    if (meta_ver != Vulkan::PipelineStore::ShaderMetaVersion ||
                        bin_ver != Vulkan::PipelineStore::ShaderBinaryVersion) {
                        keep = false;
                    }
                    break;
                }
            }
        }
        if (!keep) {
            fs::remove_all(staging, ec);
        } else {
            for (const auto& entry : fs::directory_iterator{staging, ec}) {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".tmp") {
                    fs::remove(entry.path(), ec);
                }
            }
        }
    }
    fs::create_directories(staging, ec);
    WriteFile(sig_path, profile_header);
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

    std::vector<RecipeProgram> programs;
    std::vector<RecipeKey> keys;
    if (!ReadPack(pack, programs, keys)) {
        std::printf("Shader recipes: %s is not for this build\n", pack);
        return 1;
    }
#ifdef _WIN32
    u32 shard_idx = 0, shard_count = 1;
    if (const char* shard_env = bb_env("BB_IMPORT_SHARD")) {
        u32 idx = 0, cnt = 0;
        if (std::sscanf(shard_env, "%u/%u", &idx, &cnt) == 2 && cnt > 1 && idx < cnt) {
            shard_idx = idx;
            shard_count = cnt;
        }
    }
    const bool is_shard = shard_count > 1;
    const fs::path cur_path =
        is_shard ? (dir / ("import.current." + std::to_string(shard_idx))) : (dir / "import.current");
    const fs::path fail_path =
        is_shard ? (dir / ("import.failed." + std::to_string(shard_idx))) : (dir / "import.failed");
    if (is_shard) {
        if (const auto current = ReadFile(cur_path); current.size() == sizeof(u64)) {
            u64 hash = 0;
            std::memcpy(&hash, current.data(), sizeof(hash));
            AppendImportFailed(fail_path, hash, false);
            fs::remove(cur_path, ec);
        }
    }
#endif
    const auto cached = CachedPrograms(dir);
    std::unordered_set<u64> failed;
    const fs::path main_fail_path = dir / "import.failed";
    if (!LoadImportFailed(main_fail_path, failed, true)) {
        failed.clear();
#ifdef _WIN32
        if (!is_shard) {
            fs::remove(main_fail_path, ec);
            std::printf("Shader recipes: cleared legacy import.failed blacklist; retrying "
                        "previously skipped programs\n");
            std::fflush(stdout);
        }
#else
        fs::remove(main_fail_path, ec);
        std::printf("Shader recipes: cleared legacy import.failed blacklist; retrying "
                    "previously skipped programs\n");
        std::fflush(stdout);
#endif
    }
#ifdef _WIN32
    if (is_shard) {
        LoadImportFailed(fail_path, failed, false);
    } else {
        // Consolidate any per-shard failure logs left behind by interrupted prior runs so the
        // same failing shaders are never retried on subsequent launches.
        for (const auto& entry : fs::directory_iterator{dir, ec}) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const std::string fname = entry.path().filename().string();
            if (fname.starts_with("import.failed.")) {
                std::unordered_set<u64> shard_failed;
                LoadImportFailed(entry.path(), shard_failed, false);
                for (const u64 h : shard_failed) {
                    if (failed.insert(h).second) {
                        AppendImportFailed(main_fail_path, h, true);
                    }
                }
                fs::remove(entry.path(), ec);
            } else if (fname == "import.current" || fname.starts_with("import.current.")) {
                // Remove stale in-progress markers from an interrupted exit without blacklisting.
                fs::remove(entry.path(), ec);
            }
        }
    }
#endif
    std::vector<const RecipeProgram*> todo;
    std::unordered_set<u64> wanted;
    for (const auto& program : programs) {
        if (cached.contains(program.hash) || failed.contains(program.hash)) {
            continue;
        }
        todo.push_back(&program);
    }
    const fs::path staging = dir / "import.tmp";
    if (todo.empty()) {
#ifdef _WIN32
        if (!is_shard) {
            fs::remove_all(staging, ec);
            std::printf("Shader recipes: all %zu pack programs up to date (%zu cached, %zu failed)\n",
                        programs.size(), cached.size(), failed.size());
            std::fflush(stdout);
        }
#else
        fs::remove_all(staging, ec);
        std::printf("Shader recipes: all %zu pack programs up to date (%zu cached, %zu failed)\n",
                    programs.size(), cached.size(), failed.size());
        std::fflush(stdout);
#endif
        return 0;
    }
    const std::pair<u64, u64> motion{motion_params, motion_positions};
#ifdef _WIN32
    const u32 host_threads = std::thread::hardware_concurrency();
    const char* fg_env = bb_env("BB_IMPORT_FOREGROUND");
    const bool foreground = fg_env && fg_env[0] == '1';
    const DWORD proc_prio =
        foreground ? NORMAL_PRIORITY_CLASS
                   : (host_threads <= 4 ? IDLE_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS);
    SetPriorityClass(GetCurrentProcess(), proc_prio);
    RecipeFailStats fail_stats{};
    auto run_win_recipes = [&](const std::vector<const RecipeProgram*>& list,
                               const fs::path& current_file, const fs::path& failed_file) {
        std::unordered_set<u64> local_wanted;
        for (const auto* program : list) {
            local_wanted.insert(program->hash);
            for (const auto& perm : program->perms) {
                for (const auto& ref : perm.refs) {
                    local_wanted.insert(ref.hash);
                }
            }
        }
        GameShaders game;
        game.Load(game_dir, &local_wanted);
        void (*old_abrt)(int) = std::signal(SIGABRT, RecipeSignalHandler);
        void (*old_segv)(int) = std::signal(SIGSEGV, RecipeSignalHandler);
        void (*old_ill)(int) = std::signal(SIGILL, RecipeSignalHandler);
        PVOID veh = AddVectoredExceptionHandler(1, RecipeVectoredHandler);
        for (const auto* program : list) {
            const u64 hash = program->hash;
            if (ProgramStagedComplete(*program, staging)) {
                continue;
            }
            WriteFile(current_file, {reinterpret_cast<const u8*>(&hash), sizeof(hash)});
            if (BB_RECIPE_SETJMP() == 0) {
                g_recipe_jmp_active = true;
                RecipeFailReason reason = RecipeFailReason::None;
                if (!TranslateRecipe(*program, game, profile, motion, staging, &reason)) {
                    fail_stats.Add(reason);
                    if (failed.insert(hash).second) {
                        AppendImportFailed(failed_file, hash, !is_shard);
                    }
                }
                g_recipe_jmp_active = false;
            } else {
                g_recipe_jmp_active = false;
                GuestMemory::ReleaseActive();
                Shader::flat_buf_replay = nullptr;
                Shader::guest_mem_translate = nullptr;
                g_replay_regions = nullptr;
                g_translate_pools.reset();
                fail_stats.Add(RecipeFailReason::Fault);
                if (failed.insert(hash).second) {
                    AppendImportFailed(failed_file, hash, !is_shard);
                }
            }
        }
        std::signal(SIGABRT, old_abrt);
        std::signal(SIGSEGV, old_segv);
        std::signal(SIGILL, old_ill);
        if (veh) {
            RemoveVectoredExceptionHandler(veh);
        }
        fs::remove(current_file, ec);
    };

    if (is_shard) {
        std::vector<const RecipeProgram*> shard_todo;
        shard_todo.reserve((todo.size() + shard_count - 1) / shard_count);
        for (size_t i = shard_idx; i < todo.size(); i += shard_count) {
            if (!ProgramStagedComplete(*todo[i], staging)) {
                shard_todo.push_back(todo[i]);
            }
        }
        if (!shard_todo.empty()) {
            run_win_recipes(shard_todo, cur_path, fail_path);
        }
        return 0;
    }

    PrepareStagingDir(staging, header);
    {
        const std::string total_str = std::to_string(todo.size());
        WriteFile(staging / "total.txt",
                  {reinterpret_cast<const u8*>(total_str.data()), total_str.size()});
    }
    std::vector<const RecipeProgram*> pending_todo;
    pending_todo.reserve(todo.size());
    for (const auto* program : todo) {
        if (!ProgramStagedComplete(*program, staging)) {
            pending_todo.push_back(program);
        }
    }
    const size_t already_staged = todo.size() - pending_todo.size();
    const u32 shards = (pending_todo.size() >= 32 &&
                        (foreground ? host_threads >= 4 : host_threads > 4))
                           ? std::clamp(host_threads / 2, 2u, 4u)
                           : 1u;
    std::printf("Shader recipes: %zu programs in pack, %zu cached, %zu staged, %zu failed, %zu "
                "left to translate (%s, %u %s)\n",
                programs.size(), cached.size(), already_staged, failed.size(), pending_todo.size(),
                foreground ? "boot" : "background", shards, shards == 1 ? "worker" : "shards");
    std::fflush(stdout);
    if (shards > 1) {
        wchar_t exe[MAX_PATH];
        const bool have_exe = GetModuleFileNameW(nullptr, exe, MAX_PATH) != 0;
        std::wstring base_cmd;
        if (have_exe) {
            base_cmd = L"\"" + std::wstring{exe} + L"\" --import-shaders";
            for (const std::string& arg :
                 {std::string{cache_dir}, std::string{pack}, std::string{game_dir},
                  fmt::format("{:x}", motion_params), fmt::format("{:x}", motion_positions)}) {
                base_cmd += L" \"" + fs::path{arg}.wstring() + L"\"";
            }
        }
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        }
        auto spawn_shard = [&](u32 s) -> HANDLE {
            const std::string spec = std::to_string(s) + "/" + std::to_string(shards);
            SetEnvironmentVariableA("BB_IMPORT_SHARD", spec.c_str());
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION pi{};
            std::wstring cmd = base_cmd;
            HANDLE proc = nullptr;
            if (have_exe && CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE,
                                           foreground ? NORMAL_PRIORITY_CLASS
                                                      : BELOW_NORMAL_PRIORITY_CLASS,
                                           nullptr, nullptr, &startup, &pi)) {
                if (job) {
                    AssignProcessToJobObject(job, pi.hProcess);
                }
                CloseHandle(pi.hThread);
                proc = pi.hProcess;
            }
            SetEnvironmentVariableA("BB_IMPORT_SHARD", nullptr);
            return proc;
        };
        std::vector<HANDLE> procs(shards, nullptr);
        std::vector<u32> restarts(shards, 0);
        for (u32 s = 0; s < shards; ++s) {
            procs[s] = spawn_shard(s);
        }
        for (u32 s = 0; s < shards; ++s) {
            while (procs[s]) {
                WaitForSingleObject(procs[s], INFINITE);
                DWORD code = 0;
                GetExitCodeProcess(procs[s], &code);
                CloseHandle(procs[s]);
                procs[s] = nullptr;
                if (code == 0 || restarts[s] >= 8) {
                    break;
                }
                ++restarts[s];
                procs[s] = spawn_shard(s);
            }
        }
        if (job) {
            CloseHandle(job);
        }
        for (u32 s = 0; s < shards; ++s) {
            const fs::path sc = dir / ("import.current." + std::to_string(s));
            const fs::path sf = dir / ("import.failed." + std::to_string(s));
            if (const auto cur = ReadFile(sc); cur.size() == sizeof(u64)) {
                u64 h = 0;
                std::memcpy(&h, cur.data(), sizeof(h));
                if (failed.insert(h).second) {
                    AppendImportFailed(main_fail_path, h, true);
                }
            }
            std::unordered_set<u64> shard_failed;
            LoadImportFailed(sf, shard_failed, false);
            for (const u64 h : shard_failed) {
                if (failed.insert(h).second) {
                    AppendImportFailed(main_fail_path, h, true);
                }
            }
            fs::remove(sc, ec);
            fs::remove(sf, ec);
        }
    } else if (!pending_todo.empty()) {
        run_win_recipes(pending_todo, cur_path, fail_path);
    }
#else
    std::printf("Shader recipes: %zu programs in pack, %zu already cached, %zu to translate\n",
                programs.size(), cached.size(), todo.size());
    for (const auto* program : todo) {
        wanted.insert(program->hash);
        for (const auto& perm : program->perms) {
            for (const auto& ref : perm.refs) {
                wanted.insert(ref.hash);
            }
        }
    }
    if (setpriority(PRIO_PROCESS, 0, 10) != 0) {}
    GameShaders game;
    game.Load(game_dir, &wanted);
    PrepareStagingDir(staging, header);
    size_t next = 0;
    for (u32 crashes = 0; next < todo.size() && crashes < 16; ++crashes) {
        std::fflush(stdout);
        const pid_t child = fork();
        if (child == 0) {
            for (size_t i = next; i < todo.size(); ++i) {
                if (ProgramStagedComplete(*todo[i], staging)) {
                    continue;
                }
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
        AppendImportFailed(main_fail_path, todo[index]->hash, true);
        failed.insert(todo[index]->hash);
        next = index + 1;
    }
#endif
    fs::remove(staging / "progress", ec);
    fs::remove(staging / "profile.sig", ec);
    fs::remove(staging / "total.txt", ec);
    std::unordered_set<u64> perm_hashes;
    // Include permutations already present in the permanent cache so pipeline keys combining
    // previously cached stages with newly imported stages are also emitted.
    for (const auto& entry : fs::directory_iterator{dir, ec}) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".meta") {
            const std::string stem = entry.path().stem().string();
            if (!stem.empty()) {
                perm_hashes.insert(std::strtoull(stem.c_str(), nullptr, 16));
            }
        }
    }
    u32 imported = 0, perms = 0;
    for (const auto* program : todo) {
        if (ProgramStagedComplete(*program, staging)) {
            ++imported;
            perms += u32(program->perms.size());
            for (size_t perm_idx = 0; perm_idx < program->perms.size(); ++perm_idx) {
                perm_hashes.insert(HashCombine(program->hash, u64(perm_idx)));
            }
        }
#ifndef _WIN32
        else if (!failed.contains(program->hash) && !RecipeCodeAvailable(*program, game)) {
            AppendImportFailed(main_fail_path, program->hash, true);
        }
#endif
    }
    u32 pipelines = 0;
    for (const auto& key : keys) {
        const fs::path perm_key_path = dir / (key.name + ".key");
        if (fs::exists(perm_key_path, ec)) {
            continue;
        }
        const auto stages = KeyStages(key.bytes, true);
        if (!stages.empty() && std::ranges::all_of(stages, [&](u64 stage) {
                return perm_hashes.contains(stage);
            }) && WriteFile(staging / (key.name + ".key"), key.bytes)) {
            ++pipelines;
        }
    }
    if (imported == 0 && pipelines == 0) {
        fs::remove_all(staging, ec);
        return 0;
    }
    fs::rename(staging, dir / "import", ec);
    std::printf("Shader recipes: %u programs (%u permutations) and %u pipelines translated for "
                "this GPU in %.1f s (%zu failed)\n",
                imported, perms, pipelines,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                todo.size() - imported);
#ifdef _WIN32
    if (fail_stats.Total() > 0) {
        std::printf("Shader recipes: failure breakdown: missing_code=%u read_input=%u "
                    "missing_ref=%u map_mem=%u translate=%u rewrite_meta=%u write_file=%u "
                    "fault=%u\n",
                    fail_stats.missing_code, fail_stats.read_input, fail_stats.missing_ref,
                    fail_stats.map_memory, fail_stats.translate, fail_stats.rewrite_meta,
                    fail_stats.write_file, fail_stats.fault);
    }
#endif
    std::fflush(stdout);
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
