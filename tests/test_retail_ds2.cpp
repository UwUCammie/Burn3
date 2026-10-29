#include "takedown/guest/retail_ds2.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/slus_21050_controllers.hpp"
#include "takedown/runtime/frontend_input_replay.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_neutral_sample() {
    const auto sample = takedown::guest::make_neutral_retail_ds2_sample();
    expect(sample[0] == 0xffU && sample[1] == 0xffU, "neutral digital buttons are all released");
    expect(std::equal(sample.begin() + 2, sample.begin() + 6,
                      std::array<std::uint8_t, 4U>{0x80U, 0x7fU, 0x80U, 0x7fU}.begin()),
           "neutral analogue axes occupy decoded bytes 2 through 5");
    expect(std::all_of(sample.begin() + 6, sample.begin() + 18,
                       [](const std::uint8_t value) { return value == 0U; }),
           "neutral pressure channels are zero");
    expect(std::all_of(sample.begin() + 18, sample.begin() + 22,
                       [](const std::uint8_t value) { return value == 0x80U; }),
           "extension axes remain centered independently of the main sticks");
    expect(std::all_of(sample.begin() + 22, sample.end(),
                       [](const std::uint8_t value) { return value == 0U; }),
           "unavailable tail bytes remain clear");
}

void test_down_press_hold_release() {
    const auto neutral = takedown::guest::make_neutral_retail_ds2_sample();
    auto sample = neutral;

    takedown::guest::set_retail_ds2_button(sample, takedown::guest::RetailDs2Button::DpadDown,
                                           true);
    expect(sample[0] == 0xbfU, "Down clears active-low digital bit 6");
    expect(sample[9] == 0xffU, "Down writes retail pressure channel 23");
    expect(std::equal(sample.begin() + 2, sample.begin() + 6, neutral.begin() + 2),
           "pressing Down does not overwrite an analogue axis");
    expect((sample[0] & 0x10U) != 0U && sample[8] == 0U, "pressing Down leaves Up fully released");

    const auto held = sample;
    takedown::guest::set_retail_ds2_button(sample, takedown::guest::RetailDs2Button::DpadDown,
                                           true);
    expect(sample == held, "holding Down produces a stable decoded sample");

    takedown::guest::set_retail_ds2_button(sample, takedown::guest::RetailDs2Button::DpadDown,
                                           false);
    expect(sample == neutral, "releasing Down returns exactly to neutral");
}

void test_cross_and_circle() {
    const auto neutral = takedown::guest::make_neutral_retail_ds2_sample();

    auto cross = neutral;
    takedown::guest::set_retail_ds2_button(cross, takedown::guest::RetailDs2Button::Cross, true);
    expect(cross[1] == 0xbfU, "Cross clears active-low digital bit 14");
    expect(cross[12] == 0xffU, "Cross writes retail pressure channel 26");
    takedown::guest::set_retail_ds2_button(cross, takedown::guest::RetailDs2Button::Cross, false);
    expect(cross == neutral, "releasing Cross restores the neutral sample");

    auto circle = neutral;
    takedown::guest::set_retail_ds2_button(circle, takedown::guest::RetailDs2Button::Circle, true);
    expect(circle[1] == 0xdfU, "Circle clears active-low digital bit 13");
    expect(circle[11] == 0xffU, "Circle writes retail pressure channel 25");
    takedown::guest::set_retail_ds2_button(circle, takedown::guest::RetailDs2Button::Circle, false);
    expect(circle == neutral, "releasing Circle restores the neutral sample");
}

void test_left_stick_mapping() {
    const auto neutral = takedown::guest::make_neutral_retail_ds2_sample();
    auto sample = neutral;

    takedown::guest::set_retail_ds2_left_stick(sample, 0, 0);
    expect(sample == neutral, "a centered host stick preserves the exact retail neutral sample");

    takedown::guest::set_retail_ds2_left_stick(sample, -32'768, -32'768);
    expect(sample[4] == 0U && sample[5] == 0U,
           "full host left/up reaches the minimum retail LX/LY values");
    expect(std::equal(sample.begin(), sample.begin() + 4, neutral.begin()) &&
               std::equal(sample.begin() + 6, sample.end(), neutral.begin() + 6),
           "left-stick input does not alter buttons, the right stick, or pressure channels");

    takedown::guest::set_retail_ds2_left_stick(sample, 32'767, 32'767);
    expect(sample[4] == 0xffU && sample[5] == 0xffU,
           "full host right/down reaches the maximum retail LX/LY values");
}

void test_frontend_input_replay_schedule() {
    using takedown::runtime::FrontendInputReplaySchedule;
    using takedown::runtime::FrontendReplayInput;

    constexpr FrontendInputReplaySchedule schedule{
        .down_pulse_update = 30U,
        .up_pulse_update = 170U,
        .up_pulse_updates = 1U,
        .initial_cross_pulse_update = 70U,
        .repeated_cross_first_update = 110U,
        .repeated_cross_period = 30U,
    };
    expect(schedule.input_for_update(29U) == FrontendReplayInput::Neutral &&
               schedule.input_for_update(30U) == FrontendReplayInput::DpadDown,
           "frontend replay waits for profile UI readiness before one Down pulse");
    expect(schedule.input_for_update(31U) == FrontendReplayInput::Neutral &&
               schedule.input_for_update(69U) == FrontendReplayInput::Neutral,
           "frontend replay releases Down before confirming the selection");
    expect(schedule.input_for_update(70U) == FrontendReplayInput::Cross &&
               schedule.input_for_update(71U) == FrontendReplayInput::Neutral,
           "frontend replay emits and releases its initial Cross pulse");
    expect(schedule.input_for_update(109U) == FrontendReplayInput::Neutral &&
               schedule.input_for_update(110U) == FrontendReplayInput::Cross &&
               schedule.input_for_update(111U) == FrontendReplayInput::Neutral &&
               schedule.input_for_update(140U) == FrontendReplayInput::Cross &&
               schedule.input_for_update(170U) == FrontendReplayInput::DpadUp &&
               schedule.input_for_update(171U) == FrontendReplayInput::Neutral &&
               schedule.input_for_update(200U) == FrontendReplayInput::Cross,
           "frontend replay overrides one periodic Cross with Up for the no-card prompt");

    constexpr FrontendInputReplaySchedule no_repeat{
        .down_pulse_update = 2U,
        .initial_cross_pulse_update = 4U,
        .repeated_cross_first_update = 8U,
        .repeated_cross_period = 0U,
    };
    expect(no_repeat.input_for_update(8U) == FrontendReplayInput::Neutral,
           "a zero replay period disables repeated pulses without division by zero");

    constexpr FrontendInputReplaySchedule held_cross{
        .down_pulse_update = 2U,
        .initial_cross_pulse_update = 4U,
        .repeated_cross_first_update = 8U,
        .repeated_cross_period = 10U,
        .cross_pulse_updates = 2U,
    };
    expect(held_cross.input_for_update(4U) == FrontendReplayInput::Cross &&
               held_cross.input_for_update(5U) == FrontendReplayInput::Cross &&
               held_cross.input_for_update(6U) == FrontendReplayInput::Neutral &&
               held_cross.input_for_update(8U) == FrontendReplayInput::Cross &&
               held_cross.input_for_update(9U) == FrontendReplayInput::Cross &&
               held_cross.input_for_update(10U) == FrontendReplayInput::Neutral,
           "frontend replay can hold Cross across one retail update boundary");
}

void test_frontend_gate_pulse_handshake() {
    using Handshake = takedown::runtime::FrontendGatePulseHandshake;

    Handshake handshake;
    expect(handshake.phase() == Handshake::Phase::WaitingForNeutralPoll &&
               !handshake.pulse_ready(),
           "gate pulse waits for a registered neutral poll");
    expect(!handshake.observe_gate_poll(false, false, false) && !handshake.pulse_ready(),
           "a neutral poll before registration cannot arm the gate pulse");
    expect(!handshake.observe_gate_poll(true, true, false) && !handshake.pulse_ready(),
           "a live action edge cannot arm another gate pulse");
    expect(handshake.observe_gate_poll(true, false, false) && handshake.pulse_ready(),
           "the first registered neutral poll arms the next controller update");
    expect(!handshake.observe_gate_poll(true, false, false) && handshake.pulse_ready(),
           "extra neutral polls leave the current gate attempt armed");
    expect(handshake.begin_pulse() && !handshake.pulse_ready() &&
               handshake.phase() == Handshake::Phase::PulseInFlight,
           "the armed controller update begins exactly one pulse");
    expect(!handshake.begin_pulse(), "an in-flight gate pulse cannot begin twice");
    expect(handshake.finish_pulse_update() &&
               handshake.phase() == Handshake::Phase::WaitingForAcceptance,
           "decoder completion advances the gate pulse to raw release");
    expect(!handshake.finish_pulse_update(), "the raw gate release occurs only once");
    expect(!handshake.observe_gate_poll(true, true, false) &&
               handshake.phase() == Handshake::Phase::WaitingForAcceptance &&
               !handshake.pulse_ready(),
           "an unsuccessful poll with a live edge cannot re-arm the pulse");
    expect(handshake.observe_gate_poll(true, false, false) && handshake.pulse_ready(),
           "an unsuccessful neutral poll re-arms a missed gate pulse");
    expect(handshake.begin_pulse() && handshake.finish_pulse_update(),
           "a re-armed gate handshake emits and releases a second pulse");
    expect(!handshake.observe_gate_poll(true, true, true) &&
               handshake.phase() == Handshake::Phase::Complete && !handshake.pulse_ready(),
           "the retail gate's accepted poll completes the handshake");

    Handshake frontend_first;
    expect(frontend_first.observe_gate_poll(true, false, false) &&
               frontend_first.begin_pulse() && frontend_first.finish_pulse_update(),
           "a frontend poll followed by a controller update produces one complete pulse");

    Handshake controller_first;
    expect(!controller_first.pulse_ready() &&
               controller_first.observe_gate_poll(true, false, false) &&
               controller_first.begin_pulse() && controller_first.finish_pulse_update(),
           "an early controller update waits for the same neutral-poll handshake");
}

void test_independent_second_controller() {
    using namespace takedown;
    guest::EeMemory memory{guest::kEeMainRamSize};
    runtime::Slus21050Controllers controllers;
    memory.write_u32_le(controllers.decode_boundary, 0x00111880);
    memory.write_u32_le(0x4f542c, 0x4ddff0);
    auto input = guest::make_neutral_retail_ds2_sample();
    guest::set_retail_ds2_button(input, guest::RetailDs2Button::Start, true);
    guest::set_retail_ds2_left_stick(input, -32768, 32767);
    expect(controllers.publish_second(memory, true, input), "the native second physical controller registers");
    expect(memory.read_u32_le(0x4f52c4) == 0x4f542c && memory.read_u32_le(0x4f52d4) == 4 &&
        memory.read_u32_le(0x4f531c) == 1, "player two has its own device, receiving state and capability handle");
    expect(memory.read_u8(0x4f52f8) == input[0] && memory.read_u8(0x4f52fc) == input[4] &&
        memory.read_u8(0x4f52d8) == 0, "player two's Start and analogue sample never overwrite player one");
    expect(controllers.publish_second(memory, false, input) && memory.read_u32_le(0x4f52d4) == 0 &&
        memory.read_u8(0x4f52f8) == 255, "disconnect neutralizes input and exposes the original disconnect path");
    expect(controllers.publish_second(memory, true, input) && memory.read_u32_le(0x4f52d4) == 4,
        "reconnecting restores the same physical player slot");
}

} // namespace

int main() {
    test_independent_second_controller();
    test_neutral_sample();
    test_down_press_hold_release();
    test_cross_and_circle();
    test_left_stick_mapping();
    test_frontend_input_replay_schedule();
    test_frontend_gate_pulse_handshake();

    if (failures != 0) {
        std::cerr << failures << " retail DS2 test(s) failed\n";
        return 1;
    }
    std::cout << "All retail DS2 tests passed\n";
    return 0;
}
