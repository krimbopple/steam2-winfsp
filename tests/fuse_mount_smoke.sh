#!/usr/bin/env bash
set -euo pipefail

root="${1:-/tmp/steam2fs-mount}"
version="${2:-unknown}"

required=(
  "hl2.exe"
  "bin/engine.dll"
  "cstrike15/GameInfo.txt"
  "cstrike15/bin/client.dll"
  "cstrike15/bin/server.dll"
)
for relative in "${required[@]}"; do
  test -f "$root/$relative" || { echo "missing $relative" >&2; exit 1; }
done

hl2_mode="$(stat -c '%a' "$root/hl2.exe")"
client_mode="$(stat -c '%a' "$root/cstrike15/bin/client.dll")"
test "$hl2_mode" = "755"
test "$client_mode" = "644"

client_hash="$(sha256sum "$root/cstrike15/bin/client.dll" | cut -d' ' -f1)"
map_count="$(python3 - "$root/cstrike15/maps" <<'PY'
from pathlib import Path
import sys
print(sum(1 for path in Path(sys.argv[1]).iterdir() if path.is_file() and path.suffix.lower() == '.bsp'))
PY
)"
test "$map_count" -gt 0

probe="$root/s2fs-linux-v${version}.tmp"
printf 'ephemeral-linux-%s' "$version" > "$probe"
test "$(cat "$probe")" = "ephemeral-linux-$version"
truncate -s 65539 "$probe"
printf 'tail' | dd of="$probe" bs=1 seek=65535 conv=notrunc status=none
mv "$probe" "$probe.renamed"
test "$(python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).read_bytes()[-4:].decode())' "$probe.renamed")" = "tail"
rm "$probe.renamed"
test ! -e "$probe.renamed"

free_bytes="$(python3 -c 'import os,sys; value=os.statvfs(sys.argv[1]); print(value.f_bavail * value.f_frsize)' "$root")"
python3 - "$version" "$client_hash" "$map_count" "$free_bytes" <<'PY'
import json
import sys
print(json.dumps({
    'version': sys.argv[1],
    'client_sha256': sys.argv[2],
    'bsp_maps': int(sys.argv[3]),
    'free_bytes': int(sys.argv[4]),
    'hl2_mode': '755',
    'client_mode': '644',
    'ephemeral_write_rename_delete': True,
}, separators=(',', ':')))
PY
