#include "takedown/runtime/slus_21050_car_select.hpp"

#include "takedown/content/car_paint_count.hpp"
#include "takedown/content/mod_manager.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/presentation/bitmap_font.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"
#include "takedown/runtime/slus_21050_added_cars.hpp"
#include "takedown/ui/car_select.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace takedown::runtime {

namespace {

using Address = guest::GuestAddress;
using Cars = Slus21050AddedCars;

constexpr std::size_t kA0 = 4U;
// The car-select screen's object (a0 of its handlers): the palette byte of the
// current selection is at +0x14 of the 12-byte record selected by +0x244.
constexpr Address kCarSelectScreen = 32042032U;
Address palette_selection(const guest::EeMemory& memory) {
    return kCarSelectScreen + 0x14U + memory.read_u32_le(kCarSelectScreen + 0x244U) * 12U;
}
constexpr std::size_t kA1 = 5U;
constexpr std::size_t kA2 = 6U;
constexpr std::size_t kA3 = 7U;
constexpr std::size_t kT0 = 8U;
constexpr std::size_t kT2 = 10U;
constexpr std::size_t kT3 = 11U;
constexpr std::size_t kS1 = 17U;
constexpr std::size_t kS2 = 18U;
constexpr std::size_t kS3 = 19U;
constexpr std::size_t kSp = 29U;
constexpr std::size_t kV1 = 3U;
constexpr std::size_t kV0 = 2U;
constexpr std::size_t kRa = 31U;
// The prompt bar draws GlobalFont at 22.27 picture pixels per font unit with its capitals' tops at y 411.5.
constexpr float kPromptTextUnit = 22.27F, kPromptCapTop = 411.5F;
constexpr float kColorTextLeft = 200.0F;
// The game's own prompt texts either side of the skin prompt: Change ends and
// Select starts here; the R1 button picture is this wide, after this gap.
constexpr float kChangeTextRight = 395.6F, kSelectTextLeft = 531.9F;
constexpr float kSkinIconGap = 4.0F, kSkinIconWidth = 40.0F;
constexpr std::uint32_t kPromptTextColour = 0xFFBE9557U;

// The address of a visible row of the original list (0x01E8A63C and on, twelve bytes each).
Address row_address(std::size_t slot) {
    return static_cast<std::uint32_t>(slot) * 12U + 32042044U;
}

// The catalogue car with this index; null when the game asks for one that does not exist.
const ui::SelectableCar* car_at(const std::vector<ui::SelectableCar>& cars, std::uint64_t index) {
    return index < cars.size() ? &cars[index] : nullptr;
}

// UTF-16 string table of the game (id -> text), read as UTF-8.
std::string string_text(const guest::EeMemory& memory, std::uint32_t id) {
    const std::uint32_t table = memory.read_u32_le(0x0051BA44U);
    if (table == 0U || !memory.contains(table, 16U)) {
        return {};
    }
    const std::uint32_t strings = memory.read_u32_le(table + 12U);
    if (id > 65535U || strings == 0U || !memory.contains(id * 4U + strings, 4U)) {
        return {};
    }
    const std::uint32_t at = memory.read_u32_le(id * 4U + strings);
    std::string text;
    for (Address i = 0U; i != 512U && memory.contains(i * 2U + at, 2U); ++i) {
        const auto code = memory.read_u16_le(i * 2U + at);
        if (code == 0U) {
            break;
        }
        if (code <= 127U) {
            text.push_back(static_cast<char>(code));
        } else if (code <= 2047U) {
            text.push_back(static_cast<char>(static_cast<unsigned char>(code >> 6U) | 192U));
            text.push_back(static_cast<char>((static_cast<unsigned char>(code) & 63U) | 128U));
        } else {
            text.push_back(static_cast<char>(static_cast<unsigned char>(code >> 12U) | 224U));
            text.push_back(static_cast<char>((static_cast<unsigned char>(code >> 6U) & 63U) | 128U));
            text.push_back(static_cast<char>((static_cast<unsigned char>(code) & 63U) | 128U));
        }
    }
    return text;
}

// The vehicle registry entry (the game's loaded car record) whose name is `name`; 0 when none.
inline Address registry_entry(const guest::EeMemory& memory, std::string name) {
    const Address table = memory.read_u32_le(5351528U);
    if (table == 0U || !memory.contains(table, 16U)) {
        return 0U;
    }
    const std::uint32_t count = std::min<std::uint32_t>(memory.read_u32_le(table + 8U), 4096U);
    const Address base = memory.read_u32_le(table + 12U);
    if (!memory.contains(base, static_cast<std::size_t>(count * 16U))) {
        return 0U;
    }
    for (std::uint32_t i = 0U; i < count; ++i) {
        const Address entry = memory.read_u32_le(base + i * 16U + 8U);
        if (entry == 0U || !memory.contains(entry + 168U, name.size() + 1U)) {
            continue;
        }
        bool same = memory.read_u8(entry + static_cast<std::uint32_t>(name.size()) + 168U) == 0U;
        for (std::size_t c = 0U; same && c < name.size(); ++c) {
            same = memory.read_u8(entry + static_cast<std::uint32_t>(c) + 168U) == static_cast<std::uint8_t>(name[c]);
        }
        if (same) {
            return entry;
        }
    }
    return 0U;
}

// Retail code of the routines the native catalogue replaces: begin, end and the FNV-1a hash of the words.
struct Signature {
    Address begin;
    Address end;
    std::uint64_t hash;
};

constexpr std::array<Signature, 28> signatures{{
    {0x00313580U, 0x00313650U, 0x339B91D148010AC8ULL},
    {0x0033F620U, 0x0033F8B0U, 0xBA70454405DFE6B9ULL},
    {0x001D5FB0U, 0x001D6064U, 0x970EF9ED3ED4F046ULL},
    {0x0042B7C0U, 0x0042BC70U, 0xFB19C63EDB6DEC1AULL},
    {0x0042D2B0U, 0x0042DA70U, 0x8514392DE6B18D4BULL},
    {0x0042DA70U, 0x0042DAE0U, 0x5DB4B4271F504557ULL},
    {0x00427080U, 0x004272A0U, 0x97CE7D8327C57B34ULL},
    {0x004272A0U, 0x00427560U, 0xB0449DE9BD5E2906ULL},
    {0x0042CFA0U, 0x0042D2B0U, 0x9A134C58C92D3056ULL},
    {0x0042C5C0U, 0x0042C600U, 0xC3C65C8AB8010E03ULL},
    {0x0042C600U, 0x0042CD60U, 0x04D9E99D1272C1A2ULL},
    {0x0042B700U, 0x0042B7C0U, 0x7BFEDA2BA893B2DAULL},
    {0x0042B660U, 0x0042B6A0U, 0x9B1962452DD17F6DULL},
    {0x0042B6A0U, 0x0042B700U, 0x313AC337B841FFEEULL},
    {0x0042B5F0U, 0x0042B660U, 0x81A4E215DBF14DADULL},
    {0x0042BC70U, 0x0042BC80U, 0x5CC6C896B35DA4E0ULL},
    {0x0042BC80U, 0x0042BC90U, 0x3CECD4D5459F2004ULL},
    {0x0042BC90U, 0x0042BD70U, 0x14EC0595365DB0C3ULL},
    {0x0042BD70U, 0x0042BEA0U, 0x614B1C7DAB193F17ULL},
    {0x0042BEA0U, 0x0042C020U, 0xF123492793913E39ULL},
    {0x0042C020U, 0x0042C090U, 0x42C864AF8E1BA563ULL},
    {0x0042C090U, 0x0042C100U, 0x2E91653207A05392ULL},
    {0x0042C160U, 0x0042C1A0U, 0x5920E86C3AB41633ULL},
    {0x0042C1A0U, 0x0042C240U, 0x422E4C7D80365BDFULL},
    {0x0042C240U, 0x0042C270U, 0x75513D877E93384BULL},
    {0x0042C270U, 0x0042C2A0U, 0x56D2592B899AF1B8ULL},
    {0x0042C440U, 0x0042C5C0U, 0xCC90B27A4E0795E8ULL},
    {0x00251CB0U, 0x00251E10U, 0x8FDCF10185BC128CULL}}};

// Whether the game's code at the signature is the retail code (the hook points are only valid then).
bool matches(const guest::EeMemory& memory, const Signature& signature) {
    if (!memory.contains(signature.begin, signature.end - signature.begin)) {
        return false;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (Address at = signature.begin; at < signature.end; ++at) {
        hash = (hash ^ memory.read_u8(at)) * 1099511628211ULL;
    }
    return hash == signature.hash;
}

} // namespace

struct Slus21050CarSelect::State {
    explicit State(Cars& cars) : added(cars) {}

    Cars& added;
    ui::CarSelect selector;
    std::vector<ui::CarClass> classes;
    std::vector<ui::SelectableCar> cars;
    std::array<std::string, 9> class_codes;
    std::vector<std::string> native_labels;
    std::size_t visible_page{};
    std::uint32_t thumbnail_page{};
    bool initialized{};
    std::uint32_t refresh_ticks{};
    std::string forced_vehicle;
    std::string ui_assets;
    std::vector<ui::SelectableCar> previous;
    std::size_t window{};
    std::size_t previous_window{};
    bool class_move{};
    bool catalog_dirty{};
    std::string colour_word;
    std::string selected_before_skin;
    presentation::BitmapFont prompt_font;
    std::unordered_map<std::uint32_t, std::pair<std::string, std::uint32_t>> hash_a;
    platform::ParallelGsHostUi frame_ui;
    platform::ParallelGsHostUi shown_ui;
    bool thumbnail_frame_submitted{};
    std::unordered_map<std::uint32_t, std::pair<std::string, std::uint32_t>> hash_b;
    std::unordered_map<std::string, std::array<std::string, 8>> thumbnails;
    std::unordered_map<std::string, content::CarPaints> paint_counts;
    // The colour button's handler runs more than once per press, before the
    // game's own step; a call that finds the byte still holding the value the
    // previous call wrote (for the same car) is that same press.
    std::string steered_car;
    int steered_value{-1};
    bool previews_loaded{};

    // The game's own car record of a catalogue car: the retail car's, or the template of an added one.
    Address record(const guest::EeMemory& memory, const ui::SelectableCar& car) const {
        const Address direct = registry_entry(memory, car.id);
        if (direct != 0U) {
            return direct;
        }
        for (const auto& extra : added.cars()) {
            if (extra.vehicle == car.id) {
                const Address inherited = registry_entry(memory, extra.template_vehicle);
                if (inherited != 0U) {
                    return inherited;
                }
            }
        }
        return memory.read_u32_le(32130812U);
    }
    std::ptrdiff_t position(const ui::SelectableCar& car) const {
        return std::find_if(cars.begin(), cars.end(), [&](const ui::SelectableCar& other) { return other.id == car.id; }) -
               cars.begin();
    }
    std::uint32_t class_index(const ui::SelectableCar* car) const {
        if (car != nullptr) {
            for (std::size_t i = 0U; i != 9U; ++i) {
                if (class_codes[i] == car->handling_class) {
                    return static_cast<std::uint32_t>(i);
                }
            }
        }
        return 0U;
    }

    void rows(guest::EeMemory& memory, const std::vector<ui::SelectableCar>& list, std::size_t begin, bool old);
    void build(guest::EeMemory& memory);
    const std::string& thumbnail(const ui::SelectableCar& car, std::uint32_t paint) const;
    void publish();
};

void Slus21050CarSelect::State::rows(guest::EeMemory& memory, const std::vector<ui::SelectableCar>& list,
                                     std::size_t begin, bool old) {
    const Address base = old ? 32042284U : 32042044U;
    const unsigned long long count = begin < list.size() ? std::min<std::size_t>(list.size() - begin, 20U) : 0U;
    for (std::size_t i = 0U; i < 20U; ++i) {
        const unsigned row = static_cast<std::uint32_t>(i) * 12U + base;
        memory.write_u8(row + 6U, 0U);
        memory.write_u8(row + 7U, 0U);
        if (count <= i) {
            continue;
        }
        const ui::SelectableCar& car = list[begin + i];
        memory.write_u32_le(row, record(memory, car));
        memory.write_u8(row + 4U, static_cast<std::uint8_t>(position(car)));
        memory.write_u8(row + 5U, static_cast<std::uint8_t>(class_index(&car)));
        memory.write_u8(row + 6U, static_cast<std::uint8_t>(car.unlocked));
        memory.write_u8(row + 7U, static_cast<std::uint8_t>(car.unlocked));
        memory.write_u8(row + 8U, 0U);
    }
    memory.write_u32_le(old ? 32042620U : 32042616U, static_cast<std::uint32_t>(count));
}

void Slus21050CarSelect::State::build(guest::EeMemory& memory) {
    catalog_dirty = false;
    const std::size_t selected = selector.car_index();
    window = std::max<std::size_t>(selected, 2U) - 2U;
    rows(memory, selector.cars(), window, false);
    rows(memory, previous, previous_window, true);
    memory.write_u32_le(32042612U, static_cast<std::uint32_t>(selected) - static_cast<std::uint32_t>(window));
    if (class_move) {
        memory.write_u32_le(32042608U, static_cast<std::uint32_t>(selected) - static_cast<std::uint32_t>(window));
    }
    memory.write_u32_le(32042632U, static_cast<std::uint32_t>(selector.class_index()));
}

const std::string& Slus21050CarSelect::State::thumbnail(const ui::SelectableCar& car, std::uint32_t paint) const {
    // The previews are numbered by palette block (tools/carmod), the game's
    // paint by the palette's tile in the stored image (content/car_paint_count).
    static constexpr std::array<std::uint32_t, 8> kPreviewOfPaint{0U, 1U, 4U, 5U, 2U, 3U, 6U, 7U};
    const auto found = thumbnails.find(car.id);
    if (found != thumbnails.end() && !found->second[kPreviewOfPaint[paint & 7U]].empty()) {
        return found->second[kPreviewOfPaint[paint & 7U]];
    }
    return car.thumbnail;
}

void Slus21050CarSelect::State::publish() {
    auto eligible = cars;
    if (!forced_vehicle.empty()) {
        std::erase_if(eligible, [this](const ui::SelectableCar& car) { return car.id != forced_vehicle; });
    }
    selector.set_catalog(classes, std::move(eligible));
    catalog_dirty = true;
}

Slus21050CarSelect::Slus21050CarSelect(Cars& cars) : state_(std::make_shared<State>(cars)) {}

void Slus21050CarSelect::refresh(const guest::EeMemory& memory, const content::ModManager& mods, const bool content_changed) {
    static constexpr std::array<const char*, 9> names{"COMPACT SERIES", "MUSCLE SERIES", "COUPE SERIES",
                                                      "SPORTS SERIES",  "SUPER SERIES",  "SPECIAL",
                                                      "HEAVY",          "TRAFFIC",       "TRAFFIC SPECIAL"};
    State& state = *state_;
    if (content_changed || !state.previews_loaded) {
        state.thumbnails.clear();
        state.paint_counts.clear();
        for (const auto& added : mods.added_cars()) {
            if (added.problem.empty()) {
                auto& previews = state.thumbnails[added.vehicle];
                for (std::size_t paint = 0U; paint < 8U; ++paint) {
                    previews[paint] = added.thumbnail_variants[paint].string();
                }
            }
        }
        state.previews_loaded = true;
    }
    state.ui_assets = std::filesystem::absolute(mods.directory().parent_path() / "cache" / "ui").string();
    if ((state.refresh_ticks++ & 31U) != 0U && !state.cars.empty() && !content_changed) {
        return;
    }
    if (memory.read_u32_le(Cars::kRegistry) != Cars::kRegistryVersion) {
        return;
    }
    std::vector<ui::CarClass> classes;
    for (std::size_t i = 0U; i != 9U; ++i) {
        std::string prefix;
        for (Address c = 0U; c != 4U; ++c) {
            prefix.push_back(static_cast<char>(memory.read_u8(4962184U + static_cast<std::uint32_t>(i) * 8U + c)));
        }
        state.class_codes[i] = prefix;
        classes.push_back({prefix, names[i], static_cast<int>(i),
                           static_cast<std::uint8_t>(i > 5U ? (i == 6U ? 2U : 0U) : 1U)});
    }
    state.native_labels.clear();
    for (const auto& group : mods.car_classes()) {
        if (group.problem.empty()) {
            state.native_labels.push_back(group.id);
            std::erase_if(classes, [&](const ui::CarClass& other) { return other.id == group.id; });
            classes.push_back({group.id, group.name, group.order, group.modes});
        }
    }
    std::vector<ui::SelectableCar> cars;
    const std::uint32_t count = std::min<std::uint32_t>(memory.read_u32_le(Cars::kRegistry + 4U), Cars::kRegistryCapacity);
    for (std::uint32_t i = 0U; count > i; ++i) {
        if (memory.read_u32_le(Cars::kRegistryFlags + i * 4U) == 0U) {
            continue;
        }
        const std::uint64_t key = memory.read_u64_le(Cars::kRegistryKeys + i * 8U);
        ui::SelectableCar car;
        car.vehicle_key = key;
        car.id = decode_vehicle_name(key);
        car.class_id = car.handling_class = car.id.substr(0U, 4U);
        car.name = car.id;
        car.unlocked = state.added.unlocked(key, memory);
        car.paints = 8U;
        for (std::uint32_t id = 0U; id != Cars::kCarCount; ++id) {
            if (key == memory.read_u64_le(Cars::kCarKeys + id * 8U)) {
                const auto localized = string_text(memory, memory.read_u32_le(Cars::kCarNamesUpper + id * 4U));
                if (!localized.empty()) {
                    car.name = localized;
                }
                car.requirement = string_text(memory, memory.read_u32_le(Cars::kCarUnlockTexts + id * 4U));
            }
        }
        const std::filesystem::path picture = mods.directory().parent_path() / "cache" / "ui" / (car.id + ".png");
        std::error_code error;
        if (std::filesystem::is_regular_file(picture, error)) {
            car.thumbnail = std::filesystem::absolute(picture).string();
        }
        for (const auto& extra : state.added.cars()) {
            if (encode_vehicle_name(extra.vehicle) == key) {
                car.name = extra.name;
                if (!extra.class_id.empty()) {
                    car.class_id = extra.class_id;
                }
                car.handling_class = extra.template_vehicle.substr(0U, 4U);
                car.thumbnail = extra.thumbnail;
                car.requirement = state.added.requirement(key);
                if (extra.skin) {
                    car.skin_of = extra.template_vehicle;
                }
                const auto original = extra.skin ? encode_vehicle_name(extra.template_vehicle) : std::optional<std::uint64_t>{};
                for (std::uint32_t id = 0U; original && id != Cars::kCarCount; ++id) {
                    if (memory.read_u64_le(Cars::kCarKeys + id * 8U) == *original) {
                        car.requirement = string_text(memory, memory.read_u32_le(Cars::kCarUnlockTexts + id * 4U));
                    }
                }
            }
        }
        cars.push_back(std::move(car));
    }
    {
        // A car's colour choices are the distinct palettes in its car file, so
        // retail cars and mod cars/skins alike show their real count.
        const bool recount = content_changed || state.paint_counts.empty();
        if (recount) {
            const content::ModOverlay overlay = mods.build_overlay();
            for (auto& car : cars) {
                auto paints = content::car_paints(overlay, mods.game_root(), car.id);
                car.paints = static_cast<std::uint32_t>(paints.choices.size());
                state.paint_counts[car.id] = std::move(paints);
            }
        } else {
            for (auto& car : cars) {
                if (const auto found = state.paint_counts.find(car.id);
                    found != state.paint_counts.end()) {
                    car.paints = static_cast<std::uint32_t>(found->second.choices.size());
                }
            }
        }
    }
    if (state.classes == classes && state.cars == cars) {
        return;
    }
    state.classes = std::move(classes);
    state.cars = std::move(cars);
    state.publish();
}

std::span<const std::uint32_t> Slus21050CarSelect::hook_addresses() {
    static constexpr std::array<std::uint32_t, 19> hooks{
        0x0042CFA0U, 0x0042D2B0U, 0x0042C440U, 0x0042C5C0U, 0x0042BD70U, 0x0042BDB8U, 0x0042BEA0U,
        0x0042BF3CU, 0x0042C020U, 0x0042C090U, 0x0042BC90U, 0x0042DA70U, 0x00271A00U, 0x00251C10U,
        0x00427080U, 0x004274DCU, 0x0033F620U, 0x0033F818U, 0x00313580U};
    return hooks;
}

bool Slus21050CarSelect::on_dispatch(guest::R5900State& cpu, guest::EeMemory& memory) {
    State& s = *state_;
    auto& select = s.selector;
    const std::uint32_t pc = cpu.pc();
    // Ends a replaced routine: v0 is the result and the caller continues.
    const auto finish = [&cpu](std::uint64_t result) {
        cpu.write_gpr_u64(kV0, result);
        cpu.set_pc(cpu.read_gpr_u32(kRa));
        return true;
    };
    // A new frame of the thumbnail page starts: forget what the previous page drew.
    if ((pc == 0x0033F620U || pc == 0x00313580U) && memory.read_u32_le(32048164U) == 5U) {
        const std::uint32_t page = memory.read_u32_le(32048176U);
        if (s.thumbnail_page != page) {
            s.frame_ui = {};
            s.shown_ui = {};
        }
        s.thumbnail_page = page;
        s.thumbnail_frame_submitted = true;
    }
    // The original draws a car picture with a record of its own: draw the catalogue picture instead.
    if (pc == 0x0033F818U && memory.read_u32_le(32048164U) == 5U) {
        const unsigned record = cpu.read_gpr_u32(kS2);
        const std::uint32_t box_address = cpu.read_gpr_u32(kA0);
        if (cpu.read_gpr_u32(kSp) + 176U != box_address) {
            return false;
        }
        if (!memory.contains(record, 12U)) {
            return false;
        }
        if (!memory.contains(box_address, 32U)) {
            return false;
        }
        const unsigned long long key = memory.read_u64_le(record);
        const auto car = std::find_if(s.cars.begin(), s.cars.end(), [&](const ui::SelectableCar& candidate) {
            return candidate.vehicle_key == key && !s.thumbnail(candidate, memory.read_u32_le(record + 8U)).empty() &&
                   std::any_of(s.added.cars().begin(), s.added.cars().end(),
                               [&](const AddedCar& added) { return added.vehicle == candidate.id; });
        });
        if (car == s.cars.end()) {
            return false;
        }
        std::array<float, 4> box{};
        const std::uint64_t packed = cpu.read_gpr_u64(kV1);
        const std::array<std::uint32_t, 4> bits{static_cast<std::uint32_t>(packed), static_cast<std::uint32_t>(packed >> 32U),
                                                memory.read_u32_le(box_address + 8U),
                                                memory.read_u32_le(box_address + 12U)};
        for (std::size_t i = 0U; i != 4U; ++i) {
            std::memcpy(&box[i], &bits[i], sizeof(float));
            if (!std::isfinite(box[i])) {
                return false;
            }
        }
        platform::ParallelGsUiItem item;
        item.kind = platform::ParallelGsUiKind::ImageQuad;
        item.text = s.thumbnail(*car, memory.read_u32_le(record + 8U));
        item.corners = {box[0], box[1] + box[3], box[0], box[1], box[0] + box[2], box[1] + box[3], box[0] + box[2], box[1]};
        for (const float point : item.corners) {
            if (!std::isfinite(point)) {
                return false;
            }
        }
        item.colour = 0U;
        for (unsigned i = 0U; i != 4U; ++i) {
            float value;
            const std::uint32_t raw = memory.read_u32_le(box_address + i * 4U + 16U);
            std::memcpy(&value, &raw, sizeof(float));
            if (!std::isfinite(value)) {
                return false;
            }
            if (std::fabs(value) > 64.0F) {
                return false;
            }
            item.tint[i] = value;
            item.colour |= static_cast<std::uint32_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F) << (i * 8U);
        }
        if (s.frame_ui.items.size() >= platform::kParallelGsHostUiMaximumItems) {
            return false;
        }
        s.frame_ui.items.push_back(std::move(item));
        s.thumbnail_page = memory.read_u32_le(32048176U);
        s.thumbnail_frame_submitted = true;
        memory.write_u64_le(box_address, packed);
        cpu.set_pc(0x0033F820U);
        return true;
    }
    // The car select screen opens: start from the requested class and car.
    if (pc == 0x0042CFA0U && cpu.read_gpr_u32(kA0) == 32042032U) {
        s.initialized = true;
        s.visible_page = 0U;
        s.previous.clear();
        s.hash_b.clear();
        s.hash_a.clear();
        s.frame_ui.items.clear();
        s.shown_ui.items.clear();
        s.thumbnail_frame_submitted = false;
        s.forced_vehicle.clear();
        const std::uint32_t a1 = cpu.read_gpr_u32(kA1);
        const std::uint32_t a2 = cpu.read_gpr_u32(kA2);
        const unsigned initial_group = a1 == 9U ? cpu.read_gpr_u32(kA3) : a1;
        const unsigned initial_car = a2 == 0xFFFFFFFFU ? cpu.read_gpr_u32(kT0) : a2;
        std::string initial;
        if (initial_group <= 8U) {
            std::uint32_t position = 0U;
            for (const auto& car : s.cars) {
                if (car.id.substr(0U, 4U) == s.class_codes[initial_group] && initial_car == position++) {
                    initial = car.id;
                }
            }
        }
        if (a2 != 0xFFFFFFFFU) {
            s.forced_vehicle = initial.empty() ? std::string("<unavailable>") : initial;
        }
        s.publish();
        select.set_context(a1 == 6U ? ui::CarSelectMode::Crash : ui::CarSelectMode::Race,
                           a1 <= 8U ? s.class_codes[a1] : std::string{});
        if (!initial.empty()) {
            select.select(initial);
        }
        if (a1 != 9U && a2 == 0xFFFFFFFFU && select.classes().size() > 1U) {
            cpu.write_gpr_u64(kA1, 9U);
            cpu.write_gpr_u64(kA3, initial_group);
            cpu.write_gpr_u64(kT0, initial_car);
        }
    }
    if (pc == 0x0042D2B0U && cpu.read_gpr_u32(kA0) == 32042032U && cpu.read_gpr_u32(kA2) != 4U) {
        s.initialized = false;
    }
    if (!s.initialized) {
        return false;
    }
    if (pc == 0x0042C440U && cpu.read_gpr_u32(kA0) == 32042032U) {
        s.initialized = false;
        s.visible_page = 0U;
        s.frame_ui.items.clear();
        s.shown_ui.items.clear();
        return false;
    }
    if (pc == 0x0042C5C0U && cpu.read_gpr_u32(kA0) == 32042032U) {
        s.visible_page = memory.read_u32_le(32048176U);
        if (memory.read_u32_le(32042600U) == 0U) {
            const unsigned long long wanted = select.car_index() > 2U ? select.car_index() - 2U : 0U;
            if (s.catalog_dirty || s.window != wanted) {
                s.class_move = false;
                s.build(memory);
            }
        }
    }
    const std::uint32_t moving = memory.read_u32_le(32042600U);
    if ((pc == 0x0042BD70U || pc == 0x0042BEA0U) && cpu.read_gpr_u32(kA0) == 32042032U) {
        if (moving != 0U) {
            return finish(0U);
        }
        std::vector<ui::SelectableCar> previous = select.cars();
        const unsigned long long previous_window = s.window;
        if (!select.move_class(pc == 0x0042BD70U ? -1 : 1)) {
            return finish(0U);
        }
        s.previous = std::move(previous);
        s.previous_window = previous_window;
        s.class_move = true;
    }
    if ((pc == 0x0042BF3CU || pc == 0x0042BDB8U) && cpu.read_gpr_u32(kS2) == 32042032U && s.class_move) {
        cpu.write_gpr_u64(kV1, 1U);
    }
    if ((pc == 0x0042C020U || pc == 0x0042C090U) && cpu.read_gpr_u32(kA0) == 32042032U) {
        if (moving != 0U || !select.move_car(pc == 0x0042C020U ? -1 : 1)) {
            return finish(0U);
        }
        s.class_move = false;
    }
    if (pc == 0x0042BC90U && cpu.read_gpr_u32(kA0) == 32042032U && moving == 0U) {
        // The game's colour button (0x0042BC90) steps the selection's palette
        // byte to (palette + 1) % count. Set the byte one below the first
        // palette of the next distinct colour, so the game's own step lands
        // on it and skips palettes that look the same.
        if (const ui::SelectableCar* car = select.selected(); car != nullptr) {
            if (const auto found = s.paint_counts.find(car->id);
                found != s.paint_counts.end() && found->second.game_count != 0U && !found->second.choices.empty()) {
                const content::CarPaints& paints = found->second;
                const Address byte = palette_selection(memory);
                const std::uint8_t raw = memory.read_u8(byte);
                // TAKEDOWN_DEV_PAINT_TRACE logs every call of the colour button.
                static const bool trace = std::getenv("TAKEDOWN_DEV_PAINT_TRACE") != nullptr;
                if (trace) {
                    std::fprintf(stderr, "Paint trace: %s byte %u at %08x, ra %08x, choices %zu\n", car->id.c_str(),
                                 static_cast<unsigned>(raw), static_cast<unsigned>(byte),
                                 static_cast<unsigned>(cpu.read_gpr_u32(kRa)), paints.choices.size());
                }
                if (s.steered_car == car->id && s.steered_value == raw) {
                    return false;
                }
                const std::uint32_t current = raw % paints.game_count;
                const std::size_t next = (paints.group[current] + 1U) % paints.choices.size();
                const auto value = static_cast<std::uint8_t>((paints.choices[next] + paints.game_count - 1U) %
                                                             paints.game_count);
                memory.write_u8(byte, value);
                s.steered_car = car->id;
                s.steered_value = value;
            }
        }
        select.next_paint();
    }
    if (pc == 0x0042DA70U && (cpu.read_gpr_u32(kRa) == 4377412U || cpu.read_gpr_u32(kRa) == 4380732U)) {
        if (!select.classes().empty()) {
            for (std::size_t i = 0U; i != 9U; ++i) {
                if (i <= 5U && select.classes()[select.class_index()].id == s.class_codes[i] &&
                    std::find(s.native_labels.begin(), s.native_labels.end(), select.classes()[select.class_index()].id) ==
                        s.native_labels.end()) {
                    cpu.write_gpr_u64(kA0, static_cast<std::uint32_t>(i));
                    return false;
                }
            }
        }
        return finish(61439U);
    }
    // Writes a text into the game's label buffer (UTF-16) and returns from the text routine.
    const auto label = [&](Address at, const std::string& text) {
        std::size_t out = 0U;
        std::size_t i = 0U;
        while (i < text.size() && out != 510U) {
            unsigned char c = static_cast<unsigned char>(text[i++]);
            std::uint32_t code = c;
            if ((c & 0xE0U) == 0xC0U && i < text.size()) {
                code = c & 0x1FU;
                code = (code << 6U) | (static_cast<unsigned>(text[i++]) & 0x3FU);
            } else if ((c & 0xF0U) == 0xE0U && i + 1U < text.size()) {
                code = c & 0x0FU;
                code = (code << 6U) | (static_cast<unsigned>(text[i++]) & 0x3FU);
                code = (code << 6U) | (static_cast<unsigned>(text[i++]) & 0x3FU);
            }
            memory.write_u16_le(at + static_cast<std::uint32_t>(out++) * 2U, static_cast<std::uint16_t>(code));
        }
        memory.write_u16_le(at + static_cast<std::uint32_t>(out) * 2U, 0U);
        return finish(at);
    };
    if (pc == 0x00271A00U && cpu.read_gpr_u32(kA1) == 61439U &&
        (cpu.read_gpr_u32(kRa) == 4377428U || cpu.read_gpr_u32(kRa) == 4380748U)) {
        return label(720896U, select.classes().empty() ? std::string{} : select.classes()[select.class_index()].name);
    }
    if (pc == 0x00271A00U && cpu.read_gpr_u32(kRa) == 3275844U && cpu.read_gpr_u32(kA1) == 2459U) {
        s.colour_word = string_text(memory, 2459U);
        return label(721920U, " ");
    }
    if (pc == 0x00251C10U) {
        const std::uint32_t ra = cpu.read_gpr_u32(kRa);
        if (ra == 4354512U || ra == 4376916U || ra == 4376960U || ra == 4377852U || ra == 4377896U) {
            if (const ui::SelectableCar* car = car_at(s.cars, cpu.read_gpr_u32(kA2))) {
                // The thumbnail set-up (0x00427080) takes the retail picture of a
                // skin's original: the skin's own picture is drawn over it while
                // the skin is chosen, and switching back to the original (R1)
                // does not set the thumbnail up again.
                if (ra == 4354512U && !car->skin_of.empty()) {
                    for (const auto& original : s.cars) {
                        if (original.id == car->skin_of) {
                            return finish(original.vehicle_key);
                        }
                    }
                    for (const auto& original : select.cars()) {
                        if (original.id == car->skin_of) {
                            return finish(original.vehicle_key);
                        }
                    }
                    if (const auto key = encode_vehicle_name(car->skin_of)) {
                        return finish(*key);
                    }
                }
                return finish(car->vehicle_key);
            }
        }
    }
    if (pc == 0x00427080U) {
        const unsigned descriptor = cpu.read_gpr_u32(kA0) + 32U;
        s.hash_b.erase(descriptor);
        s.hash_a.erase(descriptor);
        if (const ui::SelectableCar* car = car_at(s.cars, cpu.read_gpr_u32(kT2))) {
            s.hash_a[descriptor] = {car->id, cpu.read_gpr_u32(kT3)};
        }
        if (const ui::SelectableCar* car = car_at(s.cars, cpu.read_gpr_u32(kT2));
            car != nullptr && !s.thumbnail(*car, cpu.read_gpr_u32(kT3)).empty()) {
            for (const auto& extra : s.added.cars()) {
                if (extra.vehicle == car->id) {
                    s.hash_b[descriptor] = {car->id, cpu.read_gpr_u32(kT3)};
                }
            }
        }
    }
    if (pc == 0x004274DCU) {
        s.thumbnail_frame_submitted = true;
        s.thumbnail_page = memory.read_u32_le(32048176U);
        const auto found = s.hash_b.find(cpu.read_gpr_u32(kS2));
        if (found == s.hash_b.end()) {
            return false;
        }
        const auto car = std::find_if(s.cars.begin(), s.cars.end(),
                                      [found](const ui::SelectableCar& candidate) { return candidate.id == found->second.first; });
        if (car == s.cars.end()) {
            return false;
        }
        // The paint as the descriptor holds it now: the colour button changes it
        // (0x00427070 stores it at +0x11) without setting the thumbnail up again.
        const std::uint32_t descriptor = cpu.read_gpr_u32(kS2);
        const std::uint32_t paint =
            memory.contains(descriptor + 0x11U, 1U) ? memory.read_u8(descriptor + 0x11U) : found->second.second;
        if (s.thumbnail(*car, paint).empty()) {
            return false;
        }
        const std::uint32_t quad = cpu.read_gpr_u32(kS1);
        const std::uint32_t tint = cpu.read_gpr_u32(kS3);
        if (!memory.contains(quad, 32U)) {
            return false;
        }
        if (!memory.contains(tint, 32U)) {
            return false;
        }
        platform::ParallelGsUiItem item;
        item.kind = platform::ParallelGsUiKind::ImageQuad;
        item.text = s.thumbnail(*car, paint);
        for (std::size_t i = 0U; i != 8U; ++i) {
            const std::uint32_t raw = memory.read_u32_le(static_cast<std::uint32_t>(i) * 4U + quad);
            std::memcpy(&item.corners[i], &raw, sizeof(float));
            if (!std::isfinite(item.corners[i])) {
                return false;
            }
        }
        item.colour = 0U;
        for (unsigned i = 0U; i != 4U; ++i) {
            float value;
            const std::uint32_t raw = memory.read_u32_le(i * 4U + tint + 16U);
            std::memcpy(&value, &raw, sizeof(float));
            if (!std::isfinite(value)) {
                return false;
            }
            if (std::fabs(value) > 64.0F) {
                return false;
            }
            item.tint[i] = value;
            item.colour |= static_cast<std::uint32_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F) << (i * 8U);
        }
        if (s.frame_ui.items.size() < platform::kParallelGsHostUiMaximumItems) {
            s.frame_ui.items.push_back(std::move(item));
        }
        cpu.set_pc(0x00427534U);
        return true;
    }
    return false;
}

bool Slus21050CarSelect::register_functions(NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory) {
    for (const Signature& signature : signatures) {
        if (!matches(memory, signature)) {
            return false;
        }
    }
    const std::array<std::uint32_t, 12> addresses{0x0042B7C0U, 0x0042B700U, 0x0042B660U, 0x0042B6A0U, 0x0042B5F0U,
                                                  0x0042BC70U, 0x0042BC80U, 0x0042C160U, 0x0042C1A0U, 0x0042C240U,
                                                  0x0042C270U, 0x00251CB0U};
    for (const auto address : addresses) {
        if (dispatcher.lookup(address) != nullptr) {
            return false;
        }
    }
    auto state = state_;
    for (const auto address : addresses) {
        const NativeFunctionLimits cost{1U, 1U};
        auto resolver = [state, address, cost](
                                  const NativeFunctionPrepareContext& c) -> std::optional<NativeFunctionLimits> {
            if (address == 0x00251CB0U) {
                for (const auto& car : state->added.cars()) {
                    if (encode_vehicle_name(car.vehicle) == c.cpu.read_gpr_u64(kA0) && !car.class_id.empty()) {
                        return cost;
                    }
                }
                return std::nullopt;
            }
            if (c.cpu.read_gpr_u32(kA0) != 0x01E8EC30U || !state->initialized) {
                return std::nullopt;
            }
            return cost;
        };
        auto run = [state, address](NativeFunctionContext& c) -> NativeFunctionExecution {
            auto& select = state->selector;
            const unsigned long long index = c.cpu.read_gpr_u32(kA1);
            std::uint64_t result = 0U;
            if (address == 0x0042B7C0U) {
                state->build(c.memory);
            } else if (address == 0x0042B700U) {
                if (index < select.cars().size()) {
                    result = select.cars()[index].vehicle_key;
                }
            } else if (address == 0x0042B660U) {
                result = select.cars().size();
            } else if (address == 0x0042B6A0U) {
                result = static_cast<std::uint64_t>(index < select.cars().size() && select.cars()[index].unlocked);
            } else if (address == 0x0042B5F0U) {
                if (index >= state->window && index - state->window < 20U) {
                    result = c.memory.read_u8(row_address(index - state->window) + 8U);
                }
            } else if (address == 0x0042BC70U) {
                result = select.car_index();
            } else if (address == 0x0042BC80U) {
                result = state->class_index(select.selected());
            } else if (address == 0x0042C160U) {
                result = static_cast<std::uint64_t>(select.class_index() != 0U);
            } else if (address == 0x0042C1A0U) {
                result = static_cast<std::uint64_t>(select.class_index() + 1U < select.classes().size());
            } else if (address == 0x0042C240U) {
                result = static_cast<std::uint64_t>(select.car_index() != 0U);
            } else if (address == 0x0042C270U) {
                result = static_cast<std::uint64_t>(select.car_index() + 1U < select.cars().size());
            } else if (address == 0x00251CB0U) {
                for (const auto& car : state->added.cars()) {
                    if (encode_vehicle_name(car.vehicle) == c.cpu.read_gpr_u64(kA0)) {
                        ui::SelectableCar physical;
                        physical.handling_class = car.template_vehicle.substr(0U, 4U);
                        result = state->class_index(&physical);
                    }
                }
            }
            c.cpu.write_gpr_u64(kV0, result);
            c.cpu.set_pc(c.cpu.read_gpr_u32(kRa));
            return {.retired_instructions = 1U, .ee_cycles = 1U};
        };
        if (!dispatcher
                 .register_function(address, "NativeCarCatalogue", cost,
                                    {.scheduler_state_transparent = true, .single_step_host_call = true}, std::move(resolver), std::move(run))
                 .registered()) {
            return false;
        }
    }
    return true;
}

bool Slus21050CarSelect::next_skin(guest::EeMemory& memory) {
    State& state = *state_;
    if (!state.initialized || memory.read_u32_le(32042600U) != 0U || state.selector.selected() == nullptr) {
        return false;
    }
    state.selected_before_skin = state.selector.selected()->id;
    if (!state.selector.next_skin()) {
        return false;
    }
    const std::string previous = state.selected_before_skin;
    state.build(memory);
    const auto& now = state.selector.selected()->id;
    const auto end = state.added.cars().end();
    const auto car = std::find_if(state.added.cars().begin(), end, [&](const AddedCar& added) { return added.vehicle == now; });
    for (auto& [key, entry] : state.hash_a) {
        if (entry.first != previous) {
            continue;
        }
        entry.first = now;
        if (car != end) {
            state.hash_b[key] = entry;
        } else {
            state.hash_b.erase(key);
        }
    }
    if (memory.contains(32041792U, 224U)) {
        memory.write_u64_le(32041928U, state.selector.selected()->vehicle_key);
        memory.write_u8(32041936U, 0U);
        memory.write_u32_le(32042012U, 0U);
        memory.write_u32_le(32042008U, 0U);
    }
    return true;
}

platform::ParallelGsHostUi Slus21050CarSelect::host_ui(const guest::EeMemory& memory) const {
    if (state_->thumbnail_page == 0U || memory.read_u32_le(32048176U) != state_->thumbnail_page ||
        memory.read_u32_le(32048164U) != 5U) {
        state_->frame_ui.items.clear();
        state_->shown_ui.items.clear();
        return {};
    }
    if (state_->thumbnail_frame_submitted) {
        state_->shown_ui = std::move(state_->frame_ui);
        state_->frame_ui = {};
        state_->thumbnail_frame_submitted = false;
    }
    platform::ParallelGsHostUi ui = state_->shown_ui;
    const ui::SelectableCar* car = state_->selector.selected();
    if (!state_->initialized || car == nullptr || ui.items.size() + 3U > platform::kParallelGsHostUiMaximumItems) {
        return ui;
    }
    auto& font = state_->prompt_font;
    if (font.glyphs.empty() && !state_->ui_assets.empty()) {
        std::ifstream file{std::filesystem::path{state_->ui_assets} / "GlobalFont.tdfont", std::ios::binary};
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        if (!presentation::decode_bitmap_font(bytes.data(), bytes.size(), font)) {
            font = {};
        }
    }
    struct Extent {
        float left{}, top{}, right{}, bottom{};
        bool valid{};
    };
    const auto extent = [&](std::string_view value) {
        Extent e;
        float pen = 0;
        for (const char c : value) {
            const unsigned char code = static_cast<unsigned char>(c);
            auto glyph = font.find(code);
            if (!glyph && code >= 'a' && code <= 'z') {
                glyph = font.find(c - 'a' + 'A');
            }
            if (!glyph) {
                glyph = font.find('?');
            }
            if (!glyph) {
                continue;
            }
            const float x = pen + glyph->bearing_x * font.scale_x, y = glyph->bearing_y * font.scale_y;
            const float w = font.scale_x * glyph->width, h = font.scale_y * glyph->height;
            if (glyph->u >= 0 && w > 0 && h > 0) {
                e.left = e.valid ? std::min(e.left, x) : x;
                e.top = e.valid ? std::min(e.top, y) : y;
                e.right = e.valid ? std::max(e.right, x + w) : x + w;
                e.bottom = e.valid ? std::max(e.bottom, y + h) : y + h;
                e.valid = true;
            }
            pen += glyph->advance * font.scale_x;
        }
        return e;
    };
    const auto capitals = extent("H");
    const auto prompt = [&](std::string value, float anchor, bool right) {
        const auto e = extent(value);
        if (!e.valid || !capitals.valid) {
            return;
        }
        platform::ParallelGsUiItem text;
        text.kind = platform::ParallelGsUiKind::Text;
        text.flags = platform::kParallelGsUiBurnoutFont | platform::kParallelGsUiNoOutline |
                     platform::kParallelGsUiExactOrigin | (right ? platform::kParallelGsUiAlignRight : 0U);
        const float width = (e.right - e.left) * kPromptTextUnit, height = (e.bottom - e.top) * kPromptTextUnit;
        const float top = kPromptCapTop - (capitals.top - e.top) * kPromptTextUnit;
        text.corners[0] = right ? anchor - width : anchor;
        text.corners[1] = top;
        text.corners[2] = width;
        text.corners[3] = height;
        text.width = static_cast<std::uint16_t>(std::lround(width));
        text.height = static_cast<std::uint16_t>(std::lround(height));
        text.x = static_cast<std::int16_t>(std::lround(text.corners[0]));
        text.y = static_cast<std::int16_t>(std::lround(top));
        text.colour = kPromptTextColour;
        text.text = std::move(value);
        ui.items.push_back(std::move(text));
    };
    // The game hides its own prompts (Select, the colour button) on a car
    // that is not owned; the colour count goes with them.
    if (!state_->colour_word.empty() && car->unlocked) {
        // The colour the game shows: its palette's group, not a press count.
        std::uint32_t shown = state_->selector.paint();
        if (const auto found = state_->paint_counts.find(car->id);
            found != state_->paint_counts.end() && found->second.game_count != 0U) {
            const Address byte = palette_selection(memory);
            shown = found->second.group[memory.read_u8(byte) % found->second.game_count];
        }
        prompt(state_->colour_word + " " + std::to_string(shown + 1U) + "/" +
                   std::to_string(std::max(car->paints, 1U)),
               kColorTextLeft, false);
    }
    const auto skins = state_->selector.skins();
    if (skins.size() > 1U) {
        std::size_t at = 0U;
        for (std::size_t i = 0U; i < skins.size(); ++i) {
            if (skins[i]->id == car->id) {
                at = i;
            }
        }
        // "Skin a/b" and the R1 button sit centred between the game's Change
        // and Select prompts, so the gaps either side match.
        const std::string label = "Skin " + std::to_string(at + 1U) + "/" + std::to_string(skins.size());
        const auto e = extent(label);
        const float text_width = e.valid ? (e.right - e.left) * kPromptTextUnit : 0.0F;
        const float group = text_width + kSkinIconGap + kSkinIconWidth;
        const float left = (kChangeTextRight + kSelectTextLeft - group) * 0.5F;
        prompt(label, left, false);
        platform::ParallelGsUiItem icon;
        icon.kind = platform::ParallelGsUiKind::Image;
        icon.x = static_cast<std::int16_t>(std::lround(left + text_width + kSkinIconGap));
        icon.y = 409;
        icon.width = static_cast<std::uint16_t>(kSkinIconWidth);
        icon.height = 19;
        icon.text = (std::filesystem::path{state_->ui_assets} / "R1Button.png").string();
        ui.items.push_back(std::move(icon));
    }
    return ui;
}

} // namespace takedown::runtime
