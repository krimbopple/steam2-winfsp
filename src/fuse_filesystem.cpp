#define FUSE_USE_VERSION 31

#include "s2fs/fuse_filesystem.hpp"

#include "s2fs/virtual_tree.hpp"

#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <utility>
#include <vector>

namespace s2fs {
namespace {

constexpr std::uint64_t kStatBlockSize = 4096;
constexpr unsigned int kRenameNoReplace = 1;

class ErrnoError final {
public:
    explicit ErrnoError(int value) noexcept : value_(value) {}
    [[nodiscard]] int value() const noexcept { return value_; }

private:
    int value_;
};

[[noreturn]] void fail(int value)
{
    throw ErrnoError(value);
}

[[nodiscard]] bool is_error(const std::error_code& code, std::errc condition) noexcept
{
    return code == std::make_error_code(condition) ||
        code.default_error_condition() == std::make_error_condition(condition);
}

[[nodiscard]] int errno_from_error(const std::error_code& code) noexcept
{
    if (is_error(code, std::errc::no_such_file_or_directory)) return ENOENT;
    if (is_error(code, std::errc::file_exists)) return EEXIST;
    if (is_error(code, std::errc::not_a_directory)) return ENOTDIR;
    if (is_error(code, std::errc::is_a_directory)) return EISDIR;
    if (is_error(code, std::errc::directory_not_empty)) return ENOTEMPTY;
    if (is_error(code, std::errc::no_space_on_device)) return ENOSPC;
    if (is_error(code, std::errc::permission_denied)) return EACCES;
    if (is_error(code, std::errc::operation_not_permitted)) return EPERM;
    if (is_error(code, std::errc::read_only_file_system)) return EROFS;
    if (is_error(code, std::errc::invalid_argument)) return EINVAL;
    if (is_error(code, std::errc::filename_too_long)) return ENAMETOOLONG;
    if (is_error(code, std::errc::file_too_large)) return EFBIG;
    if (is_error(code, std::errc::value_too_large) ||
        is_error(code, std::errc::result_out_of_range)) return EOVERFLOW;
    if (is_error(code, std::errc::not_enough_memory)) return ENOMEM;
    if (is_error(code, std::errc::device_or_resource_busy)) return EBUSY;
    if (is_error(code, std::errc::operation_canceled)) return ECANCELED;
    if (is_error(code, std::errc::illegal_byte_sequence)) return EILSEQ;
    if (is_error(code, std::errc::io_error)) return EIO;
    if ((code.category() == std::generic_category() ||
         code.category() == std::system_category()) &&
        code.value() > 0 && code.value() <= INT_MAX) {
        return code.value();
    }
    return EIO;
}

template <typename Function>
int errno_guard(Function&& function) noexcept
{
    try {
        return std::forward<Function>(function)();
    } catch (const ErrnoError& error) {
        return -error.value();
    } catch (const std::bad_alloc&) {
        return -ENOMEM;
    } catch (const std::system_error& error) {
        return -errno_from_error(error.code());
    } catch (const std::length_error&) {
        return -ENAMETOOLONG;
    } catch (const std::invalid_argument&) {
        return -EINVAL;
    } catch (const std::out_of_range&) {
        return -EINVAL;
    } catch (...) {
        return -EIO;
    }
}

[[nodiscard]] std::wstring checked_path(const char* path)
{
    if (path == nullptr || path[0] != '/') {
        fail(EINVAL);
    }
    return utf8_to_wide(path);
}

[[nodiscard]] std::wstring parent_path(std::wstring_view path)
{
    if (path == L"/") {
        return {};
    }
    const std::size_t separator = path.find_last_of(L"/\\");
    if (separator == std::wstring_view::npos) {
        fail(EINVAL);
    }
    return separator == 0 ? std::wstring(L"/") : std::wstring(path.substr(0, separator));
}

[[nodiscard]] int missing_path_errno(VirtualTree& tree, std::wstring_view path)
{
    std::size_t separator = 1;
    while ((separator = path.find_first_of(L"/\\", separator)) != std::wstring_view::npos) {
        if (separator != 0) {
            NodeHandle prefix = tree.lookup(path.substr(0, separator));
            if (prefix && !tree.info(prefix).directory) {
                return ENOTDIR;
            }
        }
        ++separator;
    }
    return ENOENT;
}

[[nodiscard]] NodeHandle require_node(VirtualTree& tree, std::wstring_view path)
{
    NodeHandle node = tree.lookup(path);
    if (!node) {
        fail(missing_path_errno(tree, path));
    }
    return node;
}

void validate_parent(VirtualTree& tree, std::wstring_view path)
{
    const std::wstring parent = parent_path(path);
    if (parent.empty()) {
        fail(EPERM);
    }
    NodeHandle node = tree.lookup(parent);
    if (!node) {
        fail(missing_path_errno(tree, parent));
    }
    if (!tree.info(node).directory) {
        fail(ENOTDIR);
    }
}

[[nodiscard]] timespec to_timespec(std::chrono::system_clock::time_point value) noexcept
{
    using namespace std::chrono;
    const auto seconds_part = floor<seconds>(value);
    const auto nanoseconds_part = duration_cast<nanoseconds>(value - seconds_part);
    return timespec{
        .tv_sec = static_cast<time_t>(seconds_part.time_since_epoch().count()),
        .tv_nsec = static_cast<long>(nanoseconds_part.count()),
    };
}

[[nodiscard]] std::chrono::system_clock::time_point from_timespec(const timespec& value)
{
    using Clock = std::chrono::system_clock;
    using Duration = Clock::duration;
    using Seconds = std::chrono::seconds;
    if (value.tv_nsec < 0 || value.tv_nsec >= 1'000'000'000L) {
        fail(EINVAL);
    }

    const auto minimum_seconds = std::chrono::duration_cast<Seconds>(Duration::min()).count();
    const auto maximum_seconds = std::chrono::duration_cast<Seconds>(Duration::max()).count();
    if (value.tv_sec < minimum_seconds || value.tv_sec > maximum_seconds) {
        fail(EOVERFLOW);
    }
    const Duration seconds = std::chrono::duration_cast<Duration>(Seconds(value.tv_sec));
    const Duration fraction = std::chrono::duration_cast<Duration>(
        std::chrono::nanoseconds(value.tv_nsec));
    if (seconds > Duration::max() - fraction) {
        fail(EOVERFLOW);
    }
    return Clock::time_point(seconds + fraction);
}

[[nodiscard]] char ascii_lower(char value) noexcept
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] bool ascii_equal_ci(std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ascii_ends_with_ci(std::string_view value, std::string_view suffix) noexcept
{
    return value.size() >= suffix.size() &&
        ascii_equal_ci(value.substr(value.size() - suffix.size()), suffix);
}

[[nodiscard]] bool executable_name(std::string_view path) noexcept
{
    const std::size_t separator = path.find_last_of("/\\");
    const std::string_view name =
        separator == std::string_view::npos ? path : path.substr(separator + 1);
    return ascii_ends_with_ci(name, ".exe") ||
        ascii_ends_with_ci(name, ".sh") ||
        ascii_ends_with_ci(name, ".run") ||
        ascii_ends_with_ci(name, "_linux") ||
        ascii_equal_ci(name, "srcds_run") ||
        ascii_equal_ci(name, "srcds_linux");
}

[[nodiscard]] mode_t node_mode(const NodeInfo& info, std::string_view name) noexcept
{
    if (info.directory) {
        return S_IFDIR | 0755;
    }
    return S_IFREG | (executable_name(name) ? 0755 : 0644);
}

void fill_stat(
    const NodeHandle& node, const NodeInfo& info, std::string_view name,
    struct stat* output)
{
    if (output == nullptr) {
        fail(EINVAL);
    }
    std::memset(output, 0, sizeof(*output));
    output->st_ino = static_cast<ino_t>(reinterpret_cast<std::uintptr_t>(node.get()));
    output->st_mode = node_mode(info, name);
    output->st_nlink = info.directory ? 2 : 1;
    output->st_size = static_cast<off_t>(std::min<std::uint64_t>(
        info.size, static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())));
    output->st_blksize = static_cast<blksize_t>(kStatBlockSize);
    output->st_blocks = static_cast<blkcnt_t>((info.size + 511) / 512);
    output->st_atim = to_timespec(info.access_time);
    output->st_mtim = to_timespec(info.write_time);
    output->st_ctim = output->st_mtim;
    if (const fuse_context* context = fuse_get_context()) {
        output->st_uid = context->uid;
        output->st_gid = context->gid;
    }
}

} // namespace

struct FuseFileSystem::Impl {
    struct OpenHandle {
        NodeHandle node;
        OpenHandle* previous{};
        OpenHandle* next{};
    };

    VirtualTree& tree;
    std::string mount_point;
    std::mutex lifecycle_mutex;
    fuse* active_fuse{};
    bool exit_requested{};
    bool running{};
    OpenHandle* open_handles{};

    Impl(VirtualTree& value, std::filesystem::path mount)
        : tree(value), mount_point(mount.string())
    {
        if (mount_point.empty()) {
            throw std::invalid_argument("FUSE mount point is empty");
        }
    }

    [[nodiscard]] static Impl& self()
    {
        const fuse_context* context = fuse_get_context();
        if (context == nullptr || context->private_data == nullptr) {
            fail(EIO);
        }
        return *static_cast<Impl*>(context->private_data);
    }

    [[nodiscard]] static OpenHandle& open_handle(const fuse_file_info* info)
    {
        if (info == nullptr || info->fh == 0) {
            fail(EBADF);
        }
        return *reinterpret_cast<OpenHandle*>(static_cast<std::uintptr_t>(info->fh));
    }
    void install_handle(
        fuse_file_info& info, std::unique_ptr<OpenHandle> opened) noexcept
    {
        opened->next = open_handles;
        if (open_handles != nullptr) {
            open_handles->previous = opened.get();
        }
        open_handles = opened.get();
        info.fh = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(opened.release()));
    }

    void remove_handle(OpenHandle& opened) noexcept
    {
        if (opened.previous != nullptr) {
            opened.previous->next = opened.next;
        } else {
            open_handles = opened.next;
        }
        if (opened.next != nullptr) {
            opened.next->previous = opened.previous;
        }
        delete &opened;
    }

    void clear_handles() noexcept
    {
        while (open_handles != nullptr) {
            OpenHandle* next = open_handles->next;
            delete open_handles;
            open_handles = next;
        }
    }

    [[nodiscard]] static NodeHandle node_for(
        Impl& implementation, const char* path, const fuse_file_info* info)
    {
        if (info != nullptr && info->fh != 0) {
            return open_handle(info).node;
        }
        const std::wstring wide_path = checked_path(path);
        return require_node(implementation.tree, wide_path);
    }

    static int GetAttr(const char* path, struct stat* output, fuse_file_info* info) noexcept
    {
        return errno_guard([&] {
            Impl& implementation = self();
            NodeHandle node = node_for(implementation, path, info);
            fill_stat(
                node, implementation.tree.info(node),
                path != nullptr ? std::string_view(path) : std::string_view{}, output);
            return 0;
        });
    }

    static int Open(const char* path, fuse_file_info* info) noexcept
    {
        return errno_guard([&] {
            if (info == nullptr) {
                fail(EINVAL);
            }
            Impl& implementation = self();
            const std::wstring wide_path = checked_path(path);
            NodeHandle node = require_node(implementation.tree, wide_path);
            if (implementation.tree.info(node).directory) {
                fail(EISDIR);
            }
            auto opened = std::make_unique<OpenHandle>();
            opened->node = node;
            if ((info->flags & O_TRUNC) != 0) {
                implementation.tree.resize(node, 0);
            }
            implementation.install_handle(*info, std::move(opened));
            return 0;
        });
    }

    static int OpenDir(const char* path, fuse_file_info* info) noexcept
    {
        return errno_guard([&] {
            if (info == nullptr) {
                fail(EINVAL);
            }
            Impl& implementation = self();
            const std::wstring wide_path = checked_path(path);
            NodeHandle node = require_node(implementation.tree, wide_path);
            if (!implementation.tree.info(node).directory) {
                fail(ENOTDIR);
            }
            auto opened = std::make_unique<OpenHandle>();
            opened->node = std::move(node);
            implementation.install_handle(*info, std::move(opened));
            return 0;
        });
    }

    static int Release(const char*, fuse_file_info* info) noexcept
    {
        return errno_guard([&] {
            Impl& implementation = self();
            OpenHandle& opened = open_handle(info);
            implementation.remove_handle(opened);
            info->fh = 0;
            return 0;
        });
    }

    static int Read(
        const char* path, char* buffer, std::size_t size, off_t offset,
        fuse_file_info* info) noexcept
    {
        return errno_guard([&] {
            if (offset < 0 || (size != 0 && buffer == nullptr)) {
                fail(EINVAL);
            }
            Impl& implementation = self();
            NodeHandle node = node_for(implementation, path, info);
            if (implementation.tree.info(node).directory) {
                fail(EISDIR);
            }
            const std::size_t requested = std::min<std::size_t>(size, INT_MAX);
            const std::size_t completed = implementation.tree.read(
                node, static_cast<std::uint64_t>(offset),
                std::span<std::byte>(reinterpret_cast<std::byte*>(buffer), requested));
            if (completed > static_cast<std::size_t>(INT_MAX)) {
                fail(EIO);
            }
            return static_cast<int>(completed);
        });
    }

    static int Write(
        const char* path, const char* buffer, std::size_t size, off_t offset,
        fuse_file_info* info) noexcept
    {
        return errno_guard([&] {
            if (offset < 0 || (size != 0 && buffer == nullptr)) {
                fail(EINVAL);
            }
            Impl& implementation = self();
            NodeHandle node = node_for(implementation, path, info);
            NodeInfo node_info = implementation.tree.info(node);
            if (node_info.directory) {
                fail(EISDIR);
            }
            std::uint64_t position = static_cast<std::uint64_t>(offset);
            if (info != nullptr && (info->flags & O_APPEND) != 0) {
                position = node_info.size;
            }
            const std::size_t requested = std::min<std::size_t>(size, INT_MAX);
            const std::size_t completed = implementation.tree.write(
                node, position,
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer), requested));
            if (completed > static_cast<std::size_t>(INT_MAX)) {
                fail(EIO);
            }
            return static_cast<int>(completed);
        });
    }

    static int Create(const char* path, mode_t, fuse_file_info* info) noexcept
    {
        return errno_guard([&] {
            if (info == nullptr) {
                fail(EINVAL);
            }
            Impl& implementation = self();
            const std::wstring wide_path = checked_path(path);
            validate_parent(implementation.tree, wide_path);
            NodeHandle node = implementation.tree.create(wide_path, false, false);
            try {
                auto opened = std::make_unique<OpenHandle>();
                opened->node = node;
                implementation.install_handle(*info, std::move(opened));
            } catch (...) {
                try {
                    implementation.tree.remove(node);
                } catch (...) {
                }
                throw;
            }
            return 0;
        });
    }

    static int MakeNode(const char* path, mode_t mode, dev_t) noexcept
    {
        return errno_guard([&] {
            const mode_t type = mode & S_IFMT;
            if (type != 0 && type != S_IFREG) {
                fail(EOPNOTSUPP);
            }
            Impl& implementation = self();
            const std::wstring wide_path = checked_path(path);
            validate_parent(implementation.tree, wide_path);
            static_cast<void>(implementation.tree.create(wide_path, false, false));
            return 0;
        });
    }

    static int MakeDirectory(const char* path, mode_t) noexcept
    {
        return errno_guard([&] {
            Impl& implementation = self();
            const std::wstring wide_path = checked_path(path);
            validate_parent(implementation.tree, wide_path);
            static_cast<void>(implementation.tree.create(wide_path, true, false));
            return 0;
        });
    }

    static int Unlink(const char* path) noexcept
    {
        return errno_guard([&] {
            Impl& implementation = self();
            const std::wstring wide_path = checked_path(path);
            NodeHandle node = require_node(implementation.tree, wide_path);
            if (implementation.tree.info(node).directory) {
                fail(EISDIR);
            }
            implementation.tree.remove(node);
            return 0;
        });
    }

    static int RemoveDirectory(const char* path) noexcept
    {
        return errno_guard([&] {
            Impl& implementation = self();
            const std::wstring wide_path = checked_path(path);
            NodeHandle node = require_node(implementation.tree, wide_path);
            if (!implementation.tree.info(node).directory) {
                fail(ENOTDIR);
            }
            implementation.tree.remove(node);
            return 0;
        });
    }

    static int Rename(const char* old_path, const char* new_path, unsigned int flags) noexcept
    {
        return errno_guard([&] {
            if ((flags & ~kRenameNoReplace) != 0) {
                fail(EINVAL);
            }
            Impl& implementation = self();
            const std::wstring old_wide = checked_path(old_path);
            const std::wstring new_wide = checked_path(new_path);
            NodeHandle node = require_node(implementation.tree, old_wide);
            validate_parent(implementation.tree, new_wide);
            implementation.tree.rename(node, new_wide, (flags & kRenameNoReplace) == 0);
            return 0;
        });
    }

    static int Truncate(
        const char* path, off_t size, fuse_file_info* info) noexcept
    {
        return errno_guard([&] {
            if (size < 0) {
                fail(EINVAL);
            }
            Impl& implementation = self();
            NodeHandle node = node_for(implementation, path, info);
            if (implementation.tree.info(node).directory) {
                fail(EISDIR);
            }
            implementation.tree.resize(node, static_cast<std::uint64_t>(size));
            return 0;
        });
    }

    static int UpdateTimes(
        const char* path, const timespec times[2], fuse_file_info* info) noexcept
    {
        return errno_guard([&] {
            Impl& implementation = self();
            NodeHandle node = node_for(implementation, path, info);
            const auto now = std::chrono::system_clock::now();
            if (times == nullptr) {
                implementation.tree.set_times(
                    node, std::nullopt, std::optional(now), std::optional(now));
                return 0;
            }
            auto decode = [&](const timespec& value)
                -> std::optional<std::chrono::system_clock::time_point> {
                if (value.tv_nsec == UTIME_OMIT) {
                    return std::nullopt;
                }
                if (value.tv_nsec == UTIME_NOW) {
                    return now;
                }
                return from_timespec(value);
            };
            implementation.tree.set_times(
                node, std::nullopt, decode(times[0]), decode(times[1]));
            return 0;
        });
    }

    static int ReadDirectory(
        const char* path, void* buffer, fuse_fill_dir_t filler, off_t offset,
        fuse_file_info* info, fuse_readdir_flags flags) noexcept
    {
        return errno_guard([&] {
            if (buffer == nullptr || filler == nullptr || offset < 0) {
                fail(EINVAL);
            }
            static_cast<void>(flags);
            Impl& implementation = self();
            NodeHandle directory = node_for(implementation, path, info);
            const NodeInfo directory_info = implementation.tree.info(directory);
            if (!directory_info.directory) {
                fail(ENOTDIR);
            }
            const std::vector<DirectoryEntry> entries = implementation.tree.list(directory);
            const std::uint64_t start = static_cast<std::uint64_t>(offset);
            const std::uint64_t count = static_cast<std::uint64_t>(entries.size()) + 2;
            constexpr fuse_fill_dir_flags fill_flags =
                static_cast<fuse_fill_dir_flags>(0);
            for (std::uint64_t index = start; index < count; ++index) {
                std::string name;
                NodeHandle entry_node;
                NodeInfo entry_info;
                if (index == 0) {
                    name = ".";
                    entry_node = directory;
                    entry_info = directory_info;
                } else if (index == 1) {
                    name = "..";
                    entry_node = directory;
                    entry_info = directory_info;
                } else {
                    const DirectoryEntry& entry = entries[static_cast<std::size_t>(index - 2)];
                    name = wide_to_utf8(entry.name);
                    entry_info = entry.info;
                }
                struct stat stat_value{};
                if (entry_node) {
                    fill_stat(entry_node, entry_info, name, &stat_value);
                } else {
                    stat_value.st_mode = node_mode(entry_info, name);
                    stat_value.st_nlink = entry_info.directory ? 2 : 1;
                    stat_value.st_size = static_cast<off_t>(std::min<std::uint64_t>(
                        entry_info.size,
                        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())));
                    stat_value.st_atim = to_timespec(entry_info.access_time);
                    stat_value.st_mtim = to_timespec(entry_info.write_time);
                    stat_value.st_ctim = stat_value.st_mtim;
                }
                if (filler(
                        buffer, name.c_str(), &stat_value,
                        static_cast<off_t>(index + 1), fill_flags) != 0) {
                    break;
                }
            }
            return 0;
        });
    }

    static int Flush(const char*, fuse_file_info* info) noexcept
    {
        return errno_guard([&] {
            if (info != nullptr && info->fh != 0) {
                static_cast<void>(open_handle(info));
            }
            return 0;
        });
    }

    static int Sync(const char* path, int, fuse_file_info* info) noexcept
    {
        return Flush(path, info);
    }

    static int StatFs(const char*, struct statvfs* output) noexcept
    {
        return errno_guard([&] {
            if (output == nullptr) {
                fail(EINVAL);
            }
            Impl& implementation = self();
            const std::uint64_t quota = implementation.tree.write_quota();
            const std::uint64_t used = std::min(quota, implementation.tree.dirty_bytes());
            const std::uint64_t free = quota - used;
            std::memset(output, 0, sizeof(*output));
            output->f_bsize = static_cast<unsigned long>(kStatBlockSize);
            output->f_frsize = static_cast<unsigned long>(kStatBlockSize);
            output->f_blocks = static_cast<fsblkcnt_t>(
                quota / kStatBlockSize + (quota % kStatBlockSize != 0));
            output->f_bfree = static_cast<fsblkcnt_t>(free / kStatBlockSize);
            output->f_bavail = output->f_bfree;
            output->f_namemax = 255;
            return 0;
        });
    }

    [[nodiscard]] static const fuse_operations& operations() noexcept
    {
        static const fuse_operations value = [] {
            fuse_operations result{};
            result.getattr = GetAttr;
            result.read = Read;
            result.write = Write;
            result.open = Open;
            result.release = Release;
            result.opendir = OpenDir;
            result.releasedir = Release;
            result.readdir = ReadDirectory;
            result.create = Create;
            result.mknod = MakeNode;
            result.mkdir = MakeDirectory;
            result.unlink = Unlink;
            result.rmdir = RemoveDirectory;
            result.rename = Rename;
            result.truncate = Truncate;
            result.utimens = UpdateTimes;
            result.flush = Flush;
            result.fsync = Sync;
            result.statfs = StatFs;
            return result;
        }();
        return value;
    }

    int run(const std::function<void()>& mounted)
    {
        {
            std::lock_guard lock(lifecycle_mutex);
            if (running) {
                throw std::logic_error("FUSE filesystem is already running");
            }
            running = true;
        }

        char program_name[] = "steam2fs";
        char* argument_values[] = {program_name};
        fuse_args arguments = FUSE_ARGS_INIT(1, argument_values);
        fuse* instance = fuse_new(&arguments, &operations(), sizeof(fuse_operations), this);
        fuse_opt_free_args(&arguments);
        if (instance == nullptr) {
            std::lock_guard lock(lifecycle_mutex);
            running = false;
            throw std::runtime_error("cannot create FUSE filesystem");
        }

        bool is_mounted = false;
        bool signal_handlers = false;
        try {
            if (fuse_mount(instance, mount_point.c_str()) != 0) {
                throw std::system_error(errno, std::generic_category(), "cannot mount FUSE filesystem");
            }
            is_mounted = true;
            fuse_session* session = fuse_get_session(instance);
            if (fuse_set_signal_handlers(session) != 0) {
                throw std::system_error(
                    errno != 0 ? errno : EIO, std::generic_category(),
                    "cannot install FUSE signal handlers");
            }
            signal_handlers = true;
            {
                std::lock_guard lock(lifecycle_mutex);
                active_fuse = instance;
                if (exit_requested) {
                    fuse_exit(instance);
                }
            }
            if (mounted) {
                mounted();
            }
            const int result = fuse_loop(instance);
            const int loop_errno = errno;

            {
                std::lock_guard lock(lifecycle_mutex);
                active_fuse = nullptr;
            }
            fuse_remove_signal_handlers(session);
            signal_handlers = false;
            fuse_unmount(instance);
            is_mounted = false;
            fuse_destroy(instance);
            clear_handles();
            {
                std::lock_guard lock(lifecycle_mutex);
                running = false;
            }
            return result == -EINTR || (result == -1 && loop_errno == EINTR)
                ? 0
                : result;
        } catch (...) {
            {
                std::lock_guard lock(lifecycle_mutex);
                active_fuse = nullptr;
            }
            if (signal_handlers) {
                fuse_remove_signal_handlers(fuse_get_session(instance));
            }
            if (is_mounted) {
                fuse_unmount(instance);
            }
            fuse_destroy(instance);
            clear_handles();
            {
                std::lock_guard lock(lifecycle_mutex);
                running = false;
            }
            throw;
        }
    }

    void request_exit() noexcept
    {
        try {
            std::lock_guard lock(lifecycle_mutex);
            exit_requested = true;
            if (active_fuse != nullptr) {
                fuse_exit(active_fuse);
            }
        } catch (...) {
        }
    }
};

FuseFileSystem::FuseFileSystem(VirtualTree& tree, std::filesystem::path mount_point)
    : impl_(std::make_unique<Impl>(tree, std::move(mount_point))) {}

FuseFileSystem::~FuseFileSystem()
{
    request_exit();
}

int FuseFileSystem::run(const std::function<void()>& mounted)
{
    return impl_->run(mounted);
}

void FuseFileSystem::request_exit() noexcept
{
    if (impl_) {
        impl_->request_exit();
    }
}

} // namespace s2fs
