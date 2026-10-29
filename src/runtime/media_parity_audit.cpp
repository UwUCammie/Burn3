#include "takedown/runtime/media_parity_audit.hpp"

#include <sstream>

namespace takedown::runtime {

std::string_view to_string(const MediaParityState state) noexcept {
    switch (state) {
    case MediaParityState::NotObserved:
        return "not-observed";
    case MediaParityState::Blocked:
        return "blocked";
    case MediaParityState::Partial:
        return "partial";
    case MediaParityState::Verified:
        return "verified";
    }
    return "unknown";
}

MediaParityReport audit_media_parity(const MediaParityEvidence& evidence) {
    MediaParityReport report;

    // FMV: only a decoded frame that reached the displayed scanout counts.
    if (evidence.visibly_presented_movie_frames != 0U &&
        (evidence.movie_macroblock_rows_placed != 0U || evidence.movie_guest_frames_staged != 0U) &&
        !evidence.movie_lifecycle_bypass_active) {
        report.fmv = {MediaParityState::Verified,
                      "decoded movie frames reached the retail guest buffers and displayed "
                      "scanout"};
    } else if (!evidence.movie_path_observed && evidence.movie_decoder_frames == 0U &&
               evidence.movie_macroblock_rows_decoded == 0U) {
        report.fmv = {MediaParityState::NotObserved,
                      "no movie lifecycle, decoder frame, or IPU macroblock output was "
                      "observed"};
    } else if (evidence.movie_lifecycle_bypass_active) {
        report.fmv = {MediaParityState::Blocked,
                      "startup movie lifecycle used --headless-movie-eos; this reaches the "
                      "frontend but does not visibly play the EA/title/intro FMVs"};
    } else if (evidence.movie_guest_frames_staged != 0U) {
        std::ostringstream detail;
        detail << evidence.movie_guest_frames_staged
               << " decoded frame(s) reached validated retail CMV guest buffers, but zero "
                  "displayed scanout frames were verified";
        report.fmv = {MediaParityState::Partial, detail.str()};
    } else if (evidence.movie_macroblock_rows_placed == 0U) {
        std::ostringstream detail;
        detail << "movie path ran (decoder-frames=" << evidence.movie_decoder_frames
               << ", rows-decoded=" << evidence.movie_macroblock_rows_decoded
               << ") but no decoded macroblock rows reached guest presentation buffers";
        report.fmv = {MediaParityState::Blocked, detail.str()};
    } else {
        std::ostringstream detail;
        detail << evidence.movie_macroblock_rows_placed
               << " decoded macroblock row(s) reached guest RAM, but zero visibly displayed "
                  "movie frames were verified";
        report.fmv = {MediaParityState::Partial, detail.str()};
    }

    // Audio: executed retail DMA/KON traffic that the modelled SPU2 turned into sound.
    const bool executed_audio_traffic = evidence.spu_transfers_materialized != 0U &&
                                        evidence.retail_spu2_key_ons != 0U &&
                                        evidence.spu2_non_silent_frames != 0U;
    if (evidence.retail_sfx_output_verified && executed_audio_traffic) {
        report.sfx = {MediaParityState::Verified,
                      "executed retail DMA/KON traffic produced category-verified SFX through "
                      "the modelled SPU2"};
    } else if (evidence.spu_transfer_commands == 0U && evidence.spu_transfers_materialized == 0U &&
               evidence.retail_spu2_key_ons == 0U && evidence.spu2_non_silent_frames == 0U) {
        report.sfx = {MediaParityState::NotObserved,
                      "no retail SPU2 transfer, KON, or non-silent model output was observed"};
    } else if (evidence.spu_transfers_materialized == 0U) {
        std::ostringstream detail;
        detail << evidence.spu_transfer_commands
               << " retail SPU2 transfer command(s) were observed, but none materialized in "
                  "modelled SPU2 RAM";
        report.sfx = {MediaParityState::Blocked, detail.str()};
    } else if (evidence.retail_spu2_key_ons == 0U) {
        std::ostringstream detail;
        detail << evidence.spu_transfers_materialized << " retail transfer(s) copied "
               << evidence.spu_transfer_bytes
               << " byte(s), but no executed retail KON reached the model";
        report.sfx = {MediaParityState::Blocked, detail.str()};
    } else if (evidence.spu2_rendered_frames == 0U || evidence.spu2_non_silent_frames == 0U) {
        std::ostringstream detail;
        detail << evidence.retail_spu2_key_ons
               << " executed retail KON(s) reached the model, but its rendered bus remained "
                  "silent";
        report.sfx = {MediaParityState::Blocked, detail.str()};
    } else {
        std::ostringstream detail;
        detail << evidence.spu2_non_silent_frames << " non-silent modelled-SPU2 frame(s) followed "
               << evidence.retail_spu2_key_ons
               << " executed retail KON(s), but SFX-specific audible output is not verified";
        report.sfx = {MediaParityState::Partial, detail.str()};
    }

    if (evidence.retail_music_stream_reads == 0U) {
        report.music = {MediaParityState::NotObserved,
                        "no completed retail RWA stream read was observed"};
    } else if (!executed_audio_traffic) {
        std::ostringstream detail;
        detail << evidence.retail_music_stream_reads
               << " retail music-stream read(s) completed, but executed DMA/KON traffic did not "
                  "produce non-silent modelled-SPU2 output";
        report.music = {MediaParityState::Blocked, detail.str()};
    } else if (evidence.retail_music_output_verified &&
               evidence.long_run_music_continuity_verified) {
        report.music = {MediaParityState::Verified,
                        "retail stream/DMA/KON traffic produced category-verified music through "
                        "the modelled SPU2 and passed the long-run continuity gate"};
    } else {
        std::ostringstream detail;
        detail << evidence.retail_music_stream_reads << " retail music-stream read(s), "
               << evidence.retail_spu2_key_ons << " executed KON(s), and "
               << evidence.spu2_non_silent_frames
               << " non-silent modelled-SPU2 frame(s) were observed; music attribution and "
                  "long-run no-cutoff behavior are not both verified";
        report.music = {MediaParityState::Partial, detail.str()};
    }
    return report;
}

} // namespace takedown::runtime
