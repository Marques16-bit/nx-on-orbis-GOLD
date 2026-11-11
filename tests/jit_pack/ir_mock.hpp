// Test stand-in for the few Dynarmic IR types that the page reuse pass (patches/eden-jit) uses: same names, same member
// functions, same semantics as far as the pass looks (immediates, instruction arguments, a list with stable iterators).
#pragma once
#include <array>
#include <cstdint>
#include <initializer_list>
#include <list>
#include <optional>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

using s64 = std::int64_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;

namespace Dynarmic {
enum class OptimizationFlag { DisableVerification };
namespace IR {
enum class AccType { NORMAL, VEC, STREAM, VECSTREAM, ATOMIC, ORDERED, ORDEREDRW, LIMITEDORDERED, UNPRIV };
enum class Opcode {
    Other, Identity, Add64, Sub64,
    A64ReadMemory8, A64ReadMemory16, A64ReadMemory32, A64ReadMemory64, A64ReadMemory128,
    A64WriteMemory8, A64WriteMemory16, A64WriteMemory32, A64WriteMemory64, A64WriteMemory128,
    A64ExclusiveReadMemory8, A64ExclusiveReadMemory16, A64ExclusiveReadMemory32, A64ExclusiveReadMemory64, A64ExclusiveReadMemory128,
    A64ExclusiveWriteMemory8, A64ExclusiveWriteMemory16, A64ExclusiveWriteMemory32, A64ExclusiveWriteMemory64, A64ExclusiveWriteMemory128,
    A64CallSupervisor, A64ExceptionRaised, A64DataCacheOperationRaised, A64InstructionCacheOperationRaised, A64DataMemoryBarrier,
    A64DataSynchronizationBarrier, A64InstructionSynchronizationBarrier, A64ClearExclusive,
    A64EdenPageLookup,
};
struct Inst;
class Value {
public:
    Value() = default;
    explicit Value(Inst* i) : kind_(Kind::Inst), inst_(i) {}
    explicit Value(u64 v) : kind_(Kind::U64), imm_(v) {}
    explicit Value(bool v) : kind_(Kind::U1), imm_(v ? 1 : 0) {}
    explicit Value(AccType t) : kind_(Kind::Acc), imm_(static_cast<u64>(t)) {}
    bool IsImmediate() const { return kind_ != Kind::Inst; }
    Inst* GetInst() const { return inst_; }
    bool GetU1() const { return imm_ != 0; }
    u64 GetImmediateAsU64() const { return imm_; }
    AccType GetAccType() const { return static_cast<AccType>(imm_); }
private:
    enum class Kind { Empty, Inst, U64, U1, Acc } kind_ = Kind::Empty;
    Inst* inst_ = nullptr;
    u64 imm_ = 0;
};
struct Inst {
    Opcode op = Opcode::Other;
    std::array<Value, 4> args{};
    Opcode GetOpcode() const { return op; }
    Value GetArg(std::size_t i) const { return args[i]; }
    void SetArg(std::size_t i, Value v) { args[i] = v; }
};
struct Block {
    using iterator = std::list<Inst>::iterator;
    std::list<Inst> instructions;
    iterator PrependNewInst(iterator at, Opcode op, std::initializer_list<Value> list) {
        Inst inst;
        inst.op = op;
        std::size_t i = 0;
        for (const Value& v : list) {
            inst.args[i++] = v;
        }
        return instructions.insert(at, inst);
    }
};
} // namespace IR
namespace A64 {
struct UserConfig {
    void** page_table = nullptr;
    std::optional<std::uintptr_t> fastmem_pointer;
    bool check_halt_on_memory_access = false;
    bool absolute_offset_page_table = true;
    bool silently_mirror_page_table = false;
    u32 page_table_log2_stride = 3;
    u32 page_table_address_space_bits = 39;
    bool only_detect_misalignment_via_page_table_on_page_boundary = true;
    std::optional<std::uint8_t> page_table_marked_bit = std::uint8_t(0);
    std::optional<std::uint8_t> page_table_sign_extension = std::uint8_t(8);
};
} // namespace A64
} // namespace Dynarmic
