// Unit tests for mouse control of the game's menus
// (slus_21050_menu_mouse.hpp): reading the rows from a synthetic UI draw
// bucket layout shaped like the game's main menu, and the presses the pointer,
// buttons and wheel turn into.

#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/slus_21050_menu_mouse.hpp"

#include <bit>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using takedown::guest::EeMemory;
using takedown::guest::GuestAddress;
using takedown::platform::HostControllerState;
using takedown::runtime::MenuMouseInput;
using takedown::runtime::MenuMouseList;
using takedown::runtime::MenuMouseRow;
using takedown::runtime::MenuMouseScreen;
using Mouse = takedown::runtime::Slus21050MenuMouse;

int failures = 0;

void expect(const bool condition, const std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

constexpr GuestAddress kBuckets = 0x00700000U;
constexpr GuestAddress kNodes = 0x00710000U;
constexpr GuestAddress kParent = 0x00720000U;
constexpr float kScaleY = 0.9333333F;

void write_float(EeMemory& memory, GuestAddress address, float value) {
    memory.write_u32_le(address, std::bit_cast<std::uint32_t>(value));
}

// The main menu: five rows at x 84, the first highlighted (taller), one
// highlight bar under the same parent and a footer icon.
void install_main_menu(EeMemory& memory) {
    memory.write_u32_le(Mouse::kUiContext + 44U, kBuckets);
    memory.write_u32_le(Mouse::kUiContext + 48U, 3U);
    write_float(memory, kParent + 0x20U, 0.0F);
    write_float(memory, kParent + 0x24U, 0.0F);
    write_float(memory, kParent + 0x28U, 1.0F);
    write_float(memory, kParent + 0x2CU, kScaleY);
    struct Node { float x, y, w, h; GuestAddress callback; std::uint32_t bucket; };
    const std::vector<Node> nodes{
        {84, 193, 140, 30, Mouse::kRowTextCallback, 1}, {84, 156, 334, 40, Mouse::kRowTextCallback, 1},
        {84, 257, 69, 30, Mouse::kRowTextCallback, 1},  {84, 225, 139, 30, Mouse::kRowTextCallback, 1},
        {84, 289, 161, 30, Mouse::kRowTextCallback, 1}, {0, 169, 640, 14, 0x003720F0U, 0},
        {584, 437, 21, 21, Mouse::kFooterIconCallback, 2}};
    std::vector<GuestAddress> heads(3U, 0U);
    for (std::size_t index = 0U; index < nodes.size(); ++index) {
        const GuestAddress node = kNodes + 0x80U * static_cast<GuestAddress>(index);
        const auto& n = nodes[index];
        memory.write_u32_le(node, heads[n.bucket]); // link in front of the bucket's list
        heads[n.bucket] = node;
        write_float(memory, node + 8U, n.x);
        write_float(memory, node + 12U, n.y);
        write_float(memory, node + 16U, n.w);
        write_float(memory, node + 20U, n.h);
        memory.write_u32_le(node + 0x40U, kParent);
        memory.write_u32_le(node + 0x44U, n.callback);
    }
    for (std::uint32_t bucket = 0U; bucket < 3U; ++bucket)
        memory.write_u32_le(kBuckets + 16U * bucket + 8U, heads[bucket]);
}

// The pointer over the middle of a row of the main menu.
MenuMouseInput over(const MenuMouseRow& row, std::uint8_t buttons = 0U) {
    return {(row.x + 20.0F) / Mouse::kPictureWidth, (row.y + row.height * 0.5F) / Mouse::kPictureHeight, true,
            buttons, 0};
}

bool none(const HostControllerState& s) {
    return !s.dpad_up && !s.dpad_down && !s.dpad_left && !s.dpad_right && !s.cross && !s.circle && !s.square &&
           !s.triangle && !s.start && !s.select;
}

void test_read_screen() {
    EeMemory memory;
    install_main_menu(memory);
    const MenuMouseScreen screen = takedown::runtime::read_menu_mouse_screen(memory);
    expect(screen.lists.size() == 1U && screen.footer, "one list and the footer");
    if (screen.lists.size() != 1U) return;
    const auto& list = screen.lists.front();
    expect(list.rows.size() == 5U && !list.horizontal && !list.values, "five vertical rows, no value boxes");
    bool ordered = true;
    for (std::size_t index = 1U; index < list.rows.size(); ++index) ordered = ordered && list.rows[index - 1U].y < list.rows[index].y;
    expect(ordered, "rows in screen order");
    expect(list.highlighted == 0U, "the taller first row is the highlighted one");
    expect(list.rows[0].y > 145.0F && list.rows[0].y < 146.0F, "rows are placed by their parent transform");
    EeMemory empty;
    expect(takedown::runtime::read_menu_mouse_screen(empty).lists.empty(), "no UI context, no lists");
}

void test_hover_moves_highlight() {
    EeMemory memory;
    install_main_menu(memory);
    MenuMouseScreen screen = takedown::runtime::read_menu_mouse_screen(memory);
    Mouse mouse;
    const auto input = over(screen.lists[0].rows[3]);
    // The highlight walks to the hovered row one press every other poll.
    std::vector<HostControllerState> presses;
    for (int poll = 0; poll < 12; ++poll) {
        const auto state = mouse.poll(screen, input, false);
        presses.push_back(state);
        if (state.dpad_down) screen.lists[0].highlighted = *screen.lists[0].highlighted + 1U;
    }
    int downs = 0;
    for (std::size_t poll = 0; poll < presses.size(); ++poll) {
        downs += presses[poll].dpad_down ? 1 : 0;
        if (poll > 0U && presses[poll].dpad_down) expect(none(presses[poll - 1U]), "every press follows a release");
    }
    expect(downs == 3 && screen.lists[0].highlighted == 3U, "hovering row 4 presses down three times");
    expect(!presses.back().cross, "hovering never confirms");
}

void test_click_confirms_and_back() {
    EeMemory memory;
    install_main_menu(memory);
    MenuMouseScreen screen = takedown::runtime::read_menu_mouse_screen(memory);
    Mouse mouse;
    const auto row = screen.lists[0].rows[0];
    (void)mouse.poll(screen, over(row), false);
    const auto click = mouse.poll(screen, over(row, 1U), false);
    expect(click.cross && !click.dpad_down, "a click on the highlighted row confirms it");
    (void)mouse.poll(screen, over(row, 0U), false); // release poll
    const auto back = mouse.poll(screen, over(row, 4U), false);
    expect(back.triangle, "a right click is Back");
    (void)mouse.poll(screen, over(row, 0U), false);
    // Clicking another row walks there first, then confirms.
    const auto other = screen.lists[0].rows[2];
    std::vector<HostControllerState> presses;
    for (int poll = 0; poll < 8; ++poll) {
        const auto state = mouse.poll(screen, over(other, poll == 0 ? 1U : 0U), false);
        presses.push_back(state);
        if (state.dpad_down) screen.lists[0].highlighted = *screen.lists[0].highlighted + 1U;
    }
    int downs = 0, crosses = 0;
    for (const auto& state : presses) { downs += state.dpad_down; crosses += state.cross; }
    expect(downs == 2 && crosses == 1 && presses.back().cross == false, "a click on row 3 moves twice, then confirms once");
}

void test_wheel_footer_title() {
    EeMemory memory;
    install_main_menu(memory);
    MenuMouseScreen screen = takedown::runtime::read_menu_mouse_screen(memory);
    Mouse mouse;
    MenuMouseInput wheel{0.9F, 0.1F, true, 0U, -2};
    const auto first = mouse.poll(screen, wheel, false);
    wheel.wheel = 0;
    const auto gap = mouse.poll(screen, wheel, false);
    const auto second = mouse.poll(screen, wheel, false);
    expect(first.dpad_down && none(gap) && second.dpad_down, "two wheel steps towards the player are two downs");
    (void)mouse.poll(screen, wheel, false);
    const MenuMouseInput footer_back{100.0F / 640.0F, 420.0F / 448.0F, true, 1U, 0};
    expect(mouse.poll(screen, footer_back, false).triangle, "the footer's left prompt is Back");
    (void)mouse.poll(screen, {0.5F, 0.5F, true, 0U, 0}, false);
    const MenuMouseInput footer_select{600.0F / 640.0F, 420.0F / 448.0F, true, 1U, 0};
    expect(mouse.poll(screen, footer_select, false).cross, "the footer's right prompt confirms");

    Mouse title;
    const MenuMouseScreen nothing;
    const MenuMouseInput click{0.5F, 0.5F, true, 1U, 0};
    expect(title.poll(nothing, click, true).start, "a click on the title screen presses Start");
    Mouse title_text;
    expect(title_text.poll(screen, click, true).start,
           "a click on the title screen presses Start even where its text is drawn like a row");
    Mouse race;
    expect(none(race.poll(nothing, click, false)), "with no menu on screen a click does nothing");
    Mouse race_right;
    expect(none(race_right.poll(nothing, {0.5F, 0.5F, true, 4U, 3}, false)), "nor does a right click or the wheel");
    Mouse outside;
    expect(none(outside.poll(screen, {0.5F, 0.5F, false, 1U, 0}, false)), "a click outside the window does nothing");
}

void test_value_rows() {
    MenuMouseScreen screen;
    MenuMouseList list;
    for (int row = 0; row < 4; ++row)
        list.rows.push_back({68.0F, 147.0F + 30.0F * static_cast<float>(row), 150.0F, row == 0 ? 33.0F : 26.0F, 1U});
    list.highlighted = 0U;
    list.values = true;
    list.value_x = 321.0F;
    screen.lists.push_back(list);
    Mouse mouse;
    // A click on the second row's label only selects it.
    const MenuMouseInput label{100.0F / 640.0F, (177.0F + 13.0F) / 448.0F, true, 1U, 0};
    const auto select = mouse.poll(screen, label, false);
    expect(select.dpad_down && !select.cross, "a label click moves to the row");
    screen.lists[0].highlighted = 1U;
    (void)mouse.poll(screen, {label.x, label.y, true, 0U, 0}, false);
    expect(none(mouse.poll(screen, {label.x, label.y, true, 0U, 0}, false)), "and does not confirm the page");
    // A click on its value box steps the value.
    const MenuMouseInput value{400.0F / 640.0F, label.y, true, 1U, 0};
    const auto step = mouse.poll(screen, value, false);
    expect(step.dpad_right && !step.cross, "a value box click steps the value");
}

void test_script() {
    using takedown::runtime::MenuMouseScript;
    auto script = MenuMouseScript::parse("10:0.5,0.25,0;20:0.5,0.25,1,-1");
    expect(script.has_value(), "a script parses");
    if (!script) return;
    expect(!script->at(5).inside, "before its first entry the pointer is away");
    const auto first = script->at(12);
    expect(first.inside && first.x == 0.5F && first.y == 0.25F && first.buttons == 0U, "an entry holds from its poll");
    expect(script->at(20).wheel == -1 && script->at(21).wheel == 0 && script->at(21).buttons == 1U,
           "the wheel is given once, the buttons are held");
    expect(!MenuMouseScript::parse("20:0,0,0;10:0,0,0") && !MenuMouseScript::parse("x") && !MenuMouseScript::parse(""),
           "out of order or malformed scripts are refused");
}

} // namespace

int main() {
    test_script();
    test_read_screen();
    test_hover_moves_highlight();
    test_click_confirms_and_back();
    test_wheel_footer_title();
    test_value_rows();
    if (failures != 0) {
        std::cerr << failures << " menu mouse test(s) failed\n";
        return 1;
    }
    std::cout << "menu mouse tests passed\n";
    return 0;
}
