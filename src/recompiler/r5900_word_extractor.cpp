#include "takedown/recompiler/r5900_word_extractor.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace takedown::recompiler {

namespace {

void reject(R5900WordExtractionResult& result, const R5900WordExtractionDiagnosticCode code,
            const R5900WordExtractionRequest& request, std::string message) {
    result.words.clear();
    result.diagnostics.push_back(R5900WordExtractionDiagnostic{
        code, request.start_address, request.instruction_count, std::move(message)});
}

} // namespace

namespace {

[[nodiscard]] std::optional<std::size_t> validate_bounded_request(const guest::EeMemory& memory,
                                                    const R5900WordExtractionRequest& request,
                                                    R5900WordExtractionResult& result) {
    if (request.instruction_count == 0U) {
        reject(result, R5900WordExtractionDiagnosticCode::EmptyInstructionRange, request,
            "the requested function contains zero instructions");
        return std::nullopt;
    }
    if ((request.start_address & 3U) != 0U) {
        reject(result, R5900WordExtractionDiagnosticCode::MisalignedStartAddress, request,
            "the requested start address is not EE instruction aligned");
        return std::nullopt;
    }
    if (request.instruction_count > request.maximum_instruction_count) {
        reject(result, R5900WordExtractionDiagnosticCode::InstructionLimitExceeded, request,
            "the requested instruction count exceeds the caller's safety limit");
        return std::nullopt;
    }
    if (request.instruction_count > std::numeric_limits<std::size_t>::max() / 4U) {
        reject(result, R5900WordExtractionDiagnosticCode::ByteLengthOverflow, request,
            "the instruction count overflows the host byte-length type");
        return std::nullopt;
    }
    const std::size_t byte_length = request.instruction_count * 4U;
    const unsigned long long end_address = byte_length + static_cast<unsigned long long>(request.start_address);
    if (end_address > 4294967296U) {
        reject(result, R5900WordExtractionDiagnosticCode::GuestAddressRangeOverflow, request,
            "the requested instruction range wraps the 32-bit EE address space");
        return std::nullopt;
    }
    if (!memory.contains(request.start_address, byte_length)) {
        reject(result, R5900WordExtractionDiagnosticCode::RangeOutsideEeMemory, request,
               "the complete requested instruction range is not mapped in EE memory");
        return std::nullopt;
    }
    return byte_length;
}

} // namespace

namespace {

void copy_words(const guest::EeMemory& memory, const R5900WordExtractionRequest& request,
                R5900WordExtractionResult& result) {
    result.words.reserve(request.instruction_count);
    try {
        for (std::size_t index = 0U; index < request.instruction_count; ++index) {
            const unsigned long long byte_offset = index * 4U;
            const unsigned int address = request.start_address + static_cast<unsigned int>(byte_offset);
            result.words.push_back(memory.read_u32_le(address));
        }
    } catch (const std::exception& error) {
        reject(result, R5900WordExtractionDiagnosticCode::MemoryReadFailed, request,
               std::string(error.what()).insert(0, "EE memory rejected a validated word read: "));
    }
}

} // namespace

R5900WordExtractionResult extract_bounded_r5900_words(const guest::EeMemory& memory,
                                                      const R5900WordExtractionRequest& request) {
    R5900WordExtractionResult result{};
    result.start_address = request.start_address;
    result.words = std::vector<std::uint32_t>{};
    result.diagnostics = std::vector<R5900WordExtractionDiagnostic>{};
    if (validate_bounded_request(memory, request, result).has_value()) {
        copy_words(memory, request, result);
    }
    return result;
}

R5900WordExtractionResult extract_elf_r5900_words(const guest::EeMemory& memory,
                                                  const content::Elf32LoadPlan& load_plan,
                                                  const R5900WordExtractionRequest& request) {
    R5900WordExtractionResult result{};
    result.start_address = request.start_address;
    result.words = std::vector<std::uint32_t>{};
    result.diagnostics = std::vector<R5900WordExtractionDiagnostic>{};
    const auto byte_length = validate_bounded_request(memory, request, result);
    if (byte_length.has_value()) {
        if (load_plan.contains_executable_file_range(request.start_address, *byte_length)) {
            copy_words(memory, request, result);
            if (result.diagnostics.empty()) {
                result.elf_executable_range_validated = true;
            }
        } else {
            std::string message = "the complete range is not contained in one file-backed executable PT_LOAD segment";
            reject(result, R5900WordExtractionDiagnosticCode::RangeOutsideExecutableFileData, request,
                   message);
        }
    }
    return result;
}

} // namespace takedown::recompiler
