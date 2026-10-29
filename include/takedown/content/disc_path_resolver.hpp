#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace takedown::content {

enum class DiscPathErrc {
    invalid_guest_path = 1,
    ambiguous_case_match,
    outside_root,
};

[[nodiscard]] std::error_code make_error_code(DiscPathErrc error) noexcept;

// Resolves a path used by PS2 CD/DVD APIs against an already-extracted disc.
// The returned path exists, uses its host-side casing, and is canonically
// contained by root. Expected lookup and validation failures are reported in
// error; error is cleared on success.
[[nodiscard]] std::optional<std::filesystem::path>
resolve_disc_path(const std::filesystem::path& root, std::string_view guest_path,
                  std::error_code& error);

// Normalizes a guest disc path to the canonical lookup key used for
// case-insensitive matching: the same device/separator/";1" handling as
// resolve_disc_path, ASCII-lowercased components joined with '/'. Fails with
// invalid_guest_path for empty paths, "." / ".." components and foreign
// devices.
[[nodiscard]] std::optional<std::string> normalize_disc_guest_path(std::string_view guest_path,
                                                                   std::error_code& error);

} // namespace takedown::content

namespace std {

template <> struct is_error_code_enum<takedown::content::DiscPathErrc> : true_type {};

} // namespace std
