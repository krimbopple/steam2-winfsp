#pragma once

#include "s2fs/steam2_archive.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace s2fs {

struct Node;
using NodeHandle = std::shared_ptr<Node>;

struct NodeInfo {
    bool directory{};
    std::uint64_t size{};
    std::uint32_t attributes{};
    std::chrono::system_clock::time_point creation_time{};
    std::chrono::system_clock::time_point access_time{};
    std::chrono::system_clock::time_point write_time{};
};

struct DirectoryEntry {
    std::wstring name;
    NodeInfo info;
};

class VirtualTree {
public:
    explicit VirtualTree(std::uint64_t write_quota_bytes = 512ULL << 20);
    ~VirtualTree();

    VirtualTree(const VirtualTree&) = delete;
    VirtualTree& operator=(const VirtualTree&) = delete;


    void overlay(const Steam2Depot& depot);
    NodeHandle lookup(std::wstring_view path) const;
    NodeHandle create(std::wstring_view path, bool directory, bool replace);
    NodeInfo info(const NodeHandle& node) const;
    std::vector<DirectoryEntry> list(const NodeHandle& directory) const;

    std::size_t read(const NodeHandle& node, std::uint64_t offset, std::span<std::byte> output) const;
    std::size_t write(const NodeHandle& node, std::uint64_t offset, std::span<const std::byte> input);
    void resize(const NodeHandle& node, std::uint64_t size);
    void remove(const NodeHandle& node);
    void rename(const NodeHandle& node, std::wstring_view new_path, bool replace);
    void set_attributes(const NodeHandle& node, std::uint32_t attributes);
    void set_times(
        const NodeHandle& node,
        std::optional<std::chrono::system_clock::time_point> creation,
        std::optional<std::chrono::system_clock::time_point> access,
        std::optional<std::chrono::system_clock::time_point> write);

    std::uint64_t write_quota() const noexcept;
    std::uint64_t dirty_bytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::wstring utf8_to_wide(std::string_view value);
std::string wide_to_utf8(std::wstring_view value);

} // namespace s2fs
