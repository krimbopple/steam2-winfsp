#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace s2fs {

struct DepotSpec {
    std::uint32_t id{};
    std::uint32_t version{};
    std::optional<std::uint32_t> blob_crc;
    std::filesystem::path blob_directory;
    std::filesystem::path dat_directory;
    std::vector<std::filesystem::path> blob_files;
    std::vector<std::filesystem::path> dat_files;

    std::string mount_prefix;
};
struct BlobMetadata {
    std::uint32_t depot_id{};
    std::uint32_t version{};
    std::optional<std::uint32_t> filename_crc;
    std::uint32_t parent_crc{};
    std::uint64_t expected_dat_size{};
    std::uint32_t manifest_file_count{};
};

BlobMetadata inspect_blob(const std::filesystem::path& path);


class Steam2File {
public:
    std::uint64_t size() const noexcept;
    std::size_t read(std::uint64_t offset, std::span<std::byte> output) const;

private:
    struct Impl;
    explicit Steam2File(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
    friend class Steam2Depot;
};

struct DepotEntry {
    std::string path;
    std::shared_ptr<const Steam2File> file;
};

class Steam2Depot {
public:
    static Steam2Depot load(const DepotSpec& spec);

    std::uint32_t id() const noexcept;
    std::uint32_t version() const noexcept;
    const std::vector<DepotEntry>& entries() const noexcept;

private:
    std::uint32_t id_{};
    std::uint32_t version_{};
    std::vector<DepotEntry> entries_;
};

std::array<std::uint8_t, 16> depot_key(std::uint32_t depot_id);

} // namespace s2fs
