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
