#include "s2fs/steam2_archive.hpp"
#include "s2fs/build_definition.hpp"

#include "s2fs/virtual_tree.hpp"
#include "s2fs/winfsp_filesystem.hpp"

#include <windows.h>

#include <charconv>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <vector>

namespace {

struct Options {
    std::filesystem::path build_path;
    std::wstring mount_point;
    std::wstring volume_label{L"Steam2"};
    std::uint64_t write_quota_bytes{512ULL << 20};
    std::optional<std::filesystem::path> steam_dll_path;
    bool mirror_steam_dll{true};
    bool wait_stdin{};


    bool inspect_only{};
};

std::mutex stop_mutex;
std::condition_variable stop_condition;
bool stop_requested{};

void request_stop() {
    {
        std::lock_guard lock(stop_mutex);
        stop_requested = true;
    }
    stop_condition.notify_all();
}

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        request_stop();
        return TRUE;
    }
    return FALSE;
}


[[noreturn]] void usage(std::string_view message = {}) {
    if (!message.empty()) {
        std::cerr << "error: " << message << "\n\n";
    }
    std::cerr
        << "usage: steam2fs --build FILE [--mount X:] [--label NAME] "
           "[--quota-mib N] [--steam-dll FILE|--no-steam-dll] [--wait-stdin] [--inspect]\n";

    std::cerr << "       RAM writes are capped at 512 MiB by default and discarded on unmount.\n";

    throw std::invalid_argument("invalid command line");
}

std::uint64_t parse_u64(std::string_view value, std::string_view option) {
    std::uint64_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        usage(std::string(option) + " requires an unsigned integer");
    }
    return result;
}

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        auto require_value = [&]() -> std::wstring_view {
            if (++index >= argc) {
                usage("an option is missing its value");
            }
            return argv[index];
        };

        if (argument == L"--build") {
            options.build_path = std::filesystem::path(require_value());
        } else if (argument == L"--mount") {
            options.mount_point = std::wstring(require_value());
        } else if (argument == L"--label") {
            options.volume_label = std::wstring(require_value());
        } else if (argument == L"--quota-mib") {
            const auto narrow = s2fs::wide_to_utf8(require_value());
            const auto mib = parse_u64(narrow, "--quota-mib");
            if (mib == 0 || mib > (1ULL << 30)) {
                usage("--quota-mib is outside the supported range");
            }
            options.write_quota_bytes = mib * 1024ULL * 1024ULL;
        } else if (argument == L"--steam-dll") {
            options.steam_dll_path = std::filesystem::path(require_value());
        } else if (argument == L"--no-steam-dll") {
            options.mirror_steam_dll = false;
        } else if (argument == L"--wait-stdin") {
            options.wait_stdin = true;


        } else if (argument == L"--inspect") {
            options.inspect_only = true;
        } else if (argument == L"--help" || argument == L"-h") {
            usage();
        } else {
            usage("unknown option");
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

std::optional<std::wstring> environment_value(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) {
        return std::nullopt;
    }
    value.resize(written);
    return value;
}

std::optional<std::wstring> registry_value(
    HKEY root,
    const wchar_t* key,
    const wchar_t* name) {
    DWORD bytes{};
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

std::optional<std::filesystem::path> discover_steam_dll(const Options& options) {
    if (options.steam_dll_path) {
        if (!std::filesystem::is_regular_file(*options.steam_dll_path)) {
            throw std::runtime_error(
                "the explicit Steam DLL does not exist: " + options.steam_dll_path->string());
        }
        return options.steam_dll_path;
    }
    if (const auto configured = environment_value(L"STEAM_DLL_PATH")) {
        const std::filesystem::path candidate(*configured);
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    if (const auto steam_path = registry_value(
            HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath")) {
        const auto candidate = std::filesystem::path(*steam_path) / L"steam.dll";
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    if (const auto program_files = environment_value(L"ProgramFiles(x86)")) {
        const auto candidate = std::filesystem::path(*program_files) / L"Steam" / L"steam.dll";
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

void mirror_steam_runtime(s2fs::VirtualTree& tree, const Options& options) {
    if (!options.mirror_steam_dll ||
        !tree.lookup(L"\\bin\\FileSystem_Steam.dll") ||
        tree.lookup(L"\\bin\\Steam.dll")) {
        return;
    }
    const auto source = discover_steam_dll(options);
    if (!source) {
        std::cerr
            << "warning: projected FileSystem_Steam.dll requires Steam.dll, "
               "but no installed Steam DLL was found\n";
        return;
    }

    const auto size = std::filesystem::file_size(*source);
    if (size > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("Steam DLL is too large for this process");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(*source, std::ios::binary);
    if (!stream ||
        (!bytes.empty() &&
         (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
          stream.gcount() != static_cast<std::streamsize>(bytes.size())))) {
        throw std::runtime_error("cannot read Steam DLL: " + source->string());
    }

    const auto node = tree.create(L"\\bin\\Steam.dll", false, false);
    const auto written = tree.write(node, 0, bytes);
    if (written != bytes.size()) {
        throw std::runtime_error("short write while mirroring Steam DLL into RAM");
    }
    std::wcout << L"mirrored " << source->wstring()
               << L" -> \\\\bin\\\\Steam.dll (" << bytes.size() << L" bytes, RAM only)\n";
}


} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto specs = s2fs::load_build_definition(options.build_path);
        s2fs::VirtualTree tree(options.write_quota_bytes);

        std::uint64_t file_count{};
        std::uint64_t output_bytes{};
        for (const auto& spec : specs) {
            std::cout << "loading depot " << spec.id << " version " << spec.version << "...\n";
            auto depot = s2fs::Steam2Depot::load(spec);
            for (const auto& entry : depot.entries()) {
                ++file_count;
                output_bytes += entry.file->size();
            }
            tree.overlay(depot);
            std::cout << "  " << depot.entries().size() << " files\n";
        }

        std::cout << "build ready: " << file_count << " depot files, "
                  << output_bytes << " logical bytes before overlay collisions\n";
        mirror_steam_runtime(tree, options);

        if (options.inspect_only) {
            return 0;
        }

        if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
            throw std::runtime_error("cannot install console control handler");
        }

        s2fs::WinFspFileSystem file_system(tree, options.mount_point, options.volume_label);
        file_system.start();
        std::wcout << L"mounted at " << options.mount_point
                   << L"; press Ctrl+C to unmount\n" << std::flush;
        if (options.wait_stdin) {
            std::thread([] {
                std::string command;
                if (std::getline(std::cin, command)) {
                    request_stop();
                }
            }).detach();
        }


        {
            std::unique_lock lock(stop_mutex);
            stop_condition.wait(lock, [] { return stop_requested; });
        }

        file_system.stop();
        SetConsoleCtrlHandler(console_handler, FALSE);
        return 0;
    } catch (const std::invalid_argument&) {
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << "\n";
        return 1;
    }
}
