#include "s2fs/build_definition.hpp"
#include "s2fs/fuse_filesystem.hpp"
#include "s2fs/steam2_archive.hpp"
#include "s2fs/virtual_tree.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

struct Options {
    std::filesystem::path build_path;
    std::filesystem::path mount_point;
    std::uint64_t write_quota_bytes{512ULL << 20};
    bool inspect_only{};
    bool wait_stdin{};
};

class UsageError final : public std::exception {
public:
    explicit UsageError(bool help) noexcept : help_(help) {}
    [[nodiscard]] bool help() const noexcept { return help_; }
    [[nodiscard]] const char* what() const noexcept override { return "invalid command line"; }

private:
    bool help_;
};

[[noreturn]] void usage(std::string_view message = {}, bool help = false)
{
    if (!message.empty()) {
        std::cerr << "error: " << message << "\n\n";
    }
    std::cerr
        << "usage: steam2fs --build FILE --mount DIR [--quota-mib N] "
           "[--inspect] [--wait-stdin]\n"
        << "       --mount may be omitted with --inspect. RAM writes are capped at "
           "512 MiB by default and discarded on unmount.\n";
    throw UsageError(help);
}

[[nodiscard]] std::uint64_t parse_u64(std::string_view value, std::string_view option)
{
    std::uint64_t result{};
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        usage(std::string(option) + " requires an unsigned integer");
    }
    return result;
}

[[nodiscard]] Options parse_options(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        auto require_value = [&]() -> std::string_view {
            if (++index >= argc) {
                usage("an option is missing its value");
            }
            return argv[index];
        };

        if (argument == "--build") {
            options.build_path = std::filesystem::path(require_value());
        } else if (argument == "--mount") {
            options.mount_point = std::filesystem::path(require_value());
        } else if (argument == "--quota-mib") {
            const std::uint64_t mib = parse_u64(require_value(), "--quota-mib");
            constexpr std::uint64_t bytes_per_mib = 1024ULL * 1024ULL;
            if (mib == 0 || mib > std::numeric_limits<std::uint64_t>::max() / bytes_per_mib) {
                usage("--quota-mib is outside the supported range");
            }
            options.write_quota_bytes = mib * bytes_per_mib;
        } else if (argument == "--inspect") {
            options.inspect_only = true;
        } else if (argument == "--wait-stdin") {
            options.wait_stdin = true;
        } else if (argument == "--help" || argument == "-h") {
            usage({}, true);
        } else {
            usage("unknown option: " + std::string(argument));
        }
    }

    if (options.build_path.empty()) {
        usage("--build is required");
    }
    if (!options.inspect_only && options.mount_point.empty()) {
        usage("--mount is required unless --inspect is used");
    }
    return options;
}

[[nodiscard]] std::filesystem::path validate_mount_directory(
    const std::filesystem::path& mount_point)
{
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(mount_point, error);
    if (error) {
        throw std::system_error(error, "cannot inspect mount directory");
    }
    if (!std::filesystem::exists(status)) {
        throw std::runtime_error("mount directory does not exist: " + mount_point.string());
    }
    if (!std::filesystem::is_directory(status)) {
        throw std::runtime_error("mount path is not a directory: " + mount_point.string());
    }
    std::filesystem::path absolute = std::filesystem::absolute(mount_point, error);
    if (error) {
        throw std::system_error(error, "cannot resolve mount directory");
    }
    return absolute.lexically_normal();
}

void wait_for_stdin(std::stop_token stop, s2fs::FuseFileSystem& file_system) noexcept
{
    bool received_data = false;
    std::array<char, 256> buffer{};
    while (!stop.stop_requested()) {
        pollfd descriptor{
            .fd = STDIN_FILENO,
            .events = static_cast<short>(POLLIN | POLLHUP),
            .revents = 0,
        };
        const int ready = ::poll(&descriptor, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (ready == 0) {
            continue;
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            return;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP)) == 0) {
            continue;
        }

        const ssize_t count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
        if (count > 0) {
            received_data = true;
            const auto end = buffer.begin() + count;
            if (std::find(buffer.begin(), end, '\n') != end ||
                std::find(buffer.begin(), end, '\r') != end) {
                file_system.request_exit();
                return;
            }
        } else if (count == 0) {
            if (received_data) {
                file_system.request_exit();
            }
            return;
        } else if (errno != EINTR && errno != EAGAIN) {
            return;
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        Options options = parse_options(argc, argv);
        const std::vector<s2fs::DepotSpec> specs =
            s2fs::load_build_definition(options.build_path);
        s2fs::VirtualTree tree(options.write_quota_bytes);

        std::uint64_t file_count{};
        std::uint64_t logical_bytes{};
        for (const s2fs::DepotSpec& spec : specs) {
            std::cout << "loading depot " << spec.id << " version " << spec.version << "...\n";
            s2fs::Steam2Depot depot = s2fs::Steam2Depot::load(spec);
            for (const s2fs::DepotEntry& entry : depot.entries()) {
                if (file_count == std::numeric_limits<std::uint64_t>::max() ||
                    (entry.file && entry.file->size() >
                        std::numeric_limits<std::uint64_t>::max() - logical_bytes)) {
                    throw std::overflow_error("build statistics exceed 64-bit range");
                }
                ++file_count;
                if (entry.file) {
                    logical_bytes += entry.file->size();
                }
            }
            tree.overlay(depot);
            std::cout << "  " << depot.entries().size() << " files\n";
        }

        std::cout << "build ready: " << file_count << " depot files, "
                  << logical_bytes << " logical bytes before overlay collisions\n";
        if (options.inspect_only) {
            return 0;
        }

        options.mount_point = validate_mount_directory(options.mount_point);
        s2fs::FuseFileSystem file_system(tree, options.mount_point);
        std::jthread stdin_thread;
        if (options.wait_stdin) {
            stdin_thread = std::jthread([&](std::stop_token stop) {
                wait_for_stdin(stop, file_system);
            });
        }

        const int loop_result = file_system.run([&] {
            std::cout << "mounted at " << options.mount_point.string()
                      << "; press Ctrl+C to unmount\n" << std::flush;
        });
        if (stdin_thread.joinable()) {
            stdin_thread.request_stop();
            stdin_thread.join();
        }
        if (loop_result != 0) {
            throw std::runtime_error(
                "FUSE loop exited with error " + std::to_string(loop_result));
        }
        return 0;
    } catch (const UsageError& error) {
        return error.help() ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
