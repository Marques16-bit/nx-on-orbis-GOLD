// Host test shim of Eden's common/sparse_large_vector.h as the PS4 build has it (patches/eden/0002): a big reserved range, memory per host page
// only where it was committed, and GetOrDefault answering with a reference to a zero default (a different address) for a page that has none.
// Only the members common/page_table.h and its direct table use. The host page is 16 KiB like the console's.
#pragma once
#include <sys/mman.h>
#include <array>
#include <atomic>
#include <cstring>
#include <vector>
#include "common/common_types.h"
namespace Common {
inline const u64 HostPageSize = 16384;
template <typename T>
class SparseLargeVector final {
public:
    constexpr SparseLargeVector() = default;
    ~SparseLargeVector() noexcept { Release(); }
    SparseLargeVector(const SparseLargeVector&) = delete;
    SparseLargeVector& operator=(const SparseLargeVector&) = delete;
    void ResizeAndClear(std::size_t count) noexcept {
        if (count * sizeof(T) == bytes_) {
            return;
        }
        Release();
        bytes_ = count * sizeof(T);
        base_ = static_cast<T*>(mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0));
        committed_.assign((bytes_ + HostPageSize - 1) / HostPageSize, false);
    }
    const T& GetOrDefault(std::size_t index) const {
        return committed_[index * sizeof(T) / HostPageSize] ? base_[index] : *reinterpret_cast<const T*>(&default_val_);
    }
    T& GetUnchecked(std::size_t index) { return base_[index]; }
    void CommitRegion(std::size_t index, std::size_t end) {
        for (std::size_t p = index * sizeof(T) / HostPageSize; p * HostPageSize < end * sizeof(T); ++p) {
            committed_[p] = true;
        }
    }
    bool IsCommitted(std::size_t index) const { return committed_[index * sizeof(T) / HostPageSize]; }
    const T* data() const noexcept { return base_; }
    std::size_t size() const noexcept { return bytes_ / sizeof(T); }
private:
    void Release() {
        if (base_ != nullptr) {
            munmap(base_, bytes_);
        }
        base_ = nullptr;
        bytes_ = 0;
    }
    T* base_{};
    std::size_t bytes_{};
    std::vector<bool> committed_;
    alignas(16) std::array<u8, sizeof(T)> default_val_{};
};
} // namespace Common
