#pragma once

#include "takedown/audio/ps_adpcm.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takedown::audio {

// RenderWare stream chunk identifiers observed on the SLUS-21050 disc.
// A Takedown AWD "sound bank" is an outer 0x0809 envelope; the per-track
// music and effects live in sibling 0x080d stream containers (E_DJRACE.RWS
// and friends under TRACKS/AS/...). The 0x080d hierarchy is:
//
//   0x080d { size, version }            outer stream container
//     0x080e { size, version }          stream header (file uuid + name,
//                                       segment table, layer table,
//                                       layer configurations)
//        ...                            string table + segment names
//     0x080f { size, version }          interleaved audio data
//
// All fields are little-endian. The stream header layout below was derived
// from real E_DJRACE.RWS bytes (38 segments, one layer, PlayStation ADPCM,
// 32,000 Hz, stereo) and cross-checked against the public vgmstream format
// record for 0x080d streams.
struct RwChunkEnvelope {
    std::uint32_t id{0};
    std::uint32_t size{0};
    std::uint32_t library_version{0};
};

struct RwsLayerDescription {
    // From the 0x080e layer configuration block.
    std::uint32_t sample_rate{0};
    std::uint8_t bits_per_sample{0};
    std::uint8_t channels{0};
    std::uint32_t codec_id{0}; // 0xd9ea9798 = PlayStation ADPCM
    // From the 0x080e layer info block. padded_block_size is this layer's
    // direct +0x10 contribution. block_layers_size is the aggregate padded
    // stride of all layers in one physical group (the sum of every layer's
    // contribution), populated after the complete layer table is parsed.
    // Each channel occupies `interleave` bytes within this layer (channel c
    // at group_base + layer_start + c * interleave).
    std::uint32_t padded_block_size{0};
    std::uint32_t block_layers_size{0};
    std::uint16_t interleave{0};
    std::uint16_t frame_size{0}; // 16 for PlayStation ADPCM
    std::uint32_t block_size{0};
    std::uint32_t layer_start{0};
};

struct RwsSegmentDescription {
    std::string name{};
    std::uint32_t offset{0};      // byte offset of the segment's first layer data
                                  // relative to the 0x080f payload start
    std::uint32_t layers_size{0}; // bytes covered by all layers of this segment
    // Kept as the authoritative layer-zero value for callers written before
    // multi-layer metadata was exposed. Zero means no usable audio bytes.
    std::uint32_t usable_bytes{0};
    // One authoritative unpadded byte count per layer, in layer-table order.
    // The table is mandatory; a zero entry represents a padding-only layer.
    std::vector<std::uint32_t> layer_usable_bytes{};
};

// Parser for the observed 0x080d RenderWare Audio stream container. It is
// deliberately limited to the structure proven on the disc: it validates the
// chunk envelope chain and produces the segment/layer table needed to locate
// and decode PlayStation ADPCM data. Codecs other than 0xd9ea9798 are
// reported but not decoded. Parsed metadata is owned by the container, but
// data_payload() is a view into the span supplied to parse(); callers must
// keep that backing storage alive and at a stable address while using it.
class RwaStreamContainer {
  public:
    static constexpr std::uint32_t kRwChunkStream = 0x080dU;
    static constexpr std::uint32_t kRwChunkStreamHeader = 0x080eU;
    static constexpr std::uint32_t kRwChunkStreamData = 0x080fU;
    static constexpr std::uint32_t kCodecPlayStationAdpcm = 0xd9ea9798U;
    static constexpr std::uint32_t kPlayStationAdpcmSampleRate = 32000U;

    // Parses `bytes` as an 0x080d stream container. Returns nullopt with
    // `error` populated when the envelope chain or tables are inconsistent.
    [[nodiscard]] static std::optional<RwaStreamContainer> parse(
        std::span<const std::byte> bytes, std::string* error = nullptr);

    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
    [[nodiscard]] std::string_view file_name() const noexcept { return file_name_; }
    [[nodiscard]] std::span<const RwsLayerDescription> layers() const noexcept {
        return layers_;
    }
    [[nodiscard]] std::span<const RwsSegmentDescription> segments() const noexcept {
        return segments_;
    }
    [[nodiscard]] std::size_t segment_count() const noexcept { return segments_.size(); }
    [[nodiscard]] std::size_t layer_count() const noexcept { return layers_.size(); }

    // The raw 0x080f payload (interleaved audio blocks).
    [[nodiscard]] std::span<const std::byte> data_payload() const noexcept {
        return data_payload_;
    }
    // Absolute file offset of the first byte after the 0x080f envelope. This
    // remains valid when parse() was given only a header prefix and lets
    // streaming consumers map later file-offset deliveries into the payload.
    [[nodiscard]] std::size_t data_payload_file_offset() const noexcept {
        return data_payload_file_offset_;
    }
    // Payload byte count declared by the 0x080f chunk envelope. This may be
    // larger than data_payload().size() when parse() consumed a prefix.
    [[nodiscard]] std::uint32_t declared_data_payload_size() const noexcept {
        return declared_data_payload_size_;
    }

    // Physical byte range [first, last) inside the 0x080f payload covered by
    // the requested segment, including every layer and alignment padding.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
    segment_data_range(std::size_t segment_index) const;

    // Decodes one segment into interleaved PCM. Each channel gets its own
    // decoder so predictor history stays continuous across block boundaries
    // (verified against the real data: most block-start frames use non-zero
    // predictor entries). Returns nullopt when layout assumptions fail
    // (e.g. a codec other than PlayStation ADPCM, or a multi-layer layout
    // that is not yet mapped).
    [[nodiscard]] std::optional<std::vector<std::int16_t>>
    decode_segment(std::size_t segment_index) const;

  private:
    std::uint32_t version_{0};
    std::string file_name_{};
    std::vector<RwsLayerDescription> layers_{};
    std::vector<RwsSegmentDescription> segments_{};
    std::size_t data_payload_file_offset_{0};
    std::uint32_t declared_data_payload_size_{0};
    std::span<const std::byte> data_payload_{};
};

} // namespace takedown::audio
