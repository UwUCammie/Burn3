#include "takedown/runtime/slus_21050_scenery_detail.hpp"

#include "takedown/guest/ee_memory.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <sstream>

namespace takedown::runtime {
namespace {

// Retail scene pointer and the layout behind it (see the header comment).
constexpr guest::GuestAddress kScenePointer = 0x0064C994U;
constexpr std::uint32_t kSceneModelCountOffset = 12U;
constexpr std::uint32_t kSceneTypeCountOffset = 52U;
constexpr std::uint32_t kSceneTypeTableOffset = 56U;
constexpr std::uint32_t kRecordBytes = 80U;
constexpr std::uint32_t kRecordModelOffset = 56U;
constexpr std::uint32_t kRecordFlagsOffset = 62U;
constexpr std::uint32_t kRecordSwitchOffset = 64U;
constexpr std::uint32_t kRecordMaximumOffset = 68U;

// Instructions of the retail renderer that this module was written against.
struct CodeWord {
    guest::GuestAddress address;
    std::uint32_t word;
};

constexpr std::array<CodeWord, 10U> kCodeWords{{
    {0x0021c158U, 0x8c71c994U},
    {0x0021c17cU, 0x86220034U},
    {0x0021c18cU, 0x8e260038U},
    {0x0021c1a4U, 0x8465003eU},
    {0x0021c1ecU, 0xc4cc0040U},
    {0x0021c1f4U, 0xc4cd0044U},
    {0x0026a74cU, 0xc4800090U},
    {0x0026a75cU, 0x46000836U},
    {0x0026a998U, 0x460c6002U},
    {0x0026a9d8U, 0xe48d0090U},
}};

} // namespace

SceneryDetailPolicy scenery_detail_policy(const config::RenderDistance distance,
                                          const config::ModelQuality quality) noexcept {
    SceneryDetailPolicy policy{};
    switch (distance) {
    case config::RenderDistance::Far:
        policy.far_scale = 2.0F;
        break;
    case config::RenderDistance::Maximum:
        policy.far_unlimited = true;
        break;
    case config::RenderDistance::Original:
        break;
    }
    switch (quality) {
    case config::ModelQuality::Low:
        policy.lod_scale = 0.5F;
        break;
    case config::ModelQuality::Medium:
        policy.lod_scale = 2.0F;
        break;
    case config::ModelQuality::High:
        policy.lod_scale = 4.0F;
        break;
    case config::ModelQuality::Ultra:
        policy.lod_highest = true;
        break;
    case config::ModelQuality::Original:
        break;
    }
    return policy;
}

float scenery_maximum_distance(const float original_maximum, const SceneryDetailPolicy& policy) noexcept {
    if (policy.far_unlimited) {
        return std::max(original_maximum, kSceneryCameraFarMetres);
    }
    if (policy.far_scale == 1.0F || original_maximum >= kSceneryCameraFarMetres) {
        return original_maximum;
    }
    // Never below the original, never beyond the camera far plane.
    return std::max(original_maximum,
                    std::min(policy.far_scale * original_maximum, kSceneryCameraFarMetres));
}

float scenery_switch_distance(const float original_switch, const float maximum,
                              const SceneryDetailPolicy& policy) noexcept {
    if (policy.lod_highest) {
        return std::max(original_switch, maximum);
    }
    if (policy.lod_scale == 1.0F) {
        return original_switch;
    }
    const float scaled = policy.lod_scale * original_switch;
    if (policy.lod_scale > 1.0F) {
        return std::min(scaled, std::max(maximum, original_switch));
    }
    return std::max(scaled, 1.0F);
}

std::optional<std::string> Slus21050SceneryDetail::signature_mismatch(const guest::EeMemory& memory) {
    for (const CodeWord& expected : kCodeWords) {
        if (!memory.contains(expected.address, 4U)) {
            std::ostringstream message;
            message << "scenery detail address 0x" << std::hex << std::setw(8) << std::setfill('0')
                    << expected.address << " is outside guest memory";
            return message.str();
        }
        const std::uint32_t actual = memory.read_u32_le(expected.address);
        if (actual != expected.word) {
            std::ostringstream message;
            message << "scenery detail code at 0x" << std::hex << std::setw(8) << std::setfill('0')
                    << expected.address << " is 0x" << std::setw(8) << actual << ", expected 0x"
                    << std::setw(8) << expected.word;
            return message.str();
        }
    }
    return std::nullopt;
}

void Slus21050SceneryDetail::restore_all(guest::EeMemory& memory) {
    for (const Record& record : records_) {
        if (record.switch_written &&
            memory.read_u32_le(record.address + kRecordSwitchOffset) == record.written_switch) {
            memory.write_u32_le(record.address + kRecordSwitchOffset, record.original_switch);
            ++statistics_.restores;
        }
        if (record.maximum_written &&
            memory.read_u32_le(record.address + kRecordMaximumOffset) == record.written_maximum) {
            memory.write_u32_le(record.address + kRecordMaximumOffset, record.original_maximum);
            ++statistics_.restores;
        }
    }
    records_.clear();
}

// RECOVERY: the GIMPLE of this function was matched in logic and in the inlining
// decisions (the write_u32_le calls stay out of line) but not exactly: the
// original lays the rejected-scene tail out before the record loop and predicts
// the scene/table validity branches as likely (66%), and it keeps the far-model
// switch distance in a named PHI (near_distance). Same behaviour.
void Slus21050SceneryDetail::apply(guest::EeMemory& memory) {
    ++statistics_.scans;
    const std::uint32_t scene = memory.read_u32_le(kScenePointer);
    std::int32_t count = 0;
    std::int32_t models = 0;
    if (scene > 0xFFFFFU && (scene & 3U) == 0U && memory.is_main_ram_range(scene, 60U)) {
        count = static_cast<std::int16_t>(memory.read_u16_le(scene + kSceneTypeCountOffset));
        models = static_cast<std::int16_t>(memory.read_u16_le(scene + kSceneModelCountOffset));
        const guest::GuestAddress table = memory.read_u32_le(scene + kSceneTypeTableOffset);
        if (count >= 1 && count <= 1024 && models > 0 && table > 0xFFFFFU && (table & 3U) == 0U &&
            memory.is_main_ram_range(table, static_cast<std::size_t>(count) * kRecordBytes)) {
            if (records_.size() != static_cast<std::size_t>(count) || records_.front().address != table) {
                restore_all(memory);
                records_.resize(static_cast<std::size_t>(count));
                for (std::int32_t i = 0; i < count; ++i) {
                    records_[static_cast<std::size_t>(i)].address =
                        table + static_cast<std::uint32_t>(i) * kRecordBytes;
                }
            }
            for (Record& record : records_) {
                const guest::GuestAddress address = record.address;
                const std::uint16_t flags = memory.read_u16_le(address + kRecordFlagsOffset);
                const std::uint32_t current_switch = memory.read_u32_le(address + kRecordSwitchOffset);
                const std::uint32_t current_maximum = memory.read_u32_le(address + kRecordMaximumOffset);
                // A value the game rewrote (a track reload) becomes the new original.
                if (!record.switch_written || record.written_switch != current_switch) {
                    record.original_switch = current_switch;
                    record.switch_written = false;
                }
                if (!record.maximum_written || record.written_maximum != current_maximum) {
                    record.original_maximum = current_maximum;
                    record.maximum_written = false;
                }
                const float original_switch = std::bit_cast<float>(record.original_switch);
                const float original_maximum = std::bit_cast<float>(record.original_maximum);
                const std::int32_t model_near =
                    static_cast<std::int16_t>(memory.read_u16_le(address + kRecordModelOffset));
                const std::int32_t model_far =
                    static_cast<std::int16_t>(memory.read_u16_le(address + kRecordModelOffset + 2U));
                const std::int32_t model_third =
                    static_cast<std::int16_t>(memory.read_u16_le(address + kRecordModelOffset + 4U));
                if (flags > 31U || !std::isfinite(original_switch) || !std::isfinite(original_maximum) ||
                    original_switch < 1.0F || original_switch > 5000.0F || original_maximum < original_switch ||
                    original_maximum > 20000.0F || model_near < 0 || model_near >= models || model_far < 0 ||
                    model_far >= models || model_third < 0 || model_third >= models) {
                    continue;
                }
                const float maximum = scenery_maximum_distance(original_maximum, policy_);
                const float near_distance =
                    (flags & 1U) != 0U ? scenery_switch_distance(original_switch, maximum, policy_)
                                       : original_switch;
                const std::uint32_t desired_maximum = std::bit_cast<std::uint32_t>(maximum);
                const std::uint32_t desired_switch = std::bit_cast<std::uint32_t>(near_distance);
                if (current_maximum != desired_maximum) {
                    memory.write_u32_le(address + kRecordMaximumOffset, desired_maximum);
                    ++statistics_.writes;
                }
                record.written_maximum = desired_maximum;
                record.maximum_written = record.original_maximum != desired_maximum;
                if (current_switch != desired_switch) {
                    memory.write_u32_le(address + kRecordSwitchOffset, desired_switch);
                    ++statistics_.writes;
                }
                record.written_switch = desired_switch;
                record.switch_written = record.original_switch != desired_switch;
            }
            return;
        }
    }
    if (!records_.empty()) {
        restore_all(memory);
    }
    if (scene != 0U) {
        ++statistics_.rejected_scenes;
    }
}

void Slus21050SceneryDetail::on_vblank(guest::EeMemory& memory) {
    if (policy_.is_retail()) {
        if (!records_.empty()) {
            ++statistics_.scans;
            restore_all(memory);
        }
        return;
    }
    apply(memory);
}

} // namespace takedown::runtime
