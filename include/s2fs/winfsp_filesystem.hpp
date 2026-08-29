#pragma once

#include "s2fs/virtual_tree.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace s2fs {

class WinFspFileSystem {
public:
    WinFspFileSystem(VirtualTree& tree, std::wstring mount_point, std::wstring volume_label);
    ~WinFspFileSystem();

    WinFspFileSystem(const WinFspFileSystem&) = delete;
    WinFspFileSystem& operator=(const WinFspFileSystem&) = delete;

    void start();
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace s2fs
