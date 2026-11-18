// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: rewrites PS4 System V x86-64 leaf functions that store locals in the 128-byte red zone
// ([rsp - 0x80 .. rsp - 0x01]) into Windows-safe stack frames by expanding the prologue/epilogue
// stack allocation by +0x80 bytes and shifting every [rsp + disp] operand by +0x80.
// On Windows x64, exception dispatches (such as GPU page-tracking write faults) write a CONTEXT
// frame below RSP, destroying any locals left in the System V red zone.

#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

#include <Zydis/Decoder.h>
#include <Zydis/Encoder.h>

extern "C" int bbgpu_rewrite_redzone_leaf(const uint8_t* src, size_t src_len, uint64_t src_addr,
                                          uint8_t* dst, size_t dst_cap, uint64_t dst_addr,
                                          size_t* out_len) {
    if (!src || !dst || !out_len || src_len == 0 || dst_cap == 0) {
        return 0;
    }

    constexpr int64_t kRedzonePad = 0x80;

    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                       ZYDIS_STACK_WIDTH_64))) {
        return 0;
    }

    enum class InstKind {
        Verbatim,
        PrologueAlloc,
        EpilogueFree,
        RspShift,
        RipRel,
        RelBranch,
    };

    struct DecodedItem {
        size_t old_off = 0;
        size_t old_len = 0;
        size_t new_off = 0;
        size_t new_len = 0;
        InstKind kind = InstKind::Verbatim;
        ZydisEncoderRequest req{};
        uint64_t rip_target = 0;
        int rip_op_idx = -1;
        size_t branch_target_off = 0;
        uint8_t pre_encoded[ZYDIS_MAX_INSTRUCTION_LENGTH]{};
    };

    std::vector<DecodedItem> items;
    items.reserve(src_len / 3);
    std::unordered_map<size_t, size_t> old_off_to_idx;

    bool in_frame = false;
    bool seen_prologue = false;
    bool seen_epilogue = false;
    int64_t orig_frame_size = 0;

    // Pass 0: decode and validate the entire leaf function.
    size_t off = 0;
    while (off < src_len) {
        ZydisDecodedInstruction inst{};
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, src + off, src_len - off, &inst, ops))) {
            return 0;
        }
        if (inst.length == 0 || off + inst.length > src_len) {
            return 0;
        }
        if (inst.mnemonic == ZYDIS_MNEMONIC_CALL) {
            return 0; // Not a leaf function.
        }

        DecodedItem item{};
        item.old_off = off;
        item.old_len = inst.length;
        old_off_to_idx.emplace(off, items.size());

        const auto make_sub_add_rsp = [&](ZydisMnemonic mnem, int64_t imm_val) {
            ZydisEncoderRequest req{};
            req.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
            req.mnemonic = mnem;
            req.operand_count = 2;
            req.operands[0].type = ZYDIS_OPERAND_TYPE_REGISTER;
            req.operands[0].reg.value = ZYDIS_REGISTER_RSP;
            req.operands[1].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
            req.operands[1].imm.s = imm_val;
            return req;
        };

        // Check for prologue frame allocation (before entering frame).
        if (!seen_prologue && !in_frame) {
            if (inst.mnemonic == ZYDIS_MNEMONIC_SUB && inst.operand_count_visible == 2 &&
                ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                ops[0].reg.value == ZYDIS_REGISTER_RSP &&
                ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && !ops[1].imm.is_relative) {
                orig_frame_size = ops[1].imm.value.s;
                if (orig_frame_size <= 0 || orig_frame_size > 0x4000) {
                    return 0;
                }
                item.kind = InstKind::PrologueAlloc;
                item.req = make_sub_add_rsp(ZYDIS_MNEMONIC_SUB, orig_frame_size + kRedzonePad);
                seen_prologue = true;
                in_frame = true;
                items.push_back(item);
                off += inst.length;
                continue;
            }
            if (inst.mnemonic == ZYDIS_MNEMONIC_PUSH && inst.operand_count_visible == 1 &&
                ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                ops[0].reg.value == ZYDIS_REGISTER_RAX) {
                // PS4 Clang uses 1-byte `push rax` as `sub rsp, 8` for 8-byte stack allocation.
                orig_frame_size = 8;
                item.kind = InstKind::PrologueAlloc;
                item.req = make_sub_add_rsp(ZYDIS_MNEMONIC_SUB, orig_frame_size + kRedzonePad);
                seen_prologue = true;
                in_frame = true;
                items.push_back(item);
                off += inst.length;
                continue;
            }
            if (inst.mnemonic == ZYDIS_MNEMONIC_PUSH) {
                item.kind = InstKind::Verbatim;
                items.push_back(item);
                off += inst.length;
                continue;
            }
            return 0; // Unexpected instruction before prologue frame alloc.
        }

        // After epilogue free: only callee-saved pops and ret are allowed.
        if (seen_epilogue) {
            if (inst.mnemonic == ZYDIS_MNEMONIC_POP || inst.mnemonic == ZYDIS_MNEMONIC_RET ||
                inst.mnemonic == ZYDIS_MNEMONIC_NOP) {
                item.kind = InstKind::Verbatim;
                items.push_back(item);
                off += inst.length;
                continue;
            }
            return 0;
        }

        // Inside the frame: check epilogue `add rsp, orig_frame_size`.
        if (inst.mnemonic == ZYDIS_MNEMONIC_ADD && inst.operand_count_visible == 2 &&
            ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ops[0].reg.value == ZYDIS_REGISTER_RSP &&
            ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && !ops[1].imm.is_relative &&
            ops[1].imm.value.s == orig_frame_size) {
            item.kind = InstKind::EpilogueFree;
            item.req = make_sub_add_rsp(ZYDIS_MNEMONIC_ADD, orig_frame_size + kRedzonePad);
            seen_epilogue = true;
            in_frame = false;
            items.push_back(item);
            off += inst.length;
            continue;
        }

        // Disallow mid-frame stack pointer mutations or early returns.
        if (inst.mnemonic == ZYDIS_MNEMONIC_PUSH || inst.mnemonic == ZYDIS_MNEMONIC_POP ||
            inst.mnemonic == ZYDIS_MNEMONIC_RET || inst.mnemonic == ZYDIS_MNEMONIC_LEAVE) {
            return 0;
        }
        if ((inst.mnemonic == ZYDIS_MNEMONIC_SUB || inst.mnemonic == ZYDIS_MNEMONIC_ADD ||
             inst.mnemonic == ZYDIS_MNEMONIC_MOV || inst.mnemonic == ZYDIS_MNEMONIC_LEA) &&
            inst.operand_count_visible >= 1 && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ops[0].reg.value == ZYDIS_REGISTER_RSP) {
            return 0;
        }

        // Check for relative branches.
        if (inst.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
            if (inst.operand_count_visible < 1 || ops[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                !ops[0].imm.is_relative) {
                return 0; // Indirect branch not supported.
            }
            const int64_t target =
                static_cast<int64_t>(off + inst.length) + ops[0].imm.value.s;
            if (target < 0 || static_cast<size_t>(target) >= src_len) {
                return 0; // External branch.
            }
            if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(
                    &inst, ops, inst.operand_count_visible, &item.req))) {
                return 0;
            }
            item.req.branch_type = ZYDIS_BRANCH_TYPE_NEAR;
            item.req.branch_width = ZYDIS_BRANCH_WIDTH_32;
            item.kind = InstKind::RelBranch;
            item.branch_target_off = static_cast<size_t>(target);
            items.push_back(item);
            off += inst.length;
            continue;
        }

        // Check for RSP-relative or RIP-relative memory operands.
        int rsp_op = -1;
        int rip_op = -1;
        for (ZyanU8 i = 0; i < inst.operand_count_visible; ++i) {
            if (ops[i].type != ZYDIS_OPERAND_TYPE_MEMORY) {
                continue;
            }
            if (ops[i].mem.base == ZYDIS_REGISTER_RSP || ops[i].mem.index == ZYDIS_REGISTER_RSP) {
                if (ops[i].mem.base != ZYDIS_REGISTER_RSP ||
                    ops[i].mem.index != ZYDIS_REGISTER_NONE) {
                    return 0;
                }
                rsp_op = i;
            } else if (ops[i].mem.base == ZYDIS_REGISTER_RIP) {
                rip_op = i;
            }
        }
        if (rsp_op >= 0 && rip_op >= 0) {
            return 0;
        }

        if (rsp_op >= 0) {
            if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(
                    &inst, ops, inst.operand_count_visible, &item.req))) {
                return 0;
            }
            item.req.operands[rsp_op].mem.displacement += kRedzonePad;
            item.kind = InstKind::RspShift;
        } else if (rip_op >= 0) {
            if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(
                    &inst, ops, inst.operand_count_visible, &item.req))) {
                return 0;
            }
            item.kind = InstKind::RipRel;
            item.rip_op_idx = rip_op;
            item.rip_target = static_cast<uint64_t>(
                static_cast<int64_t>(src_addr + off + inst.length) + ops[rip_op].mem.disp.value);
        } else {
            item.kind = InstKind::Verbatim;
        }

        items.push_back(item);
        off += inst.length;
    }

    if (!seen_prologue || !seen_epilogue || in_frame || items.empty()) {
        return 0;
    }

    // Validate all branch targets land on decoded instruction boundaries.
    for (const auto& item : items) {
        if (item.kind == InstKind::RelBranch &&
            old_off_to_idx.find(item.branch_target_off) == old_off_to_idx.end()) {
            return 0;
        }
    }

    // Pass 1: determine exact encoded length and new offset of every instruction.
    size_t new_cursor = 0;
    for (auto& item : items) {
        item.new_off = new_cursor;
        if (item.kind == InstKind::Verbatim) {
            item.new_len = item.old_len;
        } else if (item.kind == InstKind::RelBranch) {
            ZydisEncoderRequest probe = item.req;
            probe.operands[0].imm.s = 0;
            ZyanUSize len = sizeof(item.pre_encoded);
            if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstruction(&probe, item.pre_encoded, &len))) {
                return 0;
            }
            item.new_len = len;
        } else if (item.kind == InstKind::RipRel) {
            ZydisEncoderRequest probe = item.req;
            probe.operands[item.rip_op_idx].mem.displacement = 0;
            ZyanUSize len = sizeof(item.pre_encoded);
            if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstruction(&probe, item.pre_encoded, &len))) {
                return 0;
            }
            item.new_len = len;
        } else {
            ZyanUSize len = sizeof(item.pre_encoded);
            if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstruction(&item.req, item.pre_encoded, &len))) {
                return 0;
            }
            item.new_len = len;
        }
        if (new_cursor + item.new_len > dst_cap) {
            return 0;
        }
        new_cursor += item.new_len;
    }

    // Pass 2: emit final machine code with resolved branch and RIP-relative displacements.
    for (auto& item : items) {
        uint8_t* out = dst + item.new_off;
        if (item.kind == InstKind::Verbatim) {
            std::memcpy(out, src + item.old_off, item.old_len);
        } else if (item.kind == InstKind::RelBranch) {
            const size_t target_idx = old_off_to_idx.at(item.branch_target_off);
            const int64_t rel = static_cast<int64_t>(items[target_idx].new_off) -
                                static_cast<int64_t>(item.new_off + item.new_len);
            if (rel < std::numeric_limits<int32_t>::min() ||
                rel > std::numeric_limits<int32_t>::max()) {
                return 0;
            }
            item.req.operands[0].imm.s = rel;
            ZyanUSize len = item.new_len;
            if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstruction(&item.req, out, &len)) ||
                len != item.new_len) {
                return 0;
            }
        } else if (item.kind == InstKind::RipRel) {
            const int64_t disp =
                static_cast<int64_t>(item.rip_target) -
                static_cast<int64_t>(dst_addr + item.new_off + item.new_len);
            if (disp < std::numeric_limits<int32_t>::min() ||
                disp > std::numeric_limits<int32_t>::max()) {
                return 0;
            }
            item.req.operands[item.rip_op_idx].mem.displacement = disp;
            ZyanUSize len = item.new_len;
            if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstruction(&item.req, out, &len)) ||
                len != item.new_len) {
                return 0;
            }
        } else {
            std::memcpy(out, item.pre_encoded, item.new_len);
        }
    }

    // Pass 3: decode-back verification of every emitted instruction.
    size_t verify_off = 0;
    for (const auto& item : items) {
        if (verify_off != item.new_off) {
            return 0;
        }
        ZydisDecodedInstruction inst{};
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, dst + verify_off,
                                                 new_cursor - verify_off, &inst, ops)) ||
            inst.length != item.new_len) {
            return 0;
        }
        for (ZyanU8 i = 0; i < inst.operand_count_visible; ++i) {
            if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                ops[i].mem.base == ZYDIS_REGISTER_RSP) {
                const int64_t disp = ops[i].mem.disp.has_displacement ? ops[i].mem.disp.value : 0;
                if (disp < 0) {
                    return 0; // Negative red-zone access must never survive.
                }
            }
        }
        if (item.kind == InstKind::RipRel) {
            const uint64_t resolved = static_cast<uint64_t>(
                static_cast<int64_t>(dst_addr + verify_off + inst.length) +
                ops[item.rip_op_idx].mem.disp.value);
            if (resolved != item.rip_target) {
                return 0;
            }
        } else if (item.kind == InstKind::RelBranch) {
            const size_t target_idx = old_off_to_idx.at(item.branch_target_off);
            const int64_t resolved =
                static_cast<int64_t>(verify_off + inst.length) + ops[0].imm.value.s;
            if (resolved != static_cast<int64_t>(items[target_idx].new_off)) {
                return 0;
            }
        }
        verify_off += inst.length;
    }
    if (verify_off != new_cursor) {
        return 0;
    }

    *out_len = new_cursor;
    return 1;
}
