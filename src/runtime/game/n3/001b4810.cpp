// 0x001b4810 ShouldPairCollide: a pure predicate on two world objects, "may these
// interact?". a1 = object A, a2 = object B; v0 is 0 or 1. Each object's type is the
// signed byte at +0. The owner record is the pointer at +12, read only where the
// listing reads it: owner bytes +526 (unsigned), +534 (signed), +4947 (unsigned,
// bit 2). No stores, no calls.
//
// The listing's set S = {0, 1, 2, 4, 6, 7} (its t1) and set A = {0, 1, 2, 4} (its
// v0) drive seven stages, run in order. A stage that matches returns 0.
//   1. type 3 with type 3, or type 5 with type 5.
//   2. S(A), A.+526 == 1, S(B), B.+526 == 1.
//   3. A(A) and A.+4947 bit 2.
//   4. A(B) and B.+4947 bit 2.
//   5. type 5 with type 3, or type 3 with type 5.
//   6. S(A) and A.+534 == -1.
//   7. S(B) and B.+534 == -1.
// Otherwise it returns 1. The retired count is the listing's block sizes along the
// path taken, delay slots included; each stage adds its own blocks as it runs.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// lb of the type byte: sign-extended, so types 0x80..0xff are negative.
std::int32_t collide_type(const auto& memory, const GuestAddress object) {
    return static_cast<std::int8_t>(memory.read_u8(object));
}
// sltiu on the type: 0, 1, 2.
bool collide_low(const std::int32_t type) { return static_cast<std::uint32_t>(type) < 3U; }
// sltiu on type - 4: 4, 5, 6, 7.
bool collide_four_to_seven(const std::int32_t type) { return static_cast<std::uint32_t>(type - 4) < 4U; }
// sllv 1 << (type - 4), then andi 0x0d: of 4..7 only 5 is clear. Call for 4..7 only.
bool collide_mask(const std::int32_t type) { return ((1U << static_cast<std::uint32_t>(type - 4)) & 0x0DU) != 0U; }

// v0 = 0 through the two-instruction block that sets it (beq, daddu), then jr ra (2).
n3::Run collide_no(auto& call, const std::uint32_t retired) {
    call.return_value(0U);
    return call.finish(retired + 2U + 2U);
}
// v0 = 1 through the one-instruction block (addiu), then jr ra (2).
n3::Run collide_yes(auto& call, const std::uint32_t retired) {
    call.return_value(1U);
    return call.finish(retired + 1U + 2U);
}

template <class Context>
n3::Run should_pair_collide(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object_a = call.arg(1);
    const GuestAddress object_b = call.arg(2);

    // Stage 1.
    std::uint32_t retired = 4U;                                    // 0x1b4810
    const std::int32_t type_a = collide_type(m, object_a);
    if (type_a == 3) {
        retired += 3U;                                             // 0x1b4a70
        if (collide_type(m, object_b) == 3) {
            return collide_no(call, retired);                      // 0x1b4a7c
        }
        retired += 3U;                                             // 0x1b4820, taken: A is not 5
    } else {
        retired += 3U;                                             // 0x1b4820
        if (type_a == 5) {
            retired += 3U;                                         // 0x1b482c
            if (collide_type(m, object_b) == 5) {
                return collide_no(call, retired);                  // 0x1b4838
            }
        }
    }

    // Stage 2, from 0x1b4840: S(A).
    retired += 5U;                                                 // 0x1b4840
    bool set_a = collide_low(type_a);
    if (!set_a) {
        retired += 4U;                                             // 0x1b4854
        bool in_bit = false;
        if (collide_four_to_seven(type_a)) {
            retired += 4U;                                         // 0x1b4864
            if (collide_mask(type_a)) {
                retired += 1U;                                     // 0x1b4874
                in_bit = true;
            }
        }
        set_a = in_bit;
        retired += 2U;                                             // 0x1b4878
        if (!set_a) {
            retired += 2U;                                         // 0x1b4880
        }
    }
    retired += 2U;                                                 // 0x1b4a84
    if (set_a) {
        retired += 2U;                                             // 0x1b4a8c (owner A loaded in its delay slot)
        const GuestAddress owner_a = m.read_u32_le(object_a + 12U);
        retired += 4U;                                             // 0x1b4888
        if (m.read_u8(owner_a + 526U) == 1U) {
            retired += 4U;                                         // 0x1b4898
            const std::int32_t type_b = collide_type(m, object_b);
            bool set_b = collide_low(type_b);
            if (!set_b) {
                retired += 4U;                                     // 0x1b48a8
                bool in_bit = false;
                if (collide_four_to_seven(type_b)) {
                    retired += 4U;                                 // 0x1b48b8
                    if (collide_mask(type_b)) {
                        retired += 1U;                             // 0x1b48c8
                        in_bit = true;
                    }
                }
                retired += 2U;                                     // 0x1b48cc
                if (!in_bit) {
                    retired += 1U;                                 // 0x1b48d4
                }
                set_b = in_bit;
            }
            retired += 2U;                                         // 0x1b48d8
            if (set_b) {
                retired += 5U;                                     // 0x1b48e0 (owner B loaded here)
                const GuestAddress owner_b = m.read_u32_le(object_b + 12U);
                if (m.read_u8(owner_b + 526U) == 1U) {
                    return collide_no(call, retired);              // 0x1b48f4
                }
            }
        }
    }

    // Stage 3: A(A) and owner A bit 2 of +4947.
    retired += 3U;                                                 // 0x1b48fc
    bool a_a = collide_low(type_a);
    if (!a_a) {
        retired += 2U;                                             // 0x1b4908
        a_a = type_a == 4;
    }
    retired += 2U;                                                 // 0x1b4910
    if (a_a) {
        retired += 6U;                                             // 0x1b4918
        const GuestAddress owner_a = m.read_u32_le(object_a + 12U);
        if ((m.read_u8(owner_a + 4947U) & 4U) != 0U) {
            return collide_no(call, retired);                      // 0x1b4970
        }
    }

    // Stage 4: A(B) and owner B bit 2 of +4947.
    retired += 4U;                                                 // 0x1b4930
    const std::int32_t type_b = collide_type(m, object_b);
    bool a_b = collide_low(type_b);
    if (!a_b) {
        retired += 3U;                                             // 0x1b4940
        a_b = type_b == 4;
        if (!a_b) {
            retired += 1U;                                         // 0x1b494c
        }
    }
    retired += 2U;                                                 // 0x1b4950
    if (a_b) {
        retired += 6U;                                             // 0x1b4958
        const GuestAddress owner_b = m.read_u32_le(object_b + 12U);
        if ((m.read_u8(owner_b + 4947U) & 4U) != 0U) {
            return collide_no(call, retired);                      // 0x1b4970
        }
    }

    // Stage 5: types 3 and 5 never pair with each other.
    retired += 3U;                                                 // 0x1b4978
    if (type_a == 5) {
        retired += 4U;                                             // 0x1b4984
        if (type_b == 3) {
            return collide_no(call, retired);                      // 0x1b49b0
        }
    }
    retired += 4U;                                                 // 0x1b4994
    if (type_b == 5) {
        retired += 3U;                                             // 0x1b49a4
        if (type_a == 3) {
            return collide_no(call, retired);                      // 0x1b49b0
        }
    }

    // Stage 6: S(A) again; owner A +534 (signed) == -1 means no pair.
    retired += 4U;                                                 // 0x1b49b8
    bool set_a2 = collide_low(type_a);
    if (!set_a2) {
        retired += 4U;                                             // 0x1b49c8
        bool in_bit = false;
        if (collide_four_to_seven(type_a)) {
            retired += 4U;                                         // 0x1b49d8
            if (collide_mask(type_a)) {
                retired += 1U;                                     // 0x1b49e8
                in_bit = true;
            }
        }
        retired += 2U;                                             // 0x1b49ec
        if (!in_bit) {
            retired += 2U;                                         // 0x1b49f4
        }
        set_a2 = in_bit;
    }
    retired += 2U;                                                 // 0x1b4a94
    if (set_a2) {
        retired += 2U;                                             // 0x1b4a9c (owner A loaded in its delay slot)
        const GuestAddress owner_a = m.read_u32_le(object_a + 12U);
        retired += 4U;                                             // 0x1b49fc
        if (static_cast<std::int8_t>(m.read_u8(owner_a + 534U)) == -1) {
            return collide_no(call, retired);                      // 0x1b4a68
        }
    }

    // Stage 7: S(B); owner B +534 (signed) == -1 means no pair.
    retired += 4U;                                                 // 0x1b4a0c
    bool set_b2 = collide_low(type_b);
    if (!set_b2) {
        retired += 4U;                                             // 0x1b4a1c
        bool in_bit = false;
        if (collide_four_to_seven(type_b)) {
            retired += 4U;                                         // 0x1b4a2c
            if (collide_mask(type_b)) {
                retired += 1U;                                     // 0x1b4a3c
                in_bit = true;
            }
        }
        retired += 2U;                                             // 0x1b4a40
        if (!in_bit) {
            retired += 1U;                                         // 0x1b4a48
        }
        set_b2 = in_bit;
    }
    retired += 2U;                                                 // 0x1b4a4c
    if (set_b2) {
        retired += 5U;                                             // 0x1b4a54 (owner B loaded here)
        const GuestAddress owner_b = m.read_u32_le(object_b + 12U);
        if (static_cast<std::int8_t>(m.read_u8(owner_b + 534U)) == -1) {
            return collide_no(call, retired);                      // 0x1b4a68
        }
    }
    return collide_yes(call, retired);                             // 0x1b4aa4
}

} // namespace

extern constinit const EngineFunction n3_001b4810{0x001b4810U, 672U, 0xfa7b72bf3187d7afULL, "ShouldPairCollide", 157U,
                                           n3::live<&should_pair_collide<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(001b4810, should_pair_collide)
