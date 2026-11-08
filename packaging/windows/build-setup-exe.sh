#!/bin/bash
# Build Hermes-Agent-Setup.exe: a Windows stub plus the source tree.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
stage="$(mktemp -d)"
out="${1:-/opt/cursor/artifacts/Hermes-Agent-Setup.exe}"
cc="${CC:-x86_64-w64-mingw32-gcc}"

mkdir -p "$(dirname "$out")"
"$cc" -O2 -s -mconsole -finput-charset=UTF-8 -fwide-exec-charset=UTF-16LE \
  -o "$stage/stub.exe" "$root/packaging/windows/hermes-setup-stub.c"
(
  cd "$root"
  zip -r -q -9 "$stage/payload.zip" . \
    -x '.git/*' \
    -x '*/__pycache__/*' \
    -x '*/.pytest_cache/*' \
    -x '*/.pytest_cache/**'
)
python3 - "$stage/stub.exe" "$stage/payload.zip" "$out" <<'PY'
import pathlib, sys
stub, payload, dest = map(pathlib.Path, sys.argv[1:])
blob = payload.read_bytes()
out = stub.read_bytes() + blob + len(blob).to_bytes(8, "little") + b"HMSP"
dest.write_bytes(out)
print(f"{dest} {dest.stat().st_size} bytes, zip {len(blob)} bytes")
PY
rm -rf "$stage"
