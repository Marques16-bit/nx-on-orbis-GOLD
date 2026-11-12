void A64EmitX64::EmitA64EdenPageLookup(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 span = static_cast<u32>(args[1].GetImmediateU64());
    const Xbyak::Reg64 host = ctx.reg_alloc.UseScratchGpr(code, args[0]);
    const Xbyak::Reg64 tmp = ctx.reg_alloc.ScratchGpr(code);
    const size_t valid_page_index_bits = conf.page_table_address_space_bits - page_table_const_bits;
    SharedLabel none = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();

    // The whole span inside one page?
    code.mov(tmp.cvt32(), host.cvt32());
    code.and_(tmp.cvt32(), static_cast<u32>(page_table_const_size - 1));
    code.cmp(tmp.cvt32(), static_cast<u32>(page_table_const_size - span));
    code.ja(*none, code.T_NEAR);
    // Inside the address space (it is range checked, not mirrored)?
    code.shr(host, int(page_table_const_bits));
    code.test(host, u32(-(1 << valid_page_index_bits)));
    code.jnz(*none, code.T_NEAR);
    code.mov(host, code.qword[r14 + host * 8]);
    if (conf.page_table_marked_bit) {
        code.bt(host, *conf.page_table_marked_bit);
        code.jc(*none, code.T_NEAR);
    }
    if (conf.page_table_pointer_mask != 0) {
        if (std::in_range<s32>(conf.page_table_pointer_mask)) {
            code.and_(host, static_cast<u32>(conf.page_table_pointer_mask));
        } else {
            code.mov(tmp, conf.page_table_pointer_mask);
            code.and_(host, tmp);
        }
    }
    if (conf.page_table_sign_extension) {
        code.shl(host, *conf.page_table_sign_extension);
        code.sar(host, *conf.page_table_sign_extension);
    }
    // An empty entry leaves 0 here, which the accesses treat like "take the slow path".
    code.L(*end);
    ctx.deferred_emits.emplace_back([=, this] {
        code.L(*none);
        code.xor_(host.cvt32(), host.cvt32());
        code.jmp(*end, code.T_NEAR);
    });
    ctx.reg_alloc.DefineValue(code, inst, host);
}

}  // namespace Dynarmic::Backend::X64
