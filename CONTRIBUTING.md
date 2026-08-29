# Contributing

Contributions are welcome under the project's LGPL-3.0-or-later license.

## Development workflow

1. Create a focused branch.
2. Keep parsing, archive resolution, filesystem behavior, cataloging, and UI responsibilities separated.
3. Add focused tests for behavior changes and malformed-input boundaries.
4. Build and run the full test suite using a portable CMake preset.
5. Do not commit generated `out/` files, Qt DLLs, IDE state, Steam2 payloads, API keys, credentials, or local build definitions containing private paths.

See README.md for build and test commands.

## Correctness requirements

- Never silently fill missing archive data or ancestry.
- Pair DATs by embedded expected size, not filename CRC.
- Stop ancestry at embedded zero-parent roots, including nonzero version roots.
- Validate bounds before allocation, decompression, decryption, or file access.
- Preserve read/write/memory-mapped filesystem semantics through stable handles.
- Keep writes ephemeral unless project scope explicitly changes.
- Keep GUI scanning off the UI thread.
- Treat labels and archive mtimes as informational, not authoritative release history.

## Pull requests

Describe:

- the user-visible behavior;
- affected archive/filesystem invariants;
- tests added or updated;
- manual mount/launch checks, if applicable;
- any license or bundled-resource changes.

Do not include Valve payload files in issues, commits, tests, or pull requests.
