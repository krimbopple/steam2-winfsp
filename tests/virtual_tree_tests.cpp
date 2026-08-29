#include "s2fs/virtual_tree.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using s2fs::NodeHandle;
using s2fs::VirtualTree;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const char value : text) {
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return result;
}

std::string text(std::span<const std::byte> input) {
    std::string result;
    result.reserve(input.size());
    for (const std::byte value : input) {
        result.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
    }
    return result;
}

template <class Function>
void require_error(std::errc expected, Function&& function, const char* message) {
    try {
        function();
    } catch (const std::system_error& error) {
        require(error.code() == std::make_error_code(expected), message);
        return;
    }
    throw std::runtime_error(message);
}
void test_default_ephemeral_quota() {
    VirtualTree tree;
    require(tree.write_quota() == (512ULL << 20), "default RAM overlay quota is not 512 MiB");
    require(tree.dirty_bytes() == 0, "a new ephemeral overlay contains dirty data");
}


void test_new_file_and_partial_writes() {
    VirtualTree tree;
    const NodeHandle directory = tree.create(L"/Games", true, false);
    const NodeHandle file = tree.create(L"\\Games/mixed.bin", false, false);

    const auto initial = bytes("abcdefgh");
    require(tree.write(file, 0, initial) == initial.size(), "initial write length is wrong");
    const auto patch = bytes("XY");
    require(tree.write(file, 3, patch) == patch.size(), "partial write length is wrong");

    std::array<std::byte, 8> output{};
    require(tree.read(file, 0, output) == output.size(), "new file read length is wrong");
    require(text(output) == "abcXYfgh", "partial write did not preserve untouched bytes");
    require(tree.info(file).size == 8, "new file size is wrong");
    require(tree.lookup(L"/games/MIXED.BIN") == file, "ASCII case-insensitive lookup failed");
    require(tree.lookup(L"/") != nullptr, "root slash lookup failed");
    require(tree.lookup(L"\\") != nullptr, "root backslash lookup failed");

    const auto entries = tree.list(directory);
    require(entries.size() == 1, "directory enumeration count is wrong");
    require(entries.front().name == L"mixed.bin", "directory enumeration lost display case");
    require(!entries.front().info.directory && entries.front().info.size == 8,
            "directory enumeration metadata is wrong");
}

void test_unicode_case_folding_and_encoding() {
    VirtualTree tree;
    const NodeHandle directory = tree.create(L"/\u00c4pfel", true, false);
    require(tree.lookup(L"/\u00e4PFEL") == directory, "Unicode ordinal case folding failed");

    const std::string utf8 =
        "Gr\xC3\xBC\xC3\x9F" "e/\xE6\x9D\xB1\xE4\xBA\xAC/\xF0\x9F\x98\x80";
    require(s2fs::wide_to_utf8(s2fs::utf8_to_wide(utf8)) == utf8,
            "UTF conversion did not round-trip");
    require(s2fs::utf8_to_wide("").empty(), "empty UTF-8 did not convert to an empty wide string");
    require(s2fs::wide_to_utf8(L"").empty(), "empty wide string did not convert to empty UTF-8");

    const auto require_invalid_utf8 = [](std::string_view input) {
        require_error(
            std::errc::illegal_byte_sequence,
            [input] { static_cast<void>(s2fs::utf8_to_wide(input)); },
            "invalid UTF-8 was accepted");
    };
    require_invalid_utf8("\xC3\x28");
    require_invalid_utf8("\xC0\x80");
    require_invalid_utf8("\xED\xA0\x80");
    require_invalid_utf8("\xF4\x90\x80\x80");
    require_invalid_utf8("\xE2\x82");

#ifdef _WIN32
    const std::wstring invalid_wide(1, static_cast<wchar_t>(0xd800));
#else
    const std::wstring invalid_wide(1, static_cast<wchar_t>(0x110000));
#endif
    require_error(
        std::errc::illegal_byte_sequence,
        [&invalid_wide] { static_cast<void>(s2fs::wide_to_utf8(invalid_wide)); },
        "invalid wide string was accepted");
}

void test_sparse_write_and_truncation() {
    VirtualTree tree;
    const NodeHandle file = tree.create(L"/sparse.bin", false, false);
    const auto payload = bytes("tail");
    constexpr std::uint64_t offset = 2ULL * 32ULL * 1024ULL + 17ULL;
    tree.write(file, offset, payload);
    require(tree.info(file).size == offset + payload.size(), "sparse extension size is wrong");

    std::array<std::byte, 24> around_gap{};
    require(tree.read(file, offset - 8, around_gap) == 12, "sparse tail read length is wrong");
    require(std::all_of(around_gap.begin(), around_gap.begin() + 8,
                        [](std::byte value) { return value == std::byte{}; }),
            "sparse gap is not zero-filled");
    require(text(std::span(around_gap).subspan(8, 4)) == "tail", "sparse payload is wrong");

    tree.resize(file, 3);
    require(tree.info(file).size == 3, "truncate size is wrong");
    tree.resize(file, 40);
    std::array<std::byte, 40> expanded{};
    std::fill(expanded.begin(), expanded.end(), std::byte{0xff});
    require(tree.read(file, 0, expanded) == expanded.size(), "expanded file read length is wrong");
    require(std::all_of(expanded.begin(), expanded.end(),
                        [](std::byte value) { return value == std::byte{}; }),
            "bytes beyond truncation reappeared after extension");
    require(tree.dirty_bytes() == 0,
            "truncation did not release out-of-range dirty pages");
}

void test_rename_replace_delete_and_stable_handles() {
    VirtualTree tree;
    const NodeHandle first = tree.create(L"/first.txt", false, false);
    tree.write(first, 0, bytes("first"));
    tree.rename(first, L"/Renamed.txt", false);
    require(!tree.lookup(L"/first.txt"), "rename left the source linked");
    require(tree.lookup(L"/renamed.TXT") == first, "rename target lookup failed");

    tree.rename(first, L"/RENAMED.TXT", false);
    const auto root_entries = tree.list(tree.lookup(L"/"));
    require(root_entries.size() == 1 && root_entries.front().name == L"RENAMED.TXT",
            "case-only rename did not update display case");

    const NodeHandle replaced = tree.create(L"/victim.txt", false, false);
    tree.write(replaced, 0, bytes("victim"));
    tree.rename(first, L"/victim.txt", true);
    require(tree.lookup(L"/victim.txt") == first, "replace rename did not install source");
    std::array<std::byte, 6> old_contents{};
    require(tree.read(replaced, 0, old_contents) == old_contents.size(),
            "replaced shared handle became unusable");
    require(text(old_contents) == "victim", "replaced shared handle contents changed");

    tree.remove(first);
    require(!tree.lookup(L"/victim.txt"), "removed file is still visible");
    std::array<std::byte, 5> open_contents{};
    require(tree.read(first, 0, open_contents) == open_contents.size(),
            "removed shared handle became unusable");
    require(text(open_contents) == "first", "removed shared handle contents changed");
    require_error(std::errc::no_such_file_or_directory,
                  [&] { tree.remove(first); },
                  "removing an unlinked handle did not fail");
}

void test_attributes_and_times() {
    VirtualTree tree;
    const NodeHandle file = tree.create(L"/metadata", false, false);
    tree.set_attributes(file, 0x1234U);
    const auto creation = std::chrono::system_clock::time_point(std::chrono::seconds(10));
    const auto access = std::chrono::system_clock::time_point(std::chrono::seconds(20));
    const auto write = std::chrono::system_clock::time_point(std::chrono::seconds(30));
    tree.set_times(file, creation, access, write);
    const auto info = tree.info(file);
    require(info.attributes == 0x1234U, "attributes were not stored");
    require(info.creation_time == creation && info.access_time == access && info.write_time == write,
            "timestamps were not stored");
}

void test_quota_is_hard_and_transactional() {
    constexpr std::uint64_t page = 32ULL * 1024ULL;
    VirtualTree tree(page);
    const NodeHandle file = tree.create(L"/quota.bin", false, false);
    const auto one = bytes("x");
    tree.write(file, 0, one);
    require(tree.dirty_bytes() == page, "first dirty page accounting is wrong");

    require_error(std::errc::no_space_on_device,
                  [&] { tree.write(file, page, one); },
                  "write beyond quota did not fail with no-space");
    require(tree.dirty_bytes() == page, "failed write exceeded dirty-byte quota");
    require(tree.info(file).size == 1, "failed quota write changed file size");

    tree.resize(file, 0);
    require(tree.dirty_bytes() == 0, "truncate did not return dirty-page quota");
    tree.write(file, page, one);
    require(tree.dirty_bytes() == page, "returned quota could not be reused");
}

void run_all() {
    test_default_ephemeral_quota();
    test_new_file_and_partial_writes();
    test_unicode_case_folding_and_encoding();
    test_sparse_write_and_truncation();
    test_rename_replace_delete_and_stable_handles();
    test_attributes_and_times();
    test_quota_is_hard_and_transactional();
}

} // namespace

int main() {
    try {
        run_all();
        std::cout << "virtual_tree_tests: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "virtual_tree_tests: " << error.what() << '\n';
        return 1;
    }
}
