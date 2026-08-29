# Steam2 Depot Manager

Steam2 Depot Manager mounts user-supplied Steam2 `.blob` and `.dat` archives as a normal writable filesystem without extracting the game. The native worker decodes archive chunks on demand through WinFsp on Windows or FUSE3 on Linux, while the Qt 6 GUI scans loose libraries, resolves depot ancestry, pairs DATs, composes overlays, mounts builds, and launches the first executable found in the projected tree.

> This project does not contain or distribute Valve game payloads. Users provide their own Steam2 archive files.

## Features

- Recursive scanning of loose `DEPOT_VERSION_CRC_SHA256.blob|dat` files.
- Validated blob parsing, parent-CRC ancestry, reset roots, DAT-size pairing, and depot-key readiness.
- Exact source paths: files may live in arbitrary folders or on different drives.
- Ordered multi-depot overlays; later depots replace earlier path collisions.
- Random-access zlib/AES-CFB Steam2 decoding without full extraction.
- WinFsp filesystem on Windows and FUSE3 filesystem on Linux.
- Executable and memory-mapped I/O support with Linux execute-bit heuristics.
- Ephemeral 32 KiB copy-on-write pages with a 512 MiB default RAM cap.
- RAM-only automatic mirroring of an installed `steam.dll` on Windows when a build requires it.
- Deterministic executable selection: root executables first, then recursive paths.
- Windows shell launching; Linux launches `.exe` files through Wine and native executables directly.
- Embedded labels covering all 10,876 archived depot IDs, derived from the annotated TeraRelease index and backfilled where current SteamKit PICS appinfo could provide a definitive reference.

All writes are discarded when the mount ends. There is no persistent-overlay mode.

## End-user requirements

### Windows

- 64-bit Windows 10 or 11.
- [WinFsp 2.1](https://github.com/winfsp/winfsp/releases/tag/v2.1) installed. Newer compatible 2.x versions may work, but 2.1 is the verified target.
- [Microsoft Visual C++ 2015-2022 x64 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).

Qt DLLs and plugins are included in Windows release ZIPs.

### Linux

- A Linux distribution with FUSE3 and `/dev/fuse` access.
- Qt 6.4 or newer for the GUI.
- OpenSSL 3 development/runtime libraries.
- Wine on `PATH` to launch projected Windows `.exe` files from the GUI.

Ubuntu 24.04 packages:

```bash
sudo apt install libfuse3-3 libssl3 qt6-base-dev
```

Both platforms require user-supplied Steam2 `.blob` and `.dat` files.

## Using the GUI

1. Launch `steam2gui.exe` on Windows or `steam2gui` on Linux.
2. Select **Add Folder** and choose one or more folders containing loose Steam2 files.
3. Select **Scan**. Scanning is recursive.
4. Expand a depot and add only revisions marked **Ready**.
5. Order the composition from bottom to top. Later entries win collisions.
6. Choose a free drive letter/folder on Windows or an empty mount directory on Linux, then select **Mount**.
7. Edit executable arguments if needed, then select **Play**.
8. Close the game and select **Unmount**.

The GUI writes a temporary exact-path build definition and supervises the sibling `steam2fs` worker as a separate process. It never copies or reorganizes archive files.

### Example CS:S café/CZ beta composition

For a loose library containing the app-240 version-zero roots, use:

1. Depot 200 v0 — Source Engine Runtime
2. Depot 242 v0 — Counter-Strike: Source Content
3. Depot 241 v0 — Counter-Strike: Source Client

Launch arguments:

```text
-game cstrike -steam
```

The original 2004 AppID-260 public beta is not present in the currently studied archive. Available depots 261-263 are the later 2010 `cstrike_beta` family.

## Readiness states

A revision is **Ready** only when:

- the blob parses successfully;
- its full parent-CRC ancestry reaches a zero-parent root;
- every ancestry blob has exactly one DAT with the embedded expected size;
- there are no duplicate blob identities or ambiguous DAT matches;
- a depot key is available.

Corrupt, missing-parent, missing-DAT, ambiguous, duplicate, and missing-key states are displayed rather than silently normalized.

## Native worker

Validate a directory-based build definition without mounting:

```powershell
steam2fs.exe --build C:\path\build.json --inspect
```

Mount with the default 512 MiB ephemeral write cap:

```powershell
steam2fs.exe --build C:\path\build.json --mount R:
```

Optional switches:

```text
--quota-mib N       Override the RAM write cap.
--steam-dll FILE    Override installed steam.dll discovery.
--no-steam-dll      Disable RAM-only Steam DLL mirroring.
--wait-stdin        Unmount when a line is received on stdin (used by the GUI).
--inspect           Validate and list the build without mounting.
```

Linux inspection and mount:

```bash
steam2fs --build /path/to/build.json --inspect
mkdir -p /tmp/steam2-mount
steam2fs --build /path/to/build.json --mount /tmp/steam2-mount
```

Linux accepts `--quota-mib`, `--wait-stdin`, and `--inspect`. Steam DLL mirroring switches are Windows-only.

## Building from source

### Requirements

- CMake 3.28 or newer.
- A C++23 compiler.
- Qt 6.4 or newer for GUI builds.
- Git, used by CMake FetchContent for pinned zlib and nlohmann/json sources.

Windows additionally requires Visual Studio 2022/2026 and a matching Qt MSVC x64 development build. Linux additionally requires FUSE3 and OpenSSL development packages.

Set `QT_ROOT` to the Qt architecture directory:

```powershell
$env:QT_ROOT = "C:\Qt\6.8.3\msvc2022_64"
```

Visual Studio 2022:

```powershell
cmake --preset vs2022-x64
cmake --build --preset vs2022-release --parallel
ctest --preset vs2022-release
cmake --build out/build-vs2022 --config MinSizeRel --target package_gui
```

Visual Studio 2026:

```powershell
cmake --preset vs2026-x64
cmake --build --preset vs2026-release --parallel
ctest --preset vs2026-release
cmake --build out/build-vs2026 --config MinSizeRel --target package_gui
```

Linux/WSL:

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
  libfuse3-dev libssl-dev qt6-base-dev qt6-base-dev-tools

cmake --preset linux-ninja
cmake --build --preset linux-release --parallel
ctest --preset linux-release
```

The verified WSL2 environment is Ubuntu 24.04 with FUSE 3.14, OpenSSL 3.0, and Qt 6.4. The GUI is smoke-tested under Xvfb and WSLg.

Build only the native worker and tests without Qt:

```powershell
cmake -S . -B out/build-core -G "Visual Studio 17 2022" -A x64 `
  -DS2FS_BUILD_GUI=OFF -DBUILD_TESTING=ON
cmake --build out/build-core --config MinSizeRel --parallel
ctest --test-dir out/build-core -C MinSizeRel --output-on-failure
```

## Tests

The project includes:

- synthetic Steam2 blob/DAT parser and decoder tests;
- AES/zlib and random-range read tests;
- ancestry/reset/error-boundary tests;
- RAM overlay, quota, sparse write, truncate, rename, and tombstone tests;
- recursive loose catalog, strict filename, ancestry, DAT pairing, duplicate, key, label, and deterministic-order tests;
- optional mounted-filesystem PowerShell smoke tests for local development.

No game payload is required for the normal automated test suite.

## Reproducible releases

CI releases are built only by `.github/workflows/windows.yml` from clean GitHub-hosted runners:

- Linux builds/tests must pass before the Windows release job starts;
- pushes and pull requests build and test both platforms;
- Linux artifacts contain the FUSE worker and Qt GUI;
- Windows artifacts contain the WinFsp worker, Qt GUI, DLLs, and plugins;
- `v*` tags publish both platform packages and SHA-256 checksums;
- GitHub build provenance is attested for release artifacts;
- the GitHub CLI creates the release directly from CI.

Local `out/` binaries are ignored and should never be uploaded as official releases. Users can verify a release artifact against its GitHub attestation and the corresponding source tag.

## Scope

Intentionally out of scope:

- downloading Steam2 payloads;
- installing WinFsp or configuring Linux FUSE permissions;
- persistent writes or save overlays;
- code signing;
- distributing Valve content.

## Licensing

Project code is licensed under **LGPL-3.0-or-later**. See [LICENSE](LICENSE).

Required third-party notices and license texts are in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) and [`licenses/`](licenses/). The GUI includes the required WinFsp attribution in its About dialog. Linux builds dynamically link to OpenSSL under its Apache-2.0 license.

The depot key table is attributed to the extractor source distributed with TeraRelease. The upstream archive did not state a separate license for that table; provenance is documented transparently in the third-party notice.

## Security

Archive metadata and build definitions are treated as untrusted input and validated at their boundaries. Report vulnerabilities through GitHub private vulnerability reporting as described in [SECURITY.md](SECURITY.md). Do not attach copyrighted game payloads to public issues.
