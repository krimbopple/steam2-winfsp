#pragma once

#include <filesystem>
#include <functional>
#include <memory>

namespace s2fs {

class VirtualTree;

class FuseFileSystem {
public:
    FuseFileSystem(VirtualTree& tree, std::filesystem::path mount_point);
    ~FuseFileSystem();

    FuseFileSystem(const FuseFileSystem&) = delete;
    FuseFileSystem& operator=(const FuseFileSystem&) = delete;

    // Mounts in the foreground, invokes mounted after the mount is live, and
    // blocks until FUSE is asked to exit. Returns the fuse loop result.
    int run(const std::function<void()>& mounted = {});
    void request_exit() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace s2fs
