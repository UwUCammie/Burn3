#pragma once

// P2: native world / prop submission (producer -> NativeCmdEmitter). Builds on the P1 car marker (native_car_submit.hpp).
//
// One marker format and ring family for every native submission. The marker is P1's GIF NOP quadword (magic 0x4E5, generation in word2
// bits 16..31, word3 = seq24 << 8 | kind) with the kind field now meaning
//     0  car entry        CarSubmit in car_submit::Ring (P1, bytes unchanged)
//     1  world object     WorldSubmit: the groups (VU1 runs) of one SubmitWorldObject call
//     2  prop batch       PropSubmit: the instances of one view/cell batch drawn with the groups of one section variant
// World and prop submissions share the generic header below (seq, generation, kind) and one SPSC ring, world_submit::Ring.
//
// What a submission replaces. The guest's per-object packets ([CALL mesh][ctx][queued state][1013 pair][CALL cont / MSCNT], a ~90 qw
// mesh packet and one VU1 run per group) are not built; the stub (car_submit::write_stub, no prefix) stands in their place in the chain.
// The group prologue and the view-select packets stay guest code (P2a), so every GS register the draw needs is whatever the stream holds
// at the marker, exactly as at a trusted token. At the marker the emitter runs the SAME decision core as a trusted launch
// (NativeCmdEmitter::Impl::trusted_build_draws: box_outside, trusted_mask, wire_mesh_body, DRAW, one token per visible draw), once per
// group, so the records and tokens are byte for byte those of the trusted launches the VU1 path would have produced for the same inputs.
//
// Shadow mode (TAKEDOWN_P2_SHADOW=1): the guest path runs unchanged and the producer pushes the same records into the *shadow* ring
// instead (no marker, no stub). Each trusted launch the emitter handles is compared with the next matching shadow group (see
// native_world_submit_impl.inc); mismatches are logged with Submit::record_addr.
//
// Producer contract (emulation thread). For every group of the object: a StripMesh exactly as native_cmd::decode_world3 / decode_prop
// builds it from the VU1 input (n vertices, pos, flags = position.w word, uv, col, nrm), the mesh identity `mesh_key` computed with
// group_key() over the raw words the decoders read (equal to what group_hash makes from VU1 memory), the program (10 / 11 / 3 / 4 / 12 /
// 0xFE world by material kind and flags, 14 props), the token PRIM template (prim | PRE << 11, from qw 1006), the view block (the 40 floats
// of qw 1014..1023; the far/near terms are its qw 1020.w / 1021.w), and for program 3/4 the SpecularLight, for 12 the mirror constant, for
// props the instances (3x4 transform and 0..1 tint, as read_prop_instances reads them). Lit props (a tint lane outside 0..255.5/255) and
// kind 3 / kind 2 materials must stay on the guest path: the producer declines those calls.

#include "takedown/platform/native_car_submit.hpp"
#include "takedown/platform/native_cmd_decode.hpp"
#include "takedown/platform/native_cmd_protocol.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace takedown::platform {

namespace world_submit {

namespace nc = takedown::native_cmd;

// ---- marker kinds ------------------------------------------------------------------------------------------------------------------
inline constexpr std::uint8_t kKindCar = 0U;   // car_submit::Ring (P1)
inline constexpr std::uint8_t kKindWorld = 1U; // WorldSubmit
inline constexpr std::uint8_t kKindProp = 2U;  // PropSubmit
inline constexpr std::uint8_t kKindMax = 2U;

inline constexpr std::uint32_t kMaxGroups = 16U;         // groups (VU1 runs) of one object; the sample track has at most 12
inline constexpr std::uint32_t kMaxPropInstances = 64U;  // instances of one prop batch (a VU1 launch holds 18: batches that are compared in
                                                         // shadow mode must not exceed that)
inline constexpr std::uint32_t kViewFloats = car_submit::kViewFloats; // qw 1014..1023
inline constexpr std::uint32_t kFarFloat = (1020U - 1014U) * 4U + 3U;  // qw 1020.w: far W limit
inline constexpr std::uint32_t kNearFloat = (1021U - 1014U) * 4U + 3U; // qw 1021.w: near W limit

// The kind field of a marker quadword at a tag position: any native submission kind. car_submit::is_marker only knows kind 0.
[[nodiscard]] inline bool is_marker(const nc::Quad& q) noexcept {
    return q[0] == 0x8000U && q[1] == (1U << 28U) && (q[2] & 0xFFFFU) == (0xFU | (car_submit::kMarkerMagic << 4U)) && (q[3] & 0xFFU) <= kKindMax;
}
[[nodiscard]] inline std::uint8_t marker_kind(const nc::Quad& q) noexcept { return static_cast<std::uint8_t>(q[3] & 0xFFU); }
[[nodiscard]] inline nc::Quad encode_marker(std::uint8_t kind, std::uint32_t seq24, std::uint16_t generation) noexcept {
    return car_submit::encode_marker(car_submit::Marker{seq24, generation, static_cast<car_submit::MarkerKind>(kind)});
}

// ---- the programs -----------------------------------------------------------------------------------------------------------------
// The ProgramInfo a submission's `program` number stands for (the first table entry of that number: the three prop images share
// category, decoder and number).
[[nodiscard]] inline const nc::ProgramInfo* program_info(std::uint16_t program) noexcept {
    for (const nc::ProgramInfo& info : nc::kPrograms) {
        if (info.program == program && info.decoder != nc::Decoder::kCar && info.decoder != nc::Decoder::kNone) {
            return &info;
        }
    }
    return nullptr;
}

// ---- mesh identity ----------------------------------------------------------------------------------------------------------------
// One vertex as the group hash reads it. These are the *raw* words, not the decoded floats: group_hash covers the uv as the int32 words
// the guest stored (world) / float bits (props), the colour as the four low bytes packed, and for programs 3/4 the s8 normal bytes and
// the intensity word, none of which can be recovered exactly from a StripMesh (colour alpha is replaced by the intensity there).
struct GroupVertex {
    std::uint32_t pos[3]{};     // float bits
    std::uint32_t flags{};      // position.w word (the key uses the low 16 bits)
    std::uint32_t uv[2]{};      // world: int32 words (u, v * 4096); props: float bits
    std::uint32_t color{};      // world: R | G << 8 | B << 16 | A << 24 (the low byte of each of the 4 words); props: unused
    std::uint32_t normal{};     // programs 3/4: nx | ny << 8 | nz << 16 (low bytes of the three s8 words)
    std::uint32_t intensity{};  // programs 3/4: the intensity word
};

// native_cmd::group_hash(info->decoder, vu, top, n, program * 0x9E3779B1 + 1) for a group given as raw vertices. 0 for n out of range.
[[nodiscard]] inline std::uint64_t group_key(const nc::ProgramInfo& info, const GroupVertex* v, std::uint32_t n) noexcept {
    if (n == 0U || n > nc::kMaxGroupVertices || v == nullptr) {
        return 0U;
    }
    std::uint32_t buf[9U * 340U];
    const bool prop = info.decoder == nc::Decoder::kProp;
    const bool normals = info.decoder == nc::Decoder::kWorld3N;
    const std::uint32_t words = prop ? 6U : (normals ? 9U : 7U);
    for (std::uint32_t i = 0; i != n; ++i) {
        std::uint32_t* o = buf + words * i;
        o[0] = v[i].pos[0];
        o[1] = v[i].pos[1];
        o[2] = v[i].pos[2];
        o[3] = v[i].flags & 0xFFFFU;
        o[4] = v[i].uv[0];
        o[5] = v[i].uv[1];
        if (!prop) {
            o[6] = v[i].color;
            if (normals) {
                o[7] = v[i].normal;
                o[8] = v[i].intensity;
            }
        }
    }
    return nc::hash64(buf, static_cast<std::size_t>(words) * 4U * n, info.program * 0x9E3779B1ULL + 1U);
}
// Program 254 (the arrow boards) scrolls its texture: with qw 1005.x != 0 every offset is a mesh of its own (qw 1005.y / .z bits).
[[nodiscard]] inline std::uint64_t scroll_key(std::uint64_t key, std::uint32_t y_bits, std::uint32_t z_bits) noexcept {
    return nc::hash_mix(key, (static_cast<std::uint64_t>(y_bits) << 32U) | z_bits);
}

// ---- the producer's record ----------------------------------------------------------------------------------------------------------
struct Group {
    std::shared_ptr<const nc::StripMesh> mesh; // shared with the producer's cache; n = mesh->n is the DRAW's nverts
    std::uint64_t mesh_key{};                  // group_key(), the emitter's cache identity (the wire key is hash_mix(mesh_key, 2))
    std::uint8_t missing{};                    // ImportedGroup::missing: blocks the packet lacks (1 uv, 2 colour); the trusted path's stale words differ
};

// WorldSubmit (kind 1) and PropSubmit (kind 2) in one ring slot. Fields that do not belong to the kind are ignored.
struct Submit {
    // ---- generic header (set by the ring: seq, generation, kind) ----
    std::uint32_t seq{};
    std::uint16_t generation{};
    std::uint8_t kind{};
    std::uint8_t group_count{};
    std::uint32_t record_addr{};     // guest address of the object record / section (diagnostics: shadow mismatch lines)
    // ---- common ----
    std::uint16_t program{};         // 10 11 3 4 12 0xFE (world) / 14 (props)
    std::uint16_t inst_count{};      // props: instances (== inst.size())
    std::uint32_t prim{};            // token PRIM (11 bits) | PRE << 11, from qw 1006 (what trusted_prim derives)
    float view[kViewFloats]{};       // qw 1014..1023 bit for bit (ViewBlock::ps2_block); far / near W terms are view[kFarFloat], view[kNearFloat]
    Group groups[kMaxGroups];
    // ---- WorldSubmit ----
    nc::SpecularLight spec{};        // programs 3 / 4: qw 1007..1009 and 1013.z (see Impl::spec_slot)
    float mirror_y0{};               // program 12: 2 * qw 1009.y (the mirror plane constant, row-major xform[7])
    // ---- PropSubmit ----
    std::vector<nc::PropInstance> inst; // instance k: xform (row-major 3x4, local -> world) and tint (0..1), as read_prop_instances reads qw 932..

    [[nodiscard]] float far_w() const noexcept { return view[kFarFloat]; }
    [[nodiscard]] float near_w() const noexcept { return view[kNearFloat]; }
    std::uint32_t shadow_mask{};     // shadow mode: groups already paired with a launch
    std::uint32_t stamp{};           // the ring's epoch (the emitter's vblank count) when the producer pushed the record (shadow diagnostics)
    void reset() noexcept {
        stamp = 0U;
        shadow_mask = 0U;
        for (Group& g : groups) {
            g.mesh.reset();
            g.mesh_key = 0U;
        }
        group_count = 0U;
        inst_count = 0U;
        inst.clear();
        record_addr = 0U;
        program = 0U;
        prim = 0U;
        mirror_y0 = 0.0F;
        spec = nc::SpecularLight{};
    }
};
using WorldSubmit = Submit;
using PropSubmit = Submit;

// What the emitter's consumer counts (device-model thread only; read it from the same thread or after the emitter stopped).
struct ConsumerStats {
    // markers (replace mode)
    std::uint64_t markers_world{}, markers_prop{}, kind_mismatch{}, groups{}, draws{}, offscreen{}, bad_submit{}, bad_group{}, bad_view{}, lit_dropped{};
    // shadow mode
    std::uint64_t sh_launches{};      // trusted launches seen with the shadow ring on
    std::uint64_t sh_compared{};      // launches paired with a shadow group
    std::uint64_t sh_ok{};            // ... with no difference
    std::uint64_t sh_no_record{};     // launches with an empty shadow ring
    std::uint64_t sh_unmatched{};     // launches no queued record explains (non-submit launches, or a missing record)
    std::uint64_t sh_skipped{};       // shadow groups skipped over to reach a match (their launch never came: declined, culled)
    std::uint64_t sh_key_pairs{};     // pairs made by shape (program, n) because no record had the launch's mesh key
    std::uint64_t sh_view_only{};     // launches whose vertices a record has but not with the launch's view block
    std::uint64_t sh_age[8]{};        // age (emitter vblanks since the push) of the paired records: 0..6, 7 = older
    std::uint64_t mm_program{}, mm_n{}, mm_prim{}, mm_key{}, mm_mesh{}, mm_view{}, mm_spec{}, mm_mirror{}, mm_inst{}, mm_draw{};
};

// ---- fixed-capacity SPSC ring -----------------------------------------------------------------------------------------------------
// Same protocol as car_submit::Ring (producer: emulation thread; consumer: device-model thread; head / tail atomics, release / acquire).
// A marker names its entry by seq and generation (acquire / release); the shadow ring is read in order (peek / pop_front).
class Ring {
  public:
    struct Counters {
        std::uint64_t pushed{}, full{}, declined{}, popped{}, lost{}, stale{}, missing{};
    };

    explicit Ring(std::uint32_t capacity = 4096U) : mask_(capacity - 1U), slots_(capacity) {}
    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    [[nodiscard]] std::uint32_t capacity() const noexcept { return mask_ + 1U; }
    // Entries pushed and not yet released (an estimate when read from the other thread).
    [[nodiscard]] std::uint32_t occupancy() const noexcept { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed); }

    // ---- producer ----
    [[nodiscard]] std::uint32_t free_slots() const noexcept {
        return capacity() - (head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire));
    }
    // The slot to fill (cleared; seq, generation and kind set), or nullptr when the ring is full. Publish with commit_push() or drop with
    // abort_push(). One slot is open at a time.
    [[nodiscard]] Submit* begin_push(std::uint8_t kind) noexcept {
        const std::uint32_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= capacity()) {
            full_.fetch_add(1U, std::memory_order_relaxed);
            return nullptr;
        }
        Submit& s = slots_[head & mask_];
        s.reset();
        s.seq = next_seq_;
        s.generation = generation_.load(std::memory_order_acquire);
        s.kind = kind;
        s.stamp = epoch_.load(std::memory_order_relaxed);
        return &s;
    }
    void commit_push() noexcept {
        next_seq_ = next_seq_ >= car_submit::kMaxSeq ? 1U : next_seq_ + 1U; // 0 is never used
        pushed_.fetch_add(1U, std::memory_order_relaxed);
        head_.store(head_.load(std::memory_order_relaxed) + 1U, std::memory_order_release);
    }
    void advance_epoch() noexcept { epoch_.fetch_add(1U, std::memory_order_relaxed); }
    [[nodiscard]] std::uint32_t epoch() const noexcept { return epoch_.load(std::memory_order_relaxed); }
    void abort_push() noexcept { slots_[head_.load(std::memory_order_relaxed) & mask_].reset(); }
    void note_declined() noexcept { declined_.fetch_add(1U, std::memory_order_relaxed); }
    [[nodiscard]] std::uint16_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    // The marker quadword of the slot returned by begin_push() (valid before or after commit_push()).
    [[nodiscard]] static nc::Quad marker_of(const Submit& s) noexcept { return encode_marker(s.kind, s.seq, s.generation); }

    // ---- consumer: by marker ----
    // The submission a marker names, or nullptr (stale generation, no such entry). Older entries are dropped: lost (their marker never
    // came) or stale (older generation). The slot stays owned by the consumer until release().
    [[nodiscard]] Submit* acquire(std::uint32_t seq24, std::uint16_t marker_generation) noexcept {
        const std::uint16_t current = generation_.load(std::memory_order_relaxed);
        if (marker_generation != current) {
            ++stale_;
            return nullptr;
        }
        for (;;) {
            const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
            if (tail == head_.load(std::memory_order_acquire)) {
                ++missing_;
                return nullptr;
            }
            Submit& s = slots_[tail & mask_];
            if (s.generation != current) {
                ++stale_;
                drop_front(s, tail);
            } else if (s.seq == (seq24 & car_submit::kMaxSeq)) {
                return &s;
            } else if (((seq24 - s.seq) & car_submit::kMaxSeq) < 0x800000U) {
                ++lost_;
                drop_front(s, tail);
            } else {
                ++missing_;
                return nullptr;
            }
        }
    }
    void release() noexcept {
        const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
        ++popped_;
        drop_front(slots_[tail & mask_], tail);
    }

    // ---- consumer: in order (shadow ring) ----
    // The entry `index` places behind the oldest one of the current generation, or nullptr. Entries of older generations in front are
    // dropped first.
    [[nodiscard]] Submit* peek(std::uint32_t index = 0U) noexcept {
        const std::uint16_t current = generation_.load(std::memory_order_relaxed);
        for (;;) {
            const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
            if (tail == head_.load(std::memory_order_acquire)) {
                return nullptr;
            }
            Submit& s = slots_[tail & mask_];
            if (s.generation == current) {
                break;
            }
            ++stale_;
            drop_front(s, tail);
        }
        const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
        if (index >= head_.load(std::memory_order_acquire) - tail) {
            return nullptr;
        }
        return &slots_[(tail + index) & mask_];
    }
    // Absolute position (monotone, wraps at 2^32) of the oldest entry / one past the newest: peek(i) is the entry at tail_index() + i.
    [[nodiscard]] std::uint32_t tail_index() const noexcept { return tail_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint32_t head_index() const noexcept { return head_.load(std::memory_order_acquire); }
    void pop_front() noexcept {
        const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
        if (tail != head_.load(std::memory_order_acquire)) {
            ++popped_;
            drop_front(slots_[tail & mask_], tail);
        }
    }
    // The emitter was reset: markers in flight and entries already queued are dead.
    void bump_generation() noexcept { generation_.store(static_cast<std::uint16_t>(generation_.load(std::memory_order_relaxed) + 1U), std::memory_order_release); }

    [[nodiscard]] Counters counters() const noexcept {
        return Counters{pushed_.load(std::memory_order_relaxed), full_.load(std::memory_order_relaxed), declined_.load(std::memory_order_relaxed),
                        popped_, lost_, stale_, missing_};
    }

    ConsumerStats stats; // written by the consumer only

  private:
    void drop_front(Submit& s, std::uint32_t tail) noexcept {
        s.reset();
        tail_.store(tail + 1U, std::memory_order_release);
    }

    std::uint32_t mask_;
    std::vector<Submit> slots_;
    alignas(64) std::atomic<std::uint32_t> head_{0U};
    alignas(64) std::atomic<std::uint32_t> tail_{0U};
    alignas(64) std::atomic<std::uint16_t> generation_{1U};
    std::atomic<std::uint32_t> epoch_{0U};
    std::uint32_t next_seq_{1U}; // producer only
    std::atomic<std::uint64_t> pushed_{0U}, full_{0U}, declined_{0U};
    std::uint64_t popped_{0U}, lost_{0U}, stale_{0U}, missing_{0U}; // consumer only
};

// The guest stub of a world / prop marker: car_submit::write_stub without a prefix (3 quadwords: DMA CNT, VIF DIRECT 1, marker).
inline std::size_t write_stub(std::uint32_t* out, const Submit& s) noexcept {
    return car_submit::write_stub(out, nullptr,
                                  car_submit::Marker{s.seq, s.generation, static_cast<car_submit::MarkerKind>(s.kind)});
}

} // namespace world_submit

// The rings of the live emitter, or nullptr. Defined in native_cmd_emitter.cpp; set by NativeCmdEmitter's constructor, cleared by its
// destructor.
//  * native_world_submit_ring(): TAKEDOWN_NATIVE_WORLD_SUBMIT=1 (trusted mode on): the producer replaces the guest's per-object packets
//    by a stub with a kind 1 / 2 marker.
//  * native_world_shadow_ring(): TAKEDOWN_P2_SHADOW=1 (trusted mode on): the guest path runs; the producer pushes the records of the same
//    objects here, without a marker, and the emitter compares them with the trusted launches (takes precedence over the former).
[[nodiscard]] world_submit::Ring* native_world_submit_ring() noexcept;
[[nodiscard]] world_submit::Ring* native_world_shadow_ring() noexcept;

// Test seam: while set, the emitter takes every VU1 launch for this program (the micro image is not looked at). nullptr clears it.
void native_cmd_set_program_override_for_test(const native_cmd::ProgramInfo* info) noexcept;

} // namespace takedown::platform
