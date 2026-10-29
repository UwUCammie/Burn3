#include "takedown/audio/spu2_renderer.hpp"

namespace takedown::audio {

namespace {

void put_u32_le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 24));
}

void put_u16_le(std::vector<std::uint8_t>& out, const std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put_tag(std::vector<std::uint8_t>& out, const char* tag) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(tag[i]));
    }
}

} // namespace

Spu2Renderer::Spu2Renderer(Spu2System& system) : system_(system) {}

Spu2Renderer::~Spu2Renderer() { finalize_wav_dump(); }

bool Spu2Renderer::start_wav_dump(const std::filesystem::path& path) {
    wav_file_ = std::fopen(path.string().c_str(), "wb");
    if (wav_file_ == nullptr) return false;
    wav_path_ = path;
    wav_data_bytes_ = 0;
    write_wav_header();
    return true;
}

// Canonical 44-byte PCM header. The RIFF and data sizes are written as if the
// stream were empty and patched by finalize_wav_dump().
void Spu2Renderer::write_wav_header() {
    if (wav_file_ == nullptr) return;
    std::vector<std::uint8_t> header;
    header.reserve(44);
    put_tag(header, "RIFF");
    put_u32_le(header, 36);
    put_tag(header, "WAVE");
    put_tag(header, "fmt ");
    put_u32_le(header, 16);
    put_u16_le(header, 1); // PCM
    put_u16_le(header, 2); // stereo
    put_u32_le(header, kSampleRate);
    put_u32_le(header, kSampleRate * 4U); // byte rate
    put_u16_le(header, 4);                // block align
    put_u16_le(header, 16);               // bits per sample
    put_tag(header, "data");
    put_u32_le(header, 0);
    std::fwrite(header.data(), 1, header.size(), wav_file_);
}

void Spu2Renderer::finalize_wav_dump() {
    if (wav_file_ == nullptr) return;
    std::fseek(wav_file_, 4, SEEK_SET);
    const auto riff = static_cast<std::uint32_t>(wav_data_bytes_) + 36U;
    const std::uint8_t riff_bytes[4] = {
        static_cast<std::uint8_t>(riff),
        static_cast<std::uint8_t>(riff >> 8),
        static_cast<std::uint8_t>(riff >> 16),
        static_cast<std::uint8_t>(riff >> 24),
    };
    std::fwrite(riff_bytes, 1, 4, wav_file_);
    std::fseek(wav_file_, 40, SEEK_SET);
    const std::uint8_t data_bytes[4] = {
        static_cast<std::uint8_t>(wav_data_bytes_),
        static_cast<std::uint8_t>(wav_data_bytes_ >> 8),
        static_cast<std::uint8_t>(wav_data_bytes_ >> 16),
        static_cast<std::uint8_t>(wav_data_bytes_ >> 24),
    };
    std::fwrite(data_bytes, 1, 4, wav_file_);
    std::fclose(wav_file_);
    wav_file_ = nullptr;
}

void Spu2Renderer::finish_sample(const Spu2System::Stereo s) {
    // FNV-1a over the little-endian s16 bytes of the pair.
    std::uint64_t hash = fnv1a64_;
    for (const std::int16_t channel : {s.left, s.right}) {
        for (int b = 0; b < 2; ++b) {
            hash ^= static_cast<std::uint64_t>(
                (static_cast<std::uint16_t>(channel) >> (b * 8)) & 0xFF);
            hash *= 0x100000001b3ULL;
        }
    }
    fnv1a64_ = hash;
    ++samples_rendered_;
    if (s.left != 0 || s.right != 0) ++non_silent_frames_;

    if (wav_file_ != nullptr) {
        const std::uint8_t frame[4] = {
            static_cast<std::uint8_t>(s.left),
            static_cast<std::uint8_t>(static_cast<std::uint16_t>(s.left) >> 8),
            static_cast<std::uint8_t>(s.right),
            static_cast<std::uint8_t>(static_cast<std::uint16_t>(s.right) >> 8),
        };
        std::fwrite(frame, 1, 4, wav_file_);
        wav_data_bytes_ += 4;
    }

    pending_frames_.push_back(s.left);
    pending_frames_.push_back(s.right);
    if (pending_frames_.size() >= kSubmitBatchFrames * 2) submit_pending_frames();
}

void Spu2Renderer::submit_pending_frames() {
    if (pending_frames_.empty()) return;
    const bool accepted = sink_.submit(pending_frames_);
    pending_frames_.clear();
    if (accepted && submit_observer_) submit_observer_();
}

namespace {
// next_sample_cycle_ is the cycle at which the main loop must call pump_due again; it is what the scalar-burst horizon
// uses, so it decides how often the runner leaves guest code for the SPU2. It trails the render cursor (the exact start
// cycle of the next sample to render, samples_rendered_ * kEeCyclesPerSample) by kPumpBatchSlackSamples, so the runner
// pumps once per 64 samples instead of once per sample. The sample stream is unchanged: drain_through (the guest
// write/read barrier) renders through the exact cycle from the cursor, not from next_sample_cycle_.
constexpr std::uint64_t kPumpBatchSlackSamples = 63U;
} // namespace

void Spu2Renderer::render_samples(std::uint64_t count) {
    while (count != 0) {
        const std::size_t want = std::min<std::uint64_t>(count, Spu2System::kMaxBlockSamples);
        const Spu2System::BlockResult block =
            system_.step_block(std::span<Spu2System::Stereo>(block_.data(), want));
        for (std::size_t index = 0; index < block.samples; ++index) {
            // The capture IRQ observer fires between the block's last sample
            // and the next one, so its writes land exactly where step() would
            // have put them.
            if (index + 1 == block.samples && block.irq_core_mask != 0) {
                for (std::uint8_t core = 0; core < 2; ++core) {
                    if ((static_cast<std::uint32_t>(block.irq_core_mask) >> core) & 1U) {
                        system_.notify_capture_irq(core);
                    }
                }
            }
            finish_sample(block_[index]);
        }
        count -= block.samples;
    }
    next_sample_cycle_ = (samples_rendered_ + kPumpBatchSlackSamples) * kEeCyclesPerSample;
}

void Spu2Renderer::pump_due(const std::uint64_t guest_cycle) {
    if (next_sample_cycle_ <= guest_cycle) {
        const std::uint64_t cursor = samples_rendered_ * kEeCyclesPerSample;
        const std::uint64_t due = (guest_cycle - cursor) / kEeCyclesPerSample + 1U;
        render_samples(std::min<std::uint64_t>(due, kMaxSamplesPerPump));
    }
    submit_pending_frames();
}

void Spu2Renderer::drain_through(const std::uint64_t guest_cycle) {
    const std::uint64_t cursor = samples_rendered_ * kEeCyclesPerSample;
    if (cursor <= guest_cycle) {
        render_samples((guest_cycle - cursor) / kEeCyclesPerSample + 1U);
    }
    submit_pending_frames();
}

} // namespace takedown::audio
