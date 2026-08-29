#include "s2fs/virtual_tree.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace s2fs {
namespace {

constexpr std::uint64_t page_size = 32ULL * 1024ULL;
constexpr std::uint32_t file_attribute_directory = 0x00000010U;
constexpr std::uint32_t file_attribute_archive = 0x00000020U;
constexpr std::uint32_t file_attribute_normal = 0x00000080U;

[[noreturn]] void fail(std::errc error, const char* message) {
    throw std::system_error(std::make_error_code(error), message);
}

struct CaseInsensitiveLess {
    bool operator()(std::wstring_view left, std::wstring_view right) const noexcept {
        const int result = CompareStringOrdinal(
            left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE);
        if (result == CSTR_LESS_THAN) {
            return true;
        }
        if (result == CSTR_GREATER_THAN || result == CSTR_EQUAL) {
            return false;
        }
        return left < right;
    }
};

bool equal_name(std::wstring_view left, std::wstring_view right) noexcept {
    const CaseInsensitiveLess less;
    return !less(left, right) && !less(right, left);
}

std::vector<std::wstring> path_components(std::wstring_view path) {
    std::vector<std::wstring> result;
    std::size_t start = 0;
    while (start < path.size()) {
        while (start < path.size() && (path[start] == L'\\' || path[start] == L'/')) {
            ++start;
        }
        const std::size_t component_start = start;
        while (start < path.size() && path[start] != L'\\' && path[start] != L'/') {
            ++start;
        }
        if (component_start == start) {
            continue;
        }
        std::wstring component(path.substr(component_start, start - component_start));
        if (component == L".") {
            continue;
        }
        if (component == L"..") {
            if (result.empty()) {
                fail(std::errc::invalid_argument, "path escapes the virtual root");
            }
            result.pop_back();
            continue;
        }
        if (component.find(L':') != std::wstring::npos || component.find(L'\0') != std::wstring::npos) {
            fail(std::errc::invalid_argument, "invalid virtual path component");
        }
        result.push_back(std::move(component));
    }
    return result;
}

} // namespace

struct Node {
    using Children = std::map<std::wstring, NodeHandle, CaseInsensitiveLess>;
    using Tombstones = std::set<std::wstring, CaseInsensitiveLess>;
    using Page = std::array<std::byte, static_cast<std::size_t>(page_size)>;

    std::shared_ptr<const void> owner;
    bool directory{};
    bool linked{true};
    std::wstring name;
    std::weak_ptr<Node> parent;
    Children children;
    Tombstones tombstones;

    std::shared_ptr<const Steam2File> base;
    std::uint64_t base_limit{};
    std::uint64_t size{};
    std::unordered_map<std::uint64_t, std::unique_ptr<Page>> dirty_pages;
    std::shared_ptr<std::atomic<std::uint64_t>> dirty_counter;

    ~Node() {
        if (dirty_counter) {
            dirty_counter->fetch_sub(
                static_cast<std::uint64_t>(dirty_pages.size()) * page_size,
                std::memory_order_relaxed);
        }
    }

    std::uint32_t attributes{};
    std::chrono::system_clock::time_point creation_time{};
    std::chrono::system_clock::time_point access_time{};
    std::chrono::system_clock::time_point write_time{};
};

struct VirtualTree::Impl {
    explicit Impl(std::uint64_t quota)
        : write_quota(quota),
          dirty_bytes(std::make_shared<std::atomic<std::uint64_t>>()) {
        root = make_node(L"", true);
        root->parent.reset();
    }

    NodeHandle make_node(std::wstring name, bool directory) {
        auto node = std::make_shared<Node>();
        node->owner = identity;
        node->dirty_counter = dirty_bytes;
        node->directory = directory;
        node->name = std::move(name);
        node->attributes = directory ? file_attribute_directory : file_attribute_normal;
        const auto now = std::chrono::system_clock::now();
        node->creation_time = now;
        node->access_time = now;
        node->write_time = now;
        return node;
    }

    Node& checked(const NodeHandle& handle) const {
        if (!handle || handle->owner != identity) {
            fail(std::errc::invalid_argument, "node does not belong to this virtual tree");
        }
        return *handle;
    }

    NodeHandle find_locked(const std::vector<std::wstring>& components) const {
        NodeHandle current = root;
        for (const auto& component : components) {
            if (!current->directory) {
                return {};
            }
            const auto found = current->children.find(component);
            if (found == current->children.end()) {
                return {};
            }
            current = found->second;
        }
        return current;
    }

    std::pair<NodeHandle, std::wstring> parent_locked(const std::vector<std::wstring>& components) const {
        if (components.empty()) {
            fail(std::errc::invalid_argument, "the virtual root has no parent");
        }
        NodeHandle current = root;
        for (std::size_t index = 0; index + 1 < components.size(); ++index) {
            const auto found = current->children.find(components[index]);
            if (found == current->children.end()) {
                fail(std::errc::no_such_file_or_directory, "parent directory does not exist");
            }
            current = found->second;
            if (!current->directory) {
                fail(std::errc::not_a_directory, "path parent is not a directory");
            }
        }
        return {std::move(current), components.back()};
    }


    std::shared_ptr<const void> identity = std::make_shared<int>(0);
    NodeHandle root;
    mutable std::shared_mutex mutex;
    const std::uint64_t write_quota;
    std::shared_ptr<std::atomic<std::uint64_t>> dirty_bytes;
};

VirtualTree::VirtualTree(std::uint64_t write_quota_bytes)
    : impl_(std::make_unique<Impl>(write_quota_bytes)) {}

VirtualTree::~VirtualTree() = default;

void VirtualTree::overlay(const Steam2Depot& depot) {
    std::unique_lock lock(impl_->mutex);
    for (const DepotEntry& entry : depot.entries()) {
        const auto components = path_components(utf8_to_wide(entry.path));
        if (components.empty() || !entry.file) {
            continue;
        }

        NodeHandle parent = impl_->root;
        bool hidden = false;
        for (std::size_t index = 0; index + 1 < components.size(); ++index) {
            const std::wstring& component = components[index];
            if (parent->tombstones.contains(component)) {
                hidden = true;
                break;
            }
            auto found = parent->children.find(component);
            if (found != parent->children.end() && found->second->directory) {
                parent = found->second;
                continue;
            }
            if (found != parent->children.end()) {
                found->second->linked = false;
                found->second->parent.reset();
                parent->children.erase(found);
            }
            auto directory = impl_->make_node(component, true);
            directory->parent = parent;
            parent->children.emplace(component, directory);
            parent = std::move(directory);
        }
        if (hidden || parent->tombstones.contains(components.back())) {
            continue;
        }

        auto found = parent->children.find(components.back());
        if (found != parent->children.end()) {
            found->second->linked = false;
            found->second->parent.reset();
            parent->children.erase(found);
        }
        auto file = impl_->make_node(components.back(), false);
        file->base = entry.file;
        file->base_limit = entry.file->size();
        file->size = file->base_limit;
        file->attributes = file_attribute_archive;
        file->parent = parent;
        parent->children.emplace(file->name, file);
    }
}

NodeHandle VirtualTree::lookup(std::wstring_view path) const {
    const auto components = path_components(path);
    std::shared_lock lock(impl_->mutex);
    return impl_->find_locked(components);
}

NodeHandle VirtualTree::create(std::wstring_view path, bool directory, bool replace) {
    const auto components = path_components(path);
    std::unique_lock lock(impl_->mutex);
    auto [parent, name] = impl_->parent_locked(components);
    auto found = parent->children.find(name);
    if (found != parent->children.end()) {
        if (!replace) {
            fail(std::errc::file_exists, "virtual path already exists");
        }
        Node& replaced = *found->second;
        if (replaced.directory && !replaced.children.empty()) {
            fail(std::errc::directory_not_empty, "cannot replace a non-empty directory");
        }
        replaced.linked = false;
        replaced.parent.reset();
        parent->children.erase(found);
    }

    auto node = impl_->make_node(std::move(name), directory);
    node->parent = parent;
    parent->tombstones.erase(node->name);
    parent->children.emplace(node->name, node);
    return node;
}

NodeInfo VirtualTree::info(const NodeHandle& node) const {
    std::shared_lock lock(impl_->mutex);
    const Node& value = impl_->checked(node);
    return NodeInfo{
        .directory = value.directory,
        .size = value.size,
        .attributes = value.attributes,
        .creation_time = value.creation_time,
        .access_time = value.access_time,
        .write_time = value.write_time,
    };
}

std::vector<DirectoryEntry> VirtualTree::list(const NodeHandle& directory) const {
    std::shared_lock lock(impl_->mutex);
    const Node& value = impl_->checked(directory);
    if (!value.directory) {
        fail(std::errc::not_a_directory, "cannot enumerate a file");
    }
    std::vector<DirectoryEntry> result;
    result.reserve(value.children.size());
    for (const auto& [name, child] : value.children) {
        result.push_back(DirectoryEntry{
            .name = name,
            .info = NodeInfo{
                .directory = child->directory,
                .size = child->size,
                .attributes = child->attributes,
                .creation_time = child->creation_time,
                .access_time = child->access_time,
                .write_time = child->write_time,
            },
        });
    }
    return result;
}

std::size_t VirtualTree::read(
    const NodeHandle& node,
    std::uint64_t offset,
    std::span<std::byte> output) const {
    std::shared_lock lock(impl_->mutex);
    const Node& value = impl_->checked(node);
    if (value.directory) {
        fail(std::errc::is_a_directory, "cannot read a directory");
    }
    if (offset >= value.size || output.empty()) {
        return 0;
    }

    const std::uint64_t available = value.size - offset;
    const std::size_t requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(available, output.size()));
    std::size_t completed = 0;
    while (completed < requested) {
        const std::uint64_t position = offset + completed;
        const std::uint64_t page_index = position / page_size;
        const std::size_t page_offset = static_cast<std::size_t>(position % page_size);
        const std::size_t chunk = std::min<std::size_t>(
            requested - completed, static_cast<std::size_t>(page_size) - page_offset);
        auto destination = output.subspan(completed, chunk);

        const auto dirty = value.dirty_pages.find(page_index);
        if (dirty != value.dirty_pages.end()) {
            std::copy_n(dirty->second->data() + page_offset, chunk, destination.data());
        } else {
            std::fill(destination.begin(), destination.end(), std::byte{});
            if (value.base && position < value.base_limit) {
                const std::size_t base_chunk = static_cast<std::size_t>(
                    std::min<std::uint64_t>(chunk, value.base_limit - position));
                const std::size_t got = value.base->read(position, destination.first(base_chunk));
                if (got != base_chunk) {
                    fail(std::errc::io_error, "short read from Steam2 file");
                }
            }
        }
        completed += chunk;
    }
    return completed;
}

std::size_t VirtualTree::write(
    const NodeHandle& node,
    std::uint64_t offset,
    std::span<const std::byte> input) {
    std::unique_lock lock(impl_->mutex);
    Node& value = impl_->checked(node);
    if (value.directory) {
        fail(std::errc::is_a_directory, "cannot write a directory");
    }
    if (input.empty()) {
        return 0;
    }
    if (input.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
        fail(std::errc::value_too_large, "write range overflows file size");
    }

    const std::uint64_t final_size = offset + input.size();
    const std::uint64_t first_page = offset / page_size;
    const std::uint64_t last_page = (final_size - 1) / page_size;
    std::uint64_t needed_pages = 0;
    for (std::uint64_t page = first_page;; ++page) {
        if (!value.dirty_pages.contains(page)) {
            ++needed_pages;
        }
        if (page == last_page) {
            break;
        }
    }
    const std::uint64_t dirty = impl_->dirty_bytes->load(std::memory_order_relaxed);
    if (needed_pages > (impl_->write_quota - std::min(impl_->write_quota, dirty)) / page_size) {
        fail(std::errc::no_space_on_device, "RAM overlay quota exceeded");
    }

    std::vector<std::pair<std::uint64_t, std::unique_ptr<Node::Page>>> staged;
    staged.reserve(static_cast<std::size_t>(needed_pages));
    for (std::uint64_t page = first_page;; ++page) {
        if (!value.dirty_pages.contains(page)) {
            auto contents = std::make_unique<Node::Page>();
            contents->fill(std::byte{});
            const std::uint64_t page_start = page * page_size;
            const std::uint64_t preserved_end = std::min(value.size, value.base_limit);
            if (value.base && page_start < preserved_end) {
                const std::size_t amount = static_cast<std::size_t>(
                    std::min<std::uint64_t>(page_size, preserved_end - page_start));
                const bool write_covers_preserved =
                    offset <= page_start && final_size - page_start >= amount;
                if (!write_covers_preserved) {
                    const std::size_t got =
                        value.base->read(page_start, std::span<std::byte>(*contents).first(amount));
                    if (got != amount) {
                        fail(std::errc::io_error, "short read while hydrating a dirty page");
                    }
                }
            }
            staged.emplace_back(page, std::move(contents));
        }
        if (page == last_page) {
            break;
        }
    }
    for (auto& [page, contents] : staged) {
        value.dirty_pages.emplace(page, std::move(contents));
        impl_->dirty_bytes->fetch_add(page_size, std::memory_order_relaxed);
    }

    std::size_t completed = 0;
    while (completed < input.size()) {
        const std::uint64_t position = offset + completed;
        const std::uint64_t page = position / page_size;
        const std::size_t page_offset = static_cast<std::size_t>(position % page_size);
        const std::size_t chunk = std::min<std::size_t>(
            input.size() - completed, static_cast<std::size_t>(page_size) - page_offset);
        auto& destination = *value.dirty_pages.at(page);
        std::copy_n(input.data() + completed, chunk, destination.data() + page_offset);
        completed += chunk;
    }
    value.size = std::max(value.size, final_size);
    value.write_time = std::chrono::system_clock::now();
    value.attributes &= ~file_attribute_normal;
    value.attributes |= file_attribute_archive;
    return input.size();
}

void VirtualTree::resize(const NodeHandle& node, std::uint64_t size) {
    std::unique_lock lock(impl_->mutex);
    Node& value = impl_->checked(node);
    if (value.directory) {
        fail(std::errc::is_a_directory, "cannot resize a directory");
    }
    if (size < value.size) {
        value.base_limit = std::min(value.base_limit, size);
        const std::uint64_t first_removed =
            size / page_size + static_cast<std::uint64_t>(size % page_size != 0);
        for (auto iterator = value.dirty_pages.begin(); iterator != value.dirty_pages.end();) {
            if (iterator->first >= first_removed) {
                iterator = value.dirty_pages.erase(iterator);
                impl_->dirty_bytes->fetch_sub(page_size, std::memory_order_relaxed);
            } else {
                ++iterator;
            }
        }
        const std::size_t boundary = static_cast<std::size_t>(size % page_size);
        if (boundary != 0) {
            const auto page = value.dirty_pages.find(size / page_size);
            if (page != value.dirty_pages.end()) {
                std::fill(page->second->begin() + boundary, page->second->end(), std::byte{});
            }
        }
    }
    value.size = size;
    value.write_time = std::chrono::system_clock::now();
    value.attributes &= ~file_attribute_normal;
    value.attributes |= file_attribute_archive;
}

void VirtualTree::remove(const NodeHandle& node) {
    std::unique_lock lock(impl_->mutex);
    Node& value = impl_->checked(node);
    if (node == impl_->root) {
        fail(std::errc::permission_denied, "cannot remove the virtual root");
    }
    if (!value.linked) {
        fail(std::errc::no_such_file_or_directory, "node is already unlinked");
    }
    if (value.directory && !value.children.empty()) {
        fail(std::errc::directory_not_empty, "cannot remove a non-empty directory");
    }
    auto parent = value.parent.lock();
    if (!parent) {
        fail(std::errc::no_such_file_or_directory, "node has no linked parent");
    }
    parent->children.erase(value.name);
    parent->tombstones.insert(value.name);
    value.parent.reset();
    value.linked = false;
}

void VirtualTree::rename(const NodeHandle& node, std::wstring_view new_path, bool replace) {
    const auto components = path_components(new_path);
    std::unique_lock lock(impl_->mutex);
    Node& value = impl_->checked(node);
    if (node == impl_->root) {
        fail(std::errc::permission_denied, "cannot rename the virtual root");
    }
    if (!value.linked) {
        fail(std::errc::no_such_file_or_directory, "cannot rename an unlinked node");
    }
    auto old_parent = value.parent.lock();
    if (!old_parent) {
        fail(std::errc::no_such_file_or_directory, "node has no linked parent");
    }
    auto [new_parent, new_name] = impl_->parent_locked(components);

    if (value.directory) {
        for (NodeHandle ancestor = new_parent; ancestor; ancestor = ancestor->parent.lock()) {
            if (ancestor == node) {
                fail(std::errc::invalid_argument, "cannot move a directory into itself");
            }
        }
    }

    auto target = new_parent->children.find(new_name);
    if (target != new_parent->children.end() && target->second != node) {
        if (!replace) {
            fail(std::errc::file_exists, "rename target already exists");
        }
        Node& replaced = *target->second;
        if (replaced.directory != value.directory) {
            fail(replaced.directory ? std::errc::is_a_directory : std::errc::not_a_directory,
                 "rename target has a different type");
        }
        if (replaced.directory && !replaced.children.empty()) {
            fail(std::errc::directory_not_empty, "cannot replace a non-empty directory");
        }
        replaced.linked = false;
        replaced.parent.reset();
        new_parent->children.erase(target);
    }

    const bool same_slot = old_parent == new_parent && equal_name(value.name, new_name);
    if (same_slot) {
        old_parent->children.erase(value.name);
    } else {
        old_parent->children.erase(value.name);
        old_parent->tombstones.insert(value.name);
    }
    value.name = std::move(new_name);
    value.parent = new_parent;
    new_parent->tombstones.erase(value.name);
    new_parent->children.emplace(value.name, node);
}

void VirtualTree::set_attributes(const NodeHandle& node, std::uint32_t attributes) {
    std::unique_lock lock(impl_->mutex);
    Node& value = impl_->checked(node);
    value.attributes = attributes;
}

void VirtualTree::set_times(
    const NodeHandle& node,
    std::optional<std::chrono::system_clock::time_point> creation,
    std::optional<std::chrono::system_clock::time_point> access,
    std::optional<std::chrono::system_clock::time_point> write) {
    std::unique_lock lock(impl_->mutex);
    Node& value = impl_->checked(node);
    if (creation) {
        value.creation_time = *creation;
    }
    if (access) {
        value.access_time = *access;
    }
    if (write) {
        value.write_time = *write;
    }
}

std::uint64_t VirtualTree::write_quota() const noexcept {
    return impl_->write_quota;
}

std::uint64_t VirtualTree::dirty_bytes() const noexcept {
    return impl_->dirty_bytes->load(std::memory_order_relaxed);
}

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        fail(std::errc::value_too_large, "UTF-8 string is too large");
    }
    const int input_size = static_cast<int>(value.size());
    const int output_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0);
    if (output_size == 0) {
        fail(std::errc::illegal_byte_sequence, "invalid UTF-8 string");
    }
    std::wstring result(static_cast<std::size_t>(output_size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, result.data(), output_size) == 0) {
        fail(std::errc::illegal_byte_sequence, "invalid UTF-8 string");
    }
    return result;
}

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        fail(std::errc::value_too_large, "UTF-16 string is too large");
    }
    const int input_size = static_cast<int>(value.size());
    const int output_size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0, nullptr, nullptr);
    if (output_size == 0) {
        fail(std::errc::illegal_byte_sequence, "invalid UTF-16 string");
    }
    std::string result(static_cast<std::size_t>(output_size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size,
            result.data(), output_size, nullptr, nullptr) == 0) {
        fail(std::errc::illegal_byte_sequence, "invalid UTF-16 string");
    }
    return result;
}

} // namespace s2fs
