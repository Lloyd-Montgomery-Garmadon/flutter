#!/usr/bin/env bash
# Generated from Venus; all release tools and version inputs travel with Flutter.
set -eo pipefail
flutter_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
if [[ -f "$flutter_root/venus/release_sdk.env" ]]; then
  set -a
  source "$flutter_root/venus/release_sdk.env"
  set +a
fi
python=${PYTHON:-python3}
if [[ "${1:-}" == source ]]; then
  printf 'STOP: source preparation and migration are maintained in Venus.\n' >&2
  exit 2
fi
entry="$flutter_root/venus/scripts/flutter_sdk/release_sdk.sh"
[[ -f "$entry" && ! -L "$entry" ]] || { printf 'STOP: embedded release tools missing; regenerate from Venus.\n' >&2; exit 2; }
export VENUS_RELEASE_DIR=${VENUS_RELEASE_DIR:-"${flutter_root}-sdk-release-3.44.9"}
if [[ "${1:-}" == sdk-path ]]; then
  [[ "$#" == 1 ]] || { printf 'Usage: ./release_sdk.sh sdk-path\n' >&2; exit 2; }
  printf '%s/sdk/flutter\n' "${VENUS_RELEASE_DIR%/}"
  exit 0
fi
args=("${@}")
(( ${#args[@]} )) || args=(release)
has_manifest=false
has_repository=false
for arg in "$@"; do
  [[ "$arg" != --manifest ]] || has_manifest=true
  [[ "$arg" != --repository ]] || has_repository=true
done
if [[ "$has_manifest" == false ]]; then
  tag=$("$python" -c 'import json,sys; sys.stdout.reconfigure(encoding="utf-8", newline="\n"); print(json.load(open(sys.argv[1]))["official_tag"])' \
    "$flutter_root/docs/venus/source.json")
  args+=(--manifest "$flutter_root/venus/scripts/flutter_sdk/releases/$tag/manifest.json")
fi
case "${args[0]}" in
  release|build|package)
    if [[ "$has_repository" == false ]]; then
      args+=(--repository "$flutter_root")
    fi
    ;;
esac
case "${args[0]}" in
  release|build) args+=(--from-checkout "$flutter_root") ;;
  clean) args+=(--flutter-root "$flutter_root") ;;
esac
exec bash "$entry" "${args[@]}"
