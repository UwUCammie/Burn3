#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace takedown::runtime {

// Honest, host-side acceptance gates for the media paths that must survive the
// interpreter-to-native transition.  These checks deliberately distinguish
// "bytes decoded" from "the user saw/heard the retail result"; the latter is
// the contract the finished recompilation has to satisfy.
enum class MediaParityState {
    NotObserved,
    Blocked,
    Partial,
    Verified,
};

[[nodiscard]] std::string_view to_string(MediaParityState state) noexcept;

struct MediaParityCheck {
    MediaParityState state{MediaParityState::NotObserved};
    std::string detail{};

    [[nodiscard]] bool verified() const noexcept { return state == MediaParityState::Verified; }
};

struct MediaParityEvidence {
    // FMV: decoder/IPU progress is not presentation.  A future movie renderer
    // must increment visibly_presented_movie_frames only after a decoded movie
    // frame actually reaches the displayed scanout.
    bool movie_path_observed{false};
    bool movie_lifecycle_bypass_active{false};
    std::uint64_t movie_decoder_frames{};
    std::uint64_t movie_macroblock_rows_decoded{};
    std::uint64_t movie_macroblock_rows_placed{};
    // Native-transition playback may decode a complete picture and stage it
    // directly in the validated retail CMV double buffer, bypassing only the
    // low-level IPU BDEC/CSC implementation. This is still not proof that the
    // displayed scanout contained the frame.
    std::uint64_t movie_guest_frames_staged{};
    std::uint64_t visibly_presented_movie_frames{};

    // Audio evidence may come only from executed retail transfer/libsd state
    // and samples emitted by the modelled SPU2. Asset parsing, host decoding,
    // guessed triggers, and PCM submitted around the machine are deliberately
    // not representable here.
    std::uint64_t spu_transfer_commands{};
    std::uint64_t spu_transfers_materialized{};
    std::uint64_t spu_transfer_bytes{};
    std::uint64_t retail_spu2_key_ons{};
    std::uint64_t retail_spu2_key_offs{};
    std::uint64_t spu2_rendered_frames{};
    std::uint64_t spu2_non_silent_frames{};

    // The model output is a mixed bus, so counters alone cannot prove whether
    // a particular non-silent frame was SFX or music. These proof bits must be
    // set only by a category-specific audible/differential validation.
    bool retail_sfx_output_verified{false};
    std::uint64_t retail_music_stream_reads{};
    bool retail_music_output_verified{false};
    bool long_run_music_continuity_verified{false};
};

struct MediaParityReport {
    MediaParityCheck fmv{};
    MediaParityCheck sfx{};
    MediaParityCheck music{};

    [[nodiscard]] bool verified() const noexcept {
        return fmv.verified() && sfx.verified() && music.verified();
    }
};

[[nodiscard]] MediaParityReport audit_media_parity(const MediaParityEvidence& evidence);

} // namespace takedown::runtime
