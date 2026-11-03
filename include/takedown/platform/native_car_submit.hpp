#pragma once

// P1: native car submission (producer -> NativeCmdEmitter).
//
// A native DrawCar (emulation thread) no longer builds the car's VIF/VU1 packets. For each DrawCarList entry (one set of VU1 constants
// 962..1013 plus the batches it draws) it pushes one CarSubmit into a ring and leaves a small stub in the guest's DMA chain:
//
//     DMA CNT tag | VIF NOP NOP NOP DIRECT n | [4 qw A+D GS prefix] | 1 qw car marker
//
// The marker is a GIF NOP tag (PACKED, NREG=1, NLOOP=0, EOP=1) with its own magic (0x4E5, the PATH1 token has 0x4E4) that carries the
// submission's sequence number and the ring's generation. The emitter (the primary raw GIF observer, device-model thread) tracks the
// GIF tag positions of PATH2, recognises a marker only at a tag position, pops the CarSubmit by sequence number and writes exactly the
// records and PATH1 tokens the trusted-car path writes for the same constants and batches (see native_cmd_emitter.cpp,
// NativeCmdEmitter::Impl::emit_car_submit). The marker quadword itself is not forwarded.
//
// Threads: GifPacketCursor, the marker helpers and the stub writer are plain functions. CarSubmitRing is a single-producer
// (emulation thread) / single-consumer (device-model thread) ring; head/tail are atomics (release/acquire); slot contents are
// published by the head store and returned by the tail store. A full ring makes the producer decline (the guest DrawCar then runs).

#include "takedown/platform/native_cmd_protocol.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace takedown::platform {

namespace car_submit {

// ---- constants -------------------------------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kFirstQw = 962U;       // first VU1 quadword of a part's constants
inline constexpr std::uint32_t kConstQw = 52U;        // 962..1013
inline constexpr std::uint32_t kViewFirstQw = 1014U;  // the per-view block (ViewBlock::ps2_block)
inline constexpr std::uint32_t kViewFloats = 40U;     // 1014..1023
inline constexpr std::uint32_t kGuardFloatsAt = 32U;  // qw 1022, 1023 inside the view block (guard-band scales)
inline constexpr std::uint32_t kMaxBatches = 16U;
inline constexpr std::uint32_t kPrefixQw = 4U;        // GS prefix = qw 1006..1009 (GIF tag + 3 A+D quadwords)

inline constexpr std::uint32_t kMarkerMagic = 0x4E5U; // 12 bits, word2 bits 4..15 (kTokenMagic is 0x4E4)
inline constexpr std::uint32_t kMaxSeq = 0xFFFFFFU;   // marker sequence numbers are 24 bit

enum class MarkerKind : std::uint8_t { kCarEntry = 0 };

// ---- marker quadword -------------------------------------------------------------------------------------------------------------
// word0 = EOP (NLOOP 0), word1 = FLG 0 (packed) / NREG 1, word2 = 0xF (NOP) | magic << 4 | generation << 16, word3 = seq24 << 8 | kind
struct Marker {
    std::uint32_t seq24{};
    std::uint16_t generation{};
    MarkerKind kind{MarkerKind::kCarEntry};
    bool operator==(const Marker&) const = default;
};

[[nodiscard]] inline native_cmd::Quad encode_marker(const Marker& m) noexcept {
    native_cmd::Quad q{};
    q[0] = 0x8000U;
    q[1] = 1U << 28U;
    q[2] = 0xFU | (kMarkerMagic << 4U) | (static_cast<std::uint32_t>(m.generation) << 16U);
    q[3] = ((m.seq24 & kMaxSeq) << 8U) | static_cast<std::uint32_t>(m.kind);
    return q;
}

// Meaningful only at a GIF tag position (GifPacketCursor::at_tag()); the whole shape is tested, not just the magic.
[[nodiscard]] inline bool is_marker(const native_cmd::Quad& q) noexcept {
    return q[0] == 0x8000U && q[1] == (1U << 28U) && (q[2] & 0xFFFFU) == (0xFU | (kMarkerMagic << 4U)) && (q[3] & 0xFFU) == 0U;
}

[[nodiscard]] inline Marker decode_marker(const native_cmd::Quad& q) noexcept {
    return Marker{q[3] >> 8U, static_cast<std::uint16_t>(q[2] >> 16U), static_cast<MarkerKind>(q[3] & 0xFFU)};
}

// ---- GIF packet cursor ------------------------------------------------------------------------------------------------------------
// Payload quadwords that follow a GIF tag: PACKED NLOOP*NREG (NREG 0 = 16), REGLIST ceil(NLOOP*NREG/2), IMAGE (and the reserved FLG 2)
// NLOOP. Same rule as the PATH1 parser of the emitter.
[[nodiscard]] inline std::uint32_t gif_payload_quadwords(const native_cmd::Quad& tag) noexcept {
    const std::uint32_t nloop = tag[0] & 0x7FFFU;
    const std::uint32_t flg = (tag[1] >> 26U) & 3U;
    const std::uint32_t nreg = (tag[1] >> 28U) == 0U ? 16U : (tag[1] >> 28U);
    if (flg == 0U) {
        return nloop * nreg;
    }
    if (flg == 1U) {
        return (nloop * nreg + 1U) / 2U;
    }
    return nloop;
}

// Follows one GIF path's quadwords so a marker is only ever looked for where a tag starts: never inside the payload of another packet
// (an IMAGE upload may contain any bit pattern).
class GifPacketCursor {
  public:
    [[nodiscard]] bool at_tag() const noexcept { return remaining_ == 0U; }
    [[nodiscard]] std::uint32_t remaining() const noexcept { return remaining_; }
    // The quadword at a tag position was `tag`: its payload follows.
    void begin(const native_cmd::Quad& tag) noexcept { remaining_ = gif_payload_quadwords(tag); }
    // `count` payload quadwords went by (count <= remaining()).
    void skip(std::uint32_t count) noexcept { remaining_ -= count; }
    void reset() noexcept { remaining_ = 0U; }

  private:
    std::uint32_t remaining_{0U};
};

// ---- the producer's record -------------------------------------------------------------------------------------------------------
struct Batch {
    std::shared_ptr<const std::vector<std::uint8_t>> mesh; // CarBatchHeader + words (MeshLayout::kCarBatch body), shared with the producer's cache
    std::uint64_t mesh_key{};                              // any stream-scoped 64-bit identity of the image
    std::uint32_t count{};                                 // XITOP: vertices of the batch (the DRAW's nverts)
    bool env{};                                            // reflective: a second (environment-map) strip follows the body strip
};

struct Submit {
    std::uint32_t seq{};          // 24-bit marker id, set by the ring
    std::uint16_t generation{};   // set by the ring
    std::uint16_t batch_count{};
    std::uint32_t qw[kConstQw][4]{}; // VU1 qw 962..1013 bit for bit (incl. prefix 1006..1009 and flags 1013)
    float view[kViewFloats]{};       // VU1 qw 1014..1023 (ViewBlock::ps2_block)
    Batch batches[kMaxBatches];
};

// ---- fixed-capacity SPSC ring -----------------------------------------------------------------------------------------------------
class Ring {
  public:
    struct Counters {
        std::uint64_t pushed{}, full{}, declined{}, popped{}, lost{}, stale{}, missing{};
    };

    // `capacity` must be a power of two.
    explicit Ring(std::uint32_t capacity = 4096U) : mask_(capacity - 1U), slots_(capacity) {}
    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    [[nodiscard]] std::uint32_t capacity() const noexcept { return mask_ + 1U; }

    // ---- producer (emulation thread) ----
    [[nodiscard]] std::uint32_t free_slots() const noexcept {
        return capacity() - (head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire));
    }
    // The slot to fill (seq and generation already set, batch_count 0), or nullptr when the ring is full. Publish it with
    // commit_push(), or drop it with abort_push(). Only one slot is open at a time.
    [[nodiscard]] Submit* begin_push() noexcept {
        const std::uint32_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= capacity()) {
            full_.fetch_add(1U, std::memory_order_relaxed);
            return nullptr;
        }
        Submit& s = slots_[head & mask_];
        s.seq = next_seq_;
        s.generation = generation_.load(std::memory_order_acquire);
        s.batch_count = 0;
        return &s;
    }
    void commit_push() noexcept {
        next_seq_ = next_seq_ >= kMaxSeq ? 1U : next_seq_ + 1U; // 0 is never used
        pushed_.fetch_add(1U, std::memory_order_relaxed);
        head_.store(head_.load(std::memory_order_relaxed) + 1U, std::memory_order_release);
    }
    void abort_push() noexcept {
        Submit& s = slots_[head_.load(std::memory_order_relaxed) & mask_];
        clear(s);
    }
    // The producer declined a call (ring full, unsupported case): counted here so the emitter's stats line has it.
    void note_declined() noexcept { declined_.fetch_add(1U, std::memory_order_relaxed); }
    [[nodiscard]] std::uint16_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }

    // ---- consumer (device-model thread) ----
    // A marker was recognised: the submission with that sequence number, or nullptr (stale generation, or no such entry). Entries in
    // front of it are older markers that never arrived: they are dropped and counted as lost; entries of an older generation as
    // stale. The returned slot stays in the ring (the producer cannot reuse it) until release().
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
            } else if (s.seq == (seq24 & kMaxSeq)) {
                return &s;
            } else if (((seq24 - s.seq) & kMaxSeq) < 0x800000U) {
                ++lost_; // the entry is older than the marker: its own marker never reached us
                drop_front(s, tail);
            } else {
                ++missing_; // the marker's entry is already gone
                return nullptr;
            }
        }
    }
    // The submission returned by the last acquire() is done.
    void release() noexcept {
        const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
        ++popped_;
        drop_front(slots_[tail & mask_], tail);
    }
    // The emitter was reset (GIF reset, reconnect): markers still in flight and entries already in the ring are dead.
    void bump_generation() noexcept { generation_.store(static_cast<std::uint16_t>(generation_.load(std::memory_order_relaxed) + 1U), std::memory_order_release); }

    [[nodiscard]] Counters counters() const noexcept {
        return Counters{pushed_.load(std::memory_order_relaxed), full_.load(std::memory_order_relaxed), declined_.load(std::memory_order_relaxed),
                        popped_, lost_, stale_, missing_};
    }

  private:
    static void clear(Submit& s) noexcept {
        for (std::uint32_t i = 0; i != kMaxBatches; ++i) {
            s.batches[i].mesh.reset();
        }
        s.batch_count = 0;
    }
    void drop_front(Submit& s, std::uint32_t tail) noexcept {
        clear(s);
        tail_.store(tail + 1U, std::memory_order_release);
    }

    std::uint32_t mask_;
    std::vector<Submit> slots_;
    alignas(64) std::atomic<std::uint32_t> head_{0U};
    alignas(64) std::atomic<std::uint32_t> tail_{0U};
    alignas(64) std::atomic<std::uint16_t> generation_{1U};
    std::uint32_t next_seq_{1U}; // producer only
    std::atomic<std::uint64_t> pushed_{0U}, full_{0U}, declined_{0U};
    std::uint64_t popped_{0U}, lost_{0U}, stale_{0U}, missing_{0U}; // consumer only
};

// ---- the guest stub ----------------------------------------------------------------------------------------------------------------
// One DMA CNT chain element, 3 quadwords (marker only) or 7 (with the 4-quadword A+D prefix):
//   qw0  DMA tag: QWC = 1 + n, ID = CNT (1), ADDR 0 (unused by CNT); the tag's two VIF words are NOP (they are transferred when the
//        channel has TTE set, and do nothing)
//   qw1  VIF NOP, NOP, NOP, DIRECT n          (n = 1 or 5 quadwords of GIF data follow)
//   qw2.. [prefix tag + 3 A+D quadwords,] marker
// DIRECT (not DIRECTHL) so a PATH2 packet can interrupt PATH3 as the original car packets (PATH1) could. The producer owns the
// destination (display-list reserve) and the guest-visible cursor advance.
inline constexpr std::uint32_t kStubQwMarkerOnly = 3U;
inline constexpr std::uint32_t kStubQwWithPrefix = 3U + kPrefixQw;
inline constexpr std::uint32_t kDmaTagCnt = 1U << 28U;
inline constexpr std::uint32_t kVifCmdDirect = 0x50U;

// Writes the stub to `out` (4 words per quadword, room for kStubQwWithPrefix quadwords); returns the number of quadwords written.
// `prefix` is the entry's qw 1006..1009 (4 quadwords) or nullptr.
inline std::size_t write_stub(std::uint32_t* out, const native_cmd::Quad* prefix, const Marker& marker) noexcept {
    const std::uint32_t n = 1U + (prefix != nullptr ? kPrefixQw : 0U);
    out[0] = (n + 1U) | kDmaTagCnt;
    out[1] = out[2] = out[3] = 0U;
    out[4] = out[5] = out[6] = 0U;
    out[7] = (kVifCmdDirect << 24U) | n;
    std::size_t at = 8U;
    if (prefix != nullptr) {
        for (std::uint32_t i = 0; i != kPrefixQw; ++i, at += 4U) {
            std::memcpy(out + at, prefix[i].data(), 16U);
        }
    }
    const native_cmd::Quad m = encode_marker(marker);
    std::memcpy(out + at, m.data(), 16U);
    return (at + 4U) / 4U;
}

// ---- record building shared by the trusted-launch path and the submission path ---------------------------------------------------
// The 52 quadwords of a part and the two guard quadwords, copied bit for bit (flags, columns and offset are left to the caller).
// `qw52` points at 52 contiguous quadwords (VU1 962.. or Submit::qw), `guard2` at 2 contiguous quadwords (VU1 1022..).
[[nodiscard]] inline native_cmd::CarConst make_const_base(const void* qw52, const void* guard2) noexcept {
    native_cmd::CarConst c{};
    std::memcpy(c.qw, qw52, sizeof c.qw);
    std::memcpy(c.guard, guard2, sizeof c.guard);
    return c;
}

// CarConst::flags bits that follow from qw 1013 (index 51): clip (x bit 1), damaged (z low half set), reflective (y low half clear).
[[nodiscard]] inline std::uint32_t const_flags_from_words(const std::uint32_t (&qw)[kConstQw][4]) noexcept {
    return ((qw[51][0] & 2U) != 0U ? native_cmd::kCarClip : 0U) | ((qw[51][2] & 0xFFFFU) != 0U ? native_cmd::kCarDamaged : 0U) |
           ((qw[51][1] & 0xFFFFU) == 0U ? native_cmd::kCarReflective : 0U);
}

// CarConst of a submission: columns and offset (the exact-prepared transform of the trusted path) are not produced and stay zero;
// the renderer reads neither of them.
[[nodiscard]] inline native_cmd::CarConst make_const(const Submit& s) noexcept {
    native_cmd::CarConst c = make_const_base(s.qw, s.view + kGuardFloatsAt);
    c.flags = const_flags_from_words(s.qw);
    return c;
}

[[nodiscard]] inline std::uint64_t const_key(const native_cmd::CarConst& c) noexcept { return native_cmd::hash64(&c, sizeof c, 0xC0457ULL); }

// The GIF tag the car program sends for a strip of `count` vertices: pass 0 is the body strip (EOP unless an environment strip
// follows, PRIM 0x5C with PRE), pass 1 the environment strip (EOP, PRIM 0x25C). Words as in Slus21050CarTrusted::run.
inline constexpr std::uint32_t kBodyTagWord1 = 808337408U;  // 0x302E4000
inline constexpr std::uint32_t kEnvTagWord1 = 825114624U;   // 0x312E4000
[[nodiscard]] inline native_cmd::Quad strip_tag(std::uint32_t count, std::uint32_t pass, bool env) noexcept {
    if (pass == 0U) {
        return native_cmd::Quad{env ? count : (count | 0x8000U), kBodyTagWord1, 1042U, 1065353216U};
    }
    return native_cmd::Quad{count | 0xFFFF8000U, kEnvTagWord1, 1042U, 0U};
}

} // namespace car_submit

using CarSubmit = car_submit::Submit;
using CarSubmitRing = car_submit::Ring;

// The ring of the live emitter, or nullptr when native car submission is off (TAKEDOWN_NATIVE_CAR_SUBMIT unset, no token mode with
// car trust, no emitter). Set by NativeCmdEmitter's constructor, cleared by its destructor; read by the producer to decide, once at
// boot, whether to register the native DrawCar. Defined in native_cmd_emitter.cpp.
[[nodiscard]] CarSubmitRing* native_car_submit_ring() noexcept;

} // namespace takedown::platform
