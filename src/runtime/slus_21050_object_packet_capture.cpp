#include "takedown/runtime/slus_21050_object_packet_capture.hpp"

#include "takedown/guest/ee_memory.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace takedown::runtime::slus_21050 {
namespace {

void write_le(std::ostream& s, std::uint64_t value, const unsigned bytes) {
    for (unsigned i = 0; i != bytes; ++i) {
        s.put(static_cast<char>(value));
        value >>= 8U;
    }
}

[[nodiscard]] std::uint64_t read_le(std::istream& s, const unsigned bytes) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        const auto c = s.get();
        if (c == std::char_traits<char>::eof()) {
            throw std::runtime_error("truncated object packet case");
        }
        value |= static_cast<std::uint64_t>(c & 0xff) << (i * 8U);
    }
    return value;
}

[[nodiscard]] std::uint32_t read_u32(std::istream& s) {
    return static_cast<std::uint32_t>(read_le(s, 4));
}

[[nodiscard]] guest::Gpr128 read_gpr128(std::istream& s) {
    guest::Gpr128 value{};
    for (auto& v : value.words) {
        v = read_u32(s);
    }
    return value;
}

void cpu_out(std::ostream& s, const guest::R5900State& cpu) {
    write_le(s, cpu.pc(), 4);
    write_le(s, cpu.next_pc(), 4);
    write_le(s, cpu.is_executing_delay_slot(), 4);
    for (unsigned i = 0; i != 32; ++i) {
        for (auto v : cpu.read_gpr(i).words) {
            write_le(s, v, 4);
        }
    }
    for (unsigned i = 0; i != 2; ++i) {
        write_le(s, cpu.read_hi(i), 8);
        write_le(s, cpu.read_lo(i), 8);
    }
    for (unsigned i = 0; i != 32; ++i) {
        write_le(s, cpu.read_fpr_bits(i), 4);
    }
    for (unsigned i = 0; i != 32; ++i) {
        write_le(s, cpu.read_cop0(i), 4);
    }
    for (unsigned i = 0; i != 32; ++i) {
        for (auto v : cpu.read_vu0_vector(i).words) {
            write_le(s, v, 4);
        }
    }
    for (auto v : cpu.vu0_accumulator().words) {
        write_le(s, v, 4);
    }
    for (unsigned i = 0; i != 32; ++i) {
        write_le(s, cpu.read_vu0_control(i), 4);
    }
    for (auto v : cpu.vu0_mac_flag_history()) {
        write_le(s, v, 4);
    }
    write_le(s, cpu.fpu_accumulator_bits(), 4);
    write_le(s, cpu.fcr31_bits(), 4);
    write_le(s, cpu.sa_byte_count(), 4);
}

guest::R5900State cpu_in(std::istream& s) {
    const auto pc = read_u32(s);
    const auto next = read_u32(s);
    const auto delay = read_u32(s);
    if (next != pc + 4U || delay != 0U) {
        throw std::runtime_error("object packet boundary is not sequential");
    }
    guest::R5900State cpu{pc};
    for (unsigned i = 0; i != 32; ++i) {
        cpu.write_gpr(i, read_gpr128(s));
    }
    for (unsigned i = 0; i != 2; ++i) {
        cpu.write_hi(i, read_le(s, 8));
        cpu.write_lo(i, read_le(s, 8));
    }
    for (unsigned i = 0; i != 32; ++i) {
        cpu.write_fpr_bits(i, read_u32(s));
    }
    for (unsigned i = 0; i != 32; ++i) {
        cpu.write_cop0(i, read_u32(s));
    }
    for (unsigned i = 0; i != 32; ++i) {
        cpu.write_vu0_vector(i, read_gpr128(s));
    }
    cpu.set_vu0_accumulator(read_gpr128(s));
    for (unsigned i = 0; i != 32; ++i) {
        cpu.write_vu0_control(i, read_u32(s));
    }
    guest::R5900State::Vu0MacFlagHistory history;
    for (auto& v : history) {
        v = read_u32(s);
    }
    cpu.persist_vu0_mac_flag_history(history);
    cpu.set_fpu_accumulator_bits(read_u32(s));
    cpu.set_fcr31_bits(read_u32(s));
    const auto sa = read_u32(s);
    if (sa > 255U) {
        throw std::runtime_error("invalid object packet SA");
    }
    cpu.set_sa_byte_count(static_cast<std::uint8_t>(sa));
    return cpu;
}
} // namespace

bool same_object_packet_cpu(const guest::R5900State& a, const guest::R5900State& b) {
    std::ostringstream x, y;
    cpu_out(x, a);
    cpu_out(y, b);
    return x.str() == y.str();
}

void write_object_packet_case(std::ostream& s, const ObjectPacketCase& c) {
    s.write("B3OBJ001", 8);
    write_le(s, c.sequence, 8);
    write_le(s, c.retired, 8);
    cpu_out(s, c.before);
    cpu_out(s, c.after);
    write_le(s, c.regions.size(), 4);
    for (const auto& r : c.regions) {
        if (r.before.size() != r.after.size()) {
            throw std::runtime_error("object case region sizes differ");
        }
        write_le(s, r.address, 4);
        write_le(s, r.before.size(), 4);
        for (const auto* bytes : {&r.before, &r.after}) {
            s.write(reinterpret_cast<const char*>(bytes->data()),
                    static_cast<std::streamsize>(bytes->size()));
        }
    }
    if (!s) {
        throw std::runtime_error("object case write failed");
    }
}

ObjectPacketCase read_object_packet_case(std::istream& s) {
    char magic[8]{};
    s.read(magic, 8);
    if (std::string_view(magic, sizeof magic) != "B3OBJ001") {
        throw std::runtime_error("invalid object case magic");
    }
    ObjectPacketCase c;
    c.sequence = read_le(s, 8);
    c.retired = read_le(s, 8);
    c.before = cpu_in(s);
    c.after = cpu_in(s);
    const auto count = read_u32(s);
    if (count > 16U) {
        throw std::runtime_error("too many object case regions");
    }
    for (unsigned i = 0; i < count; ++i) {
        ObjectPacketRegion r{};
        r.address = read_u32(s);
        const auto size = read_u32(s);
        if (size > 65536U) {
            throw std::runtime_error("oversized object case region");
        }
        for (auto* bytes : {&r.before, &r.after}) {
            bytes->resize(size);
            s.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(size));
            if (!s) {
                throw std::runtime_error("truncated object case region");
            }
        }
        c.regions.push_back(std::move(r));
    }
    if (s.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("trailing object case data");
    }
    return c;
}

ObjectPacketCapture::ObjectPacketCapture() {
    if (const char* path = std::getenv("TAKEDOWN_OBJECT_PACKET_CAPTURE_DIR");
        path != nullptr && *path != '\0') {
        directory_ = path;
    }
}

void ObjectPacketCapture::observe(const guest::R5900State& cpu, const guest::EeMemory& memory,
                                  const std::uint64_t step, const bool measured_window) {
    if (directory_.empty()) {
        return;
    }
    if (cpu.pc() == kObjectPacketEnd && pending_ &&
        cpu.read_gpr_u32(29) + 16U == pending_->before.read_gpr_u32(29) &&
        cpu.read_gpr_u32(31) == pending_->before.read_gpr_u32(31)) {
        pending_->after = cpu;
        pending_->retired = step - start_step_;
        for (auto& region : pending_->regions) {
            const auto view = memory.view(region.address, region.before.size());
            region.after.assign(view.begin(), view.end());
        }
        std::filesystem::create_directories(directory_);
        const auto path = directory_ / ("object-" + std::to_string(pending_->sequence) + ".b3obj");
        if (std::filesystem::exists(path)) {
            throw std::runtime_error("refusing to overwrite object case");
        }
        std::ofstream output(path, std::ios::binary);
        write_object_packet_case(output, *pending_);
        pending_.reset();
        ++captured_;
    }
    if (cpu.pc() != kObjectPacketBegin || !measured_window || captured_ >= 16U) {
        return;
    }
    const auto sequence = sequence_++;
    if (sequence % 257U != 0U) {
        return;
    }
    if (pending_) {
        throw std::runtime_error("nested object capture boundary");
    }
    ObjectPacketCase c;
    c.sequence = sequence;
    c.before = cpu;
    const auto add = [&memory, &c](const std::uint32_t address, const std::uint32_t length) {
        if (!memory.contains(address, length)) {
            throw std::runtime_error("unmapped object capture region");
        }
        const auto view = memory.view(address, length);
        c.regions.push_back({.address = address, .before = {view.begin(), view.end()}});
    };
    const auto gp = cpu.read_gpr_u32(28);
    const auto a0 = cpu.read_gpr_u32(4);
    const auto packet = memory.read_u32_le(gp - 23648U);
    const auto count = memory.read_u32_le(packet);
    if (count > 1024U) {
        throw std::runtime_error("unexpected object packet count");
    }
    add(gp - 23648U, 4);
    add(a0, 192);
    add(cpu.read_gpr_u32(5), 64);
    const auto selection = static_cast<std::int8_t>(memory.read_u8(a0 + 153U));
    add(static_cast<std::uint32_t>(selection * 96 + 31664080), 96);
    add(cpu.read_gpr_u32(29) - 16U, 16);
    add(packet, (count + 1U) * 16U);
    add(memory.read_u32_le(packet + 4U), (count + 5U) * 16U);
    add(0x11000000U, 4096);
    add(0x11004000U, 4096);
    add(kObjectPacketBegin, kObjectPacketEnd - kObjectPacketBegin);
    pending_ = std::move(c);
    start_step_ = step;
}
}
