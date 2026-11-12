                  Common::PhysicalAddress target, Common::PageType type) {
        auto base = GetInteger(base_address);

        LOG_DEBUG(HW_Memory, "Mapping {:016X} onto {:016X}-{:016X}", GetInteger(target),
                  base * YUZU_PAGESIZE, (base + size) * YUZU_PAGESIZE);

        const auto end = base + size;
        ASSERT_MSG(end <= page_table.entries.size(), "out of range mapping at {:016X}", base + page_table.entries.size());

        if (!target) {
            ASSERT_MSG(type != Common::PageType::Memory,
                       "Mapping memory page without a pointer @ {:016x}", base * YUZU_PAGESIZE);

            page_table.entries.ZeroRegion(base, end);
        } else {
            auto current_block = block_count.fetch_add(1, std::memory_order_relaxed);
            ASSERT(current_block != 65535);

            page_table.entries.CommitRegion(base, end);
            while (base != end) {
                auto host_ptr = reinterpret_cast<u64>(system.DeviceMemory().GetPointer<u8>(target)) - (base << YUZU_PAGEBITS);;
                auto& entry = page_table.entries.GetUnchecked(base);

                entry.Store(false, type, current_block, host_ptr);
                ASSERT_MSG(page_table.entries[base].Pointer(),
                           "memory mapping base yield a nullptr within the table");

                base += 1;
                target += YUZU_PAGESIZE;
            }
        }
    }

    template<typename F, typename G>
    [[nodiscard]] u8* GetPointerImpl(u64 vaddr, F&& on_unmapped, G&& on_rasterizer) const {
        // AARCH64 masks the upper 16 bit of all memory accesses
