// 0x0015a7d0 (a0 = object, a1 = stride, a2 = base): RecordAddressByCount. Returns base + stride * (n + 1), where n
// is the signed word at object + 736: addiu makes n + 1, mult puts stride * (n + 1) in v0 (LO and HI take the
// sign-extended halves of the product, v0 the low one), and addu in the jr delay slot adds the base. Leaf code: no
// stores and no calls, five retired instructions on every path, so the declared cost is 5.

#include "takedown/runtime/game/n3_support.hpp"

#include <cstddef>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCountOffset = 736U;     // lw $v0, 736($a0)
constexpr unsigned kV0 = 2U;
constexpr unsigned kA1 = 5U;

// A 32-bit value sign-extended to 64 bits, as a GPR or LO/HI half holds it.
std::uint64_t sign_extend(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

// MULT rd, rs, rt on lane 0: the signed 32 x 32 product of the low words of rs and rt. LO and HI take the
// sign-extended low and high words of the product, and rd takes LO.
void multiply(guest::R5900State& cpu, const unsigned rd, const unsigned rs, const unsigned rt) {
    const auto product = static_cast<std::int64_t>(static_cast<std::int32_t>(cpu.read_gpr_u32(rs))) *
                         static_cast<std::int64_t>(static_cast<std::int32_t>(cpu.read_gpr_u32(rt)));
    const auto bits = static_cast<std::uint64_t>(product);
    const std::uint64_t low = sign_extend(static_cast<std::uint32_t>(bits));
    cpu.write_lo(0U, low);
    cpu.write_hi(0U, sign_extend(static_cast<std::uint32_t>(bits >> 32U)));
    cpu.write_gpr_u64(rd, low);
}

template <class Context>
n3::Run record_address_by_count(Context& context) {
    auto call = n3::guest_call(context);
    guest::R5900State& cpu = call.cpu();
    // lw $v0, 736($a0); addiu $v0, $v0, 1 (the 32-bit sum, sign-extended into v0)
    const std::uint32_t count = call.memory().read_u32_le(call.arg(0) + kCountOffset);
    cpu.write_gpr_sign_extended_32(kV0, count + 1U);
    // mult $v0, $a1, $v0
    multiply(cpu, kV0, kA1, kV0);
    // jr $ra, with addu $v0, $a2, $v0 in its delay slot (32-bit sum, sign-extended by return_value)
    call.return_value(call.arg(2) + cpu.read_gpr_u32(kV0));
    return call.finish(5U);
}

} // namespace

extern constinit const EngineFunction n3_0015a7d0{0x0015a7d0U, 20U, 0x55d60ab05aa10716ULL, "RecordAddressByCount",
                                                  5U, n3::live<&record_address_by_count<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0015a7d0, record_address_by_count)
