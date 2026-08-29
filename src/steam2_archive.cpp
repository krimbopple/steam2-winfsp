#include "s2fs/steam2_archive.hpp"

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif
#include <zlib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <memory>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <unordered_set>


namespace s2fs {
namespace {

constexpr std::uint16_t kBlobMagic = 0x5001;
constexpr std::uint16_t kCompressedBlobMagic = 0x4301;
constexpr std::uint32_t kChecksumMagic = 0x34457234;
constexpr std::uint32_t kBlockSize = 0x8000;

[[noreturn]] void malformed(std::string_view detail) {
    throw std::runtime_error("malformed Steam2 metadata: " + std::string(detail));
}

[[noreturn]] void unsupported(std::string_view detail) {
    throw std::invalid_argument("unsupported Steam2 metadata: " + std::string(detail));
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    template<class T>
    T read() {
        static_assert(std::is_integral_v<T>);
        const auto value = at<T>(position_);
        position_ += sizeof(T);
        return value;
    }

    template<class T>
    T at(std::size_t position) const {
        static_assert(std::is_integral_v<T>);
        if (position > bytes_.size() || bytes_.size() - position < sizeof(T)) {
            malformed("truncated integer");
        }
        using U = std::make_unsigned_t<T>;
        U value = 0;
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            value |= static_cast<U>(std::to_integer<unsigned char>(bytes_[position + index])) << (index * 8);
        }
        return static_cast<T>(value);
    }

    std::span<const std::byte> take(std::size_t size) {
        if (position_ > bytes_.size() || bytes_.size() - position_ < size) {
            malformed("truncated byte range");
        }
        const auto result = bytes_.subspan(position_, size);
        position_ += size;
        return result;
    }

    void seek(std::size_t position) {
        if (position > bytes_.size()) {
            malformed("offset outside metadata");
        }
        position_ = position;
    }

    std::size_t position() const noexcept { return position_; }
    std::size_t size() const noexcept { return bytes_.size(); }

private:
    std::span<const std::byte> bytes_;
    std::size_t position_{};
};

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw std::filesystem::filesystem_error("cannot size Steam2 blob", path, error);
    }
    if (size > std::numeric_limits<std::size_t>::max() ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::length_error("Steam2 blob is too large for this process");
    }
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::filesystem::filesystem_error(
            "cannot open Steam2 blob", path, std::make_error_code(std::errc::io_error));
    }
    if (!result.empty()) {
        input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
        if (input.gcount() != static_cast<std::streamsize>(result.size())) {
            throw std::runtime_error("truncated Steam2 blob file: " + path.string());
        }
    }
    return result;
}

using BlobValues = std::map<std::uint32_t, std::span<const std::byte>>;

BlobValues parse_blob(std::span<const std::byte> bytes) {
    Reader reader(bytes);
    if (reader.read<std::uint16_t>() != kBlobMagic) {
        malformed("bad blob magic");
    }
    const auto total_size = reader.read<std::uint32_t>();
    const auto slack_size = reader.read<std::uint32_t>();
    if (total_size < 10 || total_size > bytes.size() ||
        slack_size > bytes.size() - total_size) {
        malformed("invalid blob size fields");
    }
    const std::size_t entries_end = total_size;

    BlobValues values;
    while (reader.position() < entries_end) {
        const auto key_size = reader.read<std::uint16_t>();
        const auto value_size = reader.read<std::uint32_t>();
        if (key_size != sizeof(std::uint32_t)) {
            unsupported("blob key width is not four bytes");
        }
        const auto key_bytes = reader.take(key_size);
        Reader key_reader(key_bytes);
        const auto key = key_reader.read<std::uint32_t>();
        const auto value = reader.take(value_size);
        if (reader.position() > entries_end) {
            malformed("blob entry extends into slack");
        }
        if (!values.emplace(key, value).second) {
            malformed("duplicate blob key");
        }
    }
    if (reader.position() != entries_end) {
        malformed("blob entries do not end at the declared boundary");
    }
    return values;
}

const std::span<const std::byte>& required_value(const BlobValues& blob, std::uint32_t key) {
    const auto found = blob.find(key);
    if (found == blob.end()) {
        malformed("required blob field " + std::to_string(key) + " is missing");
    }
    return found->second;
}

std::uint32_t value_u32(const BlobValues& blob, std::uint32_t key) {
    const auto value = required_value(blob, key);
    if (value.size() != sizeof(std::uint32_t)) {
        malformed("blob field " + std::to_string(key) + " has the wrong width");
    }
    return Reader(value).read<std::uint32_t>();
}

std::uint64_t value_u64(const BlobValues& blob, std::uint32_t key) {
    const auto value = required_value(blob, key);
    if (value.size() != sizeof(std::uint64_t)) {
        malformed("blob field " + std::to_string(key) + " has the wrong width");
    }
    return Reader(value).read<std::uint64_t>();
}

std::vector<std::byte> decompress_blob(std::span<const std::byte> bytes) {
    Reader reader(bytes);
    if (reader.read<std::uint16_t>() != kCompressedBlobMagic) {
        malformed("bad compressed-blob magic");
    }
    const auto packed_size = reader.read<std::uint64_t>();
    const auto unpacked_size = reader.read<std::uint64_t>();
    (void)reader.read<std::uint16_t>(); // writer's compression-level hint
    const auto payload = reader.take(reader.size() - reader.position());
    if (packed_size != payload.size() && packed_size != bytes.size()) {
        malformed("compressed-blob packed size does not match its payload");
    }
    if (unpacked_size > std::numeric_limits<std::size_t>::max() ||
        unpacked_size > std::numeric_limits<uLongf>::max() ||
        payload.size() > std::numeric_limits<uLong>::max()) {
        throw std::length_error("compressed Steam2 blob is too large");
    }
    std::vector<std::byte> output(static_cast<std::size_t>(unpacked_size));
    uLongf actual = static_cast<uLongf>(output.size());
    const int result = uncompress(
        reinterpret_cast<Bytef*>(output.data()), &actual,
        reinterpret_cast<const Bytef*>(payload.data()), static_cast<uLong>(payload.size()));
    if (result != Z_OK || actual != output.size()) {
        malformed("compressed blob cannot be decompressed to its declared size");
    }
    return output;
}

struct BlobFile {
    BlobFile(
        std::filesystem::path blob_path,
        std::uint32_t blob_version,
        std::optional<std::uint32_t> crc,
        std::vector<std::byte> blob_bytes,
        BlobValues blob_values,
        std::uint32_t parent,
        std::uint64_t dat_size) noexcept
        : path(std::move(blob_path)),
          version(blob_version),
          filename_crc(crc),
          bytes(std::move(blob_bytes)),
          values(std::move(blob_values)),
          parent_crc(parent),
          expected_dat_size(dat_size) {}

    BlobFile(const BlobFile&) = delete;
    BlobFile& operator=(const BlobFile&) = delete;
    BlobFile(BlobFile&&) noexcept = default;
    BlobFile& operator=(BlobFile&&) noexcept = default;

    std::filesystem::path path;
    std::uint32_t version{};
    std::optional<std::uint32_t> filename_crc;
    std::vector<std::byte> bytes;
    BlobValues values;
    std::uint32_t parent_crc{};
    std::uint64_t expected_dat_size{};
};

std::optional<std::uint32_t> parse_decimal(std::string_view text) {
    std::uint32_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint32_t> parse_hex(std::string_view text) {
    if (text.size() != 8) {
        return std::nullopt;
    }
    std::uint32_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

struct ParsedName {
    std::uint32_t version{};
    std::optional<std::uint32_t> crc;
};

std::optional<std::uint32_t> parse_depot_id(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    const auto separator = filename.find('_');
    if (separator == std::string::npos) {
        return std::nullopt;
    }
    return parse_decimal(std::string_view(filename).substr(0, separator));
}

std::optional<ParsedName> parse_candidate_name(
    const std::filesystem::path& path, std::uint32_t depot_id, std::string_view extension) {
    const auto filename = path.filename().string();
    if (filename.size() <= extension.size() ||
        filename.compare(filename.size() - extension.size(), extension.size(), extension) != 0) {
        return std::nullopt;
    }
    const auto stem = std::string_view(filename).substr(0, filename.size() - extension.size());
    const auto first = stem.find('_');
    if (first == std::string_view::npos || parse_decimal(stem.substr(0, first)) != depot_id) {
        return std::nullopt;
    }
    const auto second = stem.find('_', first + 1);
    const auto version_text = stem.substr(first + 1, second == std::string_view::npos
        ? std::string_view::npos : second - first - 1);
    const auto version = parse_decimal(version_text);
    if (!version) {
        return std::nullopt;
    }
    std::optional<std::uint32_t> crc;
    if (second != std::string_view::npos) {
        const auto third = stem.find('_', second + 1);
        crc = parse_hex(stem.substr(second + 1, third == std::string_view::npos
            ? std::string_view::npos : third - second - 1));
    }
    return ParsedName{*version, crc};
}

struct BlobCandidate {
    std::filesystem::path path;
    ParsedName name;
};

std::vector<BlobCandidate> discover_blob_candidates(const DepotSpec& spec) {
    std::vector<BlobCandidate> result;
    const auto add = [&](const std::filesystem::path& path, bool exact) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            if (error) {
                throw std::filesystem::filesystem_error(
                    "cannot inspect Steam2 blob candidate", path, error);
            }
            if (exact) {
                throw std::runtime_error("exact Steam2 blob path is not a file: " + path.string());
            }
            return;
        }
        const auto parsed = parse_candidate_name(path, spec.id, ".blob");
        if (!parsed) {
            if (exact) {
                throw std::runtime_error(
                    "exact Steam2 blob path does not match depot " +
                    std::to_string(spec.id) + ": " + path.string());
            }
            return;
        }
        if (parsed->version <= spec.version) {
            result.push_back({path, *parsed});
        }
    };

    if (!spec.blob_files.empty()) {
        for (const auto& path : spec.blob_files) {
            add(path, true);
        }
        return result;
    }

    std::error_code error;
    std::filesystem::directory_iterator iterator(spec.blob_directory, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "cannot enumerate Steam2 blob directory", spec.blob_directory, error);
    }
    for (const auto& entry : iterator) {
        add(entry.path(), false);
    }
    return result;
}


BlobFile load_blob_candidate(const BlobCandidate& candidate) {
    auto bytes = read_file(candidate.path);
    auto values = parse_blob(bytes);
    const auto format = value_u32(values, 0);
    if (format != 3 && format != 4) {
        unsupported("depot blob format version " + std::to_string(format));
    }
    const auto expected_size = format == 3 ? value_u32(values, 13) : value_u64(values, 13);
    const auto parent_crc = value_u32(values, 12);
    return BlobFile(candidate.path, candidate.name.version, candidate.name.crc, std::move(bytes),
        std::move(values), parent_crc, expected_size);
}

std::vector<BlobFile> resolve_ancestry(
    const DepotSpec& spec, const std::vector<BlobCandidate>& candidates) {
    std::vector<const BlobCandidate*> top_matches;
    for (const auto& candidate : candidates) {
        if (candidate.name.version == spec.version &&
            (!spec.blob_crc || candidate.name.crc == spec.blob_crc)) {
            top_matches.push_back(&candidate);
        }
    }
    if (top_matches.empty()) {
        throw std::runtime_error("Steam2 top blob was not found for depot " + std::to_string(spec.id) +
            " version " + std::to_string(spec.version));
    }
    if (top_matches.size() != 1) {
        throw std::runtime_error("Steam2 top blob is ambiguous; specify blob_crc");
    }

    std::vector<BlobFile> newest_first;
    const BlobCandidate* candidate = top_matches.front();
    for (;;) {
        newest_first.push_back(load_blob_candidate(*candidate));
        const auto current_version = newest_first.back().version;
        const auto parent_crc = newest_first.back().parent_crc;
        if (parent_crc == 0) {
            break;
        }
        if (current_version == 0) {
            malformed("version-zero blob names a parent");
        }
        std::vector<const BlobCandidate*> parents;
        for (const auto& possible_parent : candidates) {
            if (possible_parent.name.version == current_version - 1 &&
                possible_parent.name.crc == parent_crc) {
                parents.push_back(&possible_parent);
            }
        }
        if (parents.empty()) {
            throw std::runtime_error("missing Steam2 blob ancestry: parent CRC " +
                std::to_string(parent_crc) + " for version " + std::to_string(current_version));
        }
        if (parents.size() != 1) {
            throw std::runtime_error("ambiguous Steam2 blob ancestry for parent CRC " +
                std::to_string(parent_crc));
        }
        candidate = parents.front();
    }
    std::reverse(newest_first.begin(), newest_first.end());
    return newest_first;
}

struct DatSource {
    explicit DatSource(std::filesystem::path source_path, std::uint64_t source_size)
        : path(std::move(source_path)), size(source_size), stream(path, std::ios::binary) {
        if (!stream) {
            throw std::filesystem::filesystem_error(
                "cannot open Steam2 DAT", path, std::make_error_code(std::errc::io_error));
        }
    }

    std::vector<std::byte> read_exact(std::uint64_t offset, std::size_t count) const {
        if (offset > size || count > size - offset) {
            throw std::runtime_error("Steam2 DAT block lies outside archive: " + path.string());
        }
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
            count > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            throw std::length_error("Steam2 DAT read cannot be represented by the stream API");
        }
        std::vector<std::byte> bytes(count);
        std::scoped_lock lock(mutex);
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream) {
            throw std::runtime_error("cannot seek Steam2 DAT: " + path.string());
        }
        if (count != 0) {
            stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
            if (stream.gcount() != static_cast<std::streamsize>(count)) {
                throw std::runtime_error("truncated Steam2 DAT read: " + path.string());
            }
        }
        return bytes;
    }

    std::filesystem::path path;
    std::uint64_t size{};
    mutable std::mutex mutex;
    mutable std::ifstream stream;
};

std::shared_ptr<DatSource> pair_dat(
    const BlobFile& blob,
    const std::vector<std::pair<std::filesystem::path, ParsedName>>& dats) {
    std::vector<std::filesystem::path> matches;
    for (const auto& [path, name] : dats) {
        if (name.version != blob.version) {
            continue;
        }
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error) {
            throw std::filesystem::filesystem_error("cannot size Steam2 DAT", path, error);
        }
        if (size == blob.expected_dat_size) {
            matches.push_back(path);
        }
    }
    if (matches.empty()) {
        throw std::runtime_error("no Steam2 DAT of the embedded expected size for version " +
            std::to_string(blob.version));
    }
    if (matches.size() != 1) {
        throw std::runtime_error("ambiguous Steam2 DAT pairing for version " + std::to_string(blob.version));
    }
    return std::make_shared<DatSource>(matches.front(), blob.expected_dat_size);
}

std::vector<std::pair<std::filesystem::path, ParsedName>> discover_dats(const DepotSpec& spec) {
    std::vector<std::pair<std::filesystem::path, ParsedName>> result;
    const auto add = [&](const std::filesystem::path& path, bool exact) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            if (error) {
                throw std::filesystem::filesystem_error(
                    "cannot inspect Steam2 DAT candidate", path, error);
            }
            if (exact) {
                throw std::runtime_error("exact Steam2 DAT path is not a file: " + path.string());
            }
            return;
        }
        const auto parsed = parse_candidate_name(path, spec.id, ".dat");
        if (!parsed) {
            if (exact) {
                throw std::runtime_error(
                    "exact Steam2 DAT path does not match depot " +
                    std::to_string(spec.id) + ": " + path.string());
            }
            return;
        }
        result.emplace_back(path, *parsed);
    };

    if (!spec.dat_files.empty()) {
        for (const auto& path : spec.dat_files) {
            add(path, true);
        }
        return result;
    }

    std::error_code error;
    std::filesystem::directory_iterator iterator(spec.dat_directory, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "cannot enumerate Steam2 DAT directory", spec.dat_directory, error);
    }
    for (const auto& entry : iterator) {
        add(entry.path(), false);
    }
    return result;
}


struct EncodedBlock {
    std::uint64_t archive_offset{};
    std::uint32_t encoded_size{};
    std::uint32_t checksum{};
    std::uint64_t logical_offset{};
    std::uint32_t logical_size{};
};

struct FileMapping {
    std::uint8_t mode{};
    std::uint64_t size{};
    std::shared_ptr<DatSource> source;
    std::vector<EncodedBlock> blocks;
};

std::map<std::uint32_t, FileMapping> parse_checksum_table(
    std::span<const std::byte> bytes, const std::shared_ptr<DatSource>& source) {
    Reader reader(bytes);
    if (reader.read<std::uint32_t>() != kChecksumMagic) {
        malformed("bad checksum-table magic");
    }
    const auto version = reader.read<std::uint32_t>();
    if (version != 0 && version != 1) {
        unsupported("checksum-table version " + std::to_string(version));
    }
    const auto file_block_count = reader.read<std::uint32_t>();
    const auto item_count = reader.read<std::uint32_t>();
    const auto offset1 = reader.read<std::uint32_t>();
    const auto offset2 = reader.read<std::uint32_t>();
    const auto block_size = reader.read<std::uint32_t>();
    const auto reported_largest = reader.read<std::uint32_t>();
    if (offset1 != 0x20 || block_size != kBlockSize) {
        malformed("invalid checksum-table header constants");
    }
    const std::uint64_t expected_offset2 = 0x20ULL + 0x10ULL * file_block_count;
    if (expected_offset2 != offset2 || expected_offset2 > bytes.size()) {
        malformed("invalid checksum-table mapping offset");
    }

    struct Run { std::uint32_t start; std::uint32_t count; std::uint32_t offset; };
    std::vector<Run> runs;
    runs.reserve(file_block_count);
    for (std::uint32_t index = 0; index < file_block_count; ++index) {
        const auto start = reader.read<std::uint32_t>();
        const auto count = reader.read<std::uint32_t>();
        const auto offset = reader.read<std::uint32_t>();
        (void)reader.read<std::uint32_t>();
        if (count != 0 && start > std::numeric_limits<std::uint32_t>::max() - (count - 1)) {
            malformed("checksum-table file-ID range overflows");
        }
        runs.push_back({start, count, offset});
    }

    std::map<std::uint32_t, FileMapping> mappings;
    std::uint64_t actual_items = 0;
    std::uint32_t actual_largest = 0;
    std::size_t expected_position = offset2;
    for (const auto& run : runs) {
        if (run.offset != expected_position) {
            malformed("checksum-table run offset is not contiguous");
        }
        reader.seek(run.offset);
        actual_items += run.count;
        if (actual_items > item_count) {
            malformed("checksum-table contains too many files");
        }
        for (std::uint32_t item = 0; item < run.count; ++item) {
            FileMapping mapping;
            std::uint64_t archive_offset{};
            if (version == 0) {
                mapping.size = reader.read<std::uint32_t>();
                archive_offset = reader.read<std::uint32_t>();
            } else {
                mapping.size = reader.read<std::uint64_t>();
                archive_offset = reader.read<std::uint64_t>();
            }
            const auto packed_blocks = reader.read<std::uint32_t>();
            mapping.mode = static_cast<std::uint8_t>(packed_blocks >> 24);
            const auto count = packed_blocks & 0x00ffffffU;
            if (mapping.mode < 1 || mapping.mode > 3) {
                unsupported("file chunk mode " + std::to_string(mapping.mode));
            }
            const auto expected_blocks = mapping.size == 0 ? 0 :
                1 + (mapping.size - 1) / kBlockSize;
            const bool zero_size_metadata_entry =
                mapping.size == 0 && count == 1 && mapping.mode == 1;
            if (count != expected_blocks && !zero_size_metadata_entry) {
                malformed("file size and block count disagree");
            }
            if (count > (reader.size() - reader.position()) / 8) {
                malformed("truncated checksum-table block list");
            }
            mapping.source = source;
            mapping.blocks.reserve(count);
            std::uint64_t block_offset = archive_offset;
            for (std::uint32_t block = 0; block < count; ++block) {
                const auto encoded_size = reader.read<std::uint32_t>();
                const auto checksum = reader.read<std::uint32_t>();
                const auto logical_offset = static_cast<std::uint64_t>(block) * kBlockSize;
                const auto remaining = mapping.size - logical_offset;
                const auto logical_size = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(remaining, kBlockSize));
                mapping.blocks.push_back(
                    {block_offset, encoded_size, checksum, logical_offset, logical_size});
                if (block_offset > std::numeric_limits<std::uint64_t>::max() - encoded_size) {
                    malformed("DAT block offset overflows");
                }
                block_offset += encoded_size;
            }
            for (const auto& block : mapping.blocks) {
                if (block.encoded_size > 0x10000) {
                    malformed("archive block has an impossible encoded size");
                }
                if (block.archive_offset > source->size || block.encoded_size > source->size - block.archive_offset) {
                    malformed("file block exceeds its paired DAT");
                }
                if (mapping.mode == 3 && block.encoded_size != 0 &&
                    block.encoded_size != block.logical_size) {
                    malformed("encrypted block size differs from logical size");
                }
            }
            actual_largest = std::max(actual_largest, static_cast<std::uint32_t>(mapping.blocks.size()));
            const auto file_id = run.start + item;
            if (!mappings.emplace(file_id, std::move(mapping)).second) {
                malformed("duplicate file ID in checksum table");
            }
        }
        expected_position = reader.position();
    }
    reader.seek(expected_position);
    if (reader.read<std::uint32_t>() != kChecksumMagic) {
        malformed("bad checksum-table footer magic");
    }
    if (reader.position() != reader.size()) {
        malformed("trailing bytes after checksum-table footer");
    }
    if (actual_items != item_count || actual_largest != reported_largest) {
        malformed("checksum-table summary counts disagree");
    }
    return mappings;
}

struct ManifestEntry {
    std::uint32_t file_id{};
    std::string path;
};

std::vector<ManifestEntry> parse_manifest(
    std::span<const std::byte> bytes, std::uint32_t depot_id, std::uint32_t depot_version) {
    constexpr std::size_t header_size = 14 * sizeof(std::uint32_t);
    constexpr std::size_t node_size = 7 * sizeof(std::uint32_t);
    if (bytes.size() < header_size) {
        malformed("truncated manifest header");
    }
    Reader reader(bytes);
    const auto manifest_version = reader.read<std::uint32_t>();
    if (manifest_version != 3 && manifest_version != 4) {
        unsupported("manifest version " + std::to_string(manifest_version));
    }
    const auto app_id = reader.read<std::uint32_t>();
    const auto version_id = reader.read<std::uint32_t>();
    const auto node_count = reader.read<std::uint32_t>();
    const auto file_count = reader.read<std::uint32_t>();
    (void)reader.read<std::uint32_t>();
    const auto binary_size = reader.read<std::uint32_t>();
    const auto string_table_size = reader.read<std::uint32_t>();
    for (int index = 0; index < 4; ++index) {
        (void)reader.read<std::uint32_t>();
    }
    const auto fingerprint = reader.read<std::uint32_t>();
    const auto checksum = reader.read<std::uint32_t>();
    (void)fingerprint;
    if (app_id != depot_id || version_id != depot_version) {
        malformed("manifest identity does not match requested depot");
    }
    if (binary_size != bytes.size()) {
        malformed("manifest binary size is wrong");
    }
    const std::uint64_t nodes_end64 = header_size + static_cast<std::uint64_t>(node_count) * node_size;
    if (nodes_end64 > bytes.size() || string_table_size > bytes.size() - nodes_end64) {
        malformed("manifest node or string table bounds are invalid");
    }
    if (file_count > node_count) {
        malformed("manifest reports more files than nodes");
    }

    constexpr std::size_t checksum_fields_offset = 12 * sizeof(std::uint32_t);
    const std::array<Bytef, 2 * sizeof(std::uint32_t)> zero_fields{};
    auto computed = adler32(0, reinterpret_cast<const Bytef*>(bytes.data()),
        static_cast<uInt>(checksum_fields_offset));
    computed = adler32(computed, zero_fields.data(), static_cast<uInt>(zero_fields.size()));
    computed = adler32(computed,
        reinterpret_cast<const Bytef*>(bytes.data() + header_size),
        static_cast<uInt>(bytes.size() - header_size));
    if (checksum != computed) {
        malformed("manifest checksum is wrong");
    }

    struct Node {
        std::uint32_t name_offset;
        std::uint32_t size;
        std::uint32_t file_id;
        std::uint32_t flags;
        std::uint32_t parent;
        std::uint32_t next;
        std::uint32_t first_child;
    };
    std::vector<Node> nodes;
    nodes.reserve(node_count);
    reader.seek(header_size);
    for (std::uint32_t index = 0; index < node_count; ++index) {
        nodes.push_back({reader.read<std::uint32_t>(), reader.read<std::uint32_t>(),
            reader.read<std::uint32_t>(), reader.read<std::uint32_t>(), reader.read<std::uint32_t>(),
            reader.read<std::uint32_t>(), reader.read<std::uint32_t>()});
    }
    const auto strings = bytes.subspan(static_cast<std::size_t>(nodes_end64), string_table_size);
    const auto component = [&](std::uint32_t offset) -> std::string_view {
        if (offset >= strings.size()) {
            malformed("manifest name offset is outside the string table");
        }
        const auto* first = reinterpret_cast<const char*>(strings.data() + offset);
        const auto* last = reinterpret_cast<const char*>(strings.data() + strings.size());
        const auto* zero = std::find(first, last, '\0');
        if (zero == last) {
            malformed("unterminated manifest name");
        }
        const std::string_view name(first, zero);
        if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos ||
            name == "." || name == "..") {
            malformed("unsafe manifest path component");
        }
        return name;
    };
    for (const auto& node : nodes) {
        if (node.parent != 0xffffffffU && node.parent >= nodes.size()) {
            malformed("manifest parent index is outside node table");
        }
        (void)component(node.name_offset);
    }

    std::vector<ManifestEntry> files;
    files.reserve(file_count);
    for (std::uint32_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        if (node.flags == 0) {
            continue;
        }
        std::vector<std::string_view> parts;
        std::uint32_t current = index;
        for (std::size_t depth = 0;; ++depth) {
            if (depth > nodes.size()) {
                malformed("manifest parent cycle");
            }
            const auto& path_node = nodes[current];
            if (path_node.parent == 0xffffffffU) {
                break;
            }
            const auto name = component(path_node.name_offset);
            if (name.empty()) {
                malformed("empty file path component");
            }
            parts.push_back(name);
            if (path_node.parent >= nodes.size()) {
                malformed("manifest parent index is outside node table");
            }
            current = path_node.parent;
        }
        std::string path;
        for (auto part = parts.rbegin(); part != parts.rend(); ++part) {
            if (!path.empty()) {
                path.push_back('/');
            }
            path.append(*part);
        }
        if (path.empty()) {
            malformed("manifest file has an empty path");
        }
        files.push_back({node.file_id, std::move(path)});
    }
    if (files.size() != file_count) {
        malformed("manifest file count disagrees with node flags");
    }
    return files;
}

#ifdef _WIN32
class BCryptAlgorithm {
public:
    BCryptAlgorithm() {
        check(BCryptOpenAlgorithmProvider(&handle_, BCRYPT_AES_ALGORITHM, nullptr, 0),
            "BCryptOpenAlgorithmProvider(AES)");
        const wchar_t mode[] = BCRYPT_CHAIN_MODE_ECB;
        check(BCryptSetProperty(handle_, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)), sizeof(mode), 0),
            "BCryptSetProperty(ECB)");
    }
    ~BCryptAlgorithm() { if (handle_) BCryptCloseAlgorithmProvider(handle_, 0); }
    BCryptAlgorithm(const BCryptAlgorithm&) = delete;
    BCryptAlgorithm& operator=(const BCryptAlgorithm&) = delete;
    BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

    static void check(NTSTATUS status, const char* operation) {
        if (!BCRYPT_SUCCESS(status)) {
            throw std::runtime_error(std::string(operation) + " failed with NTSTATUS " +
                std::to_string(static_cast<long>(status)));
        }
    }
private:
    BCRYPT_ALG_HANDLE handle_{};
};

std::vector<std::byte> aes_cfb_decrypt(
    std::span<const std::byte> encrypted, const std::array<std::uint8_t, 16>& key) {
    BCryptAlgorithm algorithm;
    DWORD object_length{};
    DWORD copied{};
    BCryptAlgorithm::check(BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length), &copied, 0),
        "BCryptGetProperty(OBJECT_LENGTH)");
    std::vector<UCHAR> key_object(object_length);
    BCRYPT_KEY_HANDLE key_handle{};
    BCryptAlgorithm::check(BCryptGenerateSymmetricKey(algorithm.get(), &key_handle,
        key_object.data(), object_length, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0),
        "BCryptGenerateSymmetricKey");
    struct KeyCloser {
        BCRYPT_KEY_HANDLE handle;
        ~KeyCloser() { if (handle) BCryptDestroyKey(handle); }
    } closer{key_handle};

    std::array<UCHAR, 16> feedback{};
    std::array<UCHAR, 16> stream_block{};
    std::vector<std::byte> output(encrypted.size());
    for (std::size_t offset = 0; offset < encrypted.size(); offset += feedback.size()) {
        ULONG actual{};
        BCryptAlgorithm::check(BCryptEncrypt(key_handle, feedback.data(),
            static_cast<ULONG>(feedback.size()), nullptr, nullptr, 0, stream_block.data(),
            static_cast<ULONG>(stream_block.size()), &actual, 0), "BCryptEncrypt(CFB keystream)");
        if (actual != stream_block.size()) {
            throw std::runtime_error("BCrypt returned a short AES block");
        }
        const auto count = std::min(feedback.size(), encrypted.size() - offset);
        for (std::size_t index = 0; index < count; ++index) {
            const auto cipher = std::to_integer<unsigned char>(encrypted[offset + index]);
            output[offset + index] = static_cast<std::byte>(cipher ^ stream_block[index]);
            feedback[index] = cipher;
        }
    }
    return output;
}
#else
struct EvpCipherContextDeleter {
    void operator()(EVP_CIPHER_CTX* context) const noexcept {
        EVP_CIPHER_CTX_free(context);
    }
};

using EvpCipherContext = std::unique_ptr<EVP_CIPHER_CTX, EvpCipherContextDeleter>;

void openssl_check(int result, const char* operation) {
    if (result != 1) {
        throw std::runtime_error(std::string(operation) + " failed");
    }
}

std::vector<std::byte> aes_cfb_decrypt(
    std::span<const std::byte> encrypted, const std::array<std::uint8_t, 16>& key) {
    EvpCipherContext context(EVP_CIPHER_CTX_new());
    if (!context) {
        throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    }
    openssl_check(
        EVP_EncryptInit_ex(context.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr),
        "EVP_EncryptInit_ex(AES-128-ECB)");
    openssl_check(EVP_CIPHER_CTX_set_padding(context.get(), 0), "EVP_CIPHER_CTX_set_padding");

    std::array<unsigned char, 16> feedback{};
    std::array<unsigned char, 16> stream_block{};
    std::vector<std::byte> output(encrypted.size());
    for (std::size_t offset = 0; offset < encrypted.size(); offset += feedback.size()) {
        int actual{};
        openssl_check(
            EVP_EncryptUpdate(
                context.get(), stream_block.data(), &actual,
                feedback.data(), static_cast<int>(feedback.size())),
            "EVP_EncryptUpdate(CFB keystream)");
        if (actual != static_cast<int>(stream_block.size())) {
            throw std::runtime_error("OpenSSL returned a short AES block");
        }
        const auto count = std::min(feedback.size(), encrypted.size() - offset);
        for (std::size_t index = 0; index < count; ++index) {
            const auto cipher = std::to_integer<unsigned char>(encrypted[offset + index]);
            output[offset + index] = static_cast<std::byte>(cipher ^ stream_block[index]);
            feedback[index] = cipher;
        }
    }
    return output;
}
#endif

std::vector<std::byte> decode_block(
    std::uint8_t mode, std::span<const std::byte> encoded, std::uint32_t logical_size,
    const std::array<std::uint8_t, 16>& key) {
    if (mode == 3) {
        auto output = aes_cfb_decrypt(encoded, key);
        if (output.size() != logical_size) {
            malformed("decrypted block has the wrong size");
        }
        return output;
    }

    std::span<const std::byte> compressed = encoded;
    std::vector<std::byte> decrypted;
    if (mode == 2) {
        if (encoded.size() < 8) {
            malformed("encrypted-compressed block lacks its eight-byte header");
        }
        Reader header(encoded.first(8));
        const auto encrypted_size = header.read<std::uint32_t>();
        const auto declared_size = header.read<std::uint32_t>();
        if (encrypted_size != encoded.size() - 8 || declared_size != logical_size) {
            malformed("encrypted-compressed block header is inconsistent");
        }
        decrypted = aes_cfb_decrypt(encoded.subspan(8), key);
        compressed = decrypted;
    }
    if (mode != 1 && mode != 2) {
        unsupported("file chunk mode " + std::to_string(mode));
    }
    std::vector<std::byte> output(logical_size);
    uLongf actual = logical_size;
    const auto result = uncompress(reinterpret_cast<Bytef*>(output.data()), &actual,
        reinterpret_cast<const Bytef*>(compressed.data()), static_cast<uLong>(compressed.size()));
    if (result != Z_OK || actual != logical_size) {
        throw std::runtime_error("Steam2 block decompression failed or returned the wrong size");
    }
    return output;
}

std::string prefixed_path(std::string_view prefix, std::string_view path) {
    std::string result;
    result.reserve(prefix.size() + (prefix.empty() ? 0 : 1) + path.size());
    std::size_t begin = 0;
    while (begin < prefix.size()) {
        while (begin < prefix.size() && (prefix[begin] == '/' || prefix[begin] == '\\')) {
            ++begin;
        }
        if (begin == prefix.size()) break;
        const auto separator = prefix.find_first_of("/\\", begin);
        const auto component = prefix.substr(begin, separator == std::string_view::npos
            ? std::string_view::npos : separator - begin);
        if (component == "." || component == "..") {
            throw std::invalid_argument("unsafe Steam2 mount prefix");
        }
        if (!result.empty()) result.push_back('/');
        result.append(component);
        if (separator == std::string_view::npos) break;
        begin = separator + 1;
    }
    if (!result.empty()) result.push_back('/');
    for (const char character : path) {
        if (character != ':') {
            result.push_back(character);
        }
    }
    return result;

}

} // namespace
BlobMetadata inspect_blob(const std::filesystem::path& path) {
    const auto depot_id = parse_depot_id(path);
    if (!depot_id) {
        throw std::invalid_argument("Steam2 blob filename has no depot ID: " + path.string());
    }
    const auto parsed = parse_candidate_name(path, *depot_id, ".blob");
    if (!parsed) {
        throw std::invalid_argument("invalid Steam2 blob filename: " + path.string());
    }
    const BlobCandidate candidate{path, *parsed};
    const auto blob = load_blob_candidate(candidate);
    const auto compressed_manifest = decompress_blob(required_value(blob.values, 3));
    const auto manifest_blob = parse_blob(compressed_manifest);
    const auto manifest = parse_manifest(
        required_value(manifest_blob, 0), *depot_id, parsed->version);
    return BlobMetadata{
        *depot_id,
        parsed->version,
        parsed->crc,
        blob.parent_crc,
        blob.expected_dat_size,
        static_cast<std::uint32_t>(manifest.size()),
    };
}


struct Steam2File::Impl {
    explicit Impl(FileMapping mapping, std::array<std::uint8_t, 16> depot_key)
        : mapping(std::move(mapping)), key(depot_key) {}
    const FileMapping mapping;
    const std::array<std::uint8_t, 16> key;
};

Steam2File::Steam2File(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::uint64_t Steam2File::size() const noexcept {
    return impl_->mapping.size;
}

std::size_t Steam2File::read(std::uint64_t offset, std::span<std::byte> output) const {
    if (output.empty() || offset >= impl_->mapping.size) {
        return 0;
    }
    const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
        output.size(), impl_->mapping.size - offset));
    const auto end = offset + requested;
    std::size_t written = 0;
    for (const auto& block : impl_->mapping.blocks) {
        const auto block_end = block.logical_offset + block.logical_size;
        if (block_end <= offset) continue;
        if (block.logical_offset >= end) break;
        if (block.encoded_size == 0) {
            throw std::runtime_error(
                "Steam2 file references an archive block with no payload");
        }
        const auto encoded = impl_->mapping.source->read_exact(block.archive_offset, block.encoded_size);
        const auto decoded = decode_block(impl_->mapping.mode, encoded, block.logical_size, impl_->key);
        const auto copy_begin = std::max(offset, block.logical_offset);
        const auto copy_end = std::min(end, block_end);
        const auto source_offset = static_cast<std::size_t>(copy_begin - block.logical_offset);
        const auto copy_size = static_cast<std::size_t>(copy_end - copy_begin);
        std::memcpy(output.data() + written, decoded.data() + source_offset, copy_size);
        written += copy_size;
    }
    if (written != requested) {
        throw std::runtime_error("Steam2 file mapping did not cover the requested range");
    }
    return written;
}

Steam2Depot Steam2Depot::load(const DepotSpec& spec) {
    const auto candidates = discover_blob_candidates(spec);
    const auto ancestry = resolve_ancestry(spec, candidates);
    const auto dat_candidates = discover_dats(spec);

    std::map<std::uint32_t, FileMapping> mappings;
    for (const auto& blob : ancestry) {
        auto source = pair_dat(blob, dat_candidates);
        auto updates = parse_checksum_table(required_value(blob.values, 4), source);
        for (auto& [file_id, mapping] : updates) {
            mappings.insert_or_assign(file_id, std::move(mapping));
        }
    }

    const auto& top = ancestry.back();
    const auto compressed_manifest = decompress_blob(required_value(top.values, 3));
    const auto manifest_blob = parse_blob(compressed_manifest);
    const auto manifest = parse_manifest(required_value(manifest_blob, 0), spec.id, spec.version);
    const auto key = depot_key(spec.id);

    Steam2Depot depot;
    depot.id_ = spec.id;
    depot.version_ = spec.version;
    depot.entries_.reserve(manifest.size());
    std::unordered_map<std::uint32_t, std::shared_ptr<const Steam2File>> files;
    std::unordered_set<std::string> projected_paths;

    for (const auto& entry : manifest) {
        auto file = files.find(entry.file_id);
        if (file == files.end()) {
            auto mapping = mappings.find(entry.file_id);
            if (mapping == mappings.end()) {
                malformed("manifest file ID " + std::to_string(entry.file_id) + " has no block mapping");
            }
            auto impl = std::make_shared<Steam2File::Impl>(std::move(mapping->second), key);
            auto inserted = files.emplace(entry.file_id,
                std::shared_ptr<const Steam2File>(new Steam2File(std::move(impl))));
            file = inserted.first;
        }
        auto projected_path = prefixed_path(spec.mount_prefix, entry.path);
        if (!projected_paths.insert(projected_path).second) {
            malformed("Windows path normalization produced a duplicate: " + projected_path);
        }
        depot.entries_.push_back({std::move(projected_path), file->second});
    }
    return depot;
}

std::uint32_t Steam2Depot::id() const noexcept { return id_; }
std::uint32_t Steam2Depot::version() const noexcept { return version_; }
const std::vector<DepotEntry>& Steam2Depot::entries() const noexcept { return entries_; }

} // namespace s2fs
