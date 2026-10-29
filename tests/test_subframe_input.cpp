#include "takedown/platform/input_bindings.hpp"
#include "takedown/platform/raw_input_protocol.hpp"
#include "takedown/runtime/subframe_input.hpp"
#include "takedown/ui/controls_session.hpp"
#include <SDL3/SDL.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace takedown;
using namespace platform;
void check(bool good, const char* message) {
    if (!good)
        throw std::runtime_error(message);
}
int main() try {
    auto bindings = default_input_bindings();
    RawInputState raw;
    raw.gamepads[0].connected = true;
    raw.gamepads[1].connected = true;
    raw.gamepads[0].axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 4096;
    raw.gamepads[0].axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 24576;
    raw.gamepads[1].axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 16384;
    auto p1 = map_player_input(bindings, 0, raw), p2 = map_player_input(bindings, 1, raw);
    check(p1.button_pressure[6] == 32 && !p1.cross,
          "light throttle remains analog below digital menu threshold");
    check(p1.button_pressure[8] == 191 && p1.square, "brake is independent analog pressure");
    check(p2.button_pressure[6] == 128 && p2.button_pressure[8] == 0,
          "player two has independent analog controls");
    raw.keys[SDL_SCANCODE_Z] = true;
    check(map_player_input(bindings, 0, raw).button_pressure[6] == 255,
          "digital source overrides light analog throttle");
    raw.keys.reset();
    bindings.players[0].controls[6] = {*parse_input_source("Mouse4")};
    bindings.players[0].controls[14] = {*parse_input_source("J")};
    raw.mouse_buttons = 8;
    raw.keys[SDL_SCANCODE_J] = true;
    p1 = map_player_input(bindings, 0, raw);
    check(p1.cross && p1.button_pressure[6] == 255 && p1.left_stick_x == -32768,
          "mouse button and keyboard steering remap together");
    bindings.players[0].gamepad = 4;
    raw.gamepads[3].connected = true;
    raw.gamepads[3].axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 32767;
    check(map_player_input(bindings, 0, raw).button_pressure[8] == 255,
          "player can select a stable fourth physical pad slot");
    bindings.players[0].controls[7] = {InputSource{InputSourceKind::Key, SDL_SCANCODE_COMMA, 1}};
    const auto path = std::filesystem::temp_directory_path() / "takedown-controls-unit.ini";
    std::error_code error;
    check(save_input_bindings(path, bindings, error), "controls save");
    check(load_input_bindings(path).bindings == bindings,
          "all alternate bindings survive file round-trip including comma key");
    {
        std::ofstream out(path);
        out << "[player1]\nthrottle = Mouse99\n";
    }
    const auto invalid = load_input_bindings(path);
    check(!invalid.diagnostics.empty() && invalid.bindings == default_input_bindings(),
          "invalid binding retains usable defaults");
    std::filesystem::remove(path);
    ui::ControlsSession session{path};
    RawInputState capture;
    session.activate(1, 1, capture); // keyboard/mouse editing
    capture.keys[SDL_SCANCODE_RETURN] = true;
    session.activate(row(PadControl::Cross), 0, capture); // throttle / confirm
    session.poll(capture);
    check(session.capturing(), "initiating confirm cannot bind itself");
    capture.keys.reset();
    session.poll(capture);
    capture.mouse_buttons = 16;
    check(session.poll(capture) && !session.capturing(), "mouse side button captured");
    check(session.blocks_game_input(), "new binding cannot immediately drive the underlying page");
    auto loaded = load_input_bindings(path).bindings;
    capture.mouse_buttons = 0;
    session.poll(capture);
    check(!session.blocks_game_input(), "input resumes when newly bound control is released");
    capture.mouse_buttons = 16;
    check(map_player_input(loaded, 0, capture).cross, "captured binding is saved and usable");
    capture.mouse_buttons = 0;
    capture.gamepads[0].connected = true;
    capture.gamepads[0].buttons[SDL_GAMEPAD_BUTTON_SOUTH] = true;
    check(map_player_input(loaded, 0, capture).cross,
          "keyboard remap preserves controller alternatives");
    // Edit through the same session used by the original Controllers page,
    // then verify both saved configuration and actual analog game input.
    capture.gamepads[0].buttons.reset();
    session.activate(1, 1, capture); // controller editing
    capture.gamepads[0].buttons[SDL_GAMEPAD_BUTTON_SOUTH] = true;
    session.activate(row(PadControl::Square), 0, capture); // brake
    session.poll(capture);
    check(session.capturing(), "held menu confirm must be released before controller capture");
    capture.gamepads[0].buttons.reset();
    session.poll(capture);
    capture.gamepads[0].axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 24000;
    check(session.poll(capture) && !session.capturing(), "analog trigger can be remapped in game");
    loaded = load_input_bindings(path).bindings;
    capture.gamepads[0].axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 8192;
    check(map_player_input(loaded, 0, capture).button_pressure[8] == 64,
          "saved trigger remap retains analog pressure instead of becoming a digital button");
    capture.gamepads[0].axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 0;
    session.poll(capture);
    const auto player_one = session.bindings().players[0];
    session.activate(0, 1, capture); // edit player two independently
    capture.gamepads[1].connected = true;
    session.activate(row(PadControl::Cross), 0, capture); // throttle
    session.poll(capture);
    capture.gamepads[1].axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 21000;
    check(session.poll(capture), "player two can independently remap a trigger");
    loaded = load_input_bindings(path).bindings;
    check(loaded.players[0] == player_one &&
              map_player_input(loaded, 1, capture).button_pressure[6] > 0 &&
              map_player_input(loaded, 0, capture).button_pressure[6] == 0,
          "player two remapping preserves player one and routes only the selected physical pad");
    capture.gamepads[1].axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 0;
    session.poll(capture);
    session.activate(row(PadControl::Cross), 0, capture);
    session.poll(capture);
    capture.keys[SDL_SCANCODE_ESCAPE] = true;
    session.poll(capture);
    check(!session.capturing() && session.bindings() == loaded,
          "cancel leaves both players' saved mappings unchanged");
    capture.keys.reset();
    session.poll(capture);
    session.activate(row(PadControl::Circle), 0, capture); // player two handbrake
    session.poll(capture);
    capture.gamepads[1].buttons[SDL_GAMEPAD_BUTTON_MISC6] = true;
    check(session.poll(capture), "an additional controller button can be captured");
    const auto additional_button = load_input_bindings(path);
    check(additional_button.diagnostics.empty() &&
              additional_button.bindings == session.bindings() &&
              map_player_input(additional_button.bindings, 1, capture).circle,
          "additional SDL controller buttons survive save/load and remain usable");
    KeyboardMouseSnapshot snapshot;
    snapshot.keys[SDL_SCANCODE_J] = true;
    snapshot.keys[511] = true;
    snapshot.mouse_buttons = 128;
    const auto packet = encode_raw_input(snapshot, 1U << 18);
    const auto decoded = decode_raw_input(packet.data());
    check(decoded.keys == snapshot.keys && decoded.mouse_buttons == 128,
          "raw relay retains all keyboard scancodes and mouse buttons");
    runtime::SubframeInputBridge bridge;
    p1 = {};
    p1.button_pressure[6] = 32;
    bridge.observe(p1, 0);
    p1.button_pressure[6] = 64;
    check(bridge.observe(p1, 1) == 1, "analog-only changes reach the subframe input bridge");
    std::filesystem::remove(path);
    std::cout << "native input binding tests passed\n";
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
