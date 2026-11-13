/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#define CONCATENATE_TOKENS(x, y) CONCATENATE_TOKENS_IMPL(x, y)
#define CONCATENATE_TOKENS_IMPL(x, y) x##y

#define AxxEmitX64 CONCATENATE_TOKENS(Axx, EmitX64)
#define AxxEmitContext CONCATENATE_TOKENS(Axx, EmitContext)
#define AxxJitState CONCATENATE_TOKENS(Axx, JitState)
#define AxxUserConfig Axx::UserConfig

namespace {
using Vector = std::array<u64, 2>;
}

std::optional<AxxEmitX64::DoNotFastmemMarker> AxxEmitX64::ShouldFastmem(AxxEmitContext& ctx, IR::Inst* inst) const {
    if (!conf.fastmem_pointer || !exception_handler.SupportsFastmem()) {
        return std::nullopt;
    }

    const auto marker = std::make_tuple(ctx.Location(), inst->GetName());
    if (do_not_fastmem.count(marker) > 0) {
        return std::nullopt;
    }
    return marker;
}

FakeCall AxxEmitX64::FastmemCallback(u64 rip_) {
    const auto iter = fastmem_patch_info.find(rip_);
    if (iter != fastmem_patch_info.end()) {
        FakeCall result{
            .call_rip = iter->second.callback,
            .ret_rip = iter->second.resume_rip,
        };
        if (iter->second.recompile) {
            const auto marker = iter->second.marker;
            do_not_fastmem.insert(marker);
            InvalidateBasicBlocks({std::get<0>(marker)});
        }
        return result;
    }
    fmt::print("dynarmic: Segfault happened within JITted code at rip = {:016x}\n"
        "Segfault wasn't at a fastmem patch location!\n", rip_);
    UNREACHABLE(); //("iter != fastmem_patch_info.end()");
}

// nx-on-orbis (ir/opt_passes.cpp, PS4PageReusePass): the full page table lookup of one access whose group
// lookup gave 0, out of line. The same steps as EmitVAddrLookup<A64EmitContext> for the configuration the
// pass accepts (absolute offsets, range checked address space, misalignment checked on page boundaries
// only). On success `page` is the entry (masked, sign-extended) and [page + vaddr] the host address;
// otherwise it jumps to `abort`. Templated on the config so that the A32 build, where no access is ever
// grouped, does not need the A64 members.
template<typename Config>
inline void PS4ReuseSlowLookup(BlockOfCode& code, [[maybe_unused]] const Config& conf, [[maybe_unused]] size_t bitsize,
                               Xbyak::Label& abort, [[maybe_unused]] Xbyak::Reg64 vaddr, [[maybe_unused]] Xbyak::Reg64 page,
                               [[maybe_unused]] Xbyak::Reg64 idx) {
    if constexpr (requires { conf.page_table_address_space_bits; }) {
        const size_t valid_page_index_bits = conf.page_table_address_space_bits - page_table_const_bits;
        if (bitsize != 8 && (conf.detect_misaligned_access_via_page_table & bitsize) != 0) {
            const u32 align_mask = static_cast<u32>(bitsize / 8 - 1);
            const u32 page_align_mask = static_cast<u32>(page_table_const_size - 1) & ~align_mask;
            Xbyak::Label aligned;
            code.test(vaddr, align_mask);
            code.jz(aligned);
            code.mov(page, vaddr);
            code.and_(page, page_align_mask);
            code.cmp(page, page_align_mask);
            code.je(abort, code.T_NEAR);
            code.L(aligned);
        }
        code.mov(idx, vaddr);
        code.shr(idx, int(page_table_const_bits));
        code.test(idx, u32(-(1 << valid_page_index_bits)));
        code.jnz(abort, code.T_NEAR);
        code.mov(page, code.qword[r14 + idx * 8]);
        if (conf.page_table_marked_bit) {
            code.bt(page, *conf.page_table_marked_bit);
            code.jc(abort, code.T_NEAR);
        }
        if (conf.page_table_pointer_mask != 0) {
            if (std::in_range<s32>(conf.page_table_pointer_mask)) {
                code.and_(page, static_cast<u32>(conf.page_table_pointer_mask));
            } else {
                code.mov(idx, conf.page_table_pointer_mask);
                code.and_(page, idx);
            }
        }
        if (conf.page_table_sign_extension) {
            code.shl(page, *conf.page_table_sign_extension);
            code.sar(page, *conf.page_table_sign_extension);
        }
        code.test(page, page);
        code.jz(abort, code.T_NEAR);
    } else {
        code.jmp(abort, code.T_NEAR);  // not reached: A32 accesses are never grouped
    }
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitMemoryRead(AxxEmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    // nx-on-orbis: a grouped access (PS4PageReusePass); its group's page table entry is in args[0].
    if (!args[0].IsImmediate()) {
        const Xbyak::Reg64 host = ctx.reg_alloc.UseGpr(code, args[0]);
        const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
        const Xbyak::Reg64 page = ctx.reg_alloc.ScratchGpr(code);
        const Xbyak::Reg64 idx = ctx.reg_alloc.ScratchGpr(code);
        const int value_idx = bitsize == 128 ? ctx.reg_alloc.ScratchXmm(code).getIdx() : ctx.reg_alloc.ScratchGpr(code).getIdx();
        const auto wrapped_fn = read_fallbacks[std::make_tuple(false, bitsize, vaddr.getIdx(), value_idx)];
        SharedLabel slow = ctx.GenSharedLabel(), abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
        code.test(host, host);
        code.jz(*slow, code.T_NEAR);
        EmitReadMemoryMov<bitsize>(code, value_idx, host + vaddr, false);
        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            code.L(*slow);
            PS4ReuseSlowLookup(code, conf, bitsize, *abort, vaddr, page, idx);
            EmitReadMemoryMov<bitsize>(code, value_idx, page + vaddr, false);
            code.jmp(*end, code.T_NEAR);
            code.L(*abort);
            code.call(wrapped_fn);
            EmitCheckMemoryAbort(ctx, inst, end);
            code.jmp(*end, code.T_NEAR);
        });
        code.L(*end);
        if constexpr (bitsize == 128) {
            ctx.reg_alloc.DefineValue(code, inst, Xbyak::Xmm{value_idx});
        } else {
            ctx.reg_alloc.DefineValue(code, inst, Xbyak::Reg64{value_idx});
        }
        return;
    }
    const auto fastmem_marker = ShouldFastmem(ctx, inst);

    if (!conf.page_table && !fastmem_marker) {
        // Neither fastmem nor page table: Use callbacks
        if constexpr (bitsize == 128) {
            ctx.reg_alloc.HostCall(code, nullptr, {}, args[1]);
            if (ordered) {
                code.mfence();
            }
            code.CallFunction(memory_read_128);
            ctx.reg_alloc.DefineValue(code, inst, xmm1);
        } else {
            ctx.reg_alloc.HostCall(code, inst, {}, args[1]);
            if (ordered) {
                code.mfence();
            }
            Devirtualize<callback>(conf.callbacks).EmitCall(code);
            code.ZeroExtendFrom(bitsize, code.ABI_RETURN);
        }
        EmitCheckMemoryAbort(ctx, inst);
        return;
    }

    if (ordered && bitsize == 128) {
        // Required for atomic 128-bit loads/stores
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RBX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RCX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    }

    const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
    const int value_idx = bitsize == 128 ? ctx.reg_alloc.ScratchXmm(code).getIdx() : ctx.reg_alloc.ScratchGpr(code).getIdx();

    const auto wrapped_fn = read_fallbacks[std::make_tuple(ordered, bitsize, vaddr.getIdx(), value_idx)];

    SharedLabel abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();

    if (fastmem_marker) {
        // Use fastmem
        bool require_abort_handling = false;
        const auto src_ptr = EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling);

        const auto location = EmitReadMemoryMov<bitsize>(code, value_idx, src_ptr, ordered);

        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            code.L(*abort);
            code.call(wrapped_fn);

            fastmem_patch_info.emplace(
                std::bit_cast<u64>(location),
                FastmemPatchInfo{
                    std::bit_cast<u64>(code.getCurr()),
                    std::bit_cast<u64>(wrapped_fn),
                    *fastmem_marker,
                    conf.recompile_on_fastmem_failure,
                });

            EmitCheckMemoryAbort(ctx, inst, end);
            code.jmp(*end, code.T_NEAR);
        });
    } else {
        // Use page table
        ASSERT(conf.page_table);
        const auto src_ptr = EmitVAddrLookup(code, ctx, bitsize, *abort, vaddr);
        EmitReadMemoryMov<bitsize>(code, value_idx, src_ptr, ordered);

        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            code.L(*abort);
            code.call(wrapped_fn);
            EmitCheckMemoryAbort(ctx, inst, end);
            code.jmp(*end, code.T_NEAR);
        });
    }
    code.L(*end);

    if constexpr (bitsize == 128) {
        ctx.reg_alloc.DefineValue(code, inst, Xbyak::Xmm{value_idx});
    } else {
        ctx.reg_alloc.DefineValue(code, inst, Xbyak::Reg64{value_idx});
    }
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitMemoryWrite(AxxEmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    // nx-on-orbis: a grouped access (PS4PageReusePass); its group's page table entry is in args[0].
    if (!args[0].IsImmediate()) {
        const Xbyak::Reg64 host = ctx.reg_alloc.UseGpr(code, args[0]);
        const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
        const int value_idx = bitsize == 128 ? ctx.reg_alloc.UseXmm(code, args[2]).getIdx() : ctx.reg_alloc.UseGpr(code, args[2]).getIdx();
        const Xbyak::Reg64 page = ctx.reg_alloc.ScratchGpr(code);
        const Xbyak::Reg64 idx = ctx.reg_alloc.ScratchGpr(code);
        const auto wrapped_fn = write_fallbacks[std::make_tuple(false, bitsize, vaddr.getIdx(), value_idx)];
        SharedLabel slow = ctx.GenSharedLabel(), abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
        code.test(host, host);
        code.jz(*slow, code.T_NEAR);
        EmitWriteMemoryMov<bitsize>(code, host + vaddr, value_idx, false);
        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            code.L(*slow);
            PS4ReuseSlowLookup(code, conf, bitsize, *abort, vaddr, page, idx);
            EmitWriteMemoryMov<bitsize>(code, page + vaddr, value_idx, false);
            code.jmp(*end, code.T_NEAR);
            code.L(*abort);
            code.call(wrapped_fn);
            EmitCheckMemoryAbort(ctx, inst, end);
            code.jmp(*end, code.T_NEAR);
        });
        code.L(*end);
        return;
    }
    const auto fastmem_marker = ShouldFastmem(ctx, inst);

    if (!conf.page_table && !fastmem_marker) {
        // Neither fastmem nor page table: Use callbacks
        if constexpr (bitsize == 128) {
            ctx.reg_alloc.Use(code, args[1], ABI_PARAM2);
            ctx.reg_alloc.Use(code, args[2], HostLoc::XMM1);
            ctx.reg_alloc.EndOfAllocScope();
            ctx.reg_alloc.HostCall(code, nullptr);
            code.CallFunction(memory_write_128);
        } else {
            ctx.reg_alloc.HostCall(code, nullptr, {}, args[1], args[2]);
            Devirtualize<callback>(conf.callbacks).EmitCall(code);
        }
        if (ordered) {
            code.mfence();
        }
        EmitCheckMemoryAbort(ctx, inst);
        return;
    }

    if (ordered && bitsize == 128) {
        // Required for atomic 128-bit loads/stores
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RBX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RCX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    }

    const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
    const int value_idx = bitsize == 128
                            ? ctx.reg_alloc.UseXmm(code, args[2]).getIdx()
                            : (ordered ? ctx.reg_alloc.UseScratchGpr(code, args[2]).getIdx() : ctx.reg_alloc.UseGpr(code, args[2]).getIdx());

    const auto wrapped_fn = write_fallbacks[std::make_tuple(ordered, bitsize, vaddr.getIdx(), value_idx)];

    SharedLabel abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();

    if (fastmem_marker) {
        // Use fastmem
        bool require_abort_handling = false;
        const auto dest_ptr = EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling);

        const auto location = EmitWriteMemoryMov<bitsize>(code, dest_ptr, value_idx, ordered);

        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            code.L(*abort);
            code.call(wrapped_fn);

            fastmem_patch_info.emplace(
                std::bit_cast<u64>(location),
                FastmemPatchInfo{
                    std::bit_cast<u64>(code.getCurr()),
                    std::bit_cast<u64>(wrapped_fn),
                    *fastmem_marker,
                    conf.recompile_on_fastmem_failure,
                });

            EmitCheckMemoryAbort(ctx, inst, end);
            code.jmp(*end, code.T_NEAR);
        });
    } else {
        // Use page table
        ASSERT(conf.page_table);
        const auto dest_ptr = EmitVAddrLookup(code, ctx, bitsize, *abort, vaddr);
        EmitWriteMemoryMov<bitsize>(code, dest_ptr, value_idx, ordered);

        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            code.L(*abort);
            code.call(wrapped_fn);
            EmitCheckMemoryAbort(ctx, inst, end);
            code.jmp(*end, code.T_NEAR);
        });
    }
    code.L(*end);
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitExclusiveReadMemory(AxxEmitContext& ctx, IR::Inst* inst) {
    ASSERT(conf.global_monitor != nullptr);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());

    if constexpr (bitsize != 128) {
        using T = mcl::unsigned_integer_of_size<bitsize>;

        ctx.reg_alloc.HostCall(code, inst, {}, args[1]);

        code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(1));
        code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(&conf));
        if (ordered) {
            code.mfence();
        }
        code.CallLambda(
            [](AxxUserConfig& conf, Axx::VAddr vaddr) -> T {
                return conf.global_monitor->ReadAndMark<T>(conf.processor_id, vaddr, [&]() -> T {
                    return (conf.callbacks->*callback)(vaddr);
                });
            });
        code.ZeroExtendFrom(bitsize, code.ABI_RETURN);
    } else {
        const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm(code);
        ctx.reg_alloc.Use(code, args[1], ABI_PARAM2);
        ctx.reg_alloc.EndOfAllocScope();
        ctx.reg_alloc.HostCall(code, nullptr);

        code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(1));
        code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(&conf));
        ctx.reg_alloc.AllocStackSpace(code, 16 + ABI_SHADOW_SPACE);
        code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE]);
        if (ordered) {
            code.mfence();
        }
        code.CallLambda(
            [](AxxUserConfig& conf, Axx::VAddr vaddr, Vector& ret) {
                ret = conf.global_monitor->ReadAndMark<Vector>(conf.processor_id, vaddr, [&]() -> Vector {
                    return (conf.callbacks->*callback)(vaddr);
                });
            });
        code.movups(result, xword[rsp + ABI_SHADOW_SPACE]);
        ctx.reg_alloc.ReleaseStackSpace(code, 16 + ABI_SHADOW_SPACE);

        ctx.reg_alloc.DefineValue(code, inst, result);
    }

    EmitCheckMemoryAbort(ctx, inst);
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitExclusiveWriteMemory(AxxEmitContext& ctx, IR::Inst* inst) {
    ASSERT(conf.global_monitor != nullptr);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());

    if constexpr (bitsize == 128) {
        ctx.reg_alloc.Use(code, args[1], ABI_PARAM2);
        ctx.reg_alloc.Use(code, args[2], HostLoc::XMM1);
        ctx.reg_alloc.EndOfAllocScope();
        ctx.reg_alloc.HostCall(code, inst);
    } else {
        ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
    }

    const Xbyak::Reg64 tmp = ctx.reg_alloc.ScratchGpr(code);
    Xbyak::Label end;
    code.mov(code.ABI_RETURN, u32(1));
    code.movzx(tmp.cvt32(), code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)]);
    code.test(tmp.cvt8(), tmp.cvt8());
    code.je(end);
    code.xor_(tmp.cvt32(), tmp.cvt32());
    code.xchg(tmp.cvt8(), code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)]);
    code.mov(code.ABI_PARAM1, u64(&conf));
    if constexpr (bitsize != 128) {
        using T = mcl::unsigned_integer_of_size<bitsize>;
        code.CallLambda([](AxxUserConfig& conf, Axx::VAddr vaddr, T value) -> u32 {
            return conf.global_monitor->DoExclusiveOperation<T>(conf.processor_id, vaddr, [&](T expected) -> bool {
                return (conf.callbacks->*callback)(vaddr, value, expected);
            }) ? 0 : 1;
        });
        if (ordered) {
            code.mfence();
        }
    } else {
        ctx.reg_alloc.AllocStackSpace(code, 16 + ABI_SHADOW_SPACE);
        code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE]);
        code.movaps(xword[code.ABI_PARAM3], xmm1);
        code.CallLambda([](AxxUserConfig& conf, Axx::VAddr vaddr, Vector& value) -> u32 {
            return conf.global_monitor->DoExclusiveOperation<Vector>(conf.processor_id, vaddr, [&](Vector expected) -> bool {
                return (conf.callbacks->*callback)(vaddr, value, expected);
            }) ? 0 : 1;
        });
        if (ordered) {
            code.mfence();
        }
        ctx.reg_alloc.ReleaseStackSpace(code, 16 + ABI_SHADOW_SPACE);
    }
    code.L(end);

    EmitCheckMemoryAbort(ctx, inst);
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitExclusiveReadMemoryInline(AxxEmitContext& ctx, IR::Inst* inst) {
    ASSERT(conf.global_monitor && conf.fastmem_pointer);
    if (!exception_handler.SupportsFastmem()) {
        EmitExclusiveReadMemory<bitsize, callback>(ctx, inst);
        return;
    }

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    constexpr bool ordered = true;

    if constexpr (ordered && bitsize == 128) {
        // Required for atomic 128-bit loads/stores
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RBX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RCX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    }

    const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
    const int value_idx = bitsize == 128 ? ctx.reg_alloc.ScratchXmm(code).getIdx() : ctx.reg_alloc.ScratchGpr(code).getIdx();
    const Xbyak::Reg64 tmp = ctx.reg_alloc.ScratchGpr(code);
    const Xbyak::Reg64 tmp2 = ctx.reg_alloc.ScratchGpr(code);

    const auto wrapped_fn = read_fallbacks[std::make_tuple(ordered, bitsize, vaddr.getIdx(), value_idx)];

    EmitExclusiveLock(code, conf, tmp, tmp2.cvt32());

    code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(1));
    code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorAddressPointer(conf.global_monitor, conf.processor_id)));
    code.mov(qword[tmp], vaddr);

    const auto fastmem_marker = ShouldFastmem(ctx, inst);
    if (fastmem_marker) {
        SharedLabel abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
        bool require_abort_handling = false;

        const auto src_ptr = EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling);

        const auto location = EmitReadMemoryMov<bitsize>(code, value_idx, src_ptr, ordered);

        fastmem_patch_info.emplace(
            std::bit_cast<u64>(location),
            FastmemPatchInfo{
                std::bit_cast<u64>(code.getCurr()),
                std::bit_cast<u64>(wrapped_fn),
                *fastmem_marker,
                conf.recompile_on_exclusive_fastmem_failure,
            });

        code.L(*end);

        if (require_abort_handling) {
            ctx.deferred_emits.emplace_back([=, this] {
                code.L(*abort);
                code.call(wrapped_fn);
                code.jmp(*end, code.T_NEAR);
            });
        }
    } else {
        code.call(wrapped_fn);
    }

    code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id)));
    EmitWriteMemoryMov<bitsize>(code, tmp, value_idx, false);

    EmitExclusiveUnlock(code, conf, tmp, tmp2.cvt32());

    if constexpr (bitsize == 128) {
        ctx.reg_alloc.DefineValue(code, inst, Xbyak::Xmm{value_idx});
    } else {
        ctx.reg_alloc.DefineValue(code, inst, Xbyak::Reg64{value_idx});
    }

    EmitCheckMemoryAbort(ctx, inst);
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitExclusiveWriteMemoryInline(AxxEmitContext& ctx, IR::Inst* inst) {
    ASSERT(conf.global_monitor && conf.fastmem_pointer);
    if (!exception_handler.SupportsFastmem()) {
        EmitExclusiveWriteMemory<bitsize, callback>(ctx, inst);
        return;
    }

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    constexpr bool ordered = true;

    const auto value = [&] {
        if constexpr (bitsize == 128) {
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RBX);
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RCX);
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
            return ctx.reg_alloc.UseXmm(code, args[2]);
        } else {
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
            return ctx.reg_alloc.UseGpr(code, args[2]);
        }
    }();
    const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
    const Xbyak::Reg32 status = ctx.reg_alloc.ScratchGpr(code).cvt32();
    const Xbyak::Reg64 tmp = ctx.reg_alloc.ScratchGpr(code);
    const Xbyak::Reg64 tmp2 = ctx.reg_alloc.ScratchGpr(code);

    const auto wrapped_fn = exclusive_write_fallbacks[std::make_tuple(ordered, bitsize, vaddr.getIdx(), value.getIdx())];

    EmitExclusiveLock(code, conf, tmp, tmp2.cvt32());

    SharedLabel end = ctx.GenSharedLabel();

    code.mov(status, u32(1));
    code.movzx(tmp.cvt32(), code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)]);
    code.test(tmp.cvt8(), tmp.cvt8());
    code.je(*end, code.T_NEAR);
    code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorAddressPointer(conf.global_monitor, conf.processor_id)));
    code.cmp(qword[tmp], vaddr);
    code.jne(*end, code.T_NEAR);

    EmitExclusiveTestAndClear(code, conf, vaddr, tmp, rax);

    code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(0));
    code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id)));

    if constexpr (bitsize == 128) {
        code.mov(rax, qword[tmp + 0]);
        code.mov(rdx, qword[tmp + 8]);
        if (code.HasHostFeature(HostFeature::SSE41)) {
            code.movq(rbx, value);
            code.pextrq(rcx, value, 1);
        } else {
            code.movaps(xmm0, value);
            code.movq(rbx, xmm0);
            code.punpckhqdq(xmm0, xmm0);
            code.movq(rcx, xmm0);
        }
    } else {
        EmitReadMemoryMov<bitsize>(code, rax.getIdx(), tmp, false);
    }

    const auto fastmem_marker = ShouldFastmem(ctx, inst);
    if (fastmem_marker) {
        SharedLabel abort = ctx.GenSharedLabel();
        bool require_abort_handling = false;

        const auto dest_ptr = EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling, tmp);

        const auto location = code.getCurr();

        switch (bitsize) {
        case 8:
            code.lock();
            code.cmpxchg(code.byte[dest_ptr], value.cvt8());
            break;
        case 16:
            code.lock();
            code.cmpxchg(word[dest_ptr], value.cvt16());
            break;
        case 32:
            code.lock();
            code.cmpxchg(dword[dest_ptr], value.cvt32());
            break;
        case 64:
            code.lock();
            code.cmpxchg(qword[dest_ptr], value.cvt64());
            break;
        case 128:
            code.lock();
            code.cmpxchg16b(ptr[dest_ptr]);
            break;
        default:
            UNREACHABLE();
        }

        code.setnz(status.cvt8());
        ctx.deferred_emits.emplace_back([=, this] {
            code.L(*abort);
            code.call(wrapped_fn);

            fastmem_patch_info.emplace(
                std::bit_cast<u64>(location),
                FastmemPatchInfo{
                    std::bit_cast<u64>(code.getCurr()),
                    std::bit_cast<u64>(wrapped_fn),
                    *fastmem_marker,
                    conf.recompile_on_exclusive_fastmem_failure,
                });

            code.xor_(status.cvt32(), status.cvt32()); //dep-break
            code.test(code.al, code.al);
            code.setz(status.cvt8());
            code.jmp(*end, code.T_NEAR);
        });
    } else {
        code.call(wrapped_fn);
        code.xor_(status.cvt32(), status.cvt32()); //dep-break
        code.test(code.al, code.al);
        code.setz(status.cvt8());
    }

    code.L(*end);
    EmitExclusiveUnlock(code, conf, tmp, eax);
    ctx.reg_alloc.DefineValue(code, inst, status);
    EmitCheckMemoryAbort(ctx, inst);
}

#undef AxxEmitX64
#undef AxxEmitContext
#undef AxxJitState
#undef AxxUserConfig
