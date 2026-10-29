#include "takedown/runtime/slus_21050_added_cars.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace takedown::runtime {

namespace {

constexpr std::size_t kA0 = 4U;
constexpr std::size_t kA1 = 5U;
constexpr std::size_t kV0 = 2U;
constexpr std::size_t kRa = 31U;
// The classes the game knows (0x00251CB0), as the first four characters.
constexpr std::array<std::string_view, 9> kClasses{"COMP", "MSCL", "CUPE", "SPRT", "SUPR",
                                                   "HSPC", "HEVY", "TRAF", "TSPC"};
// Longest string the native hash reads (the game's longest key path is
// about 120 characters).
constexpr std::uint32_t kMaximumHashedLength = 4096U;

// Retail code of the replaced routines; registration needs every word.
constexpr std::array<std::uint32_t, 16> kKeyToCarIdWords{
    0x3c05004dU, 0x0000102dU, 0x10000007U, 0x24a5cf80U, 0x000218c0U, 0x00a31821U, 0xdc630000U, 0x10830006U,
    0x00000000U, 0x24420001U, 0x28430043U, 0x1460fff8U, 0x00000000U, 0x2402ffffU, 0x03e00008U, 0x00000000U};
constexpr std::array<std::uint32_t, 23> kCarNameStringWords{
    0x28810000U, 0x14200010U, 0x00000000U, 0x10a00007U, 0x00000000U, 0x3c02004dU, 0x00041880U, 0x2442d2b0U,
    0x00431021U, 0x10000006U, 0x8c420000U, 0x3c02004dU, 0x00041880U, 0x2442d3c0U, 0x00431021U, 0x8c420000U,
    0x10000004U, 0x00000000U, 0x240204ffU, 0x240304feU, 0x0065100aU, 0x03e00008U, 0x00000000U};
constexpr std::array<std::uint32_t, 2> kStringLookupWords{0x0809c698U, 0x8c840000U};
constexpr std::array<std::uint32_t, 5> kStringLookupBodyWords{0x8c82000cU, 0x00051880U, 0x00431021U,
                                                              0x03e00008U, 0x8c420000U};
constexpr std::array<std::uint32_t, 26> kCarUnlockedWords{
    0x27bdffe0U, 0x3c040052U, 0xffbf0010U, 0x248486a0U, 0x7fb00000U, 0x0c094800U, 0x00a0802dU,
    0x14400003U, 0x00000000U, 0x1000000cU, 0x0000102dU, 0x0c0fac18U, 0x0200202dU, 0x2403ffffU,
    0x14430003U, 0x00000000U, 0x10000005U, 0x24020001U, 0x3c030052U, 0x2463bfa0U, 0x00621021U,
    0x90420000U, 0xdfbf0010U, 0x7bb00000U, 0x03e00008U, 0x27bd0020U};
constexpr std::array<std::uint32_t, 34> kClassUnlockedWords{
    0x27bdffd0U, 0x24020007U, 0xffbf0020U, 0x7fb10010U, 0x7fb00000U, 0x00a0802dU, 0x12020015U, 0x00000000U, 0x1000000eU,
    0x0000882dU, 0x0c0fac00U, 0x0220202dU, 0x16020009U, 0x00000000U, 0x3c020052U, 0x2442bfa0U, 0x00511021U, 0x90420000U,
    0x10400003U, 0x00000000U, 0x10000008U, 0x24020001U, 0x26310001U, 0x2a220043U, 0x1440fff1U, 0x00000000U, 0x10000002U,
    0x0000102dU, 0x24020001U, 0xdfbf0020U, 0x7bb10010U, 0x7bb00000U, 0x03e00008U, 0x27bd0030U};
constexpr std::array<std::uint32_t, 12> kUnlockTextWords{
    0x28810000U, 0x14200007U, 0x00000000U, 0x3c02004dU, 0x00041880U, 0x2442d6f0U,
    0x00431021U, 0x10000002U, 0x8c420000U, 0x240204ffU, 0x03e00008U, 0x00000000U};
constexpr std::array<std::uint32_t, 12> kUnlockTextShortWords{
    0x28810000U, 0x14200007U, 0x00000000U, 0x3c02004dU, 0x00041880U, 0x2442d800U,
    0x00431021U, 0x10000002U, 0x8c420000U, 0x240204ffU, 0x03e00008U, 0x00000000U};
constexpr std::array<std::uint32_t, 28> kHashStringWords{
    0x27bdffe0U, 0xffbf0010U, 0x7fb00000U, 0x0c049cfaU, 0x0080802dU, 0x3c05004aU, 0x2404ffffU,
    0x0000382dU, 0x1000000bU, 0x24a5b110U, 0x82060000U, 0x308300ffU, 0x00042203U, 0x24e70001U,
    0x00c31826U, 0x26100001U, 0x00031880U, 0x00a31821U, 0x8c630000U, 0x00832026U, 0x00e2182aU,
    0x1460fff4U, 0x00000000U, 0xdfbf0010U, 0x0080102dU, 0x7bb00000U, 0x03e00008U, 0x27bd0020U};
// The value database's read of one car physics value (a0 the database, a1
// the binding record), recognized by an FNV-1a hash of its code.
constexpr guest::GuestAddress kVehicleTuning = 0x0024FFC0U;
constexpr std::uint32_t kVehicleTuningSize = 0x140U;
constexpr std::uint64_t kVehicleTuningSignature = 0x859455f25bdd76ebULL;
constexpr std::uint32_t kDatabaseSize = 0xE528U;
constexpr std::uint32_t kDatabaseRowCount = 0xE520U;
constexpr std::uint32_t kDatabaseRows = 0xE524U;
constexpr std::uint32_t kMaximumDatabaseRows = 0x10000U;
constexpr std::uint32_t kMaximumText = 1024U;

// A physics value an added car's multiplier changes: where it goes and what.
struct Resolved {
    guest::GuestAddress destination{0U};
    float value{0.0F};
    bool retail{false}; // the base value came from the retail database
};

template <std::size_t N>
bool matches(const guest::EeMemory& memory, const guest::GuestAddress address,
             const std::array<std::uint32_t, N>& words) {
    if (!memory.contains(address, 4U * N)) {
        return false;
    }
    for (std::size_t index = 0U; index < N; ++index) {
        if (memory.read_u32_le(address + 4U * static_cast<guest::GuestAddress>(index)) != words[index]) {
            return false;
        }
    }
    return true;
}

std::string upper(std::string_view text) {
    std::string out{text};
    for (char& character : out) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return out;
}

} // namespace

std::optional<std::uint64_t> encode_vehicle_name(const std::string_view name) {
    if (name.empty() || name.size() > kVehicleNameLength) {
        return std::nullopt;
    }
    std::uint64_t key = 0U;
    for (std::size_t index = 0U; index < kVehicleNameLength; ++index) {
        const char character = index < name.size() ? name[index] : ' ';
        std::uint64_t code = 0U;
        if (character == ' ') {
            code = 0U;
        } else if (character >= '0' && character <= '9') {
            code = static_cast<std::uint64_t>(character - '0') + 3U;
        } else if (character >= 'A' && character <= 'Z') {
            code = static_cast<std::uint64_t>(character - 'A') + 13U;
        } else {
            return std::nullopt;
        }
        key = key * 40U + code;
    }
    return key;
}

std::string decode_vehicle_name(std::uint64_t key) {
    std::string out(kVehicleNameLength, ' ');
    for (std::size_t index = kVehicleNameLength; index-- > 0U;) {
        const auto code = static_cast<unsigned>(key % 40U);
        key /= 40U;
        char character = '?';
        if (code == 0U) {
            character = ' ';
        } else if (code >= 3U && code <= 12U) {
            character = static_cast<char>('0' + code - 3U);
        } else if (code >= 13U && code <= 38U) {
            character = static_cast<char>('A' + code - 13U);
        }
        out[index] = character;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

struct Slus21050AddedCars::State {
    struct Car {
        AddedCar car;
        std::uint64_t key{0U};
        std::uint64_t template_key{0U};
        std::string hash_from; // "/<vehicle>.cfg"
        std::string hash_to;   // "/<template>.cfg"
        // The World Tour event of an event rule, once resolved.
        std::optional<std::uint32_t> event;
        // The retail car id of a Template rule's template, once resolved.
        std::optional<std::uint32_t> template_id;
        // The requirement texts: the game's own (a retail car with the same
        // rule, as string ids) or the car's.
        std::array<std::uint32_t, 2> requirement_ids{};
        std::array<std::string, 2> requirement{};
        // The four strings in the region: name in capitals, name, requirement,
        // short requirement.
        std::array<guest::GuestAddress, kStringsPerCar> strings{};
    };
    // The game's unlock rows of its 67 cars and its World Tour event keys.
    struct RetailUnlock {
        std::uint32_t kind{0U};
        std::uint32_t value{0U};
        std::uint32_t text{0U};
        std::uint32_t text_short{0U};
    };
    std::vector<RetailUnlock> retail_unlocks;
    std::vector<std::uint64_t> events;

    std::vector<AddedCar> configured;
    std::vector<Car> cars;
    std::vector<std::string> problems;
    // Keys this module put in the registry (removed again when their car goes).
    std::vector<std::uint64_t> added_keys;
    std::unordered_map<std::uint64_t, std::uint32_t> retail_ids;
    std::optional<RetailValueDatabase> retail_values;
    bool registered{false};
    bool developer_unlock{false};
    bool strings_dirty{true};
    bool strings_published{false};
    // The key of the last key-to-id lookup, when it named an added car.
    std::optional<std::size_t> last_added;
    Statistics statistics;

    [[nodiscard]] const Car* find(const std::uint64_t key) const {
        for (const Car& car : cars) {
            if (car.key == key) {
                return &car;
            }
        }
        return nullptr;
    }

    // Resolves the event rules and the requirement texts (the game's tables
    // are imported at registration).
    void resolve() {
        for (Car& entry : cars) {
            const AddedCarUnlock& unlock = entry.car.unlock;
            entry.event.reset();
            entry.template_id.reset();
            entry.requirement_ids = {0U, 0U};
            entry.requirement = {};
            if (unlock.kind == AddedCarUnlock::Kind::Template) {
                // The template's own rule and texts.
                const auto found = retail_ids.find(entry.template_key);
                if (found != retail_ids.end()) {
                    entry.template_id = found->second;
                    if (found->second < retail_unlocks.size()) {
                        entry.requirement_ids = {retail_unlocks[found->second].text,
                                                 retail_unlocks[found->second].text_short};
                    }
                }
            } else if (unlock.kind == AddedCarUnlock::Kind::Takedowns) {
                entry.requirement = {"AWARDED AT " + std::to_string(unlock.count) + " TAKEDOWNS",
                                     std::to_string(unlock.count) + " TAKEDOWNS"};
            } else if (unlock.kind == AddedCarUnlock::Kind::Event) {
                const std::string& event = unlock.event;
                const bool number = !event.empty() && event.size() <= 3U &&
                                    std::all_of(event.begin(), event.end(), [](char c) { return c >= '0' && c <= '9'; });
                if (number) {
                    const auto index = static_cast<std::size_t>(std::stoul(event));
                    if (index < events.size()) {
                        entry.event = static_cast<std::uint32_t>(index);
                    }
                } else if (const auto key = encode_vehicle_name(upper(event))) {
                    for (std::size_t index = 0U; index < events.size(); ++index) {
                        if (events[index] == *key) {
                            entry.event = static_cast<std::uint32_t>(index);
                        }
                    }
                }
                static constexpr std::array<std::string_view, 4> kMedals{"A MEDAL", "A MEDAL", "SILVER", "GOLD"};
                const std::string medal{kMedals[std::min<std::size_t>(unlock.medal, 3U)]};
                const std::string which = entry.event.has_value() ? std::to_string(*entry.event + 1U) : "?";
                entry.requirement = {"AWARDED FOR " + medal + " IN WORLD TOUR EVENT " + which,
                                     medal + " IN EVENT " + which};
                // A retail car unlocked by gold in the same event has the
                // game's own words for it.
                if (entry.event.has_value() && unlock.medal == 3U) {
                    for (const RetailUnlock& retail : retail_unlocks) {
                        if (retail.kind == kUnlockKindEventGold && retail.value == *entry.event) {
                            entry.requirement_ids = {retail.text, retail.text_short};
                        }
                    }
                }
            }
            if (!unlock.text.empty() && unlock.kind != AddedCarUnlock::Kind::Template) {
                entry.requirement = {upper(unlock.text), upper(unlock.text)};
                entry.requirement_ids = {0U, 0U};
            }
        }
    }

    // Whether the car's rule is met by the profile the game keeps.
    [[nodiscard]] static bool unlocked(const Car& entry, const guest::EeMemory& memory) {
        const AddedCarUnlock& unlock = entry.car.unlock;
        if (unlock.kind == AddedCarUnlock::Kind::None) {
            return true;
        }
        if (unlock.kind == AddedCarUnlock::Kind::Template) {
            return !entry.template_id.has_value() || memory.read_u8(kUnlockFlags + *entry.template_id) != 0U;
        }
        const std::uint32_t profile = memory.read_u32_le(kProfilePointer);
        if (profile == 0U || !memory.contains(profile, kProfileEventMedals + kEventCount)) {
            return false;
        }
        if (unlock.kind == AddedCarUnlock::Kind::Takedowns) {
            // As the game's own takedown unlocks: the global and the profile's
            // stat, compared unsigned.
            const std::uint32_t total =
                memory.read_u32_le(kTakedownsGlobal) + memory.read_u32_le(profile + kProfileTakedowns);
            return total >= unlock.count;
        }
        if (!entry.event.has_value()) {
            return false;
        }
        const auto medal = static_cast<std::int8_t>(memory.read_u8(profile + kProfileEventMedals + *entry.event));
        return medal >= static_cast<std::int8_t>(std::clamp<std::uint8_t>(unlock.medal, 1U, 3U));
    }
};

Slus21050AddedCars::Slus21050AddedCars() : state_(std::make_shared<State>()) {}

void Slus21050AddedCars::set_cars(std::vector<AddedCar> cars) {
    if (cars == state_->configured) {
        return;
    }
    state_->configured = cars;
    state_->cars.clear();
    state_->problems.clear();
    for (AddedCar& car : cars) {
        car.vehicle = upper(car.vehicle);
        car.template_vehicle = upper(car.template_vehicle);
        const auto key = encode_vehicle_name(car.vehicle);
        const auto template_key = encode_vehicle_name(car.template_vehicle);
        const std::string_view prefix = std::string_view{car.vehicle}.substr(0U, 4U);
        std::string problem;
        if (!key.has_value() || car.vehicle.size() < 5U) {
            problem = "its vehicle name must be 5 to 12 capital letters and digits";
        } else if (car.class_id.empty() && std::find(kClasses.begin(), kClasses.end(), prefix) == kClasses.end()) {
            problem = "its vehicle name does not start with a class (COMP, MSCL, CUPE, SPRT, SUPR, HSPC, HEVY, "
                      "TRAF or TSPC)";
        } else if (!template_key.has_value()) {
            problem = "its template is not a vehicle name";
        } else if (state_->find(*key) != nullptr) {
            problem = "another added car has the same vehicle name";
        } else if (state_->cars.size() >= kMaximumCars) {
            problem = "too many added cars";
        }
        if (!problem.empty()) {
            state_->problems.push_back(car.vehicle + " (" + car.mod + "): " + problem);
            continue;
        }
        State::Car entry;
        entry.key = *key;
        entry.template_key = *template_key;
        entry.hash_from = "/" + car.vehicle + ".cfg";
        entry.hash_to = "/" + car.template_vehicle + ".cfg";
        entry.car = std::move(car);
        state_->cars.push_back(std::move(entry));
    }
    state_->resolve();
    state_->strings_dirty = true;
    state_->last_added.reset();
}

const std::vector<AddedCar>& Slus21050AddedCars::cars() const noexcept { return state_->configured; }

const std::vector<std::string>& Slus21050AddedCars::problems() const noexcept { return state_->problems; }

bool Slus21050AddedCars::unlocked(const std::uint64_t key, const guest::EeMemory& memory) const {
    if (state_->developer_unlock) {
        // Every car the registry lists, for this session only.
        if (!memory.contains(kRegistry, 0x408U + 8U * kRegistryCapacity) ||
            memory.read_u32_le(kRegistry) != kRegistryVersion) {
            return false;
        }
        const std::uint32_t count = memory.read_u32_le(kRegistry + 4U);
        if (count > kRegistryCapacity) {
            return false;
        }
        for (std::uint32_t index = 0U; index < count; ++index) {
            if (memory.read_u64_le(kRegistryKeys + 8U * index) == key) {
                return true;
            }
        }
        return false;
    }
    if (const State::Car* car = state_->find(key)) {
        return State::unlocked(*car, memory);
    }
    const auto found = state_->retail_ids.find(key);
    return found != state_->retail_ids.end() && memory.read_u8(kUnlockFlags + found->second) != 0U;
}

void Slus21050AddedCars::enable_developer_unlock() noexcept { state_->developer_unlock = true; }

bool Slus21050AddedCars::developer_unlock_enabled() const noexcept { return state_->developer_unlock; }

std::optional<OnlineCarSelection> Slus21050AddedCars::resolve_online_selection(const guest::EeMemory& memory,
                                                                               const std::uint64_t key,
                                                                               const std::uint32_t paint) const {
    if (!state_->registered || !memory.contains(kRegistry, 0x408U + 8U * kRegistryCapacity) ||
        memory.read_u32_le(kRegistry) != kRegistryVersion) {
        return std::nullopt;
    }
    const auto count = memory.read_u32_le(kRegistry + 4U);
    if (count == 0U || count > kRegistryCapacity) {
        return std::nullopt;
    }
    // Listed with its flag set and chosen as the game would allow.
    const auto available = [&](std::uint64_t candidate) {
        for (std::uint32_t index = 0U; index < count; ++index) {
            if (memory.read_u64_le(kRegistryKeys + 8U * index) == candidate &&
                memory.read_u32_le(kRegistryFlags + 4U * index) != 0U) {
                return unlocked(candidate, memory);
            }
        }
        return false;
    };
    if (available(key)) {
        return OnlineCarSelection{.vehicle_key = key, .paint = paint, .changed = false};
    }
    // Otherwise a retail car of one of the first six classes (not traffic).
    const auto eligible_retail = [this, &available](std::uint64_t candidate) {
        if (!state_->retail_ids.contains(candidate) || !available(candidate)) {
            return false;
        }
        const auto name = decode_vehicle_name(candidate);
        return std::find(kClasses.begin(), kClasses.begin() + 6, std::string_view{name}.substr(0U, 4U)) !=
               kClasses.begin() + 6;
    };
    if (const State::Car* car = state_->find(key); car != nullptr && eligible_retail(car->template_key)) {
        return OnlineCarSelection{.vehicle_key = car->template_key, .paint = 0U, .changed = true};
    }
    for (std::uint32_t index = 0U; index < count; ++index) {
        const std::uint64_t candidate = memory.read_u64_le(kRegistryKeys + 8U * index);
        if (eligible_retail(candidate)) {
            return OnlineCarSelection{.vehicle_key = candidate, .paint = 0U, .changed = true};
        }
    }
    return std::nullopt;
}

void Slus21050AddedCars::apply_online_selection(guest::EeMemory& memory, const OnlineCarSelection& selection) const {
    if (!selection.changed) {
        return;
    }
    // The game's car selection records: vehicle key and paint.
    const std::array<std::pair<guest::GuestAddress, guest::GuestAddress>, 3> records{{
        {0x01E90130U, 0x01E90170U},
        {0x01E94EF8U, 0x01E94F18U},
        {0x01E3D578U, 0x01E3D580U},
    }};
    for (const auto& [vehicle, paint] : records) {
        if (!memory.contains(vehicle, 8U) || !memory.contains(paint, 4U)) {
            return;
        }
    }
    for (const auto& [vehicle, paint] : records) {
        memory.write_u64_le(vehicle, selection.vehicle_key);
        memory.write_u32_le(paint, selection.paint);
    }
}

std::string Slus21050AddedCars::requirement(const std::uint64_t key) const {
    const State::Car* car = state_->find(key);
    return car != nullptr ? car->requirement[0] : std::string{};
}

bool Slus21050AddedCars::registered() const noexcept { return state_->registered; }

std::optional<RetailValueDatabase> RetailValueDatabase::parse(const std::span<const std::uint8_t> bytes) {
    const auto word = [&bytes](const std::size_t offset) {
        return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
               (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
               (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
    };
    if (bytes.size() < 20U || word(0U) != 2U) {
        return std::nullopt;
    }
    const std::size_t count = word(4U);
    if ((bytes.size() - 20U) / 8U < count) {
        return std::nullopt;
    }
    RetailValueDatabase database;
    database.rows_.reserve(count);
    for (std::size_t row = 0U; row < count; ++row) {
        const auto key = static_cast<std::int32_t>(word(20U + 8U * row + 4U));
        if (!database.rows_.empty() && key <= database.rows_.back().first) {
            return std::nullopt;
        }
        database.rows_.emplace_back(key, word(20U + 8U * row));
    }
    return database;
}

std::optional<std::uint32_t> RetailValueDatabase::find(const std::int32_t key) const {
    const auto found = std::lower_bound(rows_.begin(), rows_.end(), key,
                                        [](const auto& row, const std::int32_t value) { return row.first < value; });
    if (found == rows_.end() || found->first != key) {
        return std::nullopt;
    }
    return found->second;
}

void Slus21050AddedCars::set_retail_values(std::optional<RetailValueDatabase> values) {
    state_->retail_values = std::move(values);
}

bool Slus21050AddedCars::register_tuning(NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory) {
    std::uint64_t signature = 0xcbf29ce484222325ULL;
    for (auto at = kVehicleTuning; at != kVehicleTuning + kVehicleTuningSize; ++at) {
        signature = (signature ^ memory.read_u8(at)) * 0x100000001b3ULL;
    }
    if (signature != kVehicleTuningSignature || dispatcher.lookup(kVehicleTuning) != nullptr) {
        return false;
    }
    std::shared_ptr<State> state = state_;
    // The value the retail routine is about to read for an added car's
    // physics file, scaled by the car's multiplier; nullopt leaves the
    // retail routine to run.
    const auto resolve = [state](const guest::EeMemory& ram, const guest::GuestAddress database,
                                 const guest::GuestAddress record) -> std::optional<Resolved> {
        // A float binding (type 2) of a scalar or one-element value.
        if (!ram.contains(record, 32U) || !ram.contains(database, kDatabaseSize) || ram.read_u8(record + 30U) != 2U ||
            ram.read_u16_le(record + 28U) > 1U) {
            return std::nullopt;
        }
        const auto text = [&ram](const guest::GuestAddress address) {
            std::string value;
            for (auto i = 0U; i < kMaximumText && ram.contains(address + i, 1U); ++i) {
                const std::uint8_t byte = ram.read_u8(address + i);
                if (byte == 0U) {
                    return value;
                }
                value.push_back(static_cast<char>(byte));
            }
            return std::string{};
        };
        const auto file = text(ram.read_u32_le(record + 20U));
        if (file.find("/VehiclePhysics/") == std::string::npos) {
            return std::nullopt;
        }
        const auto field = text(ram.read_u32_le(record + 12U));
        float multiplier = 1.0F;
        for (const State::Car& car : state->cars) {
            if (file.ends_with(car.hash_from)) {
                if (field == "Mass (Kg)") {
                    multiplier = car.car.mass_multiplier;
                } else if (field == "Speed Limit" || field == "Max Speed In Boost") {
                    multiplier = car.car.top_speed_multiplier;
                } else if (field == "Final Gear") {
                    multiplier = 1.0F / car.car.top_speed_multiplier;
                }
                break;
            }
        }
        if (!std::isfinite(multiplier) || multiplier <= 0.0F) {
            return std::nullopt;
        }
        const std::uint32_t count = ram.read_u32_le(database + kDatabaseRowCount);
        const std::uint32_t rows = ram.read_u32_le(database + kDatabaseRows);
        const std::uint32_t destination = ram.read_u32_le(record);
        if (count > kMaximumDatabaseRows || !ram.contains(rows, 8U * static_cast<std::size_t>(count)) ||
            !ram.contains(destination, 4U)) {
            return std::nullopt;
        }
        const auto key = static_cast<std::int32_t>(ram.read_u32_le(record + 24U));
        std::uint32_t lo = 0U;
        std::uint32_t hi = count;
        while (lo < hi) {
            const auto mid = lo + (hi - lo) / 2U;
            const auto current = static_cast<std::int32_t>(ram.read_u32_le(rows + 8U * mid + 4U));
            if (current < key) {
                lo = mid + 1U;
            } else {
                hi = mid;
            }
        }
        if (lo == count || static_cast<std::int32_t>(ram.read_u32_le(rows + 8U * lo + 4U)) != key) {
            return std::nullopt;
        }
        const std::uint32_t loaded = ram.read_u32_le(rows + 8U * lo);
        const auto retail = state->retail_values.has_value() ? state->retail_values->find(key) : std::nullopt;
        const std::uint32_t base = retail.value_or(loaded);
        const float value = multiplier * std::bit_cast<float>(base);
        if (!std::isfinite(value) || std::bit_cast<std::uint32_t>(value) == loaded) {
            return std::nullopt;
        }
        return Resolved{.destination = destination, .value = value, .retail = base != loaded};
    };
    const NativeFunctionLimits cost{1U, 1U};
    return dispatcher
        .register_function(
            kVehicleTuning, "NativeVehicleTuning", cost,
            {.scheduler_state_transparent = true, .single_step_host_call = true},
            [resolve, cost](const NativeFunctionPrepareContext& context) -> std::optional<NativeFunctionLimits> {
                // Only a value this routine changes is taken over.
                return resolve(context.memory, context.cpu.read_gpr_u32(kA0), context.cpu.read_gpr_u32(kA1))
                           ? std::optional<NativeFunctionLimits>{cost}
                           : std::nullopt;
            },
            [resolve, state](NativeFunctionContext& context) -> NativeFunctionExecution {
                const auto value = resolve(context.memory, context.cpu.read_gpr_u32(kA0), context.cpu.read_gpr_u32(kA1));
                if (!value.has_value()) {
                    throw std::logic_error("native vehicle binding changed after preparation");
                }
                state->statistics.retail_handling += value->retail ? 1U : 0U;
                context.memory.write_u32_le(value->destination, std::bit_cast<std::uint32_t>(value->value));
                context.cpu.set_pc(context.cpu.read_gpr_u32(kRa));
                return {.retired_instructions = 1U, .ee_cycles = 1U};
            })
        .registered();
}

Slus21050AddedCars::Statistics Slus21050AddedCars::statistics() const noexcept { return state_->statistics; }

bool Slus21050AddedCars::register_functions(NativeFunctionDispatcher& dispatcher, const guest::EeMemory& memory) {
    if (!matches(memory, kKeyToCarId, kKeyToCarIdWords) || !matches(memory, kCarUnlocked, kCarUnlockedWords) ||
        !matches(memory, kClassUnlocked, kClassUnlockedWords) ||
        !matches(memory, kUnlockText, kUnlockTextWords) || !matches(memory, kUnlockTextShort, kUnlockTextShortWords) ||
        !memory.contains(kEvents, 8U * kEventCount) || !memory.contains(kCarUnlockKinds, 4U * kCarCount) ||
        !matches(memory, kCarNameString, kCarNameStringWords) ||
        !matches(memory, kStringLookup, kStringLookupWords) ||
        !matches(memory, kStringLookupBody, kStringLookupBodyWords) ||
        !matches(memory, kHashString, kHashStringWords) || !memory.contains(kCarKeys, 8U * kCarCount) ||
        !memory.contains(kHashTable, 1024U)) {
        return false;
    }
    State* state = state_.get();
    // The retail car ids, imported from the game's own table.
    state->retail_ids.clear();
    for (std::uint32_t id = kCarCount; id-- > 0U;) {
        state->retail_ids[memory.read_u64_le(kCarKeys + 8U * id)] = id;
    }
    state->retail_unlocks.clear();
    for (std::uint32_t id = 0U; id < kCarCount; ++id) {
        state->retail_unlocks.push_back({memory.read_u32_le(kCarUnlockKinds + 4U * id),
                                         memory.read_u32_le(kCarUnlockValues + 4U * id),
                                         memory.read_u32_le(kCarUnlockTexts + 4U * id),
                                         memory.read_u32_le(kCarUnlockTextsShort + 4U * id)});
    }
    state->events.clear();
    for (std::uint32_t index = 0U; index < kEventCount; ++index) {
        state->events.push_back(memory.read_u64_le(kEvents + 8U * index));
    }
    state->resolve();

    const NativeFunctionTraits pure{.scheduler_state_transparent = true};
    // 0x003EB060: the car id of a registry key, -1 for none. Remembers the
    // key when it is an added car, for the name routine that follows.
    const auto key_to_id = dispatcher.register_function(
        kKeyToCarId, "VehicleKeyToCarId", {4U + 9U * kCarCount + 5U, 4U + 9U * kCarCount + 5U}, pure,
        [state](NativeFunctionContext& context) -> NativeFunctionExecution {
            const std::uint64_t key = context.cpu.read_gpr_u64(kA0);
            const auto found = state->retail_ids.find(key);
            std::uint32_t result = 0xFFFFFFFFU;
            std::uint32_t scanned = kCarCount;
            state->last_added.reset();
            if (found != state->retail_ids.end()) {
                result = found->second;
                scanned = found->second;
            } else {
                for (std::size_t index = 0U; index < state->cars.size(); ++index) {
                    if (state->cars[index].key == key) {
                        state->last_added = index;
                    }
                }
            }
            context.cpu.write_gpr_sign_extended_32(kV0, result);
            context.cpu.set_pc(context.cpu.read_gpr_u32(kRa));
            const std::uint32_t cost = 4U + 9U * scanned + 5U;
            return {.retired_instructions = cost, .ee_cycles = cost};
        });
    // 0x003EAE40: the name string id of a car id (a1 != 0: upper case).
    const auto name = dispatcher.register_function(
        kCarNameString, "CarNameStringId", {11U, 11U}, pure,
        [state](NativeFunctionContext& context) -> NativeFunctionExecution {
            const auto id = static_cast<std::int32_t>(context.cpu.read_gpr_u32(kA0));
            const bool upper_case = context.cpu.read_gpr_u64(kA1) != 0U;
            std::uint32_t result = 0U;
            if (id >= 0) {
                result = context.memory.read_u32_le((upper_case ? kCarNamesUpper : kCarNamesMixed) +
                                                    4U * static_cast<std::uint32_t>(id));
            } else if (state->last_added.has_value() && state->strings_published) {
                result = kStringIdBase + kStringsPerCar * static_cast<std::uint32_t>(*state->last_added) +
                         (upper_case ? 0U : 1U);
                ++state->statistics.names_shown;
            } else {
                result = upper_case ? 1279U : 1278U;
            }
            state->last_added.reset();
            context.cpu.write_gpr_sign_extended_32(kV0, result);
            context.cpu.set_pc(context.cpu.read_gpr_u32(kRa));
            return {.retired_instructions = 11U, .ee_cycles = 11U};
        });
    // 0x00271A00: the string of a string id in a string table (a0 points to
    // the table object's pointer); the added names past the game's strings.
    const auto lookup = dispatcher.register_function(
        kStringLookup, "LookupString", {7U, 7U}, pure,
        [state](NativeFunctionContext& context) -> NativeFunctionExecution {
            const std::uint32_t id = context.cpu.read_gpr_u32(kA1);
            std::uint32_t result = 0U;
            const std::uint32_t added = id - kStringIdBase;
            if (id >= kStringIdBase && added < kStringsPerCar * state->cars.size() && state->strings_published) {
                result = state->cars[added / kStringsPerCar].strings[added % kStringsPerCar];
            } else {
                const std::uint32_t table = context.memory.read_u32_le(context.cpu.read_gpr_u32(kA0));
                const std::uint32_t array = context.memory.read_u32_le(table + 12U);
                result = context.memory.read_u32_le(array + 4U * id);
            }
            context.cpu.write_gpr_sign_extended_32(kV0, result);
            context.cpu.set_pc(context.cpu.read_gpr_u32(kRa));
            return {.retired_instructions = 7U, .ee_cycles = 7U};
        });
    // 0x0021BFA0: the CRC-32 the value database keys are made of, over a C
    // string (signed bytes, arithmetic shifts, no final inversion). A path
    // naming an added car's .cfg is hashed with its template's name.
    constexpr std::uint32_t kHashCeiling = 14U + 15U * kMaximumHashedLength;
    const auto hash = dispatcher.register_function(
        kHashString, "HashString", {kHashCeiling, kHashCeiling}, pure,
        [state](NativeFunctionContext& context) -> NativeFunctionExecution {
            const std::uint32_t source = context.cpu.read_gpr_u32(kA0);
            std::string text;
            for (std::uint32_t at = source; text.size() < kMaximumHashedLength; ++at) {
                const auto byte = static_cast<char>(context.memory.read_u8(at));
                if (byte == '\0') {
                    break;
                }
                text.push_back(byte);
            }
            for (const State::Car& car : state->cars) {
                const auto at = text.find(car.hash_from);
                if (at != std::string::npos) {
                    text.replace(at, car.hash_from.size(), car.hash_to);
                    ++state->statistics.hash_aliases;
                    break;
                }
            }
            std::uint32_t crc = 0xFFFFFFFFU;
            for (const char character : text) {
                const auto signed_byte = static_cast<std::uint32_t>(static_cast<std::int32_t>(
                    static_cast<std::int8_t>(character)));
                const std::uint32_t index = signed_byte ^ (crc & 0xFFU);
                crc = static_cast<std::uint32_t>(static_cast<std::int32_t>(crc) >> 8) ^
                      context.memory.read_u32_le(kHashTable + 4U * index);
            }
            context.cpu.write_gpr_sign_extended_32(kV0, crc);
            context.cpu.set_pc(context.cpu.read_gpr_u32(kRa));
            const std::uint32_t cost = 14U + 15U * static_cast<std::uint32_t>(text.size());
            return {.retired_instructions = cost, .ee_cycles = cost};
        });
    // 0x0013A660: whether a registry car can be chosen (a1 the key): not in
    // the registry, no; no car id, yes; else the game's unlock flag. An added
    // car with a rule is checked against the profile.
    constexpr std::uint32_t kUnlockedCeiling = 20U + 8U * kRegistryCapacity + 9U * kCarCount;
    const auto unlocked = dispatcher.register_function(
        kCarUnlocked, "IsVehicleUnlocked", {kUnlockedCeiling, kUnlockedCeiling}, pure,
        [state](NativeFunctionContext& context) -> NativeFunctionExecution {
            const std::uint64_t key = context.cpu.read_gpr_u64(kA1);
            const auto& guest_memory = context.memory;
            std::uint32_t result = 0U;
            const std::uint32_t count = std::min(guest_memory.read_u32_le(kRegistry + 4U), kRegistryCapacity);
            bool listed = false;
            for (std::uint32_t index = 0U; index < count && !listed; ++index) {
                listed = guest_memory.read_u64_le(kRegistryKeys + 8U * index) == key;
            }
            if (listed) {
                const auto found = state->retail_ids.find(key);
                if (state->developer_unlock) {
                    // Session-only: every listed car can be chosen.
                    result = 1U;
                } else if (found != state->retail_ids.end()) {
                    result = guest_memory.read_u8(kUnlockFlags + found->second);
                } else if (const State::Car* car = state->find(key)) {
                    result = State::unlocked(*car, guest_memory) ? 1U : 0U;
                    ++state->statistics.locked_checks;
                } else {
                    result = 1U;
                }
            }
            context.cpu.write_gpr_sign_extended_32(kV0, result);
            context.cpu.set_pc(context.cpu.read_gpr_u32(kRa));
            const std::uint32_t cost = 20U + 8U * count + 9U * kCarCount;
            return {.retired_instructions = cost, .ee_cycles = cost};
        });
    // 0x003EAEA0 and 0x003EAED0: a car's unlock requirement (long and short)
    // as a string id; for id -1 right after a lookup of an added car with a
    // rule, its requirement.
    const auto requirement = [state](const guest::GuestAddress table, const std::uint32_t slot) {
        return [state, table, slot](NativeFunctionContext& context) -> NativeFunctionExecution {
            const auto id = static_cast<std::int32_t>(context.cpu.read_gpr_u32(kA0));
            std::uint32_t result = 1279U;
            if (id >= 0) {
                result = context.memory.read_u32_le(table + 4U * static_cast<std::uint32_t>(id));
            } else if (state->last_added.has_value() && state->strings_published) {
                const State::Car& car = state->cars[*state->last_added];
                if (car.car.unlock.kind != AddedCarUnlock::Kind::None) {
                    result = car.requirement_ids[slot] != 0U
                                 ? car.requirement_ids[slot]
                                 : kStringIdBase + kStringsPerCar * static_cast<std::uint32_t>(*state->last_added) +
                                       2U + slot;
                }
            }
            context.cpu.write_gpr_sign_extended_32(kV0, result);
            context.cpu.set_pc(context.cpu.read_gpr_u32(kRa));
            return {.retired_instructions = 9U, .ee_cycles = 9U};
        };
    };
    const auto text = dispatcher.register_function(kUnlockText, "CarUnlockTextId", {9U, 9U}, pure,
                                                   requirement(kCarUnlockTexts, 0U));
    const auto text_short = dispatcher.register_function(kUnlockTextShort, "CarUnlockShortTextId", {9U, 9U}, pure,
                                                         requirement(kCarUnlockTextsShort, 1U));
    // 0x0013A6D0: whether a class has a car to choose (a1 the class). Only
    // replaced while the developer unlock is on: then every class with a
    // listed car is available; otherwise the retail routine runs.
    const auto class_unlocked = dispatcher.register_function(
        kClassUnlocked, "DeveloperClassAvailable", {1U, 1U},
        {.scheduler_state_transparent = true, .single_step_host_call = true},
        [state](const NativeFunctionPrepareContext&) -> std::optional<NativeFunctionLimits> {
            if (state->developer_unlock) {
                return NativeFunctionLimits{1U, 1U};
            }
            return std::nullopt;
        },
        [state](NativeFunctionContext& context) -> NativeFunctionExecution {
            const std::uint32_t class_index = context.cpu.read_gpr_u32(kA1);
            // Traffic (TRAF) is always available, as in the game.
            bool available = class_index == 7U;
            const auto& ram = context.memory;
            if (!available && class_index < kClasses.size() && ram.read_u32_le(kRegistry) == kRegistryVersion) {
                const std::uint32_t count = ram.read_u32_le(kRegistry + 4U);
                for (std::uint32_t index = 0U; count <= kRegistryCapacity && index < count && !available; ++index) {
                    if (ram.read_u32_le(kRegistryFlags + 4U * index) == 0U) {
                        continue;
                    }
                    const std::uint64_t key = ram.read_u64_le(kRegistryKeys + 8U * index);
                    const State::Car* car = state->find(key);
                    const auto vehicle = car != nullptr ? car->car.template_vehicle : decode_vehicle_name(key);
                    available = std::string_view{vehicle}.substr(0U, 4U) == kClasses[class_index];
                }
            }
            context.cpu.write_gpr_sign_extended_32(kV0, available ? 1U : 0U);
            context.cpu.set_pc(context.cpu.read_gpr_u32(kRa));
            return {.retired_instructions = 1U, .ee_cycles = 1U};
        });
    state->registered = key_to_id.registered() && name.registered() && lookup.registered() && hash.registered() &&
                        unlocked.registered() && text.registered() && text_short.registered() &&
                        class_unlocked.registered();
    return state->registered;
}

void Slus21050AddedCars::service(guest::EeMemory& memory) {
    State& state = *state_;
    if (!state.registered) {
        return;
    }
    // The names: UTF-16, upper case and as written, after a magic word.
    if (state.strings_dirty || memory.read_u32_le(kRegion) != kRegionMagic) {
        guest::GuestAddress at = kRegion + 16U;
        const auto write = [&](const std::string& text) {
            const guest::GuestAddress start = at;
            for (const char character : text) {
                if (at + 4U > kRegion + kRegionSize) {
                    break;
                }
                memory.write_u16_le(at, static_cast<std::uint16_t>(static_cast<unsigned char>(character)));
                at += 2U;
            }
            memory.write_u16_le(at, 0U);
            at = (at + 2U + 3U) & ~3U;
            return start;
        };
        for (State::Car& car : state.cars) {
            car.strings = {write(upper(car.car.name)), write(car.car.name), write(car.requirement[0]),
                           write(car.requirement[1])};
        }
        memory.write_u32_le(kRegion, kRegionMagic);
        state.strings_dirty = false;
        state.strings_published = true;
    }

    // The registry, once the game has loaded it.
    if (!memory.contains(kRegistry, 0x408U + 8U * kRegistryCapacity) ||
        memory.read_u32_le(kRegistry) != kRegistryVersion) {
        return;
    }
    std::uint32_t count = memory.read_u32_le(kRegistry + 4U);
    if (count == 0U || count > kRegistryCapacity) {
        return;
    }
    const auto key_at = [&](const std::uint32_t index) { return memory.read_u64_le(kRegistryKeys + 8U * index); };
    const auto index_of = [&](const std::uint64_t key) -> std::optional<std::uint32_t> {
        for (std::uint32_t index = 0U; index < count; ++index) {
            if (key_at(index) == key) {
                return index;
            }
        }
        return std::nullopt;
    };
    bool changed = false;
    // Entries of cars that are no longer added (a mod was turned off).
    for (auto key = state.added_keys.begin(); key != state.added_keys.end();) {
        if (state.find(*key) != nullptr) {
            ++key;
            continue;
        }
        if (const auto index = index_of(*key)) {
            for (std::uint32_t move = *index; move + 1U < count; ++move) {
                memory.write_u32_le(kRegistryFlags + 4U * move, memory.read_u32_le(kRegistryFlags + 4U * (move + 1U)));
                memory.write_u32_le(kRegistryValues + 4U * move,
                                    memory.read_u32_le(kRegistryValues + 4U * (move + 1U)));
                memory.write_u64_le(kRegistryKeys + 8U * move, key_at(move + 1U));
            }
            --count;
            changed = true;
        }
        key = state.added_keys.erase(key);
    }
    for (const State::Car& car : state.cars) {
        if (index_of(car.key).has_value() || count >= kRegistryCapacity) {
            continue;
        }
        const auto template_index = index_of(car.template_key);
        if (!template_index.has_value()) {
            continue;
        }
        memory.write_u32_le(kRegistryFlags + 4U * count, 1U);
        memory.write_u32_le(kRegistryValues + 4U * count, memory.read_u32_le(kRegistryValues + 4U * *template_index));
        memory.write_u64_le(kRegistryKeys + 8U * count, car.key);
        ++count;
        changed = true;
        if (std::find(state.added_keys.begin(), state.added_keys.end(), car.key) == state.added_keys.end()) {
            state.added_keys.push_back(car.key);
        }
    }
    if (changed) {
        memory.write_u32_le(kRegistry + 4U, count);
        ++state.statistics.registry_patches;
    }
}

} // namespace takedown::runtime
