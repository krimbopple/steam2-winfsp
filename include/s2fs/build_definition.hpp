#pragma once

#include "s2fs/steam2_archive.hpp"

#include <filesystem>
#include <vector>

namespace s2fs {

std::vector<DepotSpec> load_build_definition(const std::filesystem::path& path);

} // namespace s2fs
