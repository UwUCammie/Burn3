#include "takedown/ui/car_select.hpp"

#include <algorithm>
#include <utility>

namespace takedown::ui {

void CarSelect::set_catalog(std::vector<CarClass> classes, std::vector<SelectableCar> cars) {
    // Keep the player's car and class across a catalogue reload.
    const std::string selected_id = selected() != nullptr ? selected()->id : std::string{};
    const std::string group = visible_classes_.empty() ? std::string{} : visible_classes_[class_index_].id;
    catalog_classes_ = std::move(classes);
    catalog_cars_ = std::move(cars);
    std::stable_sort(catalog_classes_.begin(), catalog_classes_.end(),
                     [](const auto& a, const auto& b) { return a.order < b.order; });
    rebuild(selected_id, group);
}

void CarSelect::set_context(CarSelectMode mode, std::string handling_class) {
    const std::string id = selected() != nullptr ? selected()->id : std::string{};
    mode_ = mode;
    handling_class_ = std::move(handling_class);
    rebuild(id, {});
}

void CarSelect::rebuild(const std::string& selected_id, const std::string& class_id) {
    visible_classes_.clear();
    for (const auto& group : catalog_classes_) {
        if ((group.modes & static_cast<std::uint8_t>(mode_)) == 0 ||
            std::any_of(visible_classes_.begin(), visible_classes_.end(),
                        [&](const CarClass& listed) { return listed.id == group.id; })) {
            continue;
        }
        // A class with no car for this mode and handling class is not listed.
        if (std::any_of(catalog_cars_.begin(), catalog_cars_.end(), [&](const SelectableCar& car) {
                return car.class_id == group.id && (handling_class_.empty() || car.handling_class == handling_class_);
            })) {
            visible_classes_.push_back(group);
        }
    }
    class_index_ = 0;
    for (std::size_t i = 0; i < visible_classes_.size(); ++i) {
        const auto& id = visible_classes_[i].id;
        if (id == class_id) {
            class_index_ = i;
        }
        for (const auto& car : catalog_cars_) {
            if ((car.id == selected_id || car.id == original_of(selected_id)) && car.class_id == id &&
                (handling_class_.empty() || car.handling_class == handling_class_)) {
                class_index_ = i;
            }
        }
    }
    list_cars(selected_id);
}

std::string CarSelect::original_of(const std::string& id) const {
    for (const auto& car : catalog_cars_) {
        if (car.id == id && !car.skin_of.empty()) {
            return car.skin_of;
        }
    }
    return id;
}

void CarSelect::list_cars(const std::string& selected_id) {
    visible_cars_.clear();
    car_index_ = 0;
    const std::string original = original_of(selected_id);
    if (!visible_classes_.empty()) {
        for (const auto& car : catalog_cars_) {
            if (!car.skin_of.empty() || car.class_id != visible_classes_[class_index_].id ||
                (!handling_class_.empty() && car.handling_class != handling_class_)) {
                continue;
            }
            if (std::any_of(visible_cars_.begin(), visible_cars_.end(),
                            [&](const SelectableCar& listed) { return listed.id == car.id; })) {
                continue;
            }
            if (car.id == selected_id || car.id == original) {
                car_index_ = visible_cars_.size();
            }
            visible_cars_.push_back(car);
            if (car.id == original) {
                for (const auto& skin : catalog_cars_) {
                    if (skin.id == selected_id) {
                        visible_cars_.back() = skin;
                    }
                }
            }
        }
    }
    const SelectableCar* current = selected();
    if (current == nullptr || current->id != selected_id) {
        paint_ = 0;
    }
    paint_ = current != nullptr ? paint_ % std::max(current->paints, 1U) : 0U;
}

const SelectableCar* CarSelect::selected() const {
    return car_index_ < visible_cars_.size() ? &visible_cars_[car_index_] : nullptr;
}

bool CarSelect::move_class(int direction) {
    if (visible_classes_.empty() || direction == 0 || (direction < 0 && class_index_ == 0) ||
        (direction > 0 && class_index_ + 1 >= visible_classes_.size())) {
        return false;
    }
    class_index_ = direction < 0 ? class_index_ - 1 : class_index_ + 1;
    paint_ = 0;
    list_cars({});
    return true;
}

bool CarSelect::move_car(int direction) {
    if (visible_cars_.empty() || direction == 0 || (direction < 0 && car_index_ == 0) ||
        (direction > 0 && car_index_ + 1 >= visible_cars_.size())) {
        return false;
    }
    car_index_ = direction < 0 ? car_index_ - 1 : car_index_ + 1;
    paint_ = 0;
    return true;
}

void CarSelect::next_paint() {
    const SelectableCar* car = selected();
    if (car != nullptr) {
        paint_ = (paint_ + 1) % std::max(car->paints, 1U);
    }
}

std::vector<const SelectableCar*> CarSelect::skins() const {
    std::vector<const SelectableCar*> out;
    const SelectableCar* car = selected();
    if (car == nullptr) {
        return out;
    }
    const std::string original = car->skin_of.empty() ? car->id : car->skin_of;
    for (const auto& candidate : catalog_cars_) {
        if (candidate.id == original) {
            out.insert(out.begin(), &candidate);
        } else if (candidate.skin_of == original) {
            out.push_back(&candidate);
        }
    }
    return out;
}

bool CarSelect::next_skin() {
    const auto choices = skins();
    if (choices.size() <= 1 || car_index_ >= visible_cars_.size()) {
        return false;
    }
    std::size_t at = 0;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        if (choices[i]->id == visible_cars_[car_index_].id) {
            at = i;
        }
    }
    visible_cars_[car_index_] = *choices[(at + 1) % choices.size()];
    paint_ = 0;
    return true;
}

bool CarSelect::select(const std::string& id) {
    const std::string original = original_of(id);
    for (std::size_t i = 0; i < visible_classes_.size(); ++i) {
        for (const auto& car : catalog_cars_) {
            if (car.id == original && car.class_id == visible_classes_[i].id &&
                (handling_class_.empty() || car.handling_class == handling_class_)) {
                class_index_ = i;
                paint_ = 0;
                list_cars(id);
                return true;
            }
        }
    }
    return false;
}

} // namespace takedown::ui
