#pragma once

#include "takedown/content/mod_manager.hpp"
#include <stop_token>
#include <string>

namespace takedown::content {
// Content digest shared with the catalog ZIP inventory (including directories).
[[nodiscard]] std::string mod_content_identity(const std::filesystem::path& root,
                                               std::stop_token stop = {});
// SHA-256 v2 of the applied snapshot in application order: stable manifest IDs,
// relative file names and actual bytes. Independent of installation folder and
// timestamps. Disabled/downloaded extras are excluded. Pass the runtime's applied
// ModManager snapshot, on a worker (large mods require reading their files).
// Throws on unreadable/changing files, links, missing enabled mods or invalid enabled dependencies;
// never admits an incomplete scan.
[[nodiscard]] std::string installed_mod_identity(const ModManager& manager,
                                                 std::stop_token stop = {});
} // namespace takedown::content
