#!/usr/bin/env bash
# Collects everything the Windows installer packs into one directory.
#   tools/stage.sh <toolchain-bin-dir> <studio-app-dir> <stage-dir> [vsix-file]
# Layout: bin/ (tools), libs/ (standard.lib), studio/ (Coyote Studio), docs/, samples/, assets/brand/ (logo, unmodified), extras/, coyote.ico
set -euo pipefail
bin="$1"; studio="$2"; stage="$3"; vsix="${4:-}"
root="$(cd "$(dirname "$0")/.." && pwd)"
rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/libs" "$stage/studio" "$stage/docs" "$stage/samples" "$stage/assets/brand" "$stage/extras"
for t in coyote pvm cpm cpx cdt coyote-llvm; do
  f="$bin/$t.exe"; [ -f "$f" ] || f="$bin/$t"
  [ -f "$f" ] || { echo "stage: missing tool $t in $bin" >&2; exit 1; }
  cp "$f" "$stage/bin/"
done
[ -f "$bin/libs/standard.lib" ] || { echo "stage: $bin/libs/standard.lib is missing (build the stdlib target)" >&2; exit 1; }
cp "$bin"/libs/*.lib "$stage/libs/"
cp -r "$studio"/. "$stage/studio/"
[ -f "$stage/studio/CoyoteStudio.exe" ] || { echo "stage: CoyoteStudio.exe not found in $studio" >&2; exit 1; }
cp "$root"/docs/*.md "$stage/docs/"
cp "$root"/samples/* "$stage/samples/"
cp "$root"/assets/brand/* "$stage/assets/brand/"
cp "$root/studio/build/icon.ico" "$stage/coyote.ico"
cp "$root/VERSION" "$stage/VERSION"
[ -n "$vsix" ] && [ -f "$vsix" ] && cp "$vsix" "$stage/extras/coyote-vscode.vsix"
[ -f "$stage/extras/coyote-vscode.vsix" ] || echo "(no VS Code extension packaged: the 'vscode' component will be empty)" > "$stage/extras/README.txt"
echo "staged into $stage"
