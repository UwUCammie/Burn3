// 0x003096e0 (a0 = object O, a1 = request kind K, a2 = status code E, a3 = optional out pointer):
// SetRequestResult. A non-null a3 first receives 0xFFFFFFFF. Then, by K (tested in the order 14, 2, 10,
// 3, 8, 11, 6, 5, 7, otherwise default) and by c = E & 0xffff, either nothing is stored and 0 is
// returned, or O gets the fixed result record (+76 = 0, +80 = 14, +88 = X, +208 = 0, +216 = -1, +212 = 1)
// and 13 is returned. X is 1 (c = 19 or 111), 9 (c = 2, 20, 22 where K lists them), 11 (c = 28), 12
// (K = 7, c = 23) or 13. K = 5 and 6 with E >= 0 only copy E to a3 and return 0. K = 2 and 10 call other
// code (0x00309eb0, 0x00309d20) that never ran in the recorded game: call_cost returns 0 for them so the
// original runs. Returns v0.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kStatusOffset = 76U;
constexpr std::uint32_t kStateOffset = 80U;
constexpr std::uint32_t kXOffset = 88U;
constexpr std::uint32_t kWordAOffset = 208U;
constexpr std::uint32_t kWordCOffset = 216U;
constexpr std::uint32_t kWordBOffset = 212U;

// Retired counts from the listing. The prologue is 3 with a3 == 0 and 6 otherwise (the store of
// 0xFFFFFFFF sits in the second branch's delay slot); each dispatch test costs 3.
constexpr std::uint32_t kPrologueNoOut = 3U;
constexpr std::uint32_t kPrologueOut = 6U;
constexpr std::uint32_t kDispatchTest = 3U;

struct Decision {
    std::uint32_t retired = 0U;
    std::uint32_t x = 0U;         // 0: no result record
    bool copy_error = false;      // K = 5/6, E >= 0: out pointer receives E
    bool calls_other = false;     // K = 2 / 10
    std::uint32_t result = 0U;    // v0
};

Decision decide(const std::uint32_t a3, const std::uint32_t kind, const std::uint32_t error) {
    Decision d;
    const std::uint32_t c = error & 0xffffU;
    std::uint32_t pos = 0U;
    std::uint32_t body = 0U;
    std::uint32_t x = 0U;
    switch (kind) {
    case 14U:
        pos = 1U;
        switch (c) {
        case 19U: x = 1U; body = 20U; break;
        case 111U: x = 1U; body = 23U; break;
        case 2U: x = 9U; body = 27U; break;
        case 20U: x = 9U; body = 30U; break;
        case 22U: x = 9U; body = 33U; break;
        case 0U: body = 25U; break;
        default: x = 13U; body = 35U; break;
        }
        break;
    case 2U:
        d.calls_other = true;
        d.retired = (a3 != 0U ? kPrologueOut : kPrologueNoOut) + 2U * kDispatchTest + 2U;
        return d;
    case 10U:
        d.calls_other = true;
        d.retired = (a3 != 0U ? kPrologueOut : kPrologueNoOut) + 3U * kDispatchTest + 3U;
        return d;
    case 3U:
        pos = 4U;
        switch (c) {
        case 19U: x = 1U; body = 20U; break;
        case 111U: x = 1U; body = 23U; break;
        case 0U: body = 16U; break;
        default: x = 13U; body = 26U; break;
        }
        break;
    case 8U:
        pos = 5U;
        switch (c) {
        case 19U: x = 1U; body = 20U; break;
        case 111U: x = 1U; body = 23U; break;
        case 20U: x = 9U; body = 27U; break;
        case 22U: x = 9U; body = 30U; break;
        case 0U: body = 22U; break;
        default: x = 13U; body = 32U; break;
        }
        break;
    case 11U:
        pos = 6U;
        switch (c) {
        case 19U: x = 1U; body = 20U; break;
        case 111U: x = 1U; body = 23U; break;
        case 28U: x = 11U; body = 27U; break;
        case 0U: body = 19U; break;
        default: x = 13U; body = 29U; break;
        }
        break;
    case 6U:
    case 5U:
        pos = kind == 6U ? 7U : 8U;
        if (static_cast<std::int32_t>(error) >= 0) {
            d.copy_error = a3 != 0U;
            body = a3 != 0U ? 11U : 10U;
        } else {
            switch (c) {
            case 19U: x = 1U; body = 23U; break;
            case 111U: x = 1U; body = 26U; break;
            case 28U:
                if (kind == 6U) {
                    x = 11U;
                    body = 30U;
                } else {
                    x = 13U;
                    body = 27U;
                }
                break;
            default: x = 13U; body = kind == 6U ? 30U : 27U; break;
            }
        }
        break;
    case 7U:
        pos = 9U;
        switch (c) {
        case 23U: x = 12U; body = 20U; break;
        case 28U: x = 11U; body = 23U; break;
        case 19U: x = 1U; body = 25U; break;
        case 111U: x = 1U; body = 28U; break;
        case 0U: body = 21U; break;
        default: x = 13U; body = 31U; break;
        }
        break;
    default:
        pos = 9U;
        x = 13U;
        body = 14U;
        break;
    }
    d.x = x;
    d.result = x != 0U ? 13U : 0U;
    d.retired = (a3 != 0U ? kPrologueOut : kPrologueNoOut) + kDispatchTest * pos + body;
    return d;
}

template <class Context>
n3::Run set_request_result(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t error = call.arg(2);
    const GuestAddress out = call.arg(3);
    if (out != 0U) {
        m.write_u32_le(out, 0xFFFFFFFFU);
    }
    const Decision d = decide(out, call.arg(1), error);
    if (d.calls_other) {
        // Never reached: call_cost sends K = 2 and 10 to the original.
        return call.finish(d.retired);
    }
    if (d.copy_error) {
        m.write_u32_le(out, error);
    }
    if (d.x != 0U) {
        m.write_u32_le(object + kStatusOffset, 0U);
        m.write_u32_le(object + kStateOffset, 14U);
        m.write_u32_le(object + kXOffset, d.x);
        m.write_u32_le(object + kWordAOffset, 0U);
        m.write_u32_le(object + kWordCOffset, 0xFFFFFFFFU);
        m.write_u32_le(object + kWordBOffset, 1U);
    }
    call.return_value(d.result);
    return call.finish(d.retired);
}

template <class Context>
std::uint32_t set_request_result_cost(const Context& context) {
    const Decision d = decide(context.cpu.read_gpr_u32(7U), context.cpu.read_gpr_u32(5U),
                              context.cpu.read_gpr_u32(6U));
    return d.calls_other ? 0U : d.retired;
}

} // namespace

extern constinit const EngineFunction n3_003096e0{0x003096e0U, 1600U, 0x64bad9f4f1b45ec1ULL, "SetRequestResult", 64U,
                                                  n3::live<&set_request_result<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &set_request_result_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(003096e0, set_request_result, set_request_result_cost)
