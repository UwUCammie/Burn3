// 0x0031a590 (leaf, 14 instructions, 56 bytes, no call): copies six 32-bit words from a1 to a0 and returns a0.
// The words are only moved, so they are copied as raw bits (no FPU arithmetic).
//   lwc1   $f5, 0($a1)          all six source words are read before any store, so the copy is exact
//   daddu  $v0, $a0, $zero      v0 = the destination pointer, the whole 64-bit register
//   lwc1   $f4, 4($a1) .. $f0, 20($a1)
//   swc1   $f5, 0($a0) .. $f1, 16($a0)
//   jr     $ra
//   swc1   $f0, 20($a0)         in the delay slot
// f0..f5 are temporaries the caller ABI lets hold anything, so no FPR is written.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

template <class Context>
n3::Run copy_six_words(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress destination = call.arg(0);
    const GuestAddress source = call.arg(1);
    const std::uint32_t w0 = m.read_u32_le(source);
    const std::uint32_t w1 = m.read_u32_le(source + 4U);
    const std::uint32_t w2 = m.read_u32_le(source + 8U);
    const std::uint32_t w3 = m.read_u32_le(source + 12U);
    const std::uint32_t w4 = m.read_u32_le(source + 16U);
    const std::uint32_t w5 = m.read_u32_le(source + 20U);
    call.return_value64(call.arg64(0));
    m.write_u32_le(destination, w0);
    m.write_u32_le(destination + 4U, w1);
    m.write_u32_le(destination + 8U, w2);
    m.write_u32_le(destination + 12U, w3);
    m.write_u32_le(destination + 16U, w4);
    m.write_u32_le(destination + 20U, w5);
    return call.finish(14U);
}

} // namespace

extern constinit const EngineFunction n3_0031a590{0x0031a590U, 56U, 0xd951993aa8756e41ULL, "CopySixWords", 14U,
                                           n3::live<&copy_six_words<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0031a590, copy_six_words)
