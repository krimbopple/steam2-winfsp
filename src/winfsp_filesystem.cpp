#include "s2fs/winfsp_filesystem.hpp"

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <bcrypt.h>
#include <winfsp/winfsp.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#pragma comment(lib, "Advapi32.lib")

namespace s2fs {
namespace {

constexpr std::uint64_t kWindowsEpochTicks = 116'444'736'000'000'000ULL;
constexpr std::uint64_t kAllocationUnit = 32ULL * 1024ULL;
constexpr std::size_t kMaxComponentLength = 255;
constexpr std::size_t kMaxPathLength = 32'767;

class NtStatusError final {
public:
    explicit NtStatusError(NTSTATUS status) noexcept : status_(status) {}
    [[nodiscard]] NTSTATUS status() const noexcept { return status_; }

private:
    NTSTATUS status_;
};

[[noreturn]] void fail(NTSTATUS status)
{
    throw NtStatusError(status);
}

[[nodiscard]] bool is_error(const std::error_code& code, std::errc condition) noexcept
{
    return code == std::make_error_code(condition) ||
        code.default_error_condition() == std::make_error_condition(condition);
}

[[nodiscard]] NTSTATUS status_from_error(const std::error_code& code) noexcept
{
    if (is_error(code, std::errc::no_such_file_or_directory))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (is_error(code, std::errc::file_exists))
        return STATUS_OBJECT_NAME_COLLISION;
    if (is_error(code, std::errc::not_a_directory))
        return STATUS_NOT_A_DIRECTORY;
    if (is_error(code, std::errc::is_a_directory))
        return STATUS_FILE_IS_A_DIRECTORY;
    if (is_error(code, std::errc::directory_not_empty))
        return STATUS_DIRECTORY_NOT_EMPTY;
    if (is_error(code, std::errc::no_space_on_device))
        return STATUS_DISK_FULL;
    if (is_error(code, std::errc::permission_denied) ||
        is_error(code, std::errc::operation_not_permitted) ||
        is_error(code, std::errc::read_only_file_system))
        return STATUS_ACCESS_DENIED;
    if (is_error(code, std::errc::invalid_argument))
        return STATUS_INVALID_PARAMETER;
    if (is_error(code, std::errc::filename_too_long))
        return STATUS_OBJECT_NAME_INVALID;
    if (is_error(code, std::errc::file_too_large) ||
        is_error(code, std::errc::value_too_large) ||
        is_error(code, std::errc::result_out_of_range))
        return STATUS_FILE_TOO_LARGE;
    if (is_error(code, std::errc::not_enough_memory))
        return STATUS_INSUFFICIENT_RESOURCES;
    if (is_error(code, std::errc::device_or_resource_busy))
        return STATUS_SHARING_VIOLATION;
    if (is_error(code, std::errc::operation_canceled))
        return STATUS_CANCELLED;
    if (is_error(code, std::errc::io_error))
        return STATUS_IO_DEVICE_ERROR;
    if (code.category() == std::system_category())
        return FspNtStatusFromWin32(static_cast<DWORD>(code.value()));
    return STATUS_UNEXPECTED_IO_ERROR;
}

template <typename Function>
NTSTATUS status_guard(Function&& function) noexcept
{
    try
    {
        return std::forward<Function>(function)();
    }
    catch (const NtStatusError& error)
    {
        return error.status();
    }
    catch (const std::bad_alloc&)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    catch (const std::system_error& error)
    {
        return status_from_error(error.code());
    }
    catch (const std::invalid_argument&)
    {
        return STATUS_INVALID_PARAMETER;
    }
    catch (const std::length_error&)
    {
        return STATUS_NAME_TOO_LONG;
    }
    catch (const std::out_of_range&)
    {
        return STATUS_INVALID_PARAMETER;
    }
    catch (...)
    {
        return STATUS_UNEXPECTED_IO_ERROR;
    }
}

[[nodiscard]] std::wstring_view checked_path(PCWSTR path)
{
    if (path == nullptr)
        fail(STATUS_INVALID_PARAMETER);

    const std::size_t length = wcsnlen_s(path, kMaxPathLength + 1);
    if (length == 0 || length > kMaxPathLength || path[0] != L'\\')
        fail(STATUS_OBJECT_NAME_INVALID);
    return {path, length};
}

[[nodiscard]] std::wstring_view checked_name(PCWSTR name)
{
    if (name == nullptr)
        fail(STATUS_INVALID_PARAMETER);

    const std::size_t length = wcsnlen_s(name, kMaxComponentLength + 1);
    if (length > kMaxComponentLength)
        fail(STATUS_OBJECT_NAME_INVALID);
    return {name, length};
}

[[nodiscard]] std::wstring_view parent_path(std::wstring_view path)
{
    if (path == L"\\")
        return {};
    const std::size_t separator = path.find_last_of(L'\\');
    if (separator == std::wstring_view::npos)
        fail(STATUS_OBJECT_NAME_INVALID);
    return separator == 0 ? path.substr(0, 1) : path.substr(0, separator);
}

[[nodiscard]] std::uint64_t to_file_time(std::chrono::system_clock::time_point time) noexcept
{
    using TickDuration = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
    const std::int64_t unix_ticks =
        std::chrono::duration_cast<TickDuration>(time.time_since_epoch()).count();
    constexpr std::int64_t epoch_ticks = static_cast<std::int64_t>(kWindowsEpochTicks);
    if (unix_ticks <= -epoch_ticks)
        return 0;
    if (unix_ticks > std::numeric_limits<std::int64_t>::max() - epoch_ticks)
        return static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return static_cast<std::uint64_t>(unix_ticks + epoch_ticks);
}

[[nodiscard]] std::chrono::system_clock::time_point from_file_time(std::uint64_t value)
{
    constexpr std::uint64_t max_value =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + kWindowsEpochTicks;
    if (value > max_value)
        fail(STATUS_INVALID_PARAMETER);

    using TickDuration = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
    const std::int64_t unix_ticks = value >= kWindowsEpochTicks
        ? static_cast<std::int64_t>(value - kWindowsEpochTicks)
        : -static_cast<std::int64_t>(kWindowsEpochTicks - value);
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(TickDuration(unix_ticks)));
}

[[nodiscard]] std::uint64_t allocation_size(std::uint64_t size) noexcept
{
    if (size == 0)
        return 0;
    if (size > std::numeric_limits<std::uint64_t>::max() - (kAllocationUnit - 1))
        return std::numeric_limits<std::uint64_t>::max();
    return (size + kAllocationUnit - 1) / kAllocationUnit * kAllocationUnit;
}

[[nodiscard]] int compare_names(std::wstring_view left, std::wstring_view right) noexcept
{
    if (left.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
        right.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        const int result = CompareStringOrdinal(
            left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE);
        if (result == CSTR_LESS_THAN)
            return -1;
        if (result == CSTR_GREATER_THAN)
            return 1;
        if (result == CSTR_EQUAL)
            return 0;
    }
    const int fallback = left.compare(right);
    return fallback < 0 ? -1 : fallback > 0 ? 1 : 0;
}

} // namespace

struct WinFspFileSystem::Impl {
    struct OpenContext {
        NodeHandle node;
        std::wstring path;
    };

    VirtualTree& tree;
    std::wstring mount_point;
    std::wstring volume_label;
    std::vector<std::byte> security_descriptor;
    std::uint64_t creation_time{};
    std::uint32_t serial_number{};
    FSP_FILE_SYSTEM* file_system{};
    bool dispatcher_started{};
    bool winfsp_loaded{};
    std::mutex lifecycle_mutex;
    std::mutex label_mutex;

    Impl(VirtualTree& tree_value, std::wstring mount, std::wstring label)
        : tree(tree_value), mount_point(std::move(mount)), volume_label(std::move(label))
    {
        if (volume_label.size() > std::size(FSP_FSCTL_VOLUME_INFO{}.VolumeLabel))
            throw std::invalid_argument("volume label is longer than 32 UTF-16 code units");

        PSECURITY_DESCRIPTOR descriptor = nullptr;
        ULONG descriptor_size = 0;
        constexpr PCWSTR permissive_sddl =
            L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;WD)";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                permissive_sddl, SDDL_REVISION_1, &descriptor, &descriptor_size))
            throw std::system_error(
                static_cast<int>(GetLastError()), std::system_category(),
                "cannot create the volume security descriptor");

        std::unique_ptr<void, decltype(&LocalFree)> owner(descriptor, &LocalFree);
        security_descriptor.resize(descriptor_size);
        std::memcpy(security_descriptor.data(), descriptor, descriptor_size);

        creation_time = to_file_time(std::chrono::system_clock::now());
        const std::uint64_t mixed = creation_time ^
            (creation_time >> 32) ^ static_cast<std::uint64_t>(std::hash<std::wstring>{}(mount_point));
        serial_number = static_cast<std::uint32_t>(mixed);
    }

    [[nodiscard]] static Impl& self(FSP_FILE_SYSTEM* file_system_value)
    {
        if (file_system_value == nullptr || file_system_value->UserContext == nullptr)
            fail(STATUS_INVALID_PARAMETER);
        return *static_cast<Impl*>(file_system_value->UserContext);
    }

    [[nodiscard]] static OpenContext& context(PVOID value)
    {
        if (value == nullptr)
            fail(STATUS_INVALID_HANDLE);
        return *static_cast<OpenContext*>(value);
    }

    static void validate_parent(VirtualTree& tree_value, std::wstring_view path)
    {
        const std::wstring_view parent = parent_path(path);
        if (parent.empty())
            fail(STATUS_ACCESS_DENIED);
        NodeHandle parent_node = tree_value.lookup(parent);
        if (!parent_node)
            fail(STATUS_OBJECT_PATH_NOT_FOUND);
        if (!tree_value.info(parent_node).directory)
            fail(STATUS_NOT_A_DIRECTORY);
    }

    [[nodiscard]] static NTSTATUS missing_path_status(VirtualTree& tree_value, std::wstring_view path)
    {
        const std::wstring_view parent = parent_path(path);
        if (parent.empty())
            return STATUS_OBJECT_NAME_NOT_FOUND;
        NodeHandle parent_node = tree_value.lookup(parent);
        if (!parent_node)
            return STATUS_OBJECT_PATH_NOT_FOUND;
        if (!tree_value.info(parent_node).directory)
            return STATUS_OBJECT_PATH_NOT_FOUND;
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    static void fill_node_info(
        const NodeInfo& source, UINT64 index_number, FSP_FSCTL_FILE_INFO* output)
    {
        if (output == nullptr)
            fail(STATUS_INVALID_PARAMETER);

        std::memset(output, 0, sizeof(*output));
        output->FileAttributes = source.attributes;
        if (source.directory)
        {
            output->FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
            output->FileAttributes &= ~FILE_ATTRIBUTE_NORMAL;
        }
        else
        {
            output->FileAttributes &= ~FILE_ATTRIBUTE_DIRECTORY;
            if (output->FileAttributes == 0)
                output->FileAttributes = FILE_ATTRIBUTE_NORMAL;
        }
        output->AllocationSize = allocation_size(source.size);
        output->FileSize = source.size;
        output->CreationTime = to_file_time(source.creation_time);
        output->LastAccessTime = to_file_time(source.access_time);
        output->LastWriteTime = to_file_time(source.write_time);
        output->ChangeTime = output->LastWriteTime;
        output->IndexNumber = index_number;
    }

    static void fill_file_info(
        VirtualTree& tree_value, const NodeHandle& node, FSP_FSCTL_FILE_INFO* output)
    {
        if (!node)
            fail(STATUS_INVALID_HANDLE);
        fill_node_info(
            tree_value.info(node),
            static_cast<UINT64>(reinterpret_cast<std::uintptr_t>(node.get())),
            output);
    }

    static void copy_security(
        const Impl& implementation,
        PSECURITY_DESCRIPTOR descriptor, SIZE_T* descriptor_size)
    {
        if (descriptor != nullptr && descriptor_size == nullptr)
            fail(STATUS_INVALID_PARAMETER);
        if (descriptor_size == nullptr)
            return;

        const SIZE_T required = implementation.security_descriptor.size();
        const SIZE_T available = *descriptor_size;
        *descriptor_size = required;
        if (available < required)
            fail(STATUS_BUFFER_OVERFLOW);
        if (descriptor != nullptr && required != 0)
            std::memcpy(descriptor, implementation.security_descriptor.data(), required);
    }

    static NTSTATUS GetVolumeInfo(
        FSP_FILE_SYSTEM* file_system_value, FSP_FSCTL_VOLUME_INFO* volume_info) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            if (volume_info == nullptr)
                fail(STATUS_INVALID_PARAMETER);
            Impl& implementation = self(file_system_value);
            const std::uint64_t total = implementation.tree.write_quota();
            const std::uint64_t used = std::min(total, implementation.tree.dirty_bytes());

            std::memset(volume_info, 0, sizeof(*volume_info));
            volume_info->TotalSize = total;
            volume_info->FreeSize = total - used;
            std::lock_guard lock(implementation.label_mutex);
            volume_info->VolumeLabelLength = static_cast<UINT16>(
                implementation.volume_label.size() * sizeof(WCHAR));
            if (!implementation.volume_label.empty())
                std::memcpy(
                    volume_info->VolumeLabel, implementation.volume_label.data(),
                    volume_info->VolumeLabelLength);
            return STATUS_SUCCESS;
        });
    }

    static NTSTATUS SetVolumeLabel(
        FSP_FILE_SYSTEM* file_system_value, PWSTR label,
        FSP_FSCTL_VOLUME_INFO* volume_info) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            if (label == nullptr || volume_info == nullptr)
                fail(STATUS_INVALID_PARAMETER);
            const std::size_t length = wcsnlen_s(label, 33);
            if (length > 32)
                fail(STATUS_INVALID_VOLUME_LABEL);

            Impl& implementation = self(file_system_value);
            {
                std::lock_guard lock(implementation.label_mutex);
                implementation.volume_label.assign(label, length);
            }
            return GetVolumeInfo(file_system_value, volume_info);
        });
    }

    static NTSTATUS GetSecurityByName(
        FSP_FILE_SYSTEM* file_system_value, PWSTR file_name, PUINT32 file_attributes,
        PSECURITY_DESCRIPTOR descriptor, SIZE_T* descriptor_size) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            Impl& implementation = self(file_system_value);
            const std::wstring_view path = checked_path(file_name);
            NodeHandle node = implementation.tree.lookup(path);
            if (!node)
                fail(missing_path_status(implementation.tree, path));

            if (file_attributes != nullptr)
            {
                FSP_FSCTL_FILE_INFO info{};
                fill_file_info(implementation.tree, node, &info);
                *file_attributes = info.FileAttributes;
            }
            copy_security(implementation, descriptor, descriptor_size);
            return STATUS_SUCCESS;
        });
    }

    static NTSTATUS Create(
        FSP_FILE_SYSTEM* file_system_value, PWSTR file_name, UINT32 create_options,
        UINT32 granted_access, UINT32 file_attributes,
        PSECURITY_DESCRIPTOR security_descriptor_value, UINT64 requested_allocation,
        PVOID* file_context, FSP_FSCTL_FILE_INFO* file_info) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            static_cast<void>(granted_access);
            static_cast<void>(security_descriptor_value);
            if (file_context == nullptr || file_info == nullptr)
                fail(STATUS_INVALID_PARAMETER);
            *file_context = nullptr;

            Impl& implementation = self(file_system_value);
            const std::wstring_view path = checked_path(file_name);
            if ((create_options & FILE_DIRECTORY_FILE) != 0 &&
                (create_options & FILE_NON_DIRECTORY_FILE) != 0)
                fail(STATUS_INVALID_PARAMETER);
            if (implementation.tree.lookup(path))
                fail(STATUS_OBJECT_NAME_COLLISION);
            validate_parent(implementation.tree, path);

            const bool directory = (create_options & FILE_DIRECTORY_FILE) != 0;
            if (!directory && requested_allocation > implementation.tree.write_quota())
                fail(STATUS_DISK_FULL);

            NodeHandle node = implementation.tree.create(path, directory, false);
            try
            {
                UINT32 attributes = file_attributes;
                if (directory)
                {
                    attributes |= FILE_ATTRIBUTE_DIRECTORY;
                    attributes &= ~FILE_ATTRIBUTE_NORMAL;
                }
                else
                {
                    attributes &= ~FILE_ATTRIBUTE_DIRECTORY;
                    attributes |= FILE_ATTRIBUTE_ARCHIVE;
                }
                implementation.tree.set_attributes(node, attributes);
                auto opened = std::make_unique<OpenContext>(OpenContext{node, std::wstring(path)});
                fill_file_info(implementation.tree, node, file_info);
                *file_context = opened.release();
            }
            catch (...)
            {
                try
                {
                    implementation.tree.remove(node);
                }
                catch (...)
                {
                }
                throw;
            }
            return STATUS_SUCCESS;
        });
    }

    static NTSTATUS Open(
        FSP_FILE_SYSTEM* file_system_value, PWSTR file_name, UINT32 create_options,
        UINT32 granted_access, PVOID* file_context,
        FSP_FSCTL_FILE_INFO* file_info) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            static_cast<void>(granted_access);
            if (file_context == nullptr || file_info == nullptr)
                fail(STATUS_INVALID_PARAMETER);
            *file_context = nullptr;

            Impl& implementation = self(file_system_value);
            const std::wstring_view path = checked_path(file_name);
            NodeHandle node = implementation.tree.lookup(path);
            if (!node)
                fail(missing_path_status(implementation.tree, path));
            const NodeInfo node_info = implementation.tree.info(node);
            if ((create_options & FILE_DIRECTORY_FILE) != 0 && !node_info.directory)
                fail(STATUS_NOT_A_DIRECTORY);
            if ((create_options & FILE_NON_DIRECTORY_FILE) != 0 && node_info.directory)
                fail(STATUS_FILE_IS_A_DIRECTORY);

            auto opened = std::make_unique<OpenContext>(OpenContext{node, std::wstring(path)});
            fill_file_info(implementation.tree, node, file_info);
            *file_context = opened.release();
            return STATUS_SUCCESS;
        });
    }

    static NTSTATUS Overwrite(
        FSP_FILE_SYSTEM* file_system_value, PVOID file_context, UINT32 file_attributes,
        BOOLEAN replace_file_attributes, UINT64 requested_allocation,
        FSP_FSCTL_FILE_INFO* file_info) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            if (file_info == nullptr)
                fail(STATUS_INVALID_PARAMETER);
            Impl& implementation = self(file_system_value);
            OpenContext& opened = context(file_context);
            const NodeInfo old_info = implementation.tree.info(opened.node);
            if (old_info.directory)
                fail(STATUS_FILE_IS_A_DIRECTORY);
            if (requested_allocation > implementation.tree.write_quota())
                fail(STATUS_DISK_FULL);

            implementation.tree.resize(opened.node, 0);
            UINT32 attributes = replace_file_attributes
                ? file_attributes
                : old_info.attributes | file_attributes;
            attributes &= ~FILE_ATTRIBUTE_DIRECTORY;
            attributes |= FILE_ATTRIBUTE_ARCHIVE;
            implementation.tree.set_attributes(opened.node, attributes);
            fill_file_info(implementation.tree, opened.node, file_info);
            return STATUS_SUCCESS;
        });
    }

    static VOID Cleanup(
        FSP_FILE_SYSTEM* file_system_value, PVOID file_context, PWSTR file_name,
        ULONG flags) noexcept
    {
        try
        {
            Impl& implementation = self(file_system_value);
            OpenContext& opened = context(file_context);
            if ((flags & FspCleanupDelete) != 0)
                static_cast<void>(checked_path(file_name));

            const auto now = std::chrono::system_clock::now();
            implementation.tree.set_times(
                opened.node,
                std::nullopt,
                (flags & FspCleanupSetLastAccessTime) != 0
                    ? std::optional(now) : std::nullopt,
                (flags & (FspCleanupSetLastWriteTime | FspCleanupSetChangeTime)) != 0
                    ? std::optional(now) : std::nullopt);
            if ((flags & FspCleanupSetArchiveBit) != 0)
            {
                const NodeInfo info = implementation.tree.info(opened.node);
                if (!info.directory)
                    implementation.tree.set_attributes(
                        opened.node, info.attributes | FILE_ATTRIBUTE_ARCHIVE);
            }
            if ((flags & FspCleanupDelete) != 0)
                implementation.tree.remove(opened.node);
        }
        catch (...)
        {
        }
    }

    static VOID Close(FSP_FILE_SYSTEM* file_system_value, PVOID file_context) noexcept
    {
        try
        {
            if (file_system_value == nullptr ||
                file_system_value->UserContext == nullptr ||
                file_context == nullptr)
                return;
            delete static_cast<OpenContext*>(file_context);
        }
        catch (...)
        {
        }
    }

    static NTSTATUS Read(
        FSP_FILE_SYSTEM* file_system_value, PVOID file_context, PVOID buffer,
        UINT64 offset, ULONG length, PULONG bytes_transferred) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            if (bytes_transferred == nullptr || (length != 0 && buffer == nullptr))
                fail(STATUS_INVALID_PARAMETER);
            *bytes_transferred = 0;
            Impl& implementation = self(file_system_value);
            OpenContext& opened = context(file_context);
            const NodeInfo info = implementation.tree.info(opened.node);
            if (info.directory)
                fail(STATUS_FILE_IS_A_DIRECTORY);
            if (length == 0)
                return STATUS_SUCCESS;
            if (offset >= info.size)
                return STATUS_END_OF_FILE;

            const std::uint64_t available = info.size - offset;
            const std::size_t requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(length, available));
            const std::size_t read = implementation.tree.read(
                opened.node, offset,
                std::span<std::byte>(static_cast<std::byte*>(buffer), requested));
            if (read > requested || read > std::numeric_limits<ULONG>::max())
                fail(STATUS_IO_DEVICE_ERROR);
            *bytes_transferred = static_cast<ULONG>(read);
            return STATUS_SUCCESS;
        });
    }

    static NTSTATUS Write(
        FSP_FILE_SYSTEM* file_system_value, PVOID file_context, PVOID buffer,
        UINT64 offset, ULONG length, BOOLEAN write_to_end_of_file,
        BOOLEAN constrained_io, PULONG bytes_transferred,
        FSP_FSCTL_FILE_INFO* file_info) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            if (bytes_transferred == nullptr || file_info == nullptr ||
                (length != 0 && buffer == nullptr))
                fail(STATUS_INVALID_PARAMETER);
            *bytes_transferred = 0;

            Impl& implementation = self(file_system_value);
            OpenContext& opened = context(file_context);
            NodeInfo info = implementation.tree.info(opened.node);
            if (info.directory)
                fail(STATUS_FILE_IS_A_DIRECTORY);

            if (write_to_end_of_file)
                offset = info.size;
            else if (offset == std::numeric_limits<UINT64>::max())
                fail(STATUS_INVALID_PARAMETER);

            std::size_t write_length = length;
            if (constrained_io)
            {
                if (offset >= info.size)
                {
                    fill_file_info(implementation.tree, opened.node, file_info);
                    return STATUS_SUCCESS;
                }
                write_length = static_cast<std::size_t>(
                    std::min<std::uint64_t>(length, info.size - offset));
            }
            else if (length > std::numeric_limits<UINT64>::max() - offset)
            {
                fail(STATUS_FILE_TOO_LARGE);
            }

            if (write_length != 0)
            {
                const std::size_t written = implementation.tree.write(
                    opened.node, offset,
                    std::span<const std::byte>(
                        static_cast<const std::byte*>(buffer), write_length));
                if (written > write_length || written > std::numeric_limits<ULONG>::max())
                    fail(STATUS_IO_DEVICE_ERROR);
                *bytes_transferred = static_cast<ULONG>(written);
            }
            fill_file_info(implementation.tree, opened.node, file_info);
            return STATUS_SUCCESS;
        });
    }

    static NTSTATUS Flush(
        FSP_FILE_SYSTEM* file_system_value, PVOID file_context,
        FSP_FSCTL_FILE_INFO* file_info) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            Impl& implementation = self(file_system_value);
            if (file_context != nullptr)
            {
                if (file_info == nullptr)
                    fail(STATUS_INVALID_PARAMETER);
                fill_file_info(implementation.tree, context(file_context).node, file_info);
            }
            return STATUS_SUCCESS;
        });
    }

    static NTSTATUS GetFileInfo(
        FSP_FILE_SYSTEM* file_system_value, PVOID file_context,
        FSP_FSCTL_FILE_INFO* file_info) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            if (file_info == nullptr)
                fail(STATUS_INVALID_PARAMETER);
            Impl& implementation = self(file_system_value);
            fill_file_info(implementation.tree, context(file_context).node, file_info);
            return STATUS_SUCCESS;
        });
    }

    static NTSTATUS SetBasicInfo(
        FSP_FILE_SYSTEM* file_system_value, PVOID file_context, UINT32 file_attributes,
        UINT64 creation, UINT64 access, UINT64 write, UINT64 change,
        FSP_FSCTL_FILE_INFO* file_info) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            if (change != 0)
                static_cast<void>(from_file_time(change));
            if (file_info == nullptr)
                fail(STATUS_INVALID_PARAMETER);
            Impl& implementation = self(file_system_value);
            OpenContext& opened = context(file_context);
            const NodeInfo old_info = implementation.tree.info(opened.node);
            const std::optional creation_time_value =
                creation != 0 ? std::optional(from_file_time(creation)) : std::nullopt;
            const std::optional access_time_value =
                access != 0 ? std::optional(from_file_time(access)) : std::nullopt;
            const std::optional write_time_value =
                write != 0 ? std::optional(from_file_time(write)) : std::nullopt;


            if (file_attributes != INVALID_FILE_ATTRIBUTES)
            {
                UINT32 attributes = file_attributes;
                if (old_info.directory)
                    attributes |= FILE_ATTRIBUTE_DIRECTORY;
                else
                    attributes &= ~FILE_ATTRIBUTE_DIRECTORY;
                implementation.tree.set_attributes(opened.node, attributes);
            }
            implementation.tree.set_times(
                opened.node, creation_time_value, access_time_value, write_time_value);
            fill_file_info(implementation.tree, opened.node, file_info);
            return STATUS_SUCCESS;
        });
    }

    static NTSTATUS SetFileSize(
        FSP_FILE_SYSTEM* file_system_value, PVOID file_context, UINT64 new_size,
        BOOLEAN set_allocation_size, FSP_FSCTL_FILE_INFO* file_info) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            if (file_info == nullptr)
                fail(STATUS_INVALID_PARAMETER);
            Impl& implementation = self(file_system_value);
            OpenContext& opened = context(file_context);
            const NodeInfo info = implementation.tree.info(opened.node);
            if (info.directory)
                fail(STATUS_FILE_IS_A_DIRECTORY);

            if (set_allocation_size)
            {
                if (new_size > implementation.tree.write_quota())
                    fail(STATUS_DISK_FULL);
                if (new_size < info.size)
                    implementation.tree.resize(opened.node, new_size);
            }
            else
            {
                implementation.tree.resize(opened.node, new_size);
            }
            fill_file_info(implementation.tree, opened.node, file_info);
            return STATUS_SUCCESS;
        });
    }

    static NTSTATUS CanDelete(
        FSP_FILE_SYSTEM* file_system_value, PVOID file_context, PWSTR file_name) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            static_cast<void>(checked_path(file_name));
            Impl& implementation = self(file_system_value);
            OpenContext& opened = context(file_context);
            if (opened.path == L"\\")
                fail(STATUS_ACCESS_DENIED);
            if (implementation.tree.info(opened.node).directory &&
                !implementation.tree.list(opened.node).empty())
                fail(STATUS_DIRECTORY_NOT_EMPTY);
            return STATUS_SUCCESS;
        });
    }

    static NTSTATUS Rename(
        FSP_FILE_SYSTEM* file_system_value, PVOID file_context, PWSTR file_name,
        PWSTR new_file_name, BOOLEAN replace_if_exists) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            static_cast<void>(checked_path(file_name));
            const std::wstring_view new_path_view = checked_path(new_file_name);
            Impl& implementation = self(file_system_value);
            OpenContext& opened = context(file_context);
            if (opened.path == L"\\")
                fail(STATUS_ACCESS_DENIED);
            validate_parent(implementation.tree, new_path_view);

            std::wstring new_path(new_path_view);
            implementation.tree.rename(opened.node, new_path, replace_if_exists != FALSE);
            opened.path.swap(new_path);
            return STATUS_SUCCESS;
        });
    }

    static NTSTATUS GetSecurity(
        FSP_FILE_SYSTEM* file_system_value, PVOID file_context,
        PSECURITY_DESCRIPTOR descriptor, SIZE_T* descriptor_size) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            Impl& implementation = self(file_system_value);
            static_cast<void>(context(file_context));
            if (descriptor_size == nullptr)
                fail(STATUS_INVALID_PARAMETER);
            copy_security(implementation, descriptor, descriptor_size);
            return STATUS_SUCCESS;
        });
    }

    static NTSTATUS ReadDirectory(
        FSP_FILE_SYSTEM* file_system_value, PVOID file_context, PWSTR pattern,
        PWSTR marker, PVOID buffer, ULONG length, PULONG bytes_transferred) noexcept
    {
        return status_guard([&]() -> NTSTATUS {
            static_cast<void>(pattern);
            if (bytes_transferred == nullptr || (length != 0 && buffer == nullptr))
                fail(STATUS_INVALID_PARAMETER);
            *bytes_transferred = 0;

            Impl& implementation = self(file_system_value);
            OpenContext& opened = context(file_context);
            const NodeInfo directory_node_info = implementation.tree.info(opened.node);
            if (!directory_node_info.directory)
                fail(STATUS_NOT_A_DIRECTORY);
            const std::wstring_view marker_name = marker != nullptr
                ? checked_name(marker) : std::wstring_view{};

            std::vector<DirectoryEntry> entries = implementation.tree.list(opened.node);
            if (opened.path != L"\\")
            {
                entries.push_back(DirectoryEntry{L".", directory_node_info});
                NodeHandle parent = implementation.tree.lookup(parent_path(opened.path));
                if (!parent)
                    fail(STATUS_OBJECT_PATH_NOT_FOUND);
                entries.push_back(DirectoryEntry{L"..", implementation.tree.info(parent)});
            }
            std::sort(entries.begin(), entries.end(), [](const DirectoryEntry& left,
                                                      const DirectoryEntry& right) {
                return compare_names(left.name, right.name) < 0;
            });

            alignas(FSP_FSCTL_DIR_INFO)
                std::array<std::byte,
                    sizeof(FSP_FSCTL_DIR_INFO) + kMaxComponentLength * sizeof(WCHAR)> storage{};
            bool complete = true;
            for (const DirectoryEntry& entry : entries)
            {
                if (entry.name.size() > kMaxComponentLength)
                    fail(STATUS_OBJECT_NAME_INVALID);
                if (marker != nullptr && compare_names(entry.name, marker_name) <= 0)
                    continue;

                std::fill(storage.begin(), storage.end(), std::byte{});
                auto* directory_info = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(storage.data());
                directory_info->Size = static_cast<UINT16>(
                    sizeof(FSP_FSCTL_DIR_INFO) + entry.name.size() * sizeof(WCHAR));
                const std::size_t parent_id =
                    reinterpret_cast<std::uintptr_t>(opened.node.get());
                const std::size_t name_id = std::hash<std::wstring>{}(entry.name);
                fill_node_info(
                    entry.info,
                    static_cast<UINT64>(
                        name_id ^ (parent_id + 0x9e3779b9U +
                            (name_id << 6) + (name_id >> 2))),
                    &directory_info->FileInfo);
                if (!entry.name.empty())
                    std::memcpy(
                        directory_info->FileNameBuf, entry.name.data(),
                        entry.name.size() * sizeof(WCHAR));
                if (!FspFileSystemAddDirInfo(
                        directory_info, buffer, length, bytes_transferred))
                {
                    complete = false;
                    break;
                }
            }
            if (complete)
                FspFileSystemAddDirInfo(nullptr, buffer, length, bytes_transferred);
            return STATUS_SUCCESS;
        });
    }

    [[nodiscard]] static const FSP_FILE_SYSTEM_INTERFACE& interface_table() noexcept
    {
        static const FSP_FILE_SYSTEM_INTERFACE table = [] {
            FSP_FILE_SYSTEM_INTERFACE value{};
            value.GetVolumeInfo = &GetVolumeInfo;
            value.SetVolumeLabel = &SetVolumeLabel;
            value.GetSecurityByName = &GetSecurityByName;
            value.Create = &Create;
            value.Open = &Open;
            value.Overwrite = &Overwrite;
            value.Cleanup = &Cleanup;
            value.Close = &Close;
            value.Read = &Read;
            value.Write = &Write;
            value.Flush = &Flush;
            value.GetFileInfo = &GetFileInfo;
            value.SetBasicInfo = &SetBasicInfo;
            value.SetFileSize = &SetFileSize;
            value.CanDelete = &CanDelete;
            value.Rename = &Rename;
            value.GetSecurity = &GetSecurity;
            value.ReadDirectory = &ReadDirectory;
            return value;
        }();
        return table;
    }

    void start()
    {
        std::lock_guard lock(lifecycle_mutex);
        if (dispatcher_started)
            return;
        if (!winfsp_loaded)
        {
            const NTSTATUS load_status = FspLoad(nullptr);
            if (!NT_SUCCESS(load_status))
                throw_load_status(load_status);
            winfsp_loaded = true;
        }


        FSP_FSCTL_VOLUME_PARAMS parameters{};
        parameters.Version = sizeof(parameters);
        parameters.SectorSize = 512;
        parameters.SectorsPerAllocationUnit =
            static_cast<UINT16>(kAllocationUnit / parameters.SectorSize);
        parameters.MaxComponentLength = static_cast<UINT16>(kMaxComponentLength);
        parameters.VolumeCreationTime = creation_time;
        parameters.VolumeSerialNumber = serial_number;
        parameters.CaseSensitiveSearch = 0;
        parameters.CasePreservedNames = 1;
        parameters.UnicodeOnDisk = 1;
        parameters.ReadOnlyVolume = 0;
        parameters.PostCleanupWhenModifiedOnly = 1;
        parameters.PostDispositionWhenNecessaryOnly = 1;
        parameters.FlushAndPurgeOnCleanup = 0;
        parameters.SupportsPosixUnlinkRename = 1;
        parameters.UmFileContextIsUserContext2 = 1;
        parameters.AllowOpenInKernelMode = 1;
        wcscpy_s(parameters.FileSystemName, L"NTFS");

        NTSTATUS status = FspFileSystemCreate(
            const_cast<PWSTR>(L"" FSP_FSCTL_DISK_DEVICE_NAME),
            &parameters, &interface_table(), &file_system);
        if (!NT_SUCCESS(status))
            throw_status(status, "cannot create the WinFsp filesystem");

        file_system->UserContext = this;
        status = FspFileSystemSetMountPoint(
            file_system, mount_point.empty() ? nullptr : mount_point.data());
        if (!NT_SUCCESS(status))
        {
            FspFileSystemDelete(std::exchange(file_system, nullptr));
            throw_status(status, "cannot set the WinFsp mount point");
        }

        status = FspFileSystemStartDispatcher(file_system, 0);
        if (!NT_SUCCESS(status))
        {
            FspFileSystemDelete(std::exchange(file_system, nullptr));
            throw_status(status, "cannot start the WinFsp dispatcher");
        }
        dispatcher_started = true;
    }

    void stop() noexcept
    {
        std::lock_guard lock(lifecycle_mutex);
        if (file_system == nullptr)
            return;
        if (dispatcher_started)
        {
            FspFileSystemStopDispatcher(file_system);
            dispatcher_started = false;
        }
        FspFileSystemDelete(std::exchange(file_system, nullptr));
    }

    [[noreturn]] static void throw_load_status(NTSTATUS status)
    {
        const int error = status == STATUS_DLL_NOT_FOUND ||
                status == STATUS_OBJECT_NAME_NOT_FOUND
            ? ERROR_MOD_NOT_FOUND
            : ERROR_DLL_INIT_FAILED;
        throw std::system_error(
            error, std::system_category(),
            "cannot load the installed WinFsp v2.1 runtime");
    }

    [[noreturn]] static void throw_status(NTSTATUS status, const char* message)
    {
        throw std::system_error(
            static_cast<int>(FspWin32FromNtStatus(status)), std::system_category(), message);
    }
};

WinFspFileSystem::WinFspFileSystem(
    VirtualTree& tree, std::wstring mount_point, std::wstring volume_label)
    : impl_(std::make_unique<Impl>(
          tree, std::move(mount_point), std::move(volume_label)))
{
}

WinFspFileSystem::~WinFspFileSystem()
{
    stop();
}

void WinFspFileSystem::start()
{
    impl_->start();
}

void WinFspFileSystem::stop() noexcept
{
    if (impl_)
        impl_->stop();
}

} // namespace s2fs
