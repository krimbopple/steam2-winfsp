#include "s2fs/build_definition.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace s2fs {
namespace {

std::optional<std::uint32_t> parse_crc(const nlohmann::json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint32_t>();
    }
    if (!value.is_string()) {
        throw std::runtime_error("blob_crc must be null, an integer, or a hexadecimal string");
    }

    auto text = value.get<std::string>();
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.erase(0, 2);
    }
    std::uint32_t crc{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), crc, 16);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("blob_crc is not valid hexadecimal");
    }
    return crc;
}

std::filesystem::path utf8_path(const std::string& value) {
    const std::u8string encoded(value.begin(), value.end());
    return std::filesystem::path(encoded);
}

std::filesystem::path resolve_path(
    const std::filesystem::path& definition,
    const std::string& value) {
    auto path = utf8_path(value);
    return path.is_absolute() ? path : definition.parent_path() / path;
}

} // namespace

std::vector<DepotSpec> load_build_definition(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("cannot open build definition: " + path.string());
    }

    nlohmann::json document;
    stream >> document;
    const auto blob_directory = resolve_path(
        path, document.value("blob_directory", std::string{"."}));
    const auto dat_directory = resolve_path(
        path, document.value("dat_directory", std::string{"."}));

    std::vector<DepotSpec> depots;
    for (const auto& item : document.at("depots")) {
        if (!item.value("required", true)) {
            continue;
        }
        DepotSpec spec;
        spec.id = item.at("id").get<std::uint32_t>();
        spec.version = item.at("version").get<std::uint32_t>();
        spec.blob_crc = item.contains("blob_crc")
            ? parse_crc(item.at("blob_crc"))
            : std::nullopt;
        spec.blob_directory = item.contains("blob_directory")
            ? resolve_path(path, item.at("blob_directory").get<std::string>())
            : blob_directory;
        spec.dat_directory = item.contains("dat_directory")
            ? resolve_path(path, item.at("dat_directory").get<std::string>())
            : dat_directory;
        if (item.contains("blob_files")) {
            for (const auto& file : item.at("blob_files")) {
                spec.blob_files.push_back(resolve_path(path, file.get<std::string>()));
            }
        }
        if (item.contains("dat_files")) {
            for (const auto& file : item.at("dat_files")) {
                spec.dat_files.push_back(resolve_path(path, file.get<std::string>()));
            }
        }
        spec.mount_prefix = item.value("mount_prefix", std::string{});
        depots.push_back(std::move(spec));
    }

    if (depots.empty()) {
        throw std::runtime_error("build definition has no required depots");
    }
    return depots;
}

} // namespace s2fs
