#include "takedown/audio/rws_stream.hpp"

#include <algorithm>
#include <limits>

namespace takedown::audio {

namespace {

bool read_u32(const std::span<const std::byte> bytes, const std::size_t offset, std::uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return false;
    }
    value = static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
            (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    return true;
}

bool read_u16(const std::span<const std::byte> bytes, const std::size_t offset, std::uint16_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 2) {
        return false;
    }
    value = static_cast<std::uint16_t>(static_cast<std::uint32_t>(bytes[offset]) |
                                       (static_cast<std::uint32_t>(bytes[offset + 1]) << 8));
    return true;
}

// RenderWare strings are NUL-terminated and padded to a 16-byte boundary
// (the terminator always counts, so a 16-character name takes 32 bytes).
bool read_padded_string(const std::span<const std::byte> bytes, std::size_t& offset, std::string& value) {
    if (offset >= bytes.size()) {
        return false;
    }
    const std::byte* const begin = bytes.data() + offset;
    const std::size_t remaining = bytes.size() - offset;
    std::size_t length = 0;
    while (length < remaining && std::to_integer<char>(begin[length]) != '\0') {
        ++length;
    }
    if (length == remaining) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(begin), length);
    const auto padded = (length + 1 + 15) & ~std::size_t{15};
    offset += padded;
    return offset <= bytes.size();
}

// Bytes of one channel's audio in a segment, or nullopt when the segment's
// usable byte count does not fit the layer geometry.
std::optional<std::size_t> usable_bytes_per_channel(const RwsLayerDescription& layer,
                                                    const RwsSegmentDescription& segment) {
    const auto channels = static_cast<std::size_t>(layer.channels);
    const auto group = static_cast<std::size_t>(layer.block_layers_size);
    const auto interleave = static_cast<std::size_t>(layer.interleave);
    if (channels == 0 || group == 0 || interleave == 0) {
        return std::nullopt;
    }
    const auto total_usable = static_cast<std::size_t>(segment.usable_bytes);
    if (segment.usable_bytes > segment.layers_size || total_usable % channels != 0) {
        return std::nullopt;
    }
    const auto per_channel = total_usable / channels;
    const auto stored_groups = static_cast<std::size_t>(segment.layers_size) / group;
    if (per_channel % PsAdpcmDecoder::kFrameBytes != 0 || per_channel > interleave * stored_groups) {
        return std::nullopt;
    }
    return std::size_t{per_channel};
}

} // namespace

std::optional<RwaStreamContainer> RwaStreamContainer::parse(std::span<const std::byte> bytes, std::string* error) {
    const auto fail = [error](const std::string_view message) -> std::optional<RwaStreamContainer> {
        if (error != nullptr) {
            *error = std::string(message);
        }
        return std::nullopt;
    };

    std::uint32_t outer_id = 0;
    std::uint32_t outer_size = 0;
    std::uint32_t version = 0;
    if (!read_u32(bytes, 0, outer_id) || !read_u32(bytes, 4, outer_size) || !read_u32(bytes, 8, version)) {
        return fail("truncated outer stream chunk envelope");
    }
    if (outer_id != kRwChunkStream) {
        return fail("not an 0x080d RenderWare Audio stream container");
    }
    // A header-only prefix is accepted: the envelope may promise more bytes
    // than were supplied, but never fewer.
    const auto declared_file_size = static_cast<std::uint64_t>(outer_size) + 12U;
    if (declared_file_size < bytes.size()) {
        return fail("outer stream chunk size does not match the file length");
    }

    std::uint32_t header_id = 0;
    std::uint32_t header_size = 0;
    if (!read_u32(bytes, 12, header_id) || !read_u32(bytes, 16, header_size)) {
        return fail("truncated stream header chunk envelope");
    }
    if (header_id != kRwChunkStreamHeader) {
        return fail("expected an 0x080e stream header chunk");
    }
    if (bytes.size() < 24) {
        return fail("truncated stream header chunk envelope");
    }
    if (header_size < 80 || header_size > bytes.size() - 24) {
        return fail("stream header chunk size is out of range");
    }
    const std::span<const std::byte> header(bytes.data() + 24, header_size);

    std::uint32_t total_segments = 0;
    std::uint32_t total_layers = 0;
    if (!read_u32(header, 32, total_segments) || !read_u32(header, 40, total_layers) || total_segments == 0 ||
        total_layers == 0 || total_segments > 65536U || total_layers > 65536U) {
        return fail("unreasonable segment/layer counts in stream header");
    }

    std::size_t cursor = 80;
    std::string file_name;
    if (!read_padded_string(header, cursor, file_name)) {
        return fail("stream header file name is truncated");
    }

    RwaStreamContainer container;
    container.version_ = version;
    container.file_name_ = std::move(file_name);

    // Segment table: 32 bytes per segment.
    for (std::uint32_t segment = 0; segment < total_segments; ++segment) {
        if (cursor > header.size() || header.size() - cursor < 32) {
            return fail("segment table is truncated");
        }
        RwsSegmentDescription description;
        if (!read_u32(header, cursor + 24, description.layers_size) ||
            !read_u32(header, cursor + 28, description.offset)) {
            return fail("segment table entry is truncated");
        }
        container.segments_.push_back(description);
        cursor += 32;
    }

    // Usable-bytes table: one u32 per (segment, layer).
    for (std::uint32_t segment = 0; segment < total_segments; ++segment) {
        auto& description = container.segments_[segment];
        description.layer_usable_bytes.reserve(total_layers);
        for (std::uint32_t layer = 0; layer < total_layers; ++layer) {
            std::uint32_t usable = 0;
            if (!read_u32(header, cursor, usable)) {
                return fail("usable-bytes table is truncated");
            }
            description.layer_usable_bytes.push_back(usable);
            if (layer == 0) {
                description.usable_bytes = usable;
            }
            cursor += 4;
        }
    }

    // 16 bytes per segment of unknown purpose precede the segment names.
    if (cursor > header.size() || header.size() - cursor < static_cast<std::size_t>(total_segments) * 16U) {
        return fail("segment name table prefix is truncated");
    }
    cursor += static_cast<std::size_t>(total_segments) * 16U;
    for (std::uint32_t segment = 0; segment < total_segments; ++segment) {
        std::string name;
        if (!read_padded_string(header, cursor, name)) {
            return fail("segment name table is truncated");
        }
        container.segments_[segment].name = std::move(name);
    }

    // Layer info table: 40 bytes per layer.
    std::uint64_t block_layers_size = 0;
    for (std::uint32_t layer = 0; layer < total_layers; ++layer) {
        if (cursor > header.size() || header.size() - cursor < 40) {
            return fail("layer info table is truncated");
        }
        RwsLayerDescription description;
        if (!read_u32(header, cursor + 16, description.padded_block_size) ||
            !read_u16(header, cursor + 24, description.interleave) ||
            !read_u16(header, cursor + 26, description.frame_size) ||
            !read_u32(header, cursor + 32, description.block_size) ||
            !read_u32(header, cursor + 36, description.layer_start)) {
            return fail("layer info entry is truncated");
        }
        block_layers_size += description.padded_block_size;
        if (block_layers_size > std::numeric_limits<std::uint32_t>::max()) {
            return fail("aggregate layer block size overflows");
        }
        container.layers_.push_back(description);
        cursor += 40;
    }
    if (block_layers_size == 0) {
        return fail("aggregate layer block size is zero");
    }
    for (auto& layer : container.layers_) {
        layer.block_layers_size = static_cast<std::uint32_t>(block_layers_size);
    }
    // Layers must tile the physical group contiguously and in order.
    std::uint64_t expected_layer_start = 0;
    for (const auto& layer : container.layers_) {
        if (layer.layer_start != expected_layer_start ||
            layer.padded_block_size > block_layers_size - expected_layer_start) {
            return fail("layer block ranges overlap or are out of order");
        }
        expected_layer_start += layer.padded_block_size;
    }

    // Layer configuration table: 48 bytes per layer.
    for (std::uint32_t layer = 0; layer < total_layers; ++layer) {
        if (cursor > header.size() || header.size() - cursor < 44) {
            return fail("layer configuration table is truncated");
        }
        auto& description = container.layers_[layer];
        if (!read_u32(header, cursor, description.sample_rate) ||
            !read_u32(header, cursor + 28, description.codec_id)) {
            return fail("layer configuration entry is truncated");
        }
        description.bits_per_sample = std::to_integer<std::uint8_t>(header[cursor + 12]);
        description.channels = std::to_integer<std::uint8_t>(header[cursor + 13]);
        cursor += 48;
    }

    if (cursor > header.size() || header.size() - cursor < static_cast<std::size_t>(total_layers) * 16U) {
        return fail("layer name table prefix is truncated");
    }
    cursor += static_cast<std::size_t>(total_layers) * 16U;
    for (std::uint32_t layer = 0; layer < total_layers; ++layer) {
        std::string layer_name;
        if (!read_padded_string(header, cursor, layer_name)) {
            return fail("layer name table is truncated");
        }
    }

    const std::size_t data_chunk = 24 + static_cast<std::size_t>(header_size);
    std::uint32_t data_id = 0;
    std::uint32_t data_size = 0;
    if (!read_u32(bytes, data_chunk, data_id) || !read_u32(bytes, data_chunk + 4, data_size)) {
        return fail("truncated stream data chunk envelope");
    }
    if (data_id != kRwChunkStreamData) {
        return fail("expected an 0x080f stream data chunk");
    }
    const auto payload_offset = data_chunk + 12;
    if (payload_offset > bytes.size()) {
        return fail("truncated stream data chunk envelope");
    }
    const auto declared_payload_end = payload_offset + static_cast<std::uint64_t>(data_size);
    if (declared_file_size != declared_payload_end) {
        return fail("stream chunk sizes are inconsistent");
    }

    std::uint64_t previous_segment_end = 0;
    for (std::size_t segment_index = 0; segment_index < container.segments_.size(); ++segment_index) {
        const auto& segment = container.segments_[segment_index];
        const auto segment_begin = static_cast<std::uint64_t>(segment.offset);
        const auto segment_size = static_cast<std::uint64_t>(segment.layers_size);
        const auto segment_end = segment_begin + segment_size;
        if (segment_begin % block_layers_size != 0) {
            return fail("segment data offset is not group aligned");
        }
        if (segment_end > data_size) {
            return fail("segment data range exceeds the declared 0x080f payload");
        }
        if (segment_index != 0 && segment_begin < previous_segment_end) {
            return fail("segment data ranges overlap or are out of order");
        }
        if (segment_size % block_layers_size != 0) {
            return fail("segment size is not an integer number of layer blocks");
        }
        const auto stored_groups = segment_size / block_layers_size;
        for (std::size_t layer_index = 0; layer_index < container.layers_.size(); ++layer_index) {
            const auto& layer = container.layers_[layer_index];
            if (layer.padded_block_size == 0 || layer.block_size > layer.padded_block_size ||
                layer.layer_start > block_layers_size ||
                layer.padded_block_size > block_layers_size - layer.layer_start) {
                return fail("layer block geometry exceeds its physical group");
            }
            const auto usable = static_cast<std::uint64_t>(segment.layer_usable_bytes[layer_index]);
            const auto layer_storage = static_cast<std::uint64_t>(layer.block_size) * stored_groups;
            if (usable > layer_storage) {
                return fail("layer usable byte count exceeds layer storage");
            }
            const auto frame_alignment =
                static_cast<std::uint64_t>(layer.channels) * static_cast<std::uint64_t>(layer.frame_size);
            if (frame_alignment == 0 || usable % frame_alignment != 0) {
                return fail("layer usable byte count is not channel/frame aligned");
            }
        }
        previous_segment_end = segment_end;
    }

    // parse() may be handed only a prefix of the file; expose what is present.
    const auto present = std::min<std::size_t>(data_size, bytes.size() - payload_offset);
    container.data_payload_file_offset_ = payload_offset;
    container.declared_data_payload_size_ = data_size;
    container.data_payload_ = bytes.subspan(payload_offset, present);
    return container;
}

std::optional<std::pair<std::size_t, std::size_t>>
RwaStreamContainer::segment_data_range(const std::size_t segment_index) const {
    if (segment_index >= segments_.size() || layers_.empty()) {
        return std::nullopt;
    }
    const auto& segment = segments_[segment_index];
    return std::pair<std::size_t, std::size_t>{
        static_cast<std::size_t>(segment.offset) + layers_.begin()->layer_start,
        static_cast<std::size_t>(segment.layers_size)};
}

std::optional<std::vector<std::int16_t>> RwaStreamContainer::decode_segment(const std::size_t segment_index) const {
    if (segment_index >= segments_.size() || layers_.size() != 1) {
        return std::nullopt;
    }
    const auto& layer = layers_.front();
    if (layer.codec_id != kCodecPlayStationAdpcm || layer.channels == 0 || layer.channels > 2 ||
        layer.frame_size != PsAdpcmDecoder::kFrameBytes || layer.interleave == 0 || layer.block_layers_size == 0 ||
        layer.interleave % PsAdpcmDecoder::kFrameBytes != 0) {
        return std::nullopt;
    }
    const auto channels = static_cast<std::size_t>(layer.channels);
    const auto block = static_cast<std::size_t>(layer.block_layers_size);
    const auto interleave = static_cast<std::size_t>(layer.interleave);
    const auto layer_start = static_cast<std::size_t>(layer.layer_start);
    if (block < layer_start || (block - layer_start) / interleave < channels) {
        return std::nullopt;
    }

    const auto& segment = segments_[segment_index];
    const auto usable = usable_bytes_per_channel(layer, segment);
    if (!usable) {
        return std::nullopt;
    }
    const auto data_begin = static_cast<std::size_t>(segment.offset);
    const std::size_t segment_bytes = segment.layers_size;
    if (data_begin >= data_payload_.size()) {
        return std::nullopt;
    }
    // The payload may be a prefix of the file; clamp to what is present.
    const std::size_t present_bytes = data_payload_.size() - data_begin;
    const auto segment_data = data_payload_.subspan(data_begin, std::min(segment_bytes, present_bytes));
    const std::size_t present_groups = segment_data.size() / block;
    const std::size_t needed_groups = (*usable + interleave - 1) / interleave;
    const auto groups = std::min(present_groups, needed_groups);

    std::vector<std::int16_t> channel_pcm[2];
    PsAdpcmDecoder decoders[2];
    for (std::size_t channel = 0; channel < channels; ++channel) {
        channel_pcm[channel].reserve(*usable / PsAdpcmDecoder::kFrameBytes * PsAdpcmDecoder::kSamplesPerFrame);
        for (std::size_t b = 0; b < groups; ++b) {
            const auto channel_begin = b * interleave;
            const auto take = std::min<std::size_t>(interleave, *usable - channel_begin);
            const auto slot_offset = b * block + channel * interleave + layer_start;
            if (slot_offset > segment_data.size() || take > segment_data.size() - slot_offset) {
                return std::nullopt;
            }
            const std::span<const std::byte> channel_slot = segment_data.subspan(slot_offset, take);
            std::vector<std::int16_t> block_samples(take / PsAdpcmDecoder::kFrameBytes *
                                                    PsAdpcmDecoder::kSamplesPerFrame);
            decoders[channel].decode(channel_slot, block_samples);
            channel_pcm[channel].insert(channel_pcm[channel].end(), block_samples.begin(), block_samples.end());
        }
    }

    std::vector<std::int16_t> interleaved;
    const std::size_t frames = channel_pcm[0].size();
    interleaved.reserve(channels * frames);
    for (std::size_t sample = 0; sample < frames; ++sample) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            interleaved.push_back(channel_pcm[channel][sample]);
        }
    }
    return interleaved;
}

} // namespace takedown::audio
