#include "takedown/runtime/native_call_capture.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"

#include <zstd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace takedown::runtime {
namespace {

constexpr char kMagic[8] = {'B', '3', 'C', 'A', 'L', 'L', '0', '1'};

struct CaptureCounter {
    std::filesystem::path directory;
    std::uint32_t every{};
    std::uint32_t limit{};
    std::uint64_t calls{};
    std::uint32_t written{};
    std::vector<guest::GuestAddress> skipped_callers;
};

std::vector<guest::GuestAddress> parse_addresses(const std::string_view text) {
    std::vector<guest::GuestAddress> addresses;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t comma = std::min(text.find(',', start), text.size());
        const std::string item(text.substr(start, comma - start));
        if (!item.empty()) {
            addresses.push_back(static_cast<guest::GuestAddress>(std::stoul(item, nullptr, 0)));
        }
        start = comma + 1;
    }
    return addresses;
}

std::uint32_t environment_number(const char* name, const std::uint32_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return static_cast<std::uint32_t>(std::stoul(value, nullptr, 0));
}

std::vector<std::byte> read_range(const guest::EeMemory& memory, const guest::GuestAddress base,
                                  const std::size_t size) {
    std::vector<std::byte> bytes(size);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<std::byte>(memory.read_u8(base + static_cast<guest::GuestAddress>(index)));
    }
    return bytes;
}

void append_bytes(std::vector<std::byte>& out, const void* const data, const std::size_t size) {
    const auto* const begin = static_cast<const std::byte*>(data);
    out.insert(out.end(), begin, begin + size);
}

template <typename T> void append_value(std::vector<std::byte>& out, const T value) {
    append_bytes(out, &value, sizeof(T));
}

inline void append_section(std::vector<std::byte>& out, const std::vector<std::byte>& section) {
    append_value<std::uint64_t>(out, section.size());
    append_bytes(out, section.data(), section.size());
}

struct Reader {
    explicit Reader(const std::vector<std::byte>& data) : data_(data) {}

    const std::vector<std::byte>& data_;
    std::size_t offset_{};

    void read(void* const destination, const std::size_t size) {
        if (data_.size() - offset_ < size) {
            throw std::runtime_error("capture is truncated");
        }
        std::memcpy(destination, data_.data() + offset_, size);
        offset_ += size;
    }

    template <typename T> T read_value() {
        T result{};
        read(&result, sizeof(T));
        return result;
    }

    std::vector<std::byte> section(const std::uint64_t maximum) {
        const auto size = read_value<std::uint64_t>();
        if (size > maximum) {
            throw std::runtime_error("capture section is too large");
        }
        std::vector<std::byte> bytes(size);
        read(bytes.data(), bytes.size());
        return bytes;
    }
};

} // namespace

NativeCallCapture capture_native_call(const guest::GuestAddress entry, const guest::R5900State& cpu,
                                      const guest::EeMemory& memory) {
    NativeCallCapture capture;
    capture.entry = entry;
    capture.cpu = cpu;
    const auto ram = memory.bytes();
    capture.main_ram.assign(ram.begin(), ram.end());
    capture.scratchpad = read_range(memory, 0x70000000U, guest::kEeScratchpadSize);
    const auto micro = memory.vu0_micro_image();
    capture.vu0_micro.assign(micro.begin(), micro.end());
    capture.vu0_data = read_range(memory, guest::kVu0DataMemoryStart, guest::kVu0MemorySize);
    return capture;
}

bool write_native_call_capture(const std::filesystem::path& path, const NativeCallCapture& capture,
                               std::string& error) {
    std::vector<std::byte> raw;
    raw.reserve(capture.main_ram.size() + 65536U);
    append_bytes(raw, kMagic, sizeof(kMagic));
    append_value<std::uint32_t>(raw, capture.entry);
    append_value<std::uint32_t>(raw, static_cast<std::uint32_t>(sizeof(guest::R5900State)));
    append_bytes(raw, &capture.cpu, sizeof(guest::R5900State));
    append_section(raw, capture.main_ram);
    append_section(raw, capture.scratchpad);
    append_section(raw, capture.vu0_micro);
    append_section(raw, capture.vu0_data);

    std::vector<std::byte> compressed(ZSTD_compressBound(raw.size()));
    const std::size_t compressed_size =
        ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 3);
    if (ZSTD_isError(compressed_size) != 0U) {
        error = std::string("compression failed: ") + ZSTD_getErrorName(compressed_size);
        return false;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(path.parent_path(), directory_error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(compressed.data()),
               static_cast<std::streamsize>(compressed_size));
    if (!file) {
        error = "could not write " + path.string();
        return false;
    }
    return true;
}

std::optional<NativeCallCapture> read_native_call_capture(const std::filesystem::path& path,
                                                          std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "could not open " + path.string();
        return std::nullopt;
    }
    file.seekg(0, std::ios::end);
    std::vector<std::byte> compressed(static_cast<std::size_t>(file.tellg()));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(compressed.data()),
              static_cast<std::streamsize>(compressed.size()));

    const unsigned long long content_size =
        ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (content_size > 0x80000000ULL) {
        error = path.string() + " is not a capture";
        return std::nullopt;
    }
    std::vector<std::byte> raw(content_size);
    const std::size_t raw_size =
        ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(raw_size) != 0U || raw_size != raw.size()) {
        error = path.string() + " does not decompress";
        return std::nullopt;
    }

    try {
        Reader reader(raw);
        char magic[8];
        reader.read(magic, sizeof(magic));
        if (std::memcmp(magic, kMagic, sizeof(magic)) != 0) {
            error = path.string() + " is not a capture";
            return std::nullopt;
        }
        NativeCallCapture capture;
        capture.entry = reader.read_value<std::uint32_t>();
        if (reader.read_value<std::uint32_t>() != sizeof(guest::R5900State)) {
            error = path.string() + " comes from a build with a different register file layout";
            return std::nullopt;
        }
        reader.read(&capture.cpu, sizeof(guest::R5900State));
        capture.main_ram = reader.section(guest::kEeMainRamSize);
        capture.scratchpad = reader.section(guest::kEeScratchpadSize);
        capture.vu0_micro = reader.section(guest::kVu0MemorySize);
        capture.vu0_data = reader.section(guest::kVu0MemorySize);
        return capture;
    } catch (const std::exception& exception) {
        error = path.string() + ": " + exception.what();
        return std::nullopt;
    }
}

guest::R5900State restore_native_call_capture(const NativeCallCapture& capture,
                                              guest::EeMemory& memory) {
    memory.write_main_ram_bytes(0U, capture.main_ram);
    for (std::size_t index = 0; index < capture.scratchpad.size(); ++index) {
        memory.write_u8(0x70000000U + static_cast<guest::GuestAddress>(index),
                        static_cast<std::uint8_t>(capture.scratchpad[index]));
    }
    for (std::size_t index = 0; index < capture.vu0_micro.size(); ++index) {
        memory.write_u8(guest::kVu0MicroMemoryStart + static_cast<guest::GuestAddress>(index),
                        static_cast<std::uint8_t>(capture.vu0_micro[index]));
    }
    for (std::size_t index = 0; index < capture.vu0_data.size(); ++index) {
        memory.write_u8(guest::kVu0DataMemoryStart + static_cast<guest::GuestAddress>(index),
                        static_cast<std::uint8_t>(capture.vu0_data[index]));
    }
    return capture.cpu;
}

std::size_t register_native_call_captures_from_environment(NativeFunctionDispatcher& dispatcher,
                                                           std::ostream& log) {
    const char* list = std::getenv("TAKEDOWN_CAPTURE_CALLS");
    if (list == nullptr || *list == '\0') {
        return 0;
    }
    const char* directory = std::getenv("TAKEDOWN_CAPTURE_DIR");
    const std::filesystem::path root = directory != nullptr && *directory != '\0'
                                           ? std::filesystem::path(directory)
                                           : std::filesystem::path("local-analysis/captures");
    const std::uint32_t every = std::max(environment_number("TAKEDOWN_CAPTURE_EVERY", 97U), 1U);
    const std::uint32_t limit = environment_number("TAKEDOWN_CAPTURE_LIMIT", 24U);
    const char* skip = std::getenv("TAKEDOWN_CAPTURE_SKIP_CALLERS");
    const std::vector<guest::GuestAddress> skipped_callers =
        skip != nullptr ? parse_addresses(skip) : std::vector<guest::GuestAddress>{};

    std::size_t registered = 0;
    for (const auto address : parse_addresses(list)) {
        std::ostringstream name;
        name << std::hex << std::setw(8) << std::setfill('0') << address;
        auto counter = std::make_shared<CaptureCounter>();
        counter->directory = root / name.str();
        counter->every = every;
        counter->limit = limit;
        counter->skipped_callers = skipped_callers;
        const auto result = dispatcher.register_function(
            address, "Capture_" + name.str(), NativeFunctionLimits{1U, 1U},
            NativeFunctionTraits{.scheduler_state_transparent = true},
            [counter](const NativeFunctionPrepareContext& context) -> std::optional<NativeFunctionLimits> {
                if (std::find(counter->skipped_callers.begin(), counter->skipped_callers.end(),
                              context.cpu.read_gpr_u32(31U)) != counter->skipped_callers.end()) {
                    return std::nullopt;
                }
                const std::uint64_t call = counter->calls++;
                if (counter->written < counter->limit && call % counter->every == 0U) {
                    std::ostringstream file;
                    file << std::setw(4) << std::setfill('0') << counter->written << ".b3call";
                    std::string error;
                    if (write_native_call_capture(
                            counter->directory / file.str(),
                            capture_native_call(context.entry_address, context.cpu, context.memory),
                            error)) {
                        ++counter->written;
                    } else {
                        counter->written = counter->limit;
                    }
                }
                return std::nullopt;
            },
            [](NativeFunctionContext&) -> NativeFunctionExecution {
                throw std::logic_error("a capture-only entry never runs natively");
            });
        if (result.registered()) {
            ++registered;
            log << "Capturing calls to 0x" << name.str() << " into " << counter->directory.string()
                << '\n';
        } else {
            log << "Cannot capture 0x" << name.str()
                << ": the address already has a native function (disable it with "
                   "TAKEDOWN_DISABLE_NATIVE_FUNCTIONS)\n";
        }
    }
    return registered;
}

} // namespace takedown::runtime
