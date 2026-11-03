#include "takedown/runtime/native_world_mesh.hpp"

#include <algorithm>
#include <bit>
#include <bitset>
#include <cstring>

namespace takedown::runtime::world_mesh {

namespace {

constexpr std::uint32_t kMaxTags = 512U;
constexpr std::uint32_t kMaxWords = 1U << 20U;
constexpr std::uint32_t kVuQuadwords = 1024U;
// Bytes of source data per vector for each UNPACK format nibble (0 = undefined), EeMemory::expand_vif1_unpack_into.
constexpr std::array<std::uint8_t, 16> kSourceBytes = {4U, 2U, 1U, 0U, 8U, 4U, 2U, 0U, 12U, 6U, 3U, 0U, 16U, 8U, 4U, 2U};

[[nodiscard]] bool map_main_ram(std::uint32_t address, std::size_t size, std::uint32_t& offset) noexcept {
    const std::uint32_t segment = address & 0xE0000000U;
    std::uint32_t a = address;
    if (segment == 0U || segment == 0x80000000U || segment == 0xA0000000U) {
        a &= 0x1FFFFFFFU;
    } else if (a >= 0x30100000U && a <= 0x31FFFFFFU) {
        a -= 0x30000000U;
    } else if (a >= 0x20080000U && a < 0x22000000U) {
        a -= 0x20000000U;
    } else {
        return false;
    }
    if (static_cast<std::size_t>(a) + 16U > size) {
        return false;
    }
    offset = a;
    return true;
}

// ---- VIF1 UNPACK source expansion: EeMemory::expand_vif1_unpack_into -------------------------------------------------
struct Unpack {
    std::uint32_t type{};      // format nibble
    bool zero_extend{};        // usn
    std::uint32_t count{};     // destination vectors (NUM, 0 = 256)
    std::uint32_t src_count{}; // source vectors
    std::uint32_t align{};     // payload word lane of the command + 1
    std::uint32_t dest{};      // destination quadword
    std::uint32_t words_needed{};
    std::vector<std::uint32_t> words;
    bool active{false};
};

class Expander {
  public:
    Expander(const std::uint32_t* words, std::uint32_t type, bool zero) : bytes_(reinterpret_cast<const std::uint8_t*>(words)), type_(type), zero_(zero) {}
    // Fills 4 lanes for source vector `index` of `count`.
    void vector(std::uint32_t index, std::uint32_t count, std::uint32_t out[4]) const noexcept {
        const std::size_t at = static_cast<std::size_t>(index) * kSourceBytes[type_];
        const bool more = index + 1U < count;
        switch (type_) {
        case 0U: { const std::uint32_t v = u32(at); out[0] = out[1] = out[2] = out[3] = v; break; }
        case 1U: { const std::uint32_t v = e16(at); out[0] = out[1] = out[2] = out[3] = v; break; }
        case 2U: { const std::uint32_t v = e8(at); out[0] = out[1] = out[2] = out[3] = v; break; }
        case 4U: { out[0] = out[2] = u32(at); out[1] = out[3] = u32(at + 4U); break; }
        case 5U: { out[0] = out[2] = e16(at); out[1] = out[3] = e16(at + 2U); break; }
        case 6U: { out[0] = out[2] = e8(at); out[1] = out[3] = e8(at + 1U); break; }
        case 8U: out[0] = u32(at); out[1] = u32(at + 4U); out[2] = u32(at + 8U); out[3] = more ? u32(at + 12U) : 0U; break;
        case 9U: out[0] = e16(at); out[1] = e16(at + 2U); out[2] = e16(at + 4U); out[3] = more ? e16(at + 6U) : 0U; break;
        case 10U: out[0] = e8(at); out[1] = e8(at + 1U); out[2] = e8(at + 2U); out[3] = more ? e8(at + 3U) : 0U; break;
        case 12U: out[0] = u32(at); out[1] = u32(at + 4U); out[2] = u32(at + 8U); out[3] = u32(at + 12U); break;
        case 13U: out[0] = e16(at); out[1] = e16(at + 2U); out[2] = e16(at + 4U); out[3] = e16(at + 6U); break;
        case 14U: out[0] = e8(at); out[1] = e8(at + 1U); out[2] = e8(at + 2U); out[3] = e8(at + 3U); break;
        default: {
            const std::uint32_t value = u16(at);
            out[0] = (value << 3U) & 255U;
            out[1] = (value >> 2U) & 248U;
            out[2] = (value >> 7U) & 248U;
            out[3] = (value >> 8U) & 128U;
            break;
        }
        }
    }

  private:
    [[nodiscard]] std::uint32_t u16(std::size_t at) const noexcept {
        return static_cast<std::uint32_t>(bytes_[at]) | (static_cast<std::uint32_t>(bytes_[at + 1U]) << 8U);
    }
    [[nodiscard]] std::uint32_t u32(std::size_t at) const noexcept {
        return u16(at) | (u16(at + 2U) << 16U);
    }
    [[nodiscard]] std::uint32_t e16(std::size_t at) const noexcept {
        const std::uint32_t raw = u16(at);
        return zero_ ? raw : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(raw)));
    }
    [[nodiscard]] std::uint32_t e8(std::size_t at) const noexcept {
        const std::uint32_t raw = bytes_[at];
        return zero_ ? raw : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(raw)));
    }
    const std::uint8_t* bytes_;
    std::uint32_t type_;
    bool zero_;
};

// ---- the walker -------------------------------------------------------------------------------------------------------
class PacketWalker {
  public:
    PacketWalker(const QwReader& ram, std::uint16_t program, const ImportOptions& options, std::vector<ImportedGroup>& out)
        : ram_(ram), program_(program), options_(options), out_(out), decoder_(decoder_for_program(program)) {
        image_.fill(0U);
    }

    ImportStatus run(std::uint32_t address, std::uint32_t* qw_read) {
        if (decoder_ == nc::Decoder::kNone || decoder_ == nc::Decoder::kCar) {
            return ImportStatus::kBadCommand;
        }
        std::uint32_t at = address;
        std::uint32_t asr[2]{};
        int depth = 0;
        for (std::uint32_t tags = 0;; ++tags) {
            if (tags > kMaxTags || words_ > kMaxWords) {
                return ImportStatus::kBadTag;
            }
            std::uint32_t q[4];
            if (!ram_.read_qw(at, q)) {
                return ImportStatus::kUnreadable;
            }
            if (qw_read != nullptr) {
                ++*qw_read;
            }
            const std::uint32_t qwc = q[0] & 0xFFFFU, id = (q[0] >> 28U) & 7U;
            if (!feed(q[2], 0U) || !feed(q[3], 1U)) {
                return status_;
            }
            std::uint32_t payload = at + 16U;
            if (id == 0U || id == 3U || id == 4U) { // REFE, REF, REFS: payload at ADDR
                if ((q[1] & 0x80000000U) != 0U) {
                    return ImportStatus::kUnreadable;
                }
                payload = q[1] & 0x7FFFFFFFU;
            }
            for (std::uint32_t k = 0; k != qwc; ++k) {
                std::uint32_t d[4];
                if (!ram_.read_qw(payload + 16U * k, d)) {
                    return ImportStatus::kUnreadable;
                }
                if (qw_read != nullptr) {
                    ++*qw_read;
                }
                for (std::uint32_t lane = 0; lane != 4U; ++lane) {
                    if (!feed(d[lane], lane)) {
                        return status_;
                    }
                }
            }
            switch (id) {
            case 0U: // REFE
            case 7U: // END
                return finish();
            case 1U: // CNT
                at = at + 16U + 16U * qwc;
                break;
            case 2U: // NEXT
                at = q[1] & 0x7FFFFFFFU;
                break;
            case 3U: // REF
            case 4U: // REFS
                at += 16U;
                break;
            case 5U: // CALL
                if (depth >= 2) {
                    return ImportStatus::kBadTag;
                }
                asr[depth++] = at + 16U + 16U * qwc;
                at = q[1] & 0x7FFFFFFFU;
                break;
            default: // RET
                if (depth == 0) {
                    return finish();
                }
                at = asr[--depth];
                break;
            }
        }
    }

  private:
    bool fail(ImportStatus status) {
        status_ = status;
        return false;
    }

    // One VIF word; lane = position in the payload quadword (the UNPACK start alignment depends on it).
    bool feed(std::uint32_t word, std::uint32_t lane) {
        ++words_;
        if (unpack_.active) {
            unpack_.words.push_back(word);
            if (unpack_.words.size() == unpack_.words_needed) {
                unpack_.active = false;
                return execute_unpack();
            }
            return true;
        }
        const std::uint32_t command = (word >> 24U) & 0x7FU, imm = word & 0xFFFFU;
        switch (command) {
        case 0x00U: // NOP
        case 0x02U: // OFFSET
        case 0x03U: // BASE
        case 0x06U: // MSKPATH3
        case 0x07U: // MARK
        case 0x10U: case 0x11U: case 0x13U: // FLUSHE, FLUSH, FLUSHA
        case 0x14U: case 0x15U: case 0x17U: // MSCAL, MSCALF, MSCNT
            return true;
        case 0x01U: // STCYCL
            cl_ = imm & 255U;
            wl_ = (imm >> 8U) & 255U;
            if (wl_ == 0U) {
                wl_ = 256U;
            }
            return cl_ != 0U ? true : fail(ImportStatus::kBadCommand);
        case 0x04U: // ITOP
            return finish_group(imm & 0x3FFU);
        case 0x05U: // STMOD: only mode 0 (the world meshes never add rows)
            return (imm & 3U) == 0U ? true : fail(ImportStatus::kBadCommand);
        default:
            break;
        }
        if ((command & 0x60U) != 0x60U || (command & 0x10U) != 0U) { // UNPACK, unmasked only
            return fail(ImportStatus::kBadCommand);
        }
        const std::uint32_t type = command & 15U;
        if (kSourceBytes[type] == 0U) {
            return fail(ImportStatus::kBadCommand);
        }
        unpack_.type = type;
        unpack_.zero_extend = (imm & 0x4000U) != 0U;
        unpack_.count = (word >> 16U) & 255U;
        if (unpack_.count == 0U) {
            unpack_.count = 256U;
        }
        unpack_.align = lane + 1U;
        unpack_.dest = imm & 0x3FFU; // TOPS-relative addressing (flag bit) with TOPS = 0: every group is decoded at the same base
        unpack_.src_count = cl_ >= wl_ ? unpack_.count : cl_ * (unpack_.count / wl_) + std::min(cl_, unpack_.count % wl_);
        const std::uint64_t bytes = static_cast<std::uint64_t>(kSourceBytes[type]) * unpack_.src_count;
        unpack_.words_needed = static_cast<std::uint32_t>((bytes + 3U) / 4U);
        unpack_.words.clear();
        if (unpack_.words_needed == 0U) {
            return execute_unpack();
        }
        unpack_.words.reserve(unpack_.words_needed);
        unpack_.active = true;
        return true;
    }

    // EeMemory::finish_unpack for MODE 0, unmasked: filling / skipping write, V3 look-ahead on lane 3.
    bool execute_unpack() {
        unpack_.words.resize(std::max<std::size_t>(unpack_.words.size(), 4U), 0U); // 16-byte slack for the look-ahead reads
        const Expander expander(unpack_.words.data(), unpack_.type, unpack_.zero_extend);
        std::vector<std::uint32_t> lanes(static_cast<std::size_t>(unpack_.src_count) * 4U);
        for (std::uint32_t v = 0; v != unpack_.src_count; ++v) {
            expander.vector(v, unpack_.src_count, lanes.data() + 4U * v);
        }
        const bool filling = wl_ > cl_;
        std::uint32_t source = 0;
        for (std::uint32_t out = 0; out != unpack_.count; ++out) {
            const std::uint32_t slot = out % wl_;
            const bool fill_slot = (slot >= cl_) && filling;
            const std::uint32_t current = source;
            if (!fill_slot) {
                ++source;
            }
            const std::uint32_t dest = (wl_ > cl_ ? unpack_.dest + out : unpack_.dest + (out / wl_) * cl_ + slot) & (kVuQuadwords - 1U);
            for (std::uint32_t lane = 0; lane != 4U; ++lane) {
                std::uint32_t data = unpack_.src_count > current ? lanes[static_cast<std::size_t>(current) * 4U + lane] : 0U;
                if (lane == 3U && unpack_.src_count > current + 1U) {
                    if (unpack_.type == 8U) {
                        if ((((unpack_.align ^ current) & 1U) ^ 1U) == 0U) {
                            data = 0U;
                        }
                    } else if (unpack_.type == 9U) {
                        const std::uint32_t boundary = (((current + 1U) >> 2U) - unpack_.align + 5U) & 3U;
                        if (!(((current + 1U) & 1U) != 0U || boundary != 0U)) {
                            data = 0U;
                        }
                    } else if (unpack_.type == 10U) {
                        if (current + 1U != unpack_.align) {
                            data = 0U;
                        }
                    }
                }
                image_[static_cast<std::size_t>(dest) * 4U + lane] = data;
            }
            written_.set(dest);
        }
        return true;
    }

    // Quadword (3 * i + lane) written for every / any vertex i < n.
    [[nodiscard]] bool covered_all(std::uint32_t lane, std::uint32_t n, std::uint32_t stride) const noexcept {
        for (std::uint32_t i = 0; i != n; ++i) {
            if (!written_.test((stride * i + lane) & (kVuQuadwords - 1U))) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] bool any_covered(std::uint32_t lane, std::uint32_t n, std::uint32_t stride) const noexcept {
        for (std::uint32_t i = 0; i != n; ++i) {
            if (written_.test((stride * i + lane) & (kVuQuadwords - 1U))) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool covered(std::uint32_t first, std::uint32_t count) const noexcept {
        for (std::uint32_t k = 0; k != count; ++k) {
            if (!written_.test((first + k) & (kVuQuadwords - 1U))) {
                return false;
            }
        }
        return true;
    }

    bool finish_group(std::uint32_t n) {
        if (n == 0U || n > nc::kMaxGroupVertices) {
            return fail(ImportStatus::kBadGroup);
        }
        // The decoders read only what the UNPACKs wrote. Positions are required; the uv and colour blocks must be written for all
        // vertices or for none (an untextured / line mesh), otherwise a vertex would read stale VU memory.
        const bool colour = decoder_ != nc::Decoder::kProp;
        std::uint8_t missing = 0;
        if (!covered_all(0U, n, 3U)) {
            return fail(ImportStatus::kBadGroup);
        }
        if (!covered_all(1U, n, 3U)) {
            if (any_covered(1U, n, 3U)) {
                return fail(ImportStatus::kBadGroup);
            }
            missing |= ImportedGroup::kMissingUv;
        }
        if (colour && !covered_all(2U, n, 3U)) {
            if (any_covered(2U, n, 3U)) {
                return fail(ImportStatus::kBadGroup);
            }
            missing |= ImportedGroup::kMissingColour;
        }
        if (decoder_ == nc::Decoder::kWorld3N) {
            if (n > nc::kWorld3NMaxVertices) {
                return fail(ImportStatus::kBadGroup);
            }
            for (std::uint32_t g = 0; g != (n + 3U) >> 2U; ++g) {
                if (!covered(nc::kWorld3NNormalOffset + 4U * g, 4U)) {
                    return fail(ImportStatus::kBadGroup);
                }
            }
        }
        const nc::VuData vu{reinterpret_cast<const std::uint8_t*>(image_.data())};
        ImportedGroup group;
        group.n = n;
        group.missing = missing;
        group.key = nc::group_hash(decoder_, vu, 0U, n, mesh_seed(program_));
        if (options_.scroll) {
            group.key = nc::hash_mix(group.key, (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(options_.uv_offset[0])) << 32U) |
                                                    std::bit_cast<std::uint32_t>(options_.uv_offset[1]));
        }
        if (decoder_ == nc::Decoder::kProp) {
            nc::decode_prop(vu, 0U, n, group.mesh);
        } else {
            nc::decode_world3(vu, 0U, n, decoder_ == nc::Decoder::kWorld3N, group.mesh);
            if (options_.scroll && (options_.uv_offset[0] != 0.0F || options_.uv_offset[1] != 0.0F)) {
                for (std::uint32_t i = 0; i != n; ++i) {
                    group.mesh.uv[2U * i] += options_.uv_offset[0];
                    group.mesh.uv[2U * i + 1U] += options_.uv_offset[1];
                }
            }
        }
        for (std::uint32_t c = 0; c != 3U; ++c) {
            group.lo[c] = group.hi[c] = group.mesh.pos[c];
        }
        for (std::uint32_t i = 1; i != n; ++i) {
            for (std::uint32_t c = 0; c != 3U; ++c) {
                group.lo[c] = std::min(group.lo[c], group.mesh.pos[3U * i + c]);
                group.hi[c] = std::max(group.hi[c], group.mesh.pos[3U * i + c]);
            }
        }
        out_.push_back(std::move(group));
        image_.fill(0U);
        written_.reset();
        return true;
    }

    ImportStatus finish() {
        if (unpack_.active || written_.any()) { // data without an ITOP
            return ImportStatus::kBadGroup;
        }
        return ImportStatus::kOk;
    }

    const QwReader& ram_;
    std::uint16_t program_;
    ImportOptions options_;
    std::vector<ImportedGroup>& out_;
    nc::Decoder decoder_;
    std::array<std::uint32_t, kVuQuadwords * 4U> image_{};
    std::bitset<kVuQuadwords> written_;
    Unpack unpack_;
    std::uint32_t cl_{1U}, wl_{1U};
    std::uint32_t words_{};
    ImportStatus status_{ImportStatus::kOk};
};

[[nodiscard]] std::uint32_t quad_hash(const std::uint32_t q[4]) noexcept {
    std::uint32_t h = 2166136261U;
    for (int i = 0; i != 4; ++i) {
        h = (h ^ q[i]) * 16777619U;
    }
    return h;
}

} // namespace

// ---- QwReader / MainRamView -------------------------------------------------------------------------------------------
bool QwReader::read_u32(std::uint32_t address, std::uint32_t& out) const noexcept {
    std::uint32_t q[4];
    if (!read_qw(address & ~15U, q)) {
        return false;
    }
    out = q[(address >> 2U) & 3U];
    return true;
}

bool MainRamView::read_qw(std::uint32_t address, std::uint32_t out[4]) const noexcept {
    std::uint32_t offset = 0;
    if (!map_main_ram(address, size_, offset)) {
        return false;
    }
    std::memcpy(out, data_ + offset, 16U);
    return true;
}

// ---- programs ---------------------------------------------------------------------------------------------------------
std::uint16_t program_for_material(const std::uint32_t kind, const std::uint32_t flags) noexcept {
    const bool alt = (flags & 0x20U) != 0U;
    switch (kind) {
    case 0U: case 6U: return alt ? 11U : 10U;
    case 1U: return alt ? 4U : 3U;
    case 3U: return 13U;
    case 7U: return 12U;
    case 4U: case 5U: return 14U;
    default: return 0U;
    }
}

nc::Decoder decoder_for_program(const std::uint16_t program) noexcept {
    switch (program) {
    case 10U: case 11U: case 12U: case 13U: case 0xFEU: return nc::Decoder::kWorld3;
    case 3U: case 4U: return nc::Decoder::kWorld3N;
    case 14U: return nc::Decoder::kProp;
    default: return nc::Decoder::kNone;
    }
}

std::uint64_t mesh_seed(const std::uint16_t program) noexcept {
    return static_cast<std::uint64_t>(program) * 0x9E3779B1ULL + 1U;
}

// ---- import -----------------------------------------------------------------------------------------------------------
std::uint64_t ImportedMesh::bytes() const noexcept {
    std::uint64_t total = sizeof(ImportedMesh);
    for (const ImportedGroup& g : groups) {
        total += sizeof(ImportedGroup) + g.mesh.pos.size() * 4U + g.mesh.flags.size() * 4U + g.mesh.uv.size() * 4U + g.mesh.col.size() +
                 g.mesh.nrm.size() * 4U;
    }
    return total;
}

ImportStatus import_packet(const QwReader& ram, const std::uint32_t address, const std::uint16_t program, const ImportOptions& options,
                           std::vector<ImportedGroup>& out, std::uint32_t* qw_read) {
    const std::size_t before = out.size();
    PacketWalker walker(ram, program, options, out);
    const ImportStatus status = walker.run(address, qw_read);
    if (status != ImportStatus::kOk) {
        out.resize(before);
    }
    return status;
}

std::shared_ptr<ImportedMesh> import_mesh(const QwReader& ram, const std::uint32_t packet, const std::uint32_t cont,
                                          const std::uint16_t program, const ImportOptions& options) {
    auto mesh = std::make_shared<ImportedMesh>();
    mesh->program = program;
    mesh->packet = packet;
    mesh->cont = cont;
    ImportStatus status = ImportStatus::kNoGroups;
    if (packet != 0U) {
        status = import_packet(ram, packet, program, options, mesh->groups, &mesh->qw_read);
    }
    if (status == ImportStatus::kOk && cont != 0U) {
        status = import_packet(ram, cont, program, options, mesh->groups, &mesh->qw_read);
    }
    if (status == ImportStatus::kOk && mesh->groups.empty()) {
        status = ImportStatus::kNoGroups;
    }
    mesh->status = status;
    if (status != ImportStatus::kOk) {
        mesh->groups.clear();
    }
    return mesh;
}

// ---- owner blocks -----------------------------------------------------------------------------------------------------
std::size_t OwnerTable::rebuild(const QwReader& ram, const std::uint32_t wctx) {
    blocks_.clear();
    std::uint32_t dims[2]{};
    if (!ram.read_u32(wctx + kGridXDimOffset, dims[0]) || !ram.read_u32(wctx + kGridYDimOffset, dims[1])) {
        return 0;
    }
    const std::uint32_t arrays[2] = {kGridXOffset, kGridYOffset};
    for (int g = 0; g != 2; ++g) {
        // bounded: the arrays hold 10 / 18 entries in the sample, WCTX + 340 .. +380 and +380 .. +452 are all that is reserved
        const std::uint32_t limit = g == 0 ? 10U : 18U;
        for (std::uint32_t i = 0; i < std::min(dims[g], limit); ++i) {
            std::uint32_t base = 0;
            if (!ram.read_u32(wctx + arrays[g] + 4U * i, base) || base == 0U) {
                continue;
            }
            std::uint32_t q[4];
            if (!ram.read_qw(base, q) || q[2] == 0U || q[2] >= 0x1000000U) { // blocks still loading stay listed: their status is checked at use
                continue;
            }
            blocks_.push_back(OwnerBlock{base, base + q[2], BlockHeader{q[0], q[1], q[2]}});
        }
    }
    std::sort(blocks_.begin(), blocks_.end(), [](const OwnerBlock& a, const OwnerBlock& b) { return a.base < b.base; });
    return blocks_.size();
}

void OwnerTable::set_blocks(std::vector<OwnerBlock> blocks) {
    blocks_ = std::move(blocks);
    std::sort(blocks_.begin(), blocks_.end(), [](const OwnerBlock& a, const OwnerBlock& b) { return a.base < b.base; });
}

const OwnerBlock* OwnerTable::find(const std::uint32_t address) const noexcept {
    auto it = std::upper_bound(blocks_.begin(), blocks_.end(), address, [](std::uint32_t a, const OwnerBlock& b) { return a < b.base; });
    if (it == blocks_.begin()) {
        return nullptr;
    }
    --it;
    return address < it->end ? &*it : nullptr;
}

// ---- registry ---------------------------------------------------------------------------------------------------------
bool WorldMeshRegistry::Key::operator<(const Key& o) const noexcept {
    if (packet != o.packet) return packet < o.packet;
    if (cont != o.cont) return cont < o.cont;
    if (program != o.program) return program < o.program;
    if (scroll != o.scroll) return scroll < o.scroll;
    if (uv_bits[0] != o.uv_bits[0]) return uv_bits[0] < o.uv_bits[0];
    return uv_bits[1] < o.uv_bits[1];
}

WorldMeshRegistry::Snapshot WorldMeshRegistry::take_snapshot(const QwReader& ram, const std::uint32_t packet, const std::uint32_t cont) const noexcept {
    Snapshot snap;
    const std::uint32_t addresses[2] = {packet, cont};
    for (int k = 0; k != 2; ++k) {
        const std::uint32_t a = addresses[k];
        if (a == 0U) {
            continue;
        }
        std::uint32_t q[4];
        if (ram.read_qw(a, q)) {
            snap.tag[k] = q[0];
            std::uint32_t last[4];
            if ((q[0] & 0xFFFFU) != 0U && ram.read_qw(a + 16U * (q[0] & 0xFFFFU), last)) {
                snap.last[k] = quad_hash(last);
            }
        }
        if (const OwnerBlock* block = owners_.find(a)) {
            // The header is read from RAM now (the table may be older than the block that is there at the moment).
            std::uint32_t h[4];
            if (ram.read_qw(block->base, h)) {
                snap.has_owner[k] = true;
                snap.owner[k] = OwnerBlock{block->base, block->end, BlockHeader{h[0], h[1], h[2]}};
            }
        }
    }
    return snap;
}

bool WorldMeshRegistry::snapshot_valid(const QwReader& ram, const Snapshot& snap, const std::uint32_t packet, const std::uint32_t cont) const noexcept {
    const std::uint32_t addresses[2] = {packet, cont};
    for (int k = 0; k != 2; ++k) {
        const std::uint32_t a = addresses[k];
        if (a == 0U) {
            continue;
        }
        if (snap.has_owner[k]) {
            std::uint32_t q[4];
            if (!ram.read_qw(snap.owner[k].base, q) || q[0] != snap.owner[k].header.status || q[1] != snap.owner[k].header.id ||
                q[2] != snap.owner[k].header.size) {
                return false;
            }
        }
        std::uint32_t q[4];
        if (!ram.read_qw(a, q) || q[0] != snap.tag[k]) {
            return false;
        }
        if ((q[0] & 0xFFFFU) != 0U) {
            std::uint32_t last[4];
            if (!ram.read_qw(a + 16U * (q[0] & 0xFFFFU), last) || quad_hash(last) != snap.last[k]) {
                return false;
            }
        }
    }
    return true;
}

void WorldMeshRegistry::begin_frame() {
    ++frame_;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (frame_ - it->second.last_frame > config_.idle_frames) {
            if (it->second.mesh) {
                bytes_ -= it->second.mesh->bytes();
            }
            it = entries_.erase(it);
            ++stats_.evicted;
        } else {
            ++it;
        }
    }
}

void WorldMeshRegistry::clear() {
    entries_.clear();
    bytes_ = 0;
}

std::shared_ptr<const ImportedMesh> WorldMeshRegistry::acquire(const QwReader& ram, const std::uint32_t packet, const std::uint32_t cont,
                                                              const std::uint16_t program, const ImportOptions& options) {
    Key key;
    key.packet = packet;
    key.cont = cont;
    key.program = program;
    key.scroll = options.scroll ? 1U : 0U;
    if (options.scroll) {
        key.uv_bits[0] = std::bit_cast<std::uint32_t>(options.uv_offset[0]);
        key.uv_bits[1] = std::bit_cast<std::uint32_t>(options.uv_offset[1]);
    }
    const auto it = entries_.find(key);
    if (it != entries_.end()) {
        Entry& e = it->second;
        if (e.epoch == epoch_ && snapshot_valid(ram, e.snap, packet, cont)) {
            e.last_frame = frame_;
            if (config_.verify_every_use && e.mesh) {
                const auto again = import_mesh(ram, packet, cont, program, options);
                bool same = again->status == ImportStatus::kOk && again->groups.size() == e.mesh->groups.size();
                for (std::size_t i = 0; same && i != again->groups.size(); ++i) {
                    same = again->groups[i].key == e.mesh->groups[i].key;
                }
                if (!same) {
                    bytes_ -= e.mesh->bytes();
                    entries_.erase(it);
                    ++stats_.invalidated;
                    return acquire(ram, packet, cont, program, options);
                }
            }
            ++stats_.hits;
            return e.mesh;
        }
        if (e.mesh) {
            bytes_ -= e.mesh->bytes();
        }
        entries_.erase(it);
        ++stats_.invalidated;
    }
    Entry e;
    e.snap = take_snapshot(ram, packet, cont);
    e.epoch = epoch_;
    e.last_frame = frame_;
    // A block that is still being loaded is not remembered: it is imported again once its header says "loaded".
    for (int k = 0; k != 2; ++k) {
        if (e.snap.has_owner[k] && e.snap.owner[k].header.status != 2U) {
            ++stats_.failures;
            return nullptr;
        }
    }
    auto mesh = import_mesh(ram, packet, cont, program, options);
    if (mesh->status == ImportStatus::kOk) {
        e.mesh = std::move(mesh);
        bytes_ += e.mesh->bytes();
        ++stats_.imports;
    } else {
        ++stats_.failures;
    }
    std::shared_ptr<const ImportedMesh> result = e.mesh;
    entries_.emplace(key, std::move(e));
    while (bytes_ > config_.max_bytes && entries_.size() > 1U) { // evict the least recently used entry
        auto oldest = entries_.begin();
        for (auto scan = entries_.begin(); scan != entries_.end(); ++scan) {
            if (scan->second.last_frame < oldest->second.last_frame && scan->second.mesh != result) {
                oldest = scan;
            }
        }
        if (oldest->second.mesh == result) {
            break;
        }
        if (oldest->second.mesh) {
            bytes_ -= oldest->second.mesh->bytes();
        }
        entries_.erase(oldest);
        ++stats_.evicted;
    }
    return result;
}

} // namespace takedown::runtime::world_mesh
