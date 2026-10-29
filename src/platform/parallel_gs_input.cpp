#include "takedown/platform/parallel_gs_input.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include "takedown/platform/windows_gs_pipe.hpp"
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace takedown::platform {

namespace {

// Magic words of the four input packet kinds, as they appear on the wire.
constexpr std::uint32_t kButtonPacketWord = 0x4E493342U; // "B3IN"

bool has_magic(const unsigned char* packet, const char a, const char b, const char c,
               const char d) noexcept {
    return packet[0] == static_cast<unsigned char>(a) && packet[1] == static_cast<unsigned char>(b) &&
           packet[2] == static_cast<unsigned char>(c) && packet[3] == static_cast<unsigned char>(d);
}

// Little-endian u32 at the start of a packet field.
std::uint32_t load_u32_le(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

// A latched host hotkey: pressed in this packet and not held in the previous one.
bool rising_edge(const std::uint32_t next, const std::uint32_t previous,
                 const ParallelGsInputButton button) noexcept {
    const std::uint32_t bit = parallel_gs_input_button_mask(button);
    return (next & bit) != 0U && (previous & bit) == 0U;
}

} // namespace

struct ParallelGsInputReader::Impl {
    explicit Impl(const std::filesystem::path& path) : fifo_path(path) {
        if (fifo_path.empty()) {
            throw std::invalid_argument("paraLLEl-GS input requires a non-empty FIFO path");
        }
#if defined(_WIN32)
        pipe = windows_gs_pipe::open_reader(fifo_path.c_str());
#else
        // RECOVERY: POSIX branch written to match the header; the recovered object is Windows-only.
        descriptor = ::open(fifo_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (descriptor < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "could not open paraLLEl-GS input FIFO");
        }
#endif
        pending_bytes.reserve(kParallelGsInputPacketBytes * 4U);
    }

    ~Impl() {
#if defined(_WIN32)
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
        }
#else
        if (descriptor >= 0) {
            ::close(descriptor);
        }
#endif
    }

    void poll();
    void apply_buttons(const std::uint32_t next_mask) noexcept {
        const auto rising = [&](const ParallelGsInputButton button) noexcept {
            return (next_mask & parallel_gs_input_button_mask(button)) != 0U &&
                   (button_mask & parallel_gs_input_button_mask(button)) == 0U;
        };
        capture_requested = capture_requested || rising(ParallelGsInputButton::Capture);
        frame_step_toggle_requested =
            frame_step_toggle_requested || rising(ParallelGsInputButton::FrameStepToggle);
        frame_advance_requested = frame_advance_requested || rising(ParallelGsInputButton::FrameAdvance);
        guest_work_measurement_start_requested =
            guest_work_measurement_start_requested ||
            rising(ParallelGsInputButton::GuestWorkMeasurementStart);
        close_requested = close_requested || rising(ParallelGsInputButton::Quit);

        controller = {};
        controller.dpad_up = (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::DpadUp)) != 0U;
        controller.dpad_right =
            (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::DpadRight)) != 0U;
        controller.dpad_down = (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::DpadDown)) != 0U;
        controller.dpad_left = (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::DpadLeft)) != 0U;
        controller.start = (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::Start)) != 0U;
        controller.select = (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::Select)) != 0U;
        controller.cross = (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::Cross)) != 0U;
        controller.circle = (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::Circle)) != 0U;
        controller.square = (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::Square)) != 0U;
        controller.triangle = (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::Triangle)) != 0U;
        controller.left_shoulder =
            (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::LeftShoulder)) != 0U;
        controller.right_shoulder =
            (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::RightShoulder)) != 0U;
        controller.left_trigger =
            (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::LeftTrigger)) != 0U;
        controller.right_trigger =
            (next_mask & parallel_gs_input_button_mask(ParallelGsInputButton::RightTrigger)) != 0U;
        button_mask = next_mask;
    }


    std::filesystem::path fifo_path;
    void* pipe = reinterpret_cast<void*>(-1);
    int descriptor = -1;
    std::vector<std::uint8_t> pending_bytes;
    std::uint32_t button_mask = 0U;
    HostControllerState controller{};
    KeyboardMouseSnapshot keyboard_mouse{};
    std::vector<TextInputEvent> text_input;
    PointerSnapshot pointer{};
    int wheel = 0;
    bool received_raw = false;
    bool capture_requested = false;
    bool frame_step_toggle_requested = false;
    bool frame_advance_requested = false;
    bool guest_work_measurement_start_requested = false;
    bool close_requested = false;
    std::uint64_t packets = 0U;
};

void ParallelGsInputReader::Impl::poll() {
    std::array<std::uint8_t, 512U> bytes{};
    while (true) {
#if defined(_WIN32)
        const int count = windows_gs_pipe::read_available(pipe, bytes.data(), bytes.size());
#else
        // RECOVERY: POSIX branch written to match the header; the recovered object is Windows-only.
        int count = 0;
        const auto got = ::read(descriptor, bytes.data(), bytes.size());
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            count = (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        } else {
            count = got == 0 ? -1 : static_cast<int>(got);
        }
#endif
        if (count < 0) {
            // Peer gone: release every held input and request shutdown.
            apply_buttons(0U);
            keyboard_mouse = {};
            pending_bytes.erase(pending_bytes.begin(), pending_bytes.end());
            close_requested = true;
            break;
        }
        if (count == 0) {
            break;
        }
        pending_bytes.insert(pending_bytes.end(), bytes.data(), bytes.data() + count);

        std::size_t offset = 0U;
        while (pending_bytes.size() - offset > 3U) {
            const unsigned char* const packet = pending_bytes.data() + offset;
            const bool raw = has_magic(packet, 'B', '3', 'R', 'I');
            std::uint32_t word = 0U;
            std::memcpy(&word, packet, sizeof word);
            const bool text = has_magic(packet, 'B', '3', 'T', 'X');
            const bool pointer_packet = has_magic(packet, 'B', '3', 'M', 'P');
            if ((!raw & (word != kButtonPacketWord)) && !(text | pointer_packet)) {
                // Not a packet we know: resynchronise one byte later.
                ++offset;
                continue;
            }
            const unsigned long long length = ((raw | text) != 0 || pointer_packet) ? 76U : 8U;
            if (pending_bytes.size() - offset < length) {
                break;
            }

            if (pointer_packet) {
                const PointerSnapshot next = decode_pointer(packet);
                if (std::isfinite(next.x) && std::isfinite(next.y)) {
                    wheel += next.wheel;
                    pointer = next;
                    pointer.wheel = 0;
                }
                ++packets;
                offset += length;
            } else if (text) {
                // Text events are bounded by kind, length and the reserved bytes.
                if (packet[4] <= 3U && packet[5] <= 68U && packet[6] == 0U && packet[7] == 0U &&
                    text_input.size() < 256U) {
                    text_input.push_back(TextInputEvent{
                        static_cast<TextInputKind>(packet[4]),
                        std::string(reinterpret_cast<const char*>(packet + 8), packet[5])});
                }
                ++packets;
                offset += length;
            } else {
                if (raw && (packet[73] != 0U || packet[74] != 0U || packet[75] != 0U)) {
                    ++offset;
                    continue;
                }
                apply_buttons(load_u32_le(packet + 4) & (raw ? 0xFFFF0000U : 0xFFFFFFFFU));
                if (raw) {
                    keyboard_mouse = decode_raw_input(packet);
                    received_raw = true;
                }
                ++packets;
                offset += length;
            }
        }
        if (offset != 0U) {
            pending_bytes.erase(pending_bytes.begin(),
                                pending_bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
}

ParallelGsInputReader::ParallelGsInputReader(const std::filesystem::path& fifo_path)
    : impl_(std::make_unique<Impl>(fifo_path)) {}

ParallelGsInputReader::~ParallelGsInputReader() = default;

void ParallelGsInputReader::poll() {
    impl_->poll();
}

HostControllerState ParallelGsInputReader::controller_state() const noexcept {
    return impl_->controller;
}

bool ParallelGsInputReader::consume_capture_request() noexcept {
    return std::exchange(impl_->capture_requested, false);
}

bool ParallelGsInputReader::consume_frame_step_toggle_request() noexcept {
    return std::exchange(impl_->frame_step_toggle_requested, false);
}

bool ParallelGsInputReader::consume_frame_advance_request() noexcept {
    return std::exchange(impl_->frame_advance_requested, false);
}

bool ParallelGsInputReader::consume_guest_work_measurement_start_request() noexcept {
    return std::exchange(impl_->guest_work_measurement_start_requested, false);
}

bool ParallelGsInputReader::consume_close_request() noexcept {
    return std::exchange(impl_->close_requested, false);
}

std::uint64_t ParallelGsInputReader::packet_count() const noexcept {
    return impl_->packets;
}

bool ParallelGsInputReader::has_raw_input() const noexcept {
    return impl_->received_raw;
}

PointerSnapshot ParallelGsInputReader::pointer() const noexcept {
    return impl_->pointer;
}

int ParallelGsInputReader::consume_wheel() noexcept {
    return std::exchange(impl_->wheel, 0);
}

KeyboardMouseSnapshot ParallelGsInputReader::raw_input() const noexcept {
    return impl_->keyboard_mouse;
}

std::vector<TextInputEvent> ParallelGsInputReader::consume_text_input() {
    return std::exchange(impl_->text_input, {});
}

} // namespace takedown::platform
