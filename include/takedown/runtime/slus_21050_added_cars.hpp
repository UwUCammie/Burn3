#pragma once

// Cars added by mods to SLUS-21050 next to the retail cars (docs/MODDING.md,
// "Added cars"). Nothing the game ships is replaced:
//
//   * The vehicle registry (PVEH/VLIST.BIN, loaded to 0x005186A0: u32 6,
//     u32 count, u32 flags[128], u32 values[128], u64 keys[128]) gets one
//     entry per added car after the retail ones. A key is the car's name
//     packed in base 40 (12 characters: space, digits, letters); its first
//     four characters are its class (COMP, MSCL, CUPE, SPRT, SUPR, HSPC,
//     HEVY, TRAF, TSPC) and the game loads its files from
//     PVEH/<class>/<rest>.BGV, .HWD and .LWD, which the mod adds. Car select
//     lists every registry car of a class whose flag is set, so the added car
//     appears at the end of its class.
//   * The game's per-car tables (names, unlocks, stats: 67 entries indexed by
//     car id) have no row for an added car; the key-to-id lookup gives -1,
//     which the game already treats as an unlocked car without table data.
//   * The car's handling and sounds are the game's value database entries of
//     its template car: the database key is a hash of a path naming the car
//     (".../VehiclePhysics/<car>.cfg"), and the native hash routine hashes
//     the template's name in its place.
//   * Its name is shown where the game shows a car's name: the native
//     key-to-id routine remembers the key it was asked about, and the native
//     name routine gives an added car's name string for id -1 right after.
//     The name strings live in a host-owned region of low EE RAM and are
//     resolved by the native string lookup.
//   * A car skin (a retail car file a mod has, content::CarSkin) is an
//     added car whose template is that retail car and whose unlock rule is
//     the retail car's own (Template): the retail car stays as it is and the
//     skin is listed after it.
//   * Its unlock rule is checked where the game checks whether a car can be
//     chosen (0x0013A660), with the game's own counters: the takedowns total
//     its "AWARDED AT N TAKEDOWNS" cars use (the global at 0x0051C124 plus
//     the profile's stat at +512) and the medal the profile holds for a World
//     Tour event (profile + 902 + event, 3 for gold). A locked car shows its
//     requirement where the game shows a retail car's (0x003EAEA0 and
//     0x003EAED0, after the same key lookup).
//
// The routines (key to car id 0x003EB060, car name string 0x003EAE40, unlock
// texts 0x003EAEA0 and 0x003EAED0, car unlocked 0x0013A660, string lookup
// 0x00271A00 and string hash 0x0021BFA0) are registered as native functions;
// each behaves exactly as the retail routine for anything that is not an
// added car.

#include "takedown/guest/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takedown::guest {
class EeMemory;
} // namespace takedown::guest

namespace takedown::runtime {

class NativeFunctionDispatcher;

// The vehicle registry's base-40 name packing (decoders 0x0021ADF0 and
// 0x0021AB40): twelve characters, the first the most significant; space 0,
// '0'..'9' 3..12, 'A'..'Z' 13..38.
inline constexpr std::size_t kVehicleNameLength = 12U;
[[nodiscard]] std::optional<std::uint64_t> encode_vehicle_name(std::string_view name);
[[nodiscard]] std::string decode_vehicle_name(std::uint64_t key);

// When an added car can be chosen.
struct AddedCarUnlock {
    // Template: unlocked with the template (retail) car, showing its
    // requirement; the rule of a car skin.
    enum class Kind : std::uint8_t { None, Takedowns, Event, Template };
    Kind kind{Kind::None};
    std::uint32_t count{0U}; // Takedowns: the career takedowns needed
    // Event: the World Tour event, as its index (0 to 72) or its key (e.g.
    // "0A0000USC3V1"), and the medal needed in it (1 bronze, 2 silver, 3 gold).
    std::string event;
    std::uint8_t medal{1U};
    std::string text; // the requirement shown while locked; empty: generated

    bool operator==(const AddedCarUnlock&) const = default;
};

struct AddedCar {
    std::string vehicle;          // registry name, e.g. "CUPEBMWM3"
    std::string template_vehicle; // retail car its handling and sounds come from
    bool skin{false};             // a skin of the template (content::CarSkin)
    std::string name;             // shown in the game, e.g. "BMW M3 GTR"
    AddedCarUnlock unlock;
    std::string mod;              // the mod folder that adds it
    std::string class_id;         // native UI grouping, independent of the asset prefix
    std::string thumbnail;        // native image resource, absolute path
    std::array<std::string, 8> thumbnail_variants{}; // paint choices, derived or declared
    float mass_multiplier{1.0F}, top_speed_multiplier{1.0F};

    bool operator==(const AddedCar&) const = default;
};

// The game's value database file (DATA/VDB.XML): u32 version 2, u32 row
// count, three u32 the rows do not use, then the rows (f32 value, i32 key)
// sorted by key. The game binary-searches the same rows in memory.
class RetailValueDatabase {
  public:
    // Nullopt when the bytes are not that layout (keys unsorted or repeated).
    [[nodiscard]] static std::optional<RetailValueDatabase> parse(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::optional<std::uint32_t> find(std::int32_t key) const; // value bits
    [[nodiscard]] std::size_t size() const noexcept { return rows_.size(); }

  private:
    std::vector<std::pair<std::int32_t, std::uint32_t>> rows_;
};

// One explicit mod-sync replay chooses a car from the newly applied registry.
// A valid original selection, including its paint, is retained unchanged.
struct OnlineCarSelection {
    std::uint64_t vehicle_key{};
    std::uint32_t paint{};
    bool changed{};
};

class Slus21050AddedCars {
  public:
    static constexpr guest::GuestAddress kRegistry = 0x005186A0U;
    static constexpr std::uint32_t kRegistryVersion = 6U;
    static constexpr std::uint32_t kRegistryCapacity = 128U;
    static constexpr guest::GuestAddress kRegistryFlags = kRegistry + 0x008U;
    static constexpr guest::GuestAddress kRegistryValues = kRegistry + 0x208U;
    static constexpr guest::GuestAddress kRegistryKeys = kRegistry + 0x408U;
    // The car info module's tables (car id 0..66).
    static constexpr guest::GuestAddress kCarKeys = 0x004CCF80U;
    static constexpr std::uint32_t kCarCount = 67U;
    static constexpr guest::GuestAddress kCarNamesUpper = 0x004CD2B0U;
    static constexpr guest::GuestAddress kCarNamesMixed = 0x004CD3C0U;
    // The string hash's table (CRC-32, read by the retail loop).
    static constexpr guest::GuestAddress kHashTable = 0x0049B110U;
    // Retail routines replaced by native ones.
    static constexpr guest::GuestAddress kKeyToCarId = 0x003EB060U;
    static constexpr guest::GuestAddress kCarNameString = 0x003EAE40U;
    static constexpr guest::GuestAddress kStringLookup = 0x00271A00U;
    static constexpr guest::GuestAddress kStringLookupBody = 0x00271A60U;
    static constexpr guest::GuestAddress kHashString = 0x0021BFA0U;
    static constexpr guest::GuestAddress kCarUnlocked = 0x0013A660U;
    static constexpr guest::GuestAddress kClassUnlocked = 0x0013A6D0U;
    static constexpr guest::GuestAddress kUnlockText = 0x003EAEA0U;
    static constexpr guest::GuestAddress kUnlockTextShort = 0x003EAED0U;
    // The game's unlock data: per car id its kind, value and texts (string
    // ids), the World Tour events' keys, the unlock flags and the counters.
    static constexpr guest::GuestAddress kCarUnlockKinds = 0x004CD4D0U;
    static constexpr guest::GuestAddress kCarUnlockValues = 0x004CD5E0U;
    static constexpr guest::GuestAddress kCarUnlockTexts = 0x004CD6F0U;
    static constexpr guest::GuestAddress kCarUnlockTextsShort = 0x004CD800U;
    static constexpr std::uint32_t kUnlockKindEventGold = 2U;
    static constexpr guest::GuestAddress kEvents = 0x004CBAE0U;
    static constexpr std::uint32_t kEventCount = 73U;
    static constexpr guest::GuestAddress kUnlockFlags = 0x0051BFA0U;
    static constexpr guest::GuestAddress kProfilePointer = 0x0051BF9CU;
    static constexpr guest::GuestAddress kTakedownsGlobal = 0x0051C124U;
    static constexpr std::uint32_t kProfileTakedowns = 512U;
    static constexpr std::uint32_t kProfileEventMedals = 902U;
    // Host-owned region of low EE RAM (unused by the game) for the names.
    static constexpr guest::GuestAddress kRegion = 0x000D0000U;
    static constexpr std::uint32_t kRegionSize = 0x8000U;
    static constexpr std::uint32_t kRegionMagic = 0x43444441U; // "ADDC"
    // String ids of the added strings, far past the game's own: car k has
    // base + 4 k (name in capitals), + 1 (name as written), + 2 (requirement)
    // and + 3 (short requirement).
    static constexpr std::uint32_t kStringIdBase = 0xF000U;
    static constexpr std::uint32_t kStringsPerCar = 4U;
    static constexpr std::size_t kMaximumCars = 64U;

    struct Statistics {
        std::uint64_t registry_patches{0U};
        std::uint64_t hash_aliases{0U};
        std::uint64_t names_shown{0U};
        std::uint64_t locked_checks{0U};
        // Car physics values a mod's VDB.XML changed, read from the retail one.
        std::uint64_t retail_handling{0U};
    };

    Slus21050AddedCars();

    // The cars to add (validated; a car whose names cannot be packed, whose
    // class is unknown without native class metadata or that repeats another's name is left out and listed
    // in problems()). Takes effect at the next service().
    void set_cars(std::vector<AddedCar> cars);
    [[nodiscard]] const std::vector<AddedCar>& cars() const noexcept;
    [[nodiscard]] const std::vector<std::string>& problems() const noexcept;
    [[nodiscard]] bool unlocked(std::uint64_t key, const guest::EeMemory& memory) const;
    [[nodiscard]] std::string requirement(std::uint64_t key) const;

    // Session-only developer override. It never writes profile progress,
    // unlock flags, event medals, or persistent settings.
    void enable_developer_unlock() noexcept;
    [[nodiscard]] bool developer_unlock_enabled() const noexcept;

    [[nodiscard]] std::optional<OnlineCarSelection> resolve_online_selection(
        const guest::EeMemory&, std::uint64_t key, std::uint32_t paint) const;
    // Called only by the explicit mod-sync replay bridge, never per frame.
    void apply_online_selection(guest::EeMemory&, const OnlineCarSelection&) const;

    // The base game's value database. Car stats belong to the cars: a mod
    // that replaces game files (DATA/VDB.XML among them) cannot change a car's
    // physics values (".../VehiclePhysics/<car>.cfg"), which always come from
    // this copy; only an added car's handling multipliers change them, and
    // only for that car. Without it the loaded database is used as it is.
    void set_retail_values(std::optional<RetailValueDatabase> values);

    // Registers the native routines when their retail code matches.
    // False (nothing registered) otherwise.
    [[nodiscard]] bool register_tuning(NativeFunctionDispatcher&, const guest::EeMemory&);
    [[nodiscard]] bool register_functions(NativeFunctionDispatcher& dispatcher,
                                          const guest::EeMemory& memory);
    [[nodiscard]] bool registered() const noexcept;

    // Once per VBlank: keeps the registry and the name strings in step with
    // the configured cars (the game can reload its registry).
    void service(guest::EeMemory& memory);

    [[nodiscard]] Statistics statistics() const noexcept;

    struct State;

  private:
    std::shared_ptr<State> state_;
};

} // namespace takedown::runtime
