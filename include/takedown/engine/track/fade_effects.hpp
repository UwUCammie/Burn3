#pragma once

// The per-frame update of the scene's fading effects: a pool of 16 records
// that ramp one effect object's drive values (scale and level) up and down,
// and a pool of 128 records that fade an effect object's alpha in and out
// in five steps, each handing finished records to a list the owner clears.
// A third table of 16 slots eases blend values towards a target.
//
// The original calls into the effect objects' own control code (a handle
// the object may carry, a removal of a finished record) on its less common
// paths. This code does not cover those: it reports them and the adapter
// leaves such a call to the original. All arithmetic is the game's (rounds
// toward zero, see engine/math/chopped.hpp).

#include <cstdint>

namespace takedown::engine {

// Values the whole pass shares.
struct FadeContext {
    float dt{};
    float scale_a{};  // multiplies a record's scale when its flag 0x10 is clear
    float scale_b{};  // multiplies a record's level when its mode byte is clear
    std::uint8_t level_floor{};  // alpha steps stop fading in below this threshold
};

// The effect object a record drives.
struct FadeTarget {
    float scale{};            // written by the update
    std::uint8_t alpha{};     // written by the update
    std::uint8_t options{};   // the byte holding the "fading allowed" bit (2)
    bool has_handle{};        // the object carries a control handle: its own code must run
    bool has_removal{};       // the object has a removal request pending
};

// A 52-byte record of the large pool (fields at their byte offsets from the
// record's second word).
struct FadeRecord {
    std::uint8_t latch{};   // +0
    std::uint8_t state{};   // +44: 0 idle, 1 fading, 2 held, 3 fading out
    std::uint8_t step{};    // +45
    std::uint8_t kind{};    // +46: selects the table entry
    std::uint8_t flags{};   // +47
    std::uint8_t cutoff{};  // +48
    std::uint8_t mode{};    // +49
    float level{};          // +20
    float strength{};       // +24
    float remaining{};      // +28
    float span{};           // +32
    float countdown{};      // +40
    float kind_scale{};     // table entry of `kind`
    float kind_level{};
};

enum class FadeStatus : std::uint8_t {
    Done,
    NeedsOriginal,  // a path that calls the effect object's control code
};

// Original 0x00388080: updates one record of the large pool, including the
// state advance below.
[[nodiscard]] FadeStatus update_fade_record(FadeRecord& record, FadeTarget& target, const FadeContext& context) noexcept;

// Original 0x003873c0: advances the fade steps of a record.
[[nodiscard]] FadeStatus advance_fade_record(FadeRecord& record, FadeTarget& target, const FadeContext& context) noexcept;

// Original 0x00387980: whether the record is still running. Sets the latch
// of an idle record that asks for it.
[[nodiscard]] bool fade_record_running(FadeRecord& record, const FadeTarget& target) noexcept;

// A 32-byte record of the small pool.
struct FadeDrive {
    std::uint8_t state{};        // +24
    std::uint8_t step{};         // +25
    std::uint8_t kind{};         // +26
    std::uint8_t flags{};        // +27
    std::uint8_t mode{};         // +29
    float level{};               // +12
    float strength{};            // +16
    float kind_scale{};          // table entry of `kind`
    float kind_level{};
    std::uint32_t chain_value{}; // the word the drive's chain of pointers ends at
};

// What the update of a drive sets on its object.
struct FadeDriveOutput {
    float scale{};   // +152
    float blend{};   // +160
};

// Original 0x00385a00: the values a drive gives its object (a drive in
// state 2 gives it none; the original skips to the advance).
[[nodiscard]] FadeStatus update_fade_drive(const FadeDrive& drive, const FadeContext& context,
                                           FadeDriveOutput& output) noexcept;

// What a drive's advance does to its object and the word its object points
// at ("chain"), besides changing the drive itself.
enum FadeDriveEvent : std::uint32_t {
    kFadeDriveChainOne = 1U,     // stores 1 in the chain word
    kFadeDriveChainTwo = 2U,     // stores 2 in the chain word
    kFadeDriveBits80 = 4U,       // sets bit 0x80 of the object's bits
    kFadeDriveBlend = 8U,        // sets bit 2 and stores `blend`
    kFadeDriveTakeHolder = 16U,  // the drive's word at +8 takes the chain record's word at +20
    kFadeDriveGiveHolder = 32U,  // the chain record's word at +24 takes the drive's word at +8
};

struct FadeDriveAdvance {
    std::uint32_t events{};
    float blend{};
};

// Original 0x00385260: advances the drive's fade steps. `blend` is the
// object's current blend value.
[[nodiscard]] FadeDriveAdvance advance_fade_drive(FadeDrive& drive, float blend) noexcept;

// Original 0x003855f0: whether the drive is still running.
[[nodiscard]] bool fade_drive_running(const FadeDrive& drive) noexcept;

// A 20-byte slot of the easing table: eases `value` (+16, a countdown of
// `duration`, +12) between the slot's own start (+0, +4) and a target.
struct FadeEase {
    float duration{};   // +12
    float countdown{};  // +16
    float from_a{};     // +0
    float from_b{};     // +4
    float to_a{};       // the target's +0
    float to_b{};       // the target's +4
};

struct FadeEaseOutput {
    bool eased{};         // false: the slot just copies its target's values, if it has one
    float a{};
    float b{};
    float countdown{};    // written back when eased
};

[[nodiscard]] FadeEaseOutput ease_fade_slot(const FadeEase& slot, float dt) noexcept;

} // namespace takedown::engine
