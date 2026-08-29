# Steam2 Depot Manager

Steam2 Depot Manager mounts user-supplied Steam2 `.blob` and `.dat` archives as a normal writable Windows drive without extracting the game. The native worker decodes archive chunks on demand through WinFsp, while the Qt 6 GUI scans loose libraries, resolves depot ancestry, pairs DATs, composes overlays, mounts builds, and launches the first executable found in the projected tree.

> This project does not contain or distribute Valve game payloads. Users provide their own Steam2 archive files.

## Features

- Recursive scanning of loose `DEPOT_VERSION_CRC_SHA256.blob|dat` files.
- Validated blob parsing, parent-CRC ancestry, reset roots, DAT-size pairing, and depot-key readiness.
- Exact source paths: files may live in arbitrary folders or on different drives.
- Ordered multi-depot overlays; later depots replace earlier path collisions.
- Random-access zlib/AES-CFB Steam2 decoding without full extraction.
- WinFsp filesystem with executable and memory-mapped I/O support.
- Ephemeral 32 KiB copy-on-write pages with a 512 MiB default RAM cap.
- RAM-only automatic mirroring of an installed `steam.dll` when a build requires it.
- Deterministic executable selection: root executables first, then recursive paths.
- Windows shell launching with editable arguments.
- Embedded labels for 10,868 depots derived from the annotated TeraRelease index.

All writes are discarded when the mount ends. There is no persistent-overlay mode.

## End-user requirements

- 64-bit Windows 10 or 11.
- [WinFsp 2.1](https://github.com/winfsp/winfsp/releases/tag/v2.1) installed. Newer compatible 2.x versions may work, but 2.1 is the verified target.
- [Microsoft Visual C++ 2015-2022 x64 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- User-supplied Steam2 `.blob` and `.dat` files.

Qt DLLs and plugins are included in release ZIPs. Visual Studio, CMake, the Qt SDK, and WinFsp development headers are not needed to run a release.

## Using the GUI

1. Launch `steam2gui.exe` from the extracted release ZIP.
2. Select **Add Folder** and choose one or more folders containing loose Steam2 files.
3. Select **Scan**. Scanning is recursive.
4. Expand a depot and add only revisions marked **Ready**.
5. Order the composition from bottom to top. Later entries win collisions.
6. Choose a free drive letter or mount folder and select **Mount**.
7. Edit executable arguments if needed, then select **Play**.
8. Close the game and select **Unmount**.

The GUI writes a temporary exact-path build definition and supervises `steam2fs.exe` as a separate process. It never copies or reorganizes archive files.

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

## Building from source

### Requirements

- CMake 3.28 or newer.
- Visual Studio 2022 with the x64 C++ workload, or Visual Studio 2026.
- Qt 6.8.x MSVC x64 development build.
- Git, used by CMake FetchContent for pinned zlib and nlohmann/json sources.

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

Release binaries are built only by `.github/workflows/windows.yml` from a clean GitHub runner:

- pushes and pull requests build and test;
- `v*` tags create the deployable Qt ZIP;
- the workflow publishes SHA-256 checksums;
- GitHub build provenance is attested for release artifacts;
- the GitHub CLI creates the release directly from CI.

Local `out/` binaries are ignored and should never be uploaded as official releases. Users can verify a release artifact against its GitHub attestation and the corresponding source tag.

## Scope

Intentionally out of scope:

- downloading Steam2 payloads;
- installing WinFsp;
- persistent writes or save overlays;
- code signing;
- distributing Valve content.

## Licensing

Project code is licensed under **LGPL-3.0-or-later**. See [LICENSE](LICENSE).

Required third-party notices and license texts are in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) and [`licenses/`](licenses/). The GUI includes the required WinFsp attribution in its About dialog.

The depot key table is attributed to the extractor source distributed with TeraRelease. The upstream archive did not state a separate license for that table; provenance is documented transparently in the third-party notice.

## Security

Archive metadata and build definitions are treated as untrusted input and validated at their boundaries. Report vulnerabilities through GitHub private vulnerability reporting as described in [SECURITY.md](SECURITY.md). Do not attach copyrighted game payloads to public issues.
