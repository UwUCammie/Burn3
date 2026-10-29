// Checks native replacements of original game functions against captured
// calls (include/takedown/runtime/native_call_capture.hpp).
//
// For every capture the original code runs in the reference interpreter from
// the function's entry until it returns to the captured return address with
// the stack pointer back at its entry value; the capture is then restored a
// second time and the registered replacement
// (runtime::slus_21050_engine_functions()) runs on it. The two end states are
// compared:
//
//  * the result register the function declares (v0 or f0), exactly (a float
//    result within the function's tolerance);
//  * every register a caller can rely on: s0-s7, gp, sp, fp, ra and the
//    callee-saved FPRs (f20-f31). Temporaries the original leaves holding
//    whatever it computed last (a0-a3, t0-t9, at, v1, k0/k1, HI/LO, the
//    other FPRs) are only compared with --strict-registers;
//  * the continuation PC, which must be the captured return address;
//  * every word of main RAM, scratchpad and VU0 data memory that differs.
//    Words in EngineFunction::unused_words are skipped, float words are
//    accepted within the function's tolerance (loose_words by their own
//    absolute bound) and the original's dead stack frame below the entry
//    stack pointer ([sp - 64 KiB, sp)) is reported but never fails.
//
// usage: takedown_native_check <captures-dir> [options] [function...]
//   <captures-dir> holds one subfolder per function entry (hex address), or is
//   one such subfolder. A function is an address (0x00223130) or a name.
//   --max-steps N        original instruction cap per capture (default 50000000)
//   --strict-registers   also compare the caller-saved registers and HI/LO
//   --verbose            print every difference of a failing capture
//   --inject-fault       corrupt the native result (v0, s0, one RAM word) to prove the
//                        comparison fails; every capture must then fail
// Exit status is non-zero if any capture fails or times out. A directory with
// no captures passes.

#include "takedown/guest/ee_interpreter.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/native_call_capture.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"
#include "takedown/runtime/slus_21050_engine_functions.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace takedown;
namespace fs = std::filesystem;

constexpr guest::GuestAddress kScratchpadBase = 0x70000000U;
constexpr guest::GuestAddress kStackScratch = 64U * 1024U;
constexpr std::size_t kRegisterSp = 29U;
constexpr std::size_t kRegisterRa = 31U;
constexpr std::size_t kRegisterA0 = 4U;
constexpr std::size_t kRegisterV0 = 2U;

struct Options {
    fs::path directory;
    std::uint64_t max_steps{50'000'000U};
    bool strict_registers{};
    bool verbose{};
    bool inject_fault{};
    std::vector<std::string> filters;
};

struct Totals {
    std::size_t captures{};
    std::size_t passed{};
    std::size_t failed{};
    std::size_t declined{};
    std::size_t timed_out{};
    std::size_t errors{};
    std::size_t scratch_words{};
    std::size_t tolerated_words{};
    std::vector<std::uint64_t> original_steps;
    std::vector<std::uint32_t> native_steps;
};

std::string hex32(const std::uint32_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return text.str();
}

std::string hex64(const std::uint64_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return text.str();
}

float as_float(const std::uint32_t bits) { return std::bit_cast<float>(bits); }

// Zero or a normal float: anything else (denormals, infinities, NaN) is more
// likely an integer or a payload and is compared bit for bit.
bool comparable_float(const std::uint32_t bits) {
    const std::uint32_t exponent = (bits >> 23U) & 0xffU;
    if (exponent == 0xffU) {
        return false;
    }
    return exponent != 0U || (bits & 0x7fffffffU) == 0U;
}

bool floats_close(const std::uint32_t a, const std::uint32_t b, const double relative,
                  const double absolute) {
    if (a == b) {
        return true;
    }
    if (!comparable_float(a) || !comparable_float(b)) {
        return false;
    }
    const double x = static_cast<double>(as_float(a));
    const double y = static_cast<double>(as_float(b));
    const double difference = std::fabs(x - y);
    return difference <= absolute || difference <= relative * std::max(std::fabs(x), std::fabs(y));
}

struct Difference {
    std::string what;
    std::string original;
    std::string native;
};

struct CompareResult {
    std::vector<Difference> differences;
    std::size_t scratch_words{};
    std::size_t tolerated_words{};
};

struct SkipWord {
    guest::GuestAddress address{};
    float absolute{-1.0F}; // < 0: ignore the word; otherwise an absolute float bound
};

std::vector<SkipWord> skipped_words(const runtime::EngineFunction& function,
                                    const guest::R5900State& entry_cpu,
                                    const guest::EeMemory& entry_memory) {
    std::vector<SkipWord> words;
    const guest::GuestAddress record = entry_cpu.read_gpr_u32(kRegisterA0);
    for (const auto& unused : function.unused_words) {
        guest::GuestAddress base = record;
        if (unused.through_pointer_at >= 0) {
            base = entry_memory.read_u32_le(record + static_cast<guest::GuestAddress>(
                                                         unused.through_pointer_at));
        }
        words.push_back({base + unused.offset, -1.0F});
    }
    for (const auto& loose : function.loose_words) {
        words.push_back({record + loose.offset, loose.absolute});
    }
    return words;
}

const SkipWord* find_skip(const std::vector<SkipWord>& words, const guest::GuestAddress address) {
    for (const auto& word : words) {
        if (word.address == address) {
            return &word;
        }
    }
    return nullptr;
}

void compare_memory_word(const guest::GuestAddress address, const std::uint32_t original,
                         const std::uint32_t native, const runtime::EngineFunction& function,
                         const std::vector<SkipWord>& skips, const guest::GuestAddress sp,
                         CompareResult& result) {
    if (original == native) {
        return;
    }
    if (address < sp && sp - address <= kStackScratch) {
        ++result.scratch_words;
        return;
    }
    if (const SkipWord* const skip = find_skip(skips, address)) {
        if (skip->absolute < 0.0F) {
            ++result.tolerated_words;
            return;
        }
        if (floats_close(original, native, 0.0, static_cast<double>(skip->absolute))) {
            ++result.tolerated_words;
            return;
        }
    } else if (floats_close(original, native, function.check_tolerance, function.check_absolute)) {
        ++result.tolerated_words;
        return;
    }
    result.differences.push_back({"memory " + hex32(address), hex32(original), hex32(native)});
}

// Compares [base, base + size) of two memories word by word; `size` words are
// skipped quickly when the bytes are equal.
void compare_range(const guest::EeMemory& original, const guest::EeMemory& native,
                   const guest::GuestAddress base, const std::size_t size,
                   const runtime::EngineFunction& function, const std::vector<SkipWord>& skips,
                   const guest::GuestAddress sp, CompareResult& result, const bool main_ram) {
    if (main_ram) {
        const auto a = original.bytes();
        const auto b = native.bytes();
        const std::size_t count = std::min({size, a.size(), b.size()});
        constexpr std::size_t kBlock = 4096U;
        for (std::size_t block = 0; block < count; block += kBlock) {
            const std::size_t length = std::min(kBlock, count - block);
            if (std::memcmp(a.data() + block, b.data() + block, length) == 0) {
                continue;
            }
            for (std::size_t offset = block; offset < block + length; offset += 4U) {
                std::uint32_t x{};
                std::uint32_t y{};
                std::memcpy(&x, a.data() + offset, 4U);
                std::memcpy(&y, b.data() + offset, 4U);
                compare_memory_word(base + static_cast<guest::GuestAddress>(offset), x, y, function,
                                    skips, sp, result);
            }
        }
        return;
    }
    for (std::size_t offset = 0; offset < size; offset += 4U) {
        const auto address = base + static_cast<guest::GuestAddress>(offset);
        compare_memory_word(address, original.read_u32_le(address), native.read_u32_le(address),
                            function, skips, sp, result);
    }
}

void compare_registers(const guest::R5900State& original, const guest::R5900State& native,
                       const runtime::EngineFunction& function, const bool strict,
                       const guest::GuestAddress return_address, CompareResult& result) {
    static constexpr const char* kNames[32] = {
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
        "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
        "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    const auto gpr_text = [](const guest::Gpr128& value) {
        return hex32(value.words[3]).substr(2) + "_" + hex32(value.words[2]).substr(2) + "_" +
               hex32(value.words[1]).substr(2) + "_" + hex32(value.words[0]).substr(2);
    };
    for (std::size_t index = 1; index < 32U; ++index) {
        const bool callee_saved = (index >= 16U && index <= 23U) || index >= 28U;
        const bool result_register = index == kRegisterV0;
        if (!callee_saved && !result_register && !strict) {
            continue;
        }
        const auto a = original.read_gpr(index);
        const auto b = native.read_gpr(index);
        if (result_register) {
            if (function.returns == runtime::EngineReturn::Nothing && !strict) {
                continue;
            }
            if (function.returns == runtime::EngineReturn::Float && !strict) {
                continue;
            }
            // A 32-bit result may leave the upper half whatever it was; the
            // original sign-extends 32-bit values, so compare the low 64 bits.
            if (a.words[0] != b.words[0] || a.words[1] != b.words[1]) {
                result.differences.push_back({std::string("gpr v0"), gpr_text(a), gpr_text(b)});
            }
            continue;
        }
        if (a != b) {
            result.differences.push_back({std::string("gpr ") + kNames[index], gpr_text(a), gpr_text(b)});
        }
    }
    for (std::size_t index = 0; index < 32U; ++index) {
        const bool callee_saved = index >= 20U;
        const bool result_register = index == 0U && function.returns == runtime::EngineReturn::Float;
        if (!callee_saved && !result_register && !strict) {
            continue;
        }
        const std::uint32_t a = original.read_fpr_bits(index);
        const std::uint32_t b = native.read_fpr_bits(index);
        if (a == b) {
            continue;
        }
        if (result_register && floats_close(a, b, function.check_tolerance, function.check_absolute)) {
            ++result.tolerated_words;
            continue;
        }
        result.differences.push_back({"fpr f" + std::to_string(index), hex32(a), hex32(b)});
    }
    if (strict) {
        for (std::size_t lane = 0; lane < guest::R5900State::kHiLoLaneCount; ++lane) {
            if (original.read_hi(lane) != native.read_hi(lane)) {
                result.differences.push_back({"hi" + std::to_string(lane), hex64(original.read_hi(lane)),
                                              hex64(native.read_hi(lane))});
            }
            if (original.read_lo(lane) != native.read_lo(lane)) {
                result.differences.push_back({"lo" + std::to_string(lane), hex64(original.read_lo(lane)),
                                              hex64(native.read_lo(lane))});
            }
        }
    }
    if (native.pc() != return_address) {
        result.differences.push_back({"pc after return", hex32(original.pc()), hex32(native.pc())});
    }
}

struct OriginalRun {
    bool finished{};
    std::uint64_t steps{};
    std::string failure;
};

OriginalRun run_original(guest::R5900State& cpu, guest::EeMemory& memory,
                         const guest::GuestAddress return_address, const guest::GuestAddress sp,
                         const std::uint64_t max_steps) {
    OriginalRun run;
    auto cache = std::make_unique<guest::R5900InstructionCache>();
    while (run.steps < max_steps) {
        const auto step = guest::step_r5900(cpu, memory, *cache);
        ++run.steps;
        if (step.status != guest::EeStepStatus::Executed) {
            std::ostringstream text;
            text << "original stopped at " << hex32(cpu.pc()) << ": "
                 << guest::to_string(step.status);
            run.failure = text.str();
            return run;
        }
        if (cpu.pc() == return_address && !cpu.is_executing_delay_slot() &&
            cpu.read_gpr_u32(kRegisterSp) == sp) {
            run.finished = true;
            return run;
        }
    }
    run.failure = "timeout after " + std::to_string(max_steps) + " original instructions";
    return run;
}

std::optional<guest::GuestAddress> parse_address(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 16);
    if (end == nullptr || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<guest::GuestAddress>(value);
}

bool filter_matches(const Options& options, const runtime::EngineFunction& function) {
    if (options.filters.empty()) {
        return true;
    }
    for (const auto& filter : options.filters) {
        if (const auto address = parse_address(filter); address && *address == function.address) {
            return true;
        }
        if (filter == function.name) {
            return true;
        }
    }
    return false;
}

struct CaptureFolder {
    guest::GuestAddress address{};
    fs::path path;
};

std::vector<CaptureFolder> find_folders(const fs::path& directory) {
    std::vector<CaptureFolder> folders;
    const auto add = [&folders](const fs::path& path) {
        if (const auto address = parse_address(path.filename().string())) {
            folders.push_back({*address, path});
        }
    };
    bool has_captures = false;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        if (entry.is_directory()) {
            add(entry.path());
        } else if (entry.path().extension() == ".b3call") {
            has_captures = true;
        }
    }
    if (has_captures) {
        add(directory);
    }
    std::sort(folders.begin(), folders.end(),
              [](const CaptureFolder& a, const CaptureFolder& b) { return a.address < b.address; });
    return folders;
}

void print_difference_list(const CompareResult& result, const bool verbose, const char* indent) {
    const std::size_t limit = verbose ? result.differences.size() : std::min<std::size_t>(6U, result.differences.size());
    for (std::size_t index = 0; index < limit; ++index) {
        const auto& difference = result.differences[index];
        std::cout << indent << difference.what << ": original " << difference.original << " native "
                  << difference.native << '\n';
    }
    if (limit < result.differences.size()) {
        std::cout << indent << "... " << (result.differences.size() - limit)
                  << " more (--verbose lists all)\n";
    }
}

void check_capture(const Options& options, const runtime::EngineFunction& function,
                   const fs::path& file, Totals& totals) {
    ++totals.captures;
    std::string error;
    const auto capture = runtime::read_native_call_capture(file, error);
    if (!capture) {
        std::cout << "  ERROR " << file.filename().string() << ": " << error << '\n';
        ++totals.errors;
        ++totals.failed;
        return;
    }
    const std::string label = "  " + file.filename().string();

    guest::EeMemory original_memory;
    guest::R5900State original_cpu = runtime::restore_native_call_capture(*capture, original_memory);
    const guest::GuestAddress return_address = original_cpu.read_gpr_u32(kRegisterRa);
    const guest::GuestAddress sp = original_cpu.read_gpr_u32(kRegisterSp);
    original_cpu.set_pc(function.address);

    guest::EeMemory native_memory;
    guest::R5900State native_cpu = runtime::restore_native_call_capture(*capture, native_memory);
    native_cpu.set_pc(function.address);

    if (function.call_cost != nullptr &&
        function.call_cost(runtime::NativeFunctionPrepareContext{native_cpu, native_memory,
                                                                 function.address}) == 0U) {
        std::cout << label << " declined (call_cost is 0)\n";
        ++totals.declined;
        return;
    }

    const auto skips = skipped_words(function, original_cpu, original_memory);

    const OriginalRun original = run_original(original_cpu, original_memory, return_address, sp,
                                              options.max_steps);
    if (!original.finished) {
        std::cout << label << " FAIL " << original.failure << '\n';
        if (original.failure.rfind("timeout", 0) == 0) {
            ++totals.timed_out;
        } else {
            ++totals.errors;
        }
        ++totals.failed;
        return;
    }
    totals.original_steps.push_back(original.steps);

    runtime::NativeFunctionExecution execution{};
    try {
        runtime::NativeFunctionContext context{native_cpu, native_memory, function.address};
        execution = function.implementation(context);
    } catch (const std::exception& exception) {
        std::cout << label << " FAIL native code threw: " << exception.what() << '\n';
        ++totals.errors;
        ++totals.failed;
        return;
    }
    totals.native_steps.push_back(execution.retired_instructions);
    if (options.inject_fault) {
        native_cpu.write_gpr_u64(kRegisterV0, native_cpu.read_gpr_u64(kRegisterV0) ^ 1U);
        native_cpu.write_gpr_u64(16U, native_cpu.read_gpr_u64(16U) + 4U);
        native_memory.write_u32_le(0x00100000U, native_memory.read_u32_le(0x00100000U) ^ 0x80000001U);
    }

    CompareResult result;
    compare_registers(original_cpu, native_cpu, function, options.strict_registers, return_address,
                      result);
    compare_range(original_memory, native_memory, 0U, guest::kEeMainRamSize, function, skips, sp,
                  result, true);
    compare_range(original_memory, native_memory, kScratchpadBase, guest::kEeScratchpadSize,
                  function, skips, sp, result, false);
    compare_range(original_memory, native_memory, guest::kVu0DataMemoryStart,
                  guest::kVu0MemorySize, function, skips, sp, result, false);
    totals.scratch_words += result.scratch_words;
    totals.tolerated_words += result.tolerated_words;

    if (result.differences.empty()) {
        ++totals.passed;
        if (options.verbose) {
            std::cout << label << " ok (" << original.steps << " original instructions, native "
                      << execution.retired_instructions << ")\n";
        }
        return;
    }
    ++totals.failed;
    std::cout << label << " FAIL " << result.differences.size() << " difference(s), "
              << original.steps << " original instructions\n";
    print_difference_list(result, options.verbose, "    ");
}

void print_step_summary(const Totals& totals, const runtime::EngineFunction& function) {
    if (totals.original_steps.empty()) {
        return;
    }
    const auto [minimum, maximum] =
        std::minmax_element(totals.original_steps.begin(), totals.original_steps.end());
    std::uint64_t sum = 0;
    for (const auto steps : totals.original_steps) {
        sum += steps;
    }
    const std::uint64_t average = sum / totals.original_steps.size();
    std::cout << "  original instructions min/avg/max " << *minimum << "/" << average << "/"
              << *maximum << ", declared cost " << function.cost;
    if (function.cost != 0U && *maximum > function.cost) {
        std::cout << " (below the observed maximum)";
    }
    std::cout << '\n';
    if (!totals.native_steps.empty()) {
        const auto [native_minimum, native_maximum] =
            std::minmax_element(totals.native_steps.begin(), totals.native_steps.end());
        std::cout << "  native reported instructions min/max " << *native_minimum << "/"
                  << *native_maximum << '\n';
    }
}

bool parse_options(const int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--max-steps" && index + 1 < argc) {
            options.max_steps = std::strtoull(argv[++index], nullptr, 0);
        } else if (argument == "--strict-registers") {
            options.strict_registers = true;
        } else if (argument == "--inject-fault") {
            options.inject_fault = true;
        } else if (argument == "--verbose" || argument == "-v") {
            options.verbose = true;
        } else if (argument == "--function" && index + 1 < argc) {
            options.filters.emplace_back(argv[++index]);
        } else if (!argument.empty() && argument.front() == '-') {
            return false;
        } else if (options.directory.empty()) {
            options.directory = std::string(argument);
        } else {
            options.filters.emplace_back(argument);
        }
    }
    return !options.directory.empty();
}

} // namespace

int main(const int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr << "usage: takedown_native_check <captures-dir> [--max-steps N] "
                     "[--strict-registers] [--verbose] [function address or name...]\n";
        return 2;
    }
    std::error_code error;
    if (!fs::is_directory(options.directory, error)) {
        std::cout << "no captures at " << options.directory.string() << ": nothing to check\n";
        return 0;
    }

    std::map<guest::GuestAddress, const runtime::EngineFunction*> functions;
    for (const auto& function : runtime::slus_21050_engine_functions()) {
        functions[function.address] = &function;
    }

    std::size_t failed_functions = 0;
    std::size_t checked_functions = 0;
    for (const auto& folder : find_folders(options.directory)) {
        const auto found = functions.find(folder.address);
        if (found == functions.end()) {
            if (options.filters.empty() || std::any_of(options.filters.begin(), options.filters.end(),
                                                       [&folder](const std::string& filter) {
                                                           return parse_address(filter) ==
                                                                  std::optional(folder.address);
                                                       })) {
                std::cout << hex32(folder.address) << ": no native replacement registered, skipped\n";
            }
            continue;
        }
        const runtime::EngineFunction& function = *found->second;
        if (!filter_matches(options, function)) {
            continue;
        }
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(folder.path, error)) {
            if (entry.path().extension() == ".b3call") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        std::cout << hex32(function.address) << " " << function.name << ": " << files.size()
                  << " capture(s)\n";
        Totals totals;
        for (const auto& file : files) {
            check_capture(options, function, file, totals);
        }
        ++checked_functions;
        std::cout << "  " << function.name << ": captures " << totals.captures << " pass "
                  << totals.passed << " fail " << totals.failed << " declined " << totals.declined
                  << " (timeouts " << totals.timed_out << ", errors " << totals.errors << ")";
        if (totals.tolerated_words != 0U || totals.scratch_words != 0U) {
            std::cout << "; accepted " << totals.tolerated_words << " tolerance/unused word(s), "
                      << totals.scratch_words << " dead-stack word(s)";
        }
        std::cout << '\n';
        print_step_summary(totals, function);
        if (totals.failed != 0U) {
            ++failed_functions;
        }
    }
    std::cout << checked_functions << " function(s) checked, " << failed_functions
              << " with failures\n";
    return failed_functions == 0U ? 0 : 1;
}
