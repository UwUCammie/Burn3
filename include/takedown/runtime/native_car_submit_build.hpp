#pragma once

// P1 / WP5: from a native DrawCar call's part draws to CarSubmits (the producer side of platform/native_car_submit.hpp).
//
//   decode_entry_meshes()   the two static mesh packets of a draw-list entry (entry+0 and entry+4, DMA chains of VIF codes in
//                           the car file) run through a model of VIF1 UNPACK into a VU1 data-memory image, one image per
//                           MSCNT launch: exactly the batch image the emitter reads from VU1 memory at a trusted car launch
//                           (kCarBatch: 16-byte header, ((n + 3) & ~3) * 12 + 4 words), so a batch's mesh key
//                           hash_mix(group_hash(kCar, ...), n) is the one the trusted path computes.
//   MeshCache               the decoded entries, by the two packet addresses, revalidated on every use.
//   build_call_submits()    runs part_entries for each part draw of a plan and turns every entry into one or more
//                           CarSubmits (kMaxBatches batches each) plus the stub flags.
//
// Memory is any read-only accessor with u32(GuestAddress) (plus what part_entries needs): the live guest memory or a capture.
// Nothing here writes guest memory.
//
// VIF model: STCYCL, STMOD, STROW, STCOL, STMASK (must be 0), ITOP, UNPACK S/V2/V3/V4 in 32/16/8 bits (mode 0, 1, 2; write
// cycles with CL >= WL), MSCAL/MSCNT/MSCALF (a launch), NOP/FLUSH*/MARK/OFFSET/BASE ignored. Anything else makes the packet
// undecodable (the caller then leaves the call to the guest). The stub's own MSCNT (after the entry's constants, before entry+4)
// is modelled as a launch of whatever entry+0 left unlaunched. ROW starts as 65536.0 in every lane (the game's STROW).

#include "takedown/platform/native_car_submit.hpp"
#include "takedown/platform/native_cmd_decode.hpp"
#include "takedown/platform/native_cmd_protocol.hpp"
#include "takedown/runtime/native_car_constants.hpp"
#include "takedown/runtime/native_car_draw.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace takedown::runtime::native_car {

// ---- decoded meshes --------------------------------------------------------------------------------------------------------------

struct MeshBatch {
    std::uint32_t count{};                                        // XITOP at the launch
    std::shared_ptr<const std::vector<std::uint8_t>> body;        // CarBatchHeader + words
    std::uint64_t key[2]{};                                       // mesh key for an undamaged / damaged entry
    bool has_key[2]{};
};

struct EntryMeshes {
    bool ok{};
    std::uint64_t fingerprint{};
    std::uint32_t vertices{};                                     // sum of the batches' counts
    std::size_t bytes{};
    std::vector<MeshBatch> batches;
};

namespace mesh_detail {

inline constexpr std::uint32_t kVuQuadwords = 1024U;
inline constexpr std::size_t kMaxWords = 1U << 18U;

// Gathers the VIF word stream of a static packet: per DMA tag the two VIF words of the tag, then the tag's data words.
// A RET without a CALL ends the packet (the packet is the callee of the entry's CALL tag).
template <class Memory>
[[nodiscard]] bool gather_words(const Memory& memory, std::uint32_t address, std::vector<std::uint32_t>& words) {
    std::uint32_t stack[2]{};
    std::uint32_t depth = 0U;
    for (std::uint32_t tags = 0U; tags < 4096U; ++tags) {
        const std::uint32_t word0 = memory.u32(address);
        const std::uint32_t qwc = word0 & 0xffffU;
        const std::uint32_t id = (word0 >> 28U) & 7U;
        const std::uint32_t pointer = memory.u32(address + 4U) & 0x7fffffffU;
        words.push_back(memory.u32(address + 8U));
        words.push_back(memory.u32(address + 12U));
        const bool by_reference = id == 3U || id == 4U || id == 0U;
        const std::uint32_t data = by_reference ? pointer : address + 16U;
        if (words.size() + qwc * 4U > kMaxWords) {
            return false;
        }
        for (std::uint32_t i = 0U; i < qwc * 4U; ++i) {
            words.push_back(memory.u32(data + 4U * i));
        }
        const std::uint32_t after = by_reference ? address + 16U : address + 16U + 16U * qwc;
        switch (id) {
        case 1U: case 3U: case 4U: address = after; break;
        case 2U: address = pointer; break;
        case 5U:
            if (depth == 2U) {
                return false;
            }
            stack[depth++] = after;
            address = pointer;
            break;
        case 6U:
            if (depth == 0U) {
                return true;
            }
            address = stack[--depth];
            break;
        default:
            return true;                                          // refe, end
        }
    }
    return false;
}

// VU1 data memory as the UNPACKs of one batch leave it, relative to TOPS.
class VifImage {
  public:
    VifImage() { row_.fill(0x47800000U); }

    // Runs the packet's words; `launch` is called at each MSCAL/MSCNT/MSCALF. Returns false for anything outside the model.
    template <class Launch>
    [[nodiscard]] bool run(const std::vector<std::uint32_t>& words, Launch&& launch) {
        std::size_t i = 0U;
        const std::size_t n = words.size();
        while (i < n) {
            const std::uint32_t code = words[i++];
            const std::uint32_t command = (code >> 24U) & 0x7fU;
            const std::uint32_t immediate = code & 0xffffU;
            const std::uint32_t num = (code >> 16U) & 0xffU;
            if ((command & 0x60U) == 0x60U) {
                if (!unpack(command, immediate, num, words, i)) {
                    return false;
                }
                continue;
            }
            switch (command) {
            case 0x00U: case 0x06U: case 0x07U: case 0x02U: case 0x03U: case 0x10U: case 0x11U: case 0x13U:
                break;
            case 0x01U:
                cl_ = immediate & 0xffU;
                wl_ = (immediate >> 8U) & 0xffU;
                break;
            case 0x04U:
                itop_ = immediate & 0x3ffU;
                break;
            case 0x05U:
                mode_ = immediate & 3U;
                break;
            case 0x20U:
                if (i >= n) {
                    return false;
                }
                if (words[i++] != 0U) {
                    return false;                                 // write masks are not part of the model
                }
                break;
            case 0x30U:
            case 0x31U:
                if (i + 4U > n) {
                    return false;
                }
                for (std::size_t k = 0U; k < 4U; ++k) {
                    (command == 0x30U ? row_ : col_)[k] = words[i + k];
                }
                i += 4U;
                break;
            case 0x14U: case 0x15U: case 0x17U:
                if (!launch(*this)) {
                    return false;
                }
                break;
            default:
                return false;
            }
        }
        return true;
    }

    // The image of the batch about to launch, or false when nothing was unpacked since the last launch.
    [[nodiscard]] bool pending() const noexcept { return dirty_ && itop_ != 0U; }
    [[nodiscard]] std::uint32_t itop() const noexcept { return itop_; }
    [[nodiscard]] const std::uint32_t* words() const noexcept { return data_.data(); }
    // The launch consumed the image: the next batch starts on a clean buffer.
    void clear() {
        std::fill(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(used_words_), 0U);
        used_words_ = 0U;
        dirty_ = false;
        itop_ = 0U;
    }

  private:
    [[nodiscard]] bool unpack(const std::uint32_t command, const std::uint32_t immediate, const std::uint32_t num,
                              const std::vector<std::uint32_t>& words, std::size_t& at) {
        const std::uint32_t vn = (command >> 2U) & 3U;
        const std::uint32_t vl = command & 3U;
        if (vl == 3U) {
            return false;                                         // V4-5
        }
        if ((command & 0x10U) != 0U) {
            return false;                                         // masked unpack
        }
        const std::uint32_t components = vn + 1U;
        const std::uint32_t bits = vl == 0U ? 32U : (vl == 1U ? 16U : 8U);
        const bool unsigned_data = (immediate & 0x4000U) != 0U;
        const std::uint32_t address = immediate & 0x3ffU;
        const std::uint32_t items = num == 0U ? 256U : num;
        const std::uint32_t total_bits = items * components * bits;
        const std::uint32_t data_words = (total_bits + 31U) / 32U;
        if (at + data_words > words.size() || cl_ < wl_ || wl_ == 0U || mode_ == 3U) {
            return false;
        }
        const std::uint32_t* const data = &words[at];
        at += data_words;
        for (std::uint32_t item = 0U; item < items; ++item) {
            std::uint32_t value[4] = {0U, 0U, 0U, 0U};
            for (std::uint32_t c = 0U; c < components; ++c) {
                const std::uint32_t bit = (item * components + c) * bits;
                std::uint32_t element;
                if (bits == 32U) {
                    element = data[bit / 32U];
                } else {
                    element = (data[bit / 32U] >> (bit % 32U)) & ((1U << bits) - 1U);
                    if (!unsigned_data) {
                        const std::uint32_t sign = 1U << (bits - 1U);
                        element = (element ^ sign) - sign;
                    }
                }
                value[c] = element;
            }
            if (vn == 0U) {                                       // S: the one element in every lane
                value[1] = value[2] = value[3] = value[0];
            }
            for (std::uint32_t c = 0U; c < 4U; ++c) {
                if (mode_ == 1U) {
                    value[c] += row_[c];
                } else if (mode_ == 2U) {
                    row_[c] += value[c];
                    value[c] = row_[c];
                }
            }
            const std::uint32_t quadword = address + (item / wl_) * cl_ + (item % wl_);
            if (quadword >= kVuQuadwords) {
                return false;
            }
            for (std::uint32_t c = 0U; c < 4U; ++c) {
                data_[quadword * 4U + c] = value[c];
            }
            used_words_ = std::max<std::size_t>(used_words_, (static_cast<std::size_t>(quadword) + 1U) * 4U);
        }
        dirty_ = true;
        return true;
    }

    std::array<std::uint32_t, kVuQuadwords * 4U> data_{};
    std::array<std::uint32_t, 4> row_{}, col_{};
    std::size_t used_words_{};
    std::uint32_t cl_{1U}, wl_{1U}, mode_{0U}, itop_{0U};
    bool dirty_{};
};

[[nodiscard]] inline std::uint64_t fnv(std::uint64_t h, const std::uint32_t word) noexcept {
    h ^= word;
    return h * 1099511628211ULL;
}

// Cheap identity of a static packet: its first tag, and a sample of its data (first, last and 22 evenly spaced quadwords).
template <class Memory>
[[nodiscard]] std::uint64_t packet_fingerprint(const Memory& memory, const std::uint32_t address) {
    std::uint64_t h = 14695981039346656037ULL;
    if (address == 0U) {
        return h;
    }
    const std::uint32_t word0 = memory.u32(address);
    const std::uint32_t qwc = word0 & 0xffffU;
    const std::uint32_t id = (word0 >> 28U) & 7U;
    const bool by_reference = id == 3U || id == 4U || id == 0U;
    const std::uint32_t data = by_reference ? (memory.u32(address + 4U) & 0x7fffffffU) : address + 16U;
    for (std::uint32_t k = 0U; k < 4U; ++k) {
        h = fnv(h, memory.u32(address + 4U * k));
    }
    if (qwc != 0U) {
        const std::uint32_t samples = std::min<std::uint32_t>(qwc, 24U);
        for (std::uint32_t s = 0U; s < samples; ++s) {
            const std::uint32_t index = samples == 1U ? 0U : static_cast<std::uint32_t>((static_cast<std::uint64_t>(s) * (qwc - 1U)) / (samples - 1U));
            for (std::uint32_t k = 0U; k < 4U; ++k) {
                h = fnv(h, memory.u32(data + 16U * index + 4U * k));
            }
        }
    }
    return h;
}

} // namespace mesh_detail

// The batches (one per launch) of an entry's two packets. `packet1` may be 0.
template <class Memory>
[[nodiscard]] bool decode_entry_meshes(const Memory& memory, const std::uint32_t packet0, const std::uint32_t packet1, EntryMeshes& out) {
    out.batches.clear();
    out.vertices = 0U;
    out.bytes = 0U;
    mesh_detail::VifImage vif;
    std::vector<std::uint32_t> words;
    const auto launch = [&out](mesh_detail::VifImage& image) -> bool {
        if (!image.pending()) {
            image.clear();
            return true;                                          // a launch of nothing (stale buffer): no batch
        }
        const std::uint32_t count = image.itop();
        const std::uint32_t quadwords = ((count + 3U) & ~3U) * 3U + 1U;
        if (count > native_cmd::kMaxGroupVertices || quadwords > mesh_detail::kVuQuadwords) {
            return false;
        }
        const std::uint32_t body_words = quadwords * 4U;
        auto body = std::make_shared<std::vector<std::uint8_t>>(sizeof(native_cmd::CarBatchHeader) + static_cast<std::size_t>(body_words) * 4U, std::uint8_t{0});
        const native_cmd::CarBatchHeader header{count, body_words, {0U, 0U}};
        std::memcpy(body->data(), &header, sizeof header);
        std::memcpy(body->data() + sizeof header, image.words(), static_cast<std::size_t>(body_words) * 4U);
        MeshBatch batch;
        batch.count = count;
        batch.body = std::move(body);
        out.bytes += batch.body->size();
        out.vertices += count;
        out.batches.push_back(std::move(batch));
        image.clear();
        return true;
    };
    for (int packet = 0; packet != 2; ++packet) {
        const std::uint32_t address = packet == 0 ? packet0 : packet1;
        if (address == 0U) {
            continue;
        }
        words.clear();
        if (!mesh_detail::gather_words(memory, address, words) || !vif.run(words, launch)) {
            out.ok = false;
            return false;
        }
        if (packet == 0 && !launch(vif)) {                        // the stub's MSCNT behind the constants
            out.ok = false;
            return false;
        }
    }
    if (packet1 == 0U && vif.pending() && !launch(vif)) {
        out.ok = false;
        return false;
    }
    out.ok = true;
    return true;
}

// The decoded entries, by their two packet addresses. Revalidated by a fingerprint of the packets on every use; a car file loaded
// over another at the same address decodes again. Emulation thread only (the images themselves are immutable and shared).
class MeshCache {
  public:
    template <class Memory>
    [[nodiscard]] EntryMeshes* find(const Memory& memory, const std::uint32_t packet0, const std::uint32_t packet1) {
        const std::uint64_t id = (static_cast<std::uint64_t>(packet0) << 32U) | packet1;
        const std::uint64_t fingerprint = mesh_detail::fnv(mesh_detail::packet_fingerprint(memory, packet0), static_cast<std::uint32_t>(mesh_detail::packet_fingerprint(memory, packet1)));
        auto it = map_.find(id);
        if (it != map_.end() && it->second.fingerprint == fingerprint) {
            return &it->second;
        }
        if (bytes_ > kBudgetBytes) {
            map_.clear();
            bytes_ = 0U;
            it = map_.end();
        }
        if (it != map_.end()) {
            bytes_ -= it->second.bytes;
        }
        EntryMeshes decoded;
        decoded.fingerprint = fingerprint;
        static_cast<void>(decode_entry_meshes(memory, packet0, packet1, decoded));
        bytes_ += decoded.bytes;
        EntryMeshes& slot = map_[id];
        slot = std::move(decoded);
        return &slot;
    }
    [[nodiscard]] std::size_t entries() const noexcept { return map_.size(); }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

  private:
    static constexpr std::size_t kBudgetBytes = 96U << 20U;
    std::unordered_map<std::uint64_t, EntryMeshes> map_;
    std::size_t bytes_{};
};

// ---- submissions ---------------------------------------------------------------------------------------------------------------

struct PendingSubmit {
    platform::CarSubmit submit;     // seq and generation are set when the ring slot is taken
    bool prefix{};                  // the stub carries the 4-quadword A+D prefix (qw 1006..1009)

    [[nodiscard]] std::uint32_t stub_quadwords() const noexcept {
        return prefix ? platform::car_submit::kStubQwWithPrefix : platform::car_submit::kStubQwMarkerOnly;
    }
};

struct BuildStats {
    std::uint64_t entries{}, submits{}, batches{}, empty_entries{};
    std::uint64_t bad_meshes{};          // an entry whose packets cannot be decoded: the call must go to the guest
    std::uint64_t vertex_mismatch{};     // decoded counts differ from part_entries' position counts (diagnostic)
    std::uint64_t truncated{};           // a list with more than kMaxEntries drawn entries: the call must go to the guest
};

inline constexpr std::uint32_t kViewBlockAddress = 0x00666450U;   // BeginCars UNPACKs these 10 quadwords to VU1 1014..1023

template <class Memory>
void load_view_block(const Memory& memory, float (&view)[platform::car_submit::kViewFloats]) {
    for (std::uint32_t i = 0U; i < platform::car_submit::kViewFloats; ++i) {
        const std::uint32_t word = memory.u32(kViewBlockAddress + 4U * i);
        std::memcpy(&view[i], &word, sizeof word);
    }
}

// The mesh key of a batch for an entry that is damaged (1013.z low half set) or not: what the trusted path computes.
[[nodiscard]] inline std::uint64_t batch_key(MeshBatch& batch, const bool damaged) {
    const std::size_t k = damaged ? 1U : 0U;
    if (!batch.has_key[k]) {
        const native_cmd::VuData vu{batch.body->data() + sizeof(native_cmd::CarBatchHeader)};
        batch.key[k] = native_cmd::hash_mix(native_cmd::group_hash(native_cmd::Decoder::kCar, vu, 0U, batch.count, 0xCA5ULL, damaged), batch.count);
        batch.has_key[k] = true;
    }
    return batch.key[k];
}

// Appends the submissions of one draw-list entry (more than one when it launches more than kMaxBatches batches; only the first
// one carries the prefix).
template <class Memory>
void append_entry_submits(const Memory& memory, const EntryDraw& entry, const float (&view)[platform::car_submit::kViewFloats],
                          MeshCache& cache, std::vector<PendingSubmit>& out, BuildStats& stats) {
    ++stats.entries;
    if (entry.batch[0] == 0U && entry.batch[1] == 0U) {
        ++stats.empty_entries;
        return;
    }
    EntryMeshes* meshes = cache.find(memory, entry.batch[0], entry.batch[1]);
    if (!meshes->ok) {
        ++stats.bad_meshes;
        return;
    }
    if (meshes->batches.empty()) {
        ++stats.empty_entries;
        return;
    }
    if (meshes->vertices != entry.vertices[0] + entry.vertices[1]) {
        ++stats.vertex_mismatch;
    }
    const bool damaged = (entry.qw[51][2] & 0xffffU) != 0U;
    std::size_t next = 0U;
    bool first = true;
    while (next < meshes->batches.size()) {
        PendingSubmit& pending = out.emplace_back();
        platform::CarSubmit& submit = pending.submit;
        std::memcpy(submit.qw, entry.qw.data(), sizeof submit.qw);
        std::memcpy(submit.view, view, sizeof submit.view);
        const std::size_t take = std::min<std::size_t>(platform::car_submit::kMaxBatches, meshes->batches.size() - next);
        for (std::size_t b = 0U; b < take; ++b, ++next) {
            MeshBatch& source = meshes->batches[next];
            platform::car_submit::Batch& batch = submit.batches[b];
            batch.mesh = source.body;
            batch.mesh_key = batch_key(source, damaged);
            batch.count = source.count;
            batch.env = entry.env;
        }
        submit.batch_count = static_cast<std::uint16_t>(take);
        pending.prefix = first && entry.prefix;
        first = false;
        ++stats.submits;
        stats.batches += take;
    }
}

// Every entry of every part draw of the plan, in the game's order. The shading state starts fresh (StartEffect's quads, the
// lighting matrix of the first lit part). Returns false when the call cannot be made natively (see BuildStats).
template <class Memory>
[[nodiscard]] bool build_call_submits(const Memory& memory, const CarDrawPlan& plan, CarShading& shading,
                                      std::array<EntryDraw, kMaxEntries>& entries, MeshCache& cache,
                                      std::vector<PendingSubmit>& out, BuildStats& stats) {
    float view[platform::car_submit::kViewFloats];
    load_view_block(memory, view);
    start_shading(memory, plan, shading);
    for (std::size_t p = 0U; p < plan.part_count; ++p) {
        const PartDraw& part = plan.parts[p];
        const std::size_t produced = part_entries(memory, plan, part, shading, entries);
        if (produced >= kMaxEntries) {
            ++stats.truncated;
            return false;
        }
        for (std::size_t e = 0U; e < produced; ++e) {
            append_entry_submits(memory, entries[e], view, cache, out, stats);
        }
    }
    return stats.bad_meshes == 0U && stats.truncated == 0U;
}

// Total guest quadwords of the stubs of `submits`.
[[nodiscard]] inline std::uint32_t stub_quadwords_total(const std::vector<PendingSubmit>& submits) noexcept {
    std::uint32_t total = 0U;
    for (const PendingSubmit& s : submits) {
        total += s.stub_quadwords();
    }
    return total;
}

// Pushes every submission into `ring` and writes its stub through `write_quad(index, {w0, w1, w2, w3})`, quadword index counted from
// the display list cursor. Returns the number of quadwords written (the cursor advance is 16 times that), or fewer submissions than
// given when the ring runs out (`pushed` tells how many).
template <class WriteQuad>
[[nodiscard]] std::uint32_t emit_submits(platform::CarSubmitRing& ring, const std::vector<PendingSubmit>& submits, WriteQuad&& write_quad,
                                         std::size_t& pushed) {
    namespace cs = platform::car_submit;
    std::uint32_t written = 0U;
    pushed = 0U;
    for (const PendingSubmit& pending : submits) {
        platform::CarSubmit* slot = ring.begin_push();
        if (slot == nullptr) {
            break;
        }
        const std::uint32_t seq = slot->seq;
        const std::uint16_t generation = slot->generation;
        *slot = pending.submit;
        slot->seq = seq;
        slot->generation = generation;

        std::array<native_cmd::Quad, cs::kPrefixQw> prefix{};
        if (pending.prefix) {
            for (std::uint32_t i = 0U; i < cs::kPrefixQw; ++i) {
                for (std::uint32_t l = 0U; l < 4U; ++l) {
                    prefix[i][l] = pending.submit.qw[44U + i][l];
                }
            }
        }
        std::uint32_t stub[4U * cs::kStubQwWithPrefix];
        const std::size_t quadwords = cs::write_stub(stub, pending.prefix ? prefix.data() : nullptr, cs::Marker{seq, generation, cs::MarkerKind::kCarEntry});
        for (std::size_t q = 0U; q < quadwords; ++q) {
            write_quad(written + static_cast<std::uint32_t>(q), std::array<std::uint32_t, 4>{stub[4U * q], stub[4U * q + 1U], stub[4U * q + 2U], stub[4U * q + 3U]});
        }
        written += static_cast<std::uint32_t>(quadwords);
        ring.commit_push();
        ++pushed;
    }
    return written;
}

} // namespace takedown::runtime::native_car
