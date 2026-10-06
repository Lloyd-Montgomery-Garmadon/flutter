#!/usr/bin/env bash
# Generated Flutter entry. Maintain this template and all build logic in Venus.
set -eo pipefail
flutter_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
# This file is local configuration, kept beside the generated entry.
if [[ -f "$flutter_root/venus/release_sdk.env" ]]; then
  set -a
  source "$flutter_root/venus/release_sdk.env"
  set +a
fi
python=${PYTHON:-python3}
if [[ "${1:-}" == source ]]; then
  printf 'STOP: source is maintained in Venus. Run bash scripts/flutter_sdk/release_sdk.sh source from the Venus checkout; hashes are persisted in its release manifest.\n' >&2
  exit 2
fi
if [[ -z "${VENUS_ROOT:-}" ]]; then
  venus_root=$("$python" -c 'import json,sys; sys.stdout.reconfigure(encoding="utf-8", newline="\n"); print(json.load(open(sys.argv[1]))["venus_root"])' \
    "$flutter_root/docs/venus/source.json")
else
  venus_root=$VENUS_ROOT
fi
# A captured Windows path is a locator hint, not part of the source identity.
if command -v cygpath >/dev/null; then venus_root=$(cygpath -u "$venus_root"); fi
entry="$venus_root/scripts/flutter_sdk/release_sdk.sh"
[[ -f "$entry" ]] || { printf 'STOP: set VENUS_ROOT to the Venus checkout containing scripts/flutter_sdk/release_sdk.sh\n' >&2; exit 2; }
args=("${@}")
(( ${#args[@]} )) || args=(release)
has_manifest=false
for arg in "$@"; do
  [[ "$arg" != --manifest ]] || has_manifest=true
done
if [[ "$has_manifest" == false ]]; then
  tag=$("$python" -c 'import json,sys; sys.stdout.reconfigure(encoding="utf-8", newline="\n"); print(json.load(open(sys.argv[1]))["official_tag"])' \
    "$flutter_root/docs/venus/source.json")
  args+=(--manifest "$venus_root/scripts/flutter_sdk/releases/$tag/manifest.json")
fi
# Protect the checkout containing this generated entry during cleanup.
if [[ "${args[0]}" == clean ]]; then args+=(--flutter-root "$flutter_root"); fi
exec bash "$entry" "${args[@]}"
