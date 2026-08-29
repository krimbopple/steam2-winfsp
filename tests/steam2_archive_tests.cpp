#include "s2fs/steam2_archive.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;
constexpr std::uint32_t kDepot = 242;
constexpr std::uint32_t kFileId = 7;
constexpr std::uint32_t kBlockSize = 0x8000;

void append_u16(Bytes& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xff));
    bytes.push_back(static_cast<std::byte>((value >> 8) & 0xff));
}

void append_u32(Bytes& bytes, std::uint32_t value) {
    for (int shift = 0; shift != 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    }
}

void append_u64(Bytes& bytes, std::uint64_t value) {
    for (int shift = 0; shift != 64; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    }
}

void set_u32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (int shift = 0; shift != 32; shift += 8) {
        bytes.at(offset++) = static_cast<std::byte>((value >> shift) & 0xff);
    }
}

Bytes zlib_compress(std::span<const std::byte> input) {
    uLongf size = compressBound(static_cast<uLong>(input.size()));
    Bytes output(size);
    if (compress2(reinterpret_cast<Bytef*>(output.data()), &size,
            reinterpret_cast<const Bytef*>(input.data()), static_cast<uLong>(input.size()), Z_BEST_SPEED) != Z_OK) {
        throw std::runtime_error("fixture compression failed");
    }
    output.resize(size);
    return output;
}

void bcrypt_check(NTSTATUS status, const char* what) {
    if (!BCRYPT_SUCCESS(status)) {
        throw std::runtime_error(std::string("fixture ") + what + " failed");
    }
}

Bytes aes_cfb_encrypt(std::span<const std::byte> input, const std::array<std::uint8_t, 16>& key) {
    BCRYPT_ALG_HANDLE algorithm{};
    bcrypt_check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0), "open AES");
    struct AlgorithmCloser {
        BCRYPT_ALG_HANDLE value;
        ~AlgorithmCloser() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
    } algorithm_closer{algorithm};
    const wchar_t ecb[] = BCRYPT_CHAIN_MODE_ECB;
    bcrypt_check(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(ecb)), sizeof(ecb), 0), "set ECB");

    DWORD object_size{};
    DWORD copied{};
    bcrypt_check(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0), "get key size");
    std::vector<UCHAR> object(object_size);
    BCRYPT_KEY_HANDLE key_handle{};
    bcrypt_check(BCryptGenerateSymmetricKey(algorithm, &key_handle, object.data(), object_size,
        const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0), "make key");
    struct KeyCloser {
        BCRYPT_KEY_HANDLE value;
        ~KeyCloser() { if (value) BCryptDestroyKey(value); }
    } key_closer{key_handle};

    std::array<UCHAR, 16> feedback{};
    std::array<UCHAR, 16> stream_block{};
    Bytes output(input.size());
    for (std::size_t offset = 0; offset < input.size(); offset += feedback.size()) {
        ULONG actual{};
        bcrypt_check(BCryptEncrypt(key_handle, feedback.data(), static_cast<ULONG>(feedback.size()),
            nullptr, nullptr, 0, stream_block.data(), static_cast<ULONG>(stream_block.size()),
            &actual, 0), "encrypt CFB keystream");
        if (actual != stream_block.size()) throw std::runtime_error("fixture short AES block");
        const auto count = std::min(feedback.size(), input.size() - offset);
        for (std::size_t index = 0; index < count; ++index) {
            const auto plain = std::to_integer<unsigned char>(input[offset + index]);
            const auto cipher = static_cast<unsigned char>(plain ^ stream_block[index]);
            output[offset + index] = static_cast<std::byte>(cipher);
            feedback[index] = cipher;
        }
    }
    return output;
}

Bytes make_blob(const std::vector<std::pair<std::uint32_t, Bytes>>& fields) {
    Bytes bytes;
    append_u16(bytes, 0x5001);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    for (const auto& [key, value] : fields) {
        append_u16(bytes, 4);
        append_u32(bytes, static_cast<std::uint32_t>(value.size()));
        append_u32(bytes, key);
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    set_u32(bytes, 2, static_cast<std::uint32_t>(bytes.size()));
    return bytes;
}

Bytes scalar32(std::uint32_t value) {
    Bytes result;
    append_u32(result, value);
    return result;
}

Bytes scalar64(std::uint64_t value) {
    Bytes result;
    append_u64(result, value);
    return result;
}

Bytes compressed_blob(std::span<const std::byte> unpacked) {
    auto packed = zlib_compress(unpacked);
    Bytes result;
    append_u16(result, 0x4301);
    append_u64(result, packed.size());
    append_u64(result, unpacked.size());
    append_u16(result, 6);
    result.insert(result.end(), packed.begin(), packed.end());
    return result;
}

Bytes make_manifest(std::uint32_t manifest_version, std::uint32_t depot_version) {
    const std::string strings("\0folder\0payload.bin\0", 20);
    Bytes manifest;
    append_u32(manifest, manifest_version);
    append_u32(manifest, kDepot);
    append_u32(manifest, depot_version);
    append_u32(manifest, 3); // nodes
    append_u32(manifest, 1); // files
    append_u32(manifest, kBlockSize);
    append_u32(manifest, 0); // binary size, patched below
    append_u32(manifest, static_cast<std::uint32_t>(strings.size()));
    append_u32(manifest, 0);
    append_u32(manifest, 0);
    append_u32(manifest, 0);
    append_u32(manifest, 0);
    append_u32(manifest, 0); // fingerprint
    append_u32(manifest, 0); // checksum

    // Root node.
    append_u32(manifest, 0); append_u32(manifest, 0); append_u32(manifest, 0);
    append_u32(manifest, 0); append_u32(manifest, 0xffffffffU);
    append_u32(manifest, 0xffffffffU); append_u32(manifest, 1);
    // Directory node "folder".
    append_u32(manifest, 1); append_u32(manifest, 0); append_u32(manifest, 0);
    append_u32(manifest, 0); append_u32(manifest, 0);
    append_u32(manifest, 0xffffffffU); append_u32(manifest, 2);
    // File node "payload.bin".
    append_u32(manifest, 8); append_u32(manifest, 0); append_u32(manifest, kFileId);
    append_u32(manifest, 1); append_u32(manifest, 1);
    append_u32(manifest, 0xffffffffU); append_u32(manifest, 0xffffffffU);
    manifest.insert(manifest.end(), reinterpret_cast<const std::byte*>(strings.data()),
        reinterpret_cast<const std::byte*>(strings.data() + strings.size()));

    set_u32(manifest, 6 * 4, static_cast<std::uint32_t>(manifest.size()));
    const auto checksum = adler32(0, reinterpret_cast<const Bytef*>(manifest.data()),
        static_cast<uInt>(manifest.size()));
    set_u32(manifest, 13 * 4, checksum);
    return manifest;
}

struct EncodedFile {
    Bytes dat;
    std::vector<std::uint32_t> block_sizes;
};

EncodedFile encode_file(std::span<const std::byte> input, std::uint8_t mode) {
    EncodedFile result;
    const auto key = s2fs::depot_key(kDepot);
    for (std::size_t offset = 0; offset < input.size(); offset += kBlockSize) {
        const auto plain = input.subspan(offset, std::min<std::size_t>(kBlockSize, input.size() - offset));
        Bytes encoded;
        if (mode == 1) {
            encoded = zlib_compress(plain);
        } else if (mode == 2) {
            const auto compressed = zlib_compress(plain);
            const auto encrypted = aes_cfb_encrypt(compressed, key);
            append_u32(encoded, static_cast<std::uint32_t>(encrypted.size()));
            append_u32(encoded, static_cast<std::uint32_t>(plain.size()));
            encoded.insert(encoded.end(), encrypted.begin(), encrypted.end());
        } else if (mode == 3) {
            encoded = aes_cfb_encrypt(plain, key);
        } else {
            throw std::logic_error("bad fixture mode");
        }
        result.block_sizes.push_back(static_cast<std::uint32_t>(encoded.size()));
        result.dat.insert(result.dat.end(), encoded.begin(), encoded.end());
    }
    return result;
}

Bytes make_checksum_table(std::span<const std::byte> plain, const EncodedFile& encoded,
    std::uint8_t mode, std::uint32_t table_version, std::uint64_t archive_offset) {
    Bytes table;
    append_u32(table, 0x34457234);
    append_u32(table, table_version);
    append_u32(table, 1);
    append_u32(table, 1);
    append_u32(table, 0x20);
    append_u32(table, 0x30);
    append_u32(table, kBlockSize);
    append_u32(table, static_cast<std::uint32_t>(encoded.block_sizes.size()));
    append_u32(table, kFileId);
    append_u32(table, 1);
    append_u32(table, 0x30);
    append_u32(table, 0);
    if (table_version == 0) {
        append_u32(table, static_cast<std::uint32_t>(plain.size()));
        append_u32(table, static_cast<std::uint32_t>(archive_offset));
    } else {
        append_u64(table, plain.size());
        append_u64(table, archive_offset);
    }
    append_u32(table, (static_cast<std::uint32_t>(mode) << 24) |
        static_cast<std::uint32_t>(encoded.block_sizes.size()));
    for (const auto size : encoded.block_sizes) {
        append_u32(table, size);
        append_u32(table, 0); // legacy checksum is not needed for decoding
    }
    append_u32(table, 0x34457234);
    return table;
}

struct TempDirectory {
    TempDirectory() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
            ("s2fs_archive_test_" + std::to_string(stamp));
        std::filesystem::create_directories(path / "blobs");
        std::filesystem::create_directories(path / "dats");
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

void write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("fixture write failed");
}

struct FixtureOptions {
    std::uint8_t mode{1};
    std::uint32_t checksum_version{0};
    std::uint32_t manifest_version{3};
    std::uint32_t depot_blob_format{3};
    std::uint32_t depot_version{17};
    std::uint32_t filename_crc{0x1234abcd};
    std::uint32_t parent_crc{};
    std::uint64_t archive_offset{};
};

s2fs::DepotSpec write_fixture(TempDirectory& directory, std::span<const std::byte> plain,
    FixtureOptions options = {}) {
    const auto encoded = encode_file(plain, options.mode);
    const auto checksum = make_checksum_table(
        plain, encoded, options.mode, options.checksum_version, options.archive_offset);
    const auto manifest = make_manifest(options.manifest_version, options.depot_version);
    const auto nested = make_blob({{0, manifest}});
    std::vector<std::pair<std::uint32_t, Bytes>> fields;
    fields.emplace_back(0, scalar32(options.depot_blob_format));
    fields.emplace_back(3, compressed_blob(nested));
    fields.emplace_back(4, checksum);
    fields.emplace_back(12, scalar32(options.parent_crc));
    fields.emplace_back(13, options.depot_blob_format == 3
        ? scalar32(static_cast<std::uint32_t>(encoded.dat.size()))
        : scalar64(encoded.dat.size()));
    const auto blob = make_blob(fields);

    const auto crc = options.filename_crc;
    const auto crc_text = [&] {
        constexpr char hex[] = "0123456789abcdef";
        std::string value(8, '0');
        auto remaining = crc;
        for (int index = 7; index >= 0; --index) {
            value[index] = hex[remaining & 0xf];
            remaining >>= 4;
        }
        return value;
    }();
    write_bytes(directory.path / "blobs" /
        (std::to_string(kDepot) + "_" + std::to_string(options.depot_version) + "_" + crc_text + "_fixture.blob"), blob);
    write_bytes(directory.path / "dats" /
        (std::to_string(kDepot) + "_" + std::to_string(options.depot_version) + "_fixture.dat"), encoded.dat);
    return {kDepot, options.depot_version, crc,
        directory.path / "blobs", directory.path / "dats", {}, {}, "mount"};
}
s2fs::DepotSpec write_zero_size_metadata_fixture(
    TempDirectory& directory,
    std::uint8_t mode,
    bool logical_file_is_nonempty = false) {
    Bytes logical;
    if (logical_file_is_nonempty) {
        logical.push_back(std::byte{0x42});
    }
    EncodedFile encoded;
    encoded.block_sizes.push_back(0);
    const auto checksum = make_checksum_table(logical, encoded, mode, 0, 0);

    const auto manifest = make_manifest(3, 0);
    const auto nested = make_blob({{0, manifest}});
    const auto blob = make_blob({
        {0, scalar32(3)},
        {3, compressed_blob(nested)},
        {4, checksum},
        {12, scalar32(0)},
        {13, scalar32(static_cast<std::uint32_t>(encoded.dat.size()))},
    });
    constexpr std::uint32_t crc = 0x0badf00d;
    write_bytes(
        directory.path / "blobs" / "242_0_0badf00d_fixture.blob",
        blob);
    write_bytes(
        directory.path / "dats" / "242_0_fixture.dat",
        encoded.dat);
    return {
        kDepot,
        0,
        crc,
        directory.path / "blobs",
        directory.path / "dats",
        {},
        {},
        "",
    };
}


Bytes sample_data(std::size_t size) {
    Bytes data(size);
    for (std::size_t index = 0; index < size; ++index) {
        data[index] = static_cast<std::byte>((index * 37 + index / 13) & 0xff);
    }
    return data;
}

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template<class Exception = std::exception, class Function>
void require_throws(Function&& function, std::string_view contains) {
    try {
        function();
    } catch (const Exception& error) {
        require(std::string_view(error.what()).find(contains) != std::string_view::npos,
            "exception text did not identify the failure");
        return;
    }
    throw std::runtime_error("expected exception was not thrown");
}

void test_mode_and_range(std::uint8_t mode, std::uint32_t checksum_version,
    std::uint32_t manifest_version, std::uint32_t blob_format) {
    TempDirectory directory;
    const auto plain = sample_data(kBlockSize + 913);
    auto spec = write_fixture(directory, plain,
        {mode, checksum_version, manifest_version, blob_format});
    const auto depot = s2fs::Steam2Depot::load(spec);
    require(depot.id() == kDepot && depot.version() == 17, "depot identity mismatch");
    require(depot.entries().size() == 1, "fixture entry missing");
    require(depot.entries()[0].path == "mount/folder/payload.bin", "manifest path mismatch");
    const auto file = depot.entries()[0].file;
    require(file->size() == plain.size(), "file size mismatch");

    Bytes range(1400);
    const auto start = kBlockSize - 600;
    require(file->read(start, range) == range.size(), "cross-block read was short");
    require(std::equal(range.begin(), range.end(), plain.begin() + start), "cross-block range was decoded incorrectly");
    Bytes tail(2000, std::byte{0x5a});
    require(file->read(plain.size() - 100, tail) == 100, "EOF read did not clamp");
    require(std::equal(tail.begin(), tail.begin() + 100, plain.end() - 100), "EOF range mismatch");
    require(file->read(plain.size(), tail) == 0, "read at EOF was not empty");
    const auto concurrent_slice = [&plain, file](std::size_t offset) {
        Bytes bytes(777);
        return file->read(offset, bytes) == bytes.size() &&
            std::equal(bytes.begin(), bytes.end(), plain.begin() + offset);
    };
    auto first = std::async(std::launch::async, concurrent_slice, 111);
    auto second = std::async(std::launch::async, concurrent_slice, kBlockSize - 250);
    require(first.get() && second.get(), "concurrent DAT reads interfered");
}

void test_ancestry_order_replaces_mapping() {
    TempDirectory directory;
    auto old_data = sample_data(700);
    auto new_data = sample_data(700);
    std::reverse(new_data.begin(), new_data.end());
    (void)write_fixture(directory, old_data, {1, 0, 3, 3, 16, 0xfeed1234, 0});
    auto spec = write_fixture(directory, new_data, {1, 0, 3, 3, 17, 0x1234abcd, 0xfeed1234});
    const auto depot = s2fs::Steam2Depot::load(spec);
    Bytes actual(new_data.size());
    require(depot.entries()[0].file->read(0, actual) == actual.size(), "ancestry read was short");
    require(actual == new_data, "newer checksum-table mapping did not replace its ancestor");
}

void test_missing_ancestry() {
    TempDirectory directory;
    const auto plain = sample_data(100);
    auto spec = write_fixture(directory, plain, {1, 0, 3, 3, 17, 0x1234abcd, 0xfeed1234});
    require_throws<std::runtime_error>([&] { (void)s2fs::Steam2Depot::load(spec); }, "missing Steam2 blob ancestry");
}

void test_truncated_container() {
    TempDirectory directory;
    const auto plain = sample_data(100);
    auto spec = write_fixture(directory, plain);
    const auto path = directory.path / "blobs" / "242_17_1234abcd_fixture.blob";
    auto bytes = Bytes(9, std::byte{});
    bytes[0] = std::byte{1}; bytes[1] = std::byte{0x50};
    write_bytes(path, bytes);
    require_throws<std::runtime_error>([&] { (void)s2fs::Steam2Depot::load(spec); }, "malformed Steam2 metadata");
}

void test_unsupported_checksum_version() {
    TempDirectory directory;
    const auto plain = sample_data(100);
    auto spec = write_fixture(directory, plain, {1, 9, 3, 3});
    require_throws<std::invalid_argument>([&] { (void)s2fs::Steam2Depot::load(spec); }, "checksum-table version 9");
}
void test_unsupported_manifest_version() {
    TempDirectory directory;
    const auto plain = sample_data(100);
    auto spec = write_fixture(directory, plain, {1, 0, 8, 3});
    require_throws<std::invalid_argument>(
        [&] { (void)s2fs::Steam2Depot::load(spec); }, "manifest version 8");
}


void test_dat_size_pairing() {
    TempDirectory directory;
    const auto plain = sample_data(100);
    auto spec = write_fixture(directory, plain);
    const Bytes extra(1, std::byte{0xaa});
    std::ofstream output(directory.path / "dats" / "242_17_fixture.dat", std::ios::binary | std::ios::app);
    output.write(reinterpret_cast<const char*>(extra.data()), 1);
    output.close();
    require_throws<std::runtime_error>([&] { (void)s2fs::Steam2Depot::load(spec); }, "embedded expected size");
}

void test_missing_archive_bytes_are_rejected() {
    TempDirectory directory;
    const auto plain = sample_data(100);
    auto spec = write_fixture(directory, plain, {1, 0, 3, 3, 17, 0x1234abcd, 0, 1});
    require_throws<std::runtime_error>(
        [&] { (void)s2fs::Steam2Depot::load(spec); }, "block exceeds its paired DAT");
}

void test_zero_size_metadata_entry() {
    TempDirectory directory;
    auto spec = write_zero_size_metadata_fixture(directory, 1);
    const auto depot = s2fs::Steam2Depot::load(spec);
    require(depot.entries().size() == 1, "zero-size metadata entry manifest file missing");
    require(depot.entries()[0].file->size() == 0, "zero-size metadata entry became non-empty");
    std::array<std::byte, 1> output{};
    require(
        depot.entries()[0].file->read(0, output) == 0,
        "zero-size metadata entry returned archive payload bytes");
}

void test_other_zero_size_block_mismatch_is_rejected() {
    TempDirectory directory;
    auto spec = write_zero_size_metadata_fixture(directory, 2);
    require_throws<std::runtime_error>(
        [&] { (void)s2fs::Steam2Depot::load(spec); },
        "file size and block count disagree");
}

void test_nonempty_zero_payload_block_fails_on_read() {
    TempDirectory directory;
    auto spec = write_zero_size_metadata_fixture(directory, 1, true);
    const auto depot = s2fs::Steam2Depot::load(spec);
    require(depot.entries().size() == 1, "missing-payload manifest file missing");
    require(depot.entries()[0].file->size() == 1, "missing-payload file size is wrong");
    std::array<std::byte, 1> output{};
    require_throws<std::runtime_error>(
        [&] { (void)depot.entries()[0].file->read(0, output); },
        "archive block with no payload");
}

void test_unknown_key() {
    require_throws<std::out_of_range>([] { (void)s2fs::depot_key(0xffffffffU); }, "no AES key");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"mode 1, v0 table, v3 manifest/blob", [] { test_mode_and_range(1, 0, 3, 3); }},
        {"mode 2, v1 table, v4 manifest/blob", [] { test_mode_and_range(2, 1, 4, 4); }},
        {"mode 3 encrypted", [] { test_mode_and_range(3, 1, 4, 4); }},
        {"ancestry replacement order", test_ancestry_order_replaces_mapping},
        {"missing ancestry", test_missing_ancestry},
        {"truncated container", test_truncated_container},
        {"unsupported manifest", test_unsupported_manifest_version},
        {"unsupported checksum", test_unsupported_checksum_version},
        {"DAT size pairing", test_dat_size_pairing},
        {"missing archive bytes", test_missing_archive_bytes_are_rejected},
        {"zero-size metadata entry", test_zero_size_metadata_entry},
        {"invalid zero-size block mismatch", test_other_zero_size_block_mismatch_is_rejected},
        {"nonempty zero-payload block", test_nonempty_zero_payload_block_fails_on_read},
        {"unknown key", test_unknown_key},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
