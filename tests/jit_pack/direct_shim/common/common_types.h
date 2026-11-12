// Host test shim: the few types of Eden's common/common_types.h that common/page_table.h uses.
#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <tuple>
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
