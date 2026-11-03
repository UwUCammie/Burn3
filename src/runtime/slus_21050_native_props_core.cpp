// P2 WP5: prop instance cull + prepare in plain C++ -- see include/takedown/runtime/slus_21050_native_props_core.hpp.
//
// The arithmetic follows VU0 microprogram 0x27 (VU0 data memory image documented in the header) and is the arithmetic of
// slus_21050_instance_batch.cpp (recovered from the release object's GIMPLE, never wired in), restricted to what the game uses on
// the supported track data: kind 4 output, no retained transform. The eight local corners and the planes are read from the image
// the guest's setup left, not rebuilt from the section's box, so nothing about the section record is needed.
//
// CMake builds this file (like the instance batch) with -frounding-math -ffp-contract=off.

#include "takedown/runtime/slus_21050_native_props_core.hpp"

#include "takedown/guest/vu0_simd_arithmetic.hpp"
#include "takedown/runtime/slus_21050_object_submission.hpp"
#include "takedown/runtime/slus_21050_corner_visibility_fast.hpp"

#include <bit>
#include <cmath>
#include <cstddef>

namespace takedown::runtime::slus_21050::native_props {
namespace {

using Point = std::array<float, 3>;

[[nodiscard]] std::uint32_t bits_of(const float value) noexcept { return std::bit_cast<std::uint32_t>(value); }
[[nodiscard]] float float_of(const std::uint32_t bits) noexcept { return std::bit_cast<float>(bits); }



// Every arithmetic step goes through the VU clamp-mode-3 helpers: operands are clamped, products truncated, sums chopped toward zero.
[[nodiscard]] float multiply(const float left, const float right) noexcept {
    bool underflow = false;
    bool overflow = false;
    return float_of(guest::vu0_detail::multiply_bits(bits_of(left), bits_of(right), underflow, overflow));
}
[[nodiscard]] float add(const float left, const float right, const bool subtract = false) noexcept {
    bool underflow = false;
    bool overflow = false;
    return float_of(guest::vu0_detail::add_subtract_bits(bits_of(left), bits_of(right), subtract, underflow, overflow));
}

// VU FTOI0: magnitudes of at least 2^31 (and non-finite values) saturate by sign.
[[nodiscard]] std::uint32_t signed_saturated(const float value) noexcept {
    const std::uint32_t bits = bits_of(value);
    if ((bits & 0x7f800000U) >= 0x4f000000U) {
        return (bits & 0x80000000U) != 0U ? 0x80000000U : 0x7fffffffU;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

struct Transform {
    std::array<std::array<float, 3>, 3> basis{};   // columns (xyz)
    std::array<std::uint32_t, 3> basis_w{};         // the per-column parameter lanes (raw)
    Point translation{};
};

[[nodiscard]] Transform transform_of(const InstanceWords& w) noexcept {
    Transform t;
    for (std::size_t c = 0; c != 3U; ++c) {
        for (std::size_t a = 0; a != 3U; ++a) {
            t.basis[c][a] = float_of(w[4U * c + a]);
        }
        t.basis_w[c] = w[4U * c + 3U];
    }
    for (std::size_t a = 0; a != 3U; ++a) {
        t.translation[a] = float_of(w[12U + a]);
    }
    return t;
}

// Linear part first, then the translation scaled by 1.0 as the VU does (vmaddw with vf0.w).
[[nodiscard]] Point transform_point(const Transform& t, const Point& p) noexcept {
    Point out;
    for (std::size_t axis = 0; axis != 3U; ++axis) {
        const float x = multiply(t.basis[0][axis], p[0]);
        const float y = multiply(t.basis[1][axis], p[1]);
        const float xy = add(x, y);
        const float z = multiply(t.basis[2][axis], p[2]);
        const float linear = add(xy, z);
        out[axis] = add(linear, multiply(t.translation[axis], 1.0F));
    }
    return out;
}

enum class DistanceMode : std::uint8_t { kDisabled, kKeepNear, kKeepFar };

// Distance ignores X: 1 + dy^2 + dz^2 against the threshold; keep-near rejects a negative (threshold - distance), keep-far a negative
// (distance - threshold), the sign bit decides (a negative zero counts).
[[nodiscard]] bool distance_rejected(const DistanceMode mode, const Point& reference, const float threshold, const Transform& t) noexcept {
    if (mode == DistanceMode::kDisabled) {
        return false;
    }
    const float dy = add(reference[1], t.translation[1], true);
    const float dz = add(reference[2], t.translation[2], true);
    const float distance = add(add(1.0F, multiply(dy, dy)), multiply(1.0F, multiply(dz, dz)));
    const float test = mode == DistanceMode::kKeepNear ? add(threshold, distance, true) : add(distance, threshold, true);
    return (bits_of(test) & 0x80000000U) != 0U;
}

struct Plane6 {
    std::array<std::array<float, 4>, 6> planes{};
};

[[nodiscard]] Plane6 planes_of(const Vu0Image& image, const std::uint32_t slot) noexcept {
    const std::uint32_t base = kPlanesQw + kPlaneStrideQw * slot;
    const Quad x = image.quad(base), y = image.quad(base + 1U), z = image.quad(base + 2U), w = image.quad(base + 3U);
    Plane6 p;
    for (std::size_t k = 0; k != 4U; ++k) {
        p.planes[k] = {float_of(x[k]), float_of(y[k]), float_of(z[k]), float_of(w[k])};
    }
    for (std::size_t k = 4; k != 6U; ++k) {
        const Quad q = image.quad(base + static_cast<std::uint32_t>(k));
        p.planes[k] = {float_of(q[0]), float_of(q[1]), float_of(q[2]), float_of(q[3])};
    }
    return p;
}

// The record the microprogram writes for a visible instance: columns with w := z (+0 + z), translation z := visibility, w := depth,
// the tint parameters. `vis` is 0 (inside) or 1 (crosses a plane).
[[nodiscard]] Record record_of(const Transform& t, const InstanceWords& w, const std::uint32_t vis, const std::uint32_t tint_w_bits) noexcept {
    const float zero = add(1.0F, 1.0F, true);
    Record r;
    for (std::size_t c = 0; c != 3U; ++c) {
        r.qw[c] = {w[4U * c], w[4U * c + 1U], w[4U * c + 2U], bits_of(add(zero, t.basis[c][2]))};
    }
    const float depth = add(zero, t.translation[2]);
    r.qw[3] = {w[12], w[13], vis, bits_of(depth)};
    const float scale = add(0.0F, 128.5F);
    for (std::size_t c = 0; c != 3U; ++c) {
        r.qw[4][c] = signed_saturated(multiply(scale, float_of(t.basis_w[c])));
    }
    r.qw[4][3] = signed_saturated(float_of(tint_w_bits));
    return r;
}

struct State {
    std::array<Point, 8> corners{};
    DistanceMode mode{};
    Point reference{};
    float threshold{};
    std::uint32_t tint_w{};
    std::array<Plane6, kMaxSlots> planes{};
    std::array<std::vector<Record>, kMaxSlots> pending{};
    std::array<bool, kMaxSlots> flushed{};
};

[[nodiscard]] Status check_regs(const Regs& regs) noexcept {
    if (regs.slots == 0U || regs.slots > kMaxSlots || regs.lighting != 0U || regs.retained != 0U) {
        return Status::kUnsupported;
    }
    return Status::kOk;
}

// Reads the view buffers into `state.pending`.
[[nodiscard]] Status load_buffers(const Vu0Image& image, const Regs& regs, State& state) {
    for (std::uint32_t s = 0; s != regs.slots; ++s) {
        const std::uint32_t base = kBufferStrideQw * s;
        const std::uint32_t count = image.words[4U * static_cast<std::size_t>(base)];
        if (count % kRecordQw != 0U || count >= kFlushQw) {
            return Status::kBadBuffer;
        }
        std::vector<Record>& records = state.pending[s];
        records.clear();
        records.reserve(kFlushRecords);
        for (std::uint32_t k = 0; k != count / kRecordQw; ++k) {
            Record r;
            for (std::uint32_t q = 0; q != kRecordQw; ++q) {
                r.qw[q] = image.quad(base + 1U + kRecordQw * k + q);
            }
            records.push_back(r);
        }
    }
    return Status::kOk;
}

void load_setup(const Vu0Image& image, const Regs& regs, State& state) {
    for (std::uint32_t c = 0; c != 8U; ++c) {
        const Quad q = image.quad(kCornersQw + c);
        state.corners[c] = {float_of(q[0]), float_of(q[1]), float_of(q[2])};
    }
    state.mode = regs.mode == 0U ? DistanceMode::kDisabled : (regs.mode == 1U ? DistanceMode::kKeepNear : DistanceMode::kKeepFar);
    const Quad vec = image.quad(kVectorQw);
    state.reference = {float_of(vec[0]), float_of(vec[1]), float_of(vec[2])};
    state.threshold = float_of(vec[3]);
    state.tint_w = image.quad(kTintQw)[3];
    for (std::uint32_t s = 0; s != regs.slots; ++s) {
        state.planes[s] = planes_of(image, s);
    }
}

// Writes the buffers back: a count word of a flushed buffer is zeroed first (isw.xyzw), then the count goes to the x lane.
void store_buffers(Vu0Image& image, const Regs& regs, const State& state) {
    for (std::uint32_t s = 0; s != regs.slots; ++s) {
        const std::uint32_t base = kBufferStrideQw * s;
        Quad head = image.quad(base);
        if (state.flushed[s]) {
            head = Quad{0U, 0U, 0U, 0U};
        }
        const std::vector<Record>& records = state.pending[s];
        head[0] = static_cast<std::uint32_t>(records.size()) * kRecordQw;
        image.set_quad(base, head);
        for (std::size_t k = 0; k != records.size(); ++k) {
            for (std::uint32_t q = 0; q != kRecordQw; ++q) {
                image.set_quad(base + 1U + static_cast<std::uint32_t>(k) * kRecordQw + q, records[k].qw[q]);
            }
        }
    }
}

void flush_slot(State& state, const std::uint32_t slot, std::vector<Flush>& flushes) {
    Flush f;
    f.slot = slot;
    f.records = std::move(state.pending[slot]);
    state.pending[slot] = {};
    state.pending[slot].reserve(kFlushRecords);
    state.flushed[slot] = true;
    flushes.push_back(std::move(f));
}

} // namespace

Status process_cell(Vu0Image& image, const Regs& regs, const std::span<const InstanceWords> instances, std::vector<Flush>& flushes,
                    CellStats* const stats) {
    if (const Status s = check_regs(regs); s != Status::kOk) {
        return s;
    }
    State state;
    if (const Status s = load_buffers(image, regs, state); s != Status::kOk) {
        return s;
    }
    load_setup(image, regs, state);
    CellStats local;
    for (const InstanceWords& w : instances) {
        ++local.instances;
        const Transform t = transform_of(w);
        if (distance_rejected(state.mode, state.reference, state.threshold, t)) {
            ++local.distance_rejected;
        } else {
            std::array<ObjectVisibility, kMaxSlots> vis{};
            std::array<bool, kMaxSlots> decided{};
            bool all_decided = true;
            for (std::uint32_t s = 0; s != regs.slots; ++s) {
                decided[s] = slus_21050::fast_instance_classification(t.basis, t.translation, state.corners, state.planes[s].planes, vis[s]);
                all_decided = all_decided && decided[s];
            }
            if (!all_decided) {
                std::array<Point, 8> corners;
                for (std::size_t c = 0; c != 8U; ++c) {
                    corners[c] = transform_point(t, state.corners[c]);
                }
                for (std::uint32_t s = 0; s != regs.slots; ++s) {
                    if (!decided[s] && !slus_21050::fast_corner_classification(corners, state.planes[s].planes, vis[s])) {
                        vis[s] = prepare_object_corner_visibility(corners, state.planes[s].planes).classification;
                    }
                }
            }
            bool any = false;
            for (std::uint32_t s = 0; s != regs.slots; ++s) {
                const ObjectVisibility visibility = vis[s];
                if (visibility == ObjectVisibility::Outside) {
                    continue;
                }
                any = true;
                state.pending[s].push_back(record_of(t, w, visibility == ObjectVisibility::Intersecting ? 1U : 0U, state.tint_w));
                ++local.records;
            }
            if (!any) {
                ++local.outside_all;
            }
        }
        // 0x26a610 polls every registered view after the instance, accepted or not, and flushes a full one at once.
        for (std::uint32_t s = 0; s != regs.slots; ++s) {
            if (state.pending[s].size() >= kFlushRecords) {
                flush_slot(state, s, flushes);
            }
        }
    }
    store_buffers(image, regs, state);
    if (stats != nullptr) {
        *stats = local;
    }
    return Status::kOk;
}

Status drain(Vu0Image& image, const Regs& regs, std::vector<Flush>& flushes) {
    if (const Status s = check_regs(regs); s != Status::kOk) {
        return s;
    }
    State state;
    if (const Status s = load_buffers(image, regs, state); s != Status::kOk) {
        return s;
    }
    for (std::uint32_t s = 0; s != regs.slots; ++s) {
        if (!state.pending[s].empty()) {
            flush_slot(state, s, flushes);
        }
    }
    store_buffers(image, regs, state);
    return Status::kOk;
}

bool view_block_usable(const ViewBlock& block) noexcept {
    if (!block.valid) {
        return false;
    }
    const double far_w = static_cast<double>(float_of(block.words[kViewFarWord]));
    const double near_w = static_cast<double>(float_of(block.words[kViewNearWord]));
    return near_w >= 0.0 && far_w > near_w && far_w < 1e9;
}

CallPlan plan_call(const CallInputs& in) {
    CallPlan plan;
    plan.image = in.image;
    if (in.regs.slots == 0U || in.regs.slots > kMaxSlots || static_cast<std::int32_t>(in.regs.slots) != in.slots_byte || in.regs.lighting != 0U ||
        in.regs.retained != 0U) {
        plan.decline = Decline::kRegs;
        return plan;
    }
    if (in.material_kind != 4U || (in.material_flags & 0x200U) != 0U) {
        plan.decline = Decline::kMaterial;
        return plan;
    }
    for (std::uint32_t s = 0; s != in.regs.slots; ++s) {
        const std::int32_t v = in.view_of_slot[s];
        if (v < 0 || v >= static_cast<std::int32_t>(kMaxViews) || !view_block_usable(in.blocks[static_cast<std::size_t>(v)])) {
            plan.decline = Decline::kView;
            return plan;
        }
    }
    const Status status = in.drain ? drain(plan.image, in.regs, plan.flushes)
                                   : process_cell(plan.image, in.regs, std::span<const InstanceWords>(in.instances.data(), in.instances.size()), plan.flushes, &plan.stats);
    if (status != Status::kOk) {
        plan.decline = status == Status::kBadBuffer ? Decline::kBuffer : Decline::kRegs;
        plan.flushes.clear();
        return plan;
    }
    plan.prim = ((kPrimBase | (in.texture ? kPrimTexture : 0U)) & 0x7FFU) | (1U << 11U);
    // Which flushes select a render target first, and what the staging buffer takes in.
    std::int32_t current = in.current_view;
    bool unchanged = in.masks_unchanged;
    std::uint32_t queue = in.queue_qw;
    std::uint32_t need = 0;
    for (const Flush& f : plan.flushes) {
        for (const Record& r : f.records) {
            if (is_lit(instance_view(r))) {
                plan.decline = Decline::kLit;
                plan.flushes.clear();
                plan.planned.clear();
                return plan;
            }
        }
        PlannedFlush p;
        p.slot = f.slot;
        p.view = static_cast<std::uint32_t>(in.view_of_slot[f.slot]);
        if (static_cast<std::int32_t>(p.view) != current || !unchanged) {
            p.select_target = true;
            current = static_cast<std::int32_t>(p.view);
            unchanged = true;
            queue = kViewSwitchQw;
        }
        if (queue != 0U) {
            p.queue_qw = queue;
            need += 1U + queue;
            queue = 0U;
        }
        need += kStubQw;
        plan.planned.push_back(p);
    }
    plan.stage_qw = need;
    if (in.check_stage && in.stage_used_qw + need > kStageLimitQw) {
        plan.decline = Decline::kStage;
        plan.flushes.clear();
        plan.planned.clear();
    }
    return plan;
}

InstanceView instance_view(const Record& record) noexcept {
    // read_prop_instances: row i of the 3x4 is (bx[i], by[i], bz[i], t[i]) with b* = (x, y, w) of qw 0..2 and t = (x, y, w) of qw 3.
    InstanceView v;
    const auto lane = [&record](const std::size_t q, const std::size_t l) { return float_of(record.qw[q][l]); };
    const float bx[3] = {lane(0, 0), lane(0, 1), lane(0, 3)};
    const float by[3] = {lane(1, 0), lane(1, 1), lane(1, 3)};
    const float bz[3] = {lane(2, 0), lane(2, 1), lane(2, 3)};
    const float t[3] = {lane(3, 0), lane(3, 1), lane(3, 3)};
    for (std::size_t r = 0; r != 3U; ++r) {
        v.xform[4U * r] = bx[r];
        v.xform[4U * r + 1U] = by[r];
        v.xform[4U * r + 2U] = bz[r];
        v.xform[4U * r + 3U] = t[r];
    }
    for (std::size_t c = 0; c != 4U; ++c) {
        v.tint[c] = static_cast<float>(record.qw[4][c]) / 255.0F;
    }
    return v;
}

bool is_lit(const InstanceView& view) noexcept {
    for (const float t : view.tint) {
        if (!(t >= 0.0F && t * 255.0F < 255.5F)) {
            return true;
        }
    }
    return false;
}

} // namespace takedown::runtime::slus_21050::native_props
