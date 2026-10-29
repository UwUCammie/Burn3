#pragma once

// Burnout's native vehicle selector. IDs, availability and content resources
// belong to the PC game; console class numbers and UI row layouts do not.
#include <cstdint>
#include <string>
#include <vector>

namespace takedown::ui {
enum class CarSelectMode : std::uint8_t { Race = 1, Crash = 2 };
struct CarClass {
    std::string id, name;
    int order{};
    std::uint8_t modes{1};
    bool operator==(const CarClass&) const = default;
};
struct SelectableCar {
    std::string id, name, class_id, thumbnail, requirement;
    std::string handling_class;
    // A skin is not listed itself: it takes its original's place in the list
    // when chosen with next_skin().
    std::string skin_of;
    std::uint64_t vehicle_key{};
    bool unlocked{};
    std::uint32_t paints{1};
    bool operator==(const SelectableCar&) const = default;
};
class CarSelect {
public:
    void set_catalog(std::vector<CarClass> classes, std::vector<SelectableCar> cars);
    void set_context(CarSelectMode mode, std::string handling_class = {});
    bool move_class(int direction);
    bool move_car(int direction);
    void next_paint();
    // The selected car's next skin (the original, then each skin in catalogue
    // order). False when the car has none.
    bool next_skin();
    // The selected car's skins after its original, the original first.
    [[nodiscard]] std::vector<const SelectableCar*> skins() const;
    bool select(const std::string& id);
    [[nodiscard]] const std::vector<CarClass>& classes() const { return visible_classes_; }
    [[nodiscard]] const std::vector<SelectableCar>& cars() const { return visible_cars_; }
    [[nodiscard]] const SelectableCar* selected() const;
    [[nodiscard]] std::size_t class_index() const { return class_index_; }
    [[nodiscard]] std::size_t car_index() const { return car_index_; }
    [[nodiscard]] std::uint32_t paint() const { return paint_; }
private:
    void rebuild(const std::string& selected_id, const std::string& class_id);
    void list_cars(const std::string& selected_id);
    [[nodiscard]] std::string original_of(const std::string& id) const;
    std::vector<CarClass> catalog_classes_, visible_classes_;
    std::vector<SelectableCar> catalog_cars_, visible_cars_;
    CarSelectMode mode_{CarSelectMode::Race};
    std::string handling_class_;
    std::size_t class_index_{}, car_index_{};
    std::uint32_t paint_{};
};
} // namespace takedown::ui
