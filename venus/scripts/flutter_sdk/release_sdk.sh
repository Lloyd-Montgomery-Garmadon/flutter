#!/usr/bin/env bash
# Venus owns the workflow; Python owns source identity, recipes and SDK guards.
set -eo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
if [[ -f "$script_dir/release_sdk.env" ]]; then
  set -a
  source "$script_dir/release_sdk.env"
  set +a
fi

usage() {
  cat <<'HELP'
Usage: bash release_sdk.sh source|release|build|package|clean [options]

  source  --output NEW_DIR [--flutter-root EXISTING_OR_NEW_CHECKOUT]
          From Venus only: check official file hashes before prepare, then
          write source.bundle and source.json. Drift stops for agent migration.
  release --source SOURCE_DIR --output RELEASE_DIR
          Build all local groups, then package using its single build receipt.
          Default command when invoked without arguments.
  build   --source SOURCE_DIR --output NEW_JOB_DIR
          Restore to NEW_JOB_DIR/flutter, read the official plan, build all
          groups for this machine into NEW_JOB_DIR/artifacts/receipt.json.
          --from-checkout CHECKOUT exports/reuses its strictly checked source
          snapshot before release/build. Generated Flutter supplies this itself.
  package --source SOURCE_DIR --output NEW_SDK_DIR [--build-receipt FILE ...]
          Defaults to VENUS_RELEASE_DIR/build/artifacts/receipt.json and its
          build/flutter LLVM tools. Explicit receipts remain available.
  clean   --output RELEASE_DIR [--flutter-root PROTECTED_CHECKOUT]
          After successful packaging, verify delivery hashes and remove only
          source/, build/, sdk/smoke/ and sdk/pub-cache/. Keep the final SDK,
          archive and delivery record. May be repeated; no source required.
          --flutter-root may repeat; use a physical output path (no symlinks).

Common: --manifest FILE (default releases/3.44.9/manifest.json)
        --repository URL_OR_MIRROR (prepare/restore/package)
Build/package: --nm FILE --readobj FILE
Package: --flutter-root LLVM_TOOL_CHECKOUT
         --mingit-archive FILE --mingit-sha256 HEX
With VENUS_RELEASE_DIR configured, --output/--source default to its
source/, build/ and sdk/ directories. CLI options override these defaults.
Set PYTHON to a Python 3.10+ executable (Windows Git Bash: PYTHON=python).

Build/package outputs must be new. All workers must share ONE source snapshot.
Do not run clean concurrently with source/build/package.
Full SDKs contain debug/profile/release engines. On macOS, Apple/Android/Web engines are built locally for a complete Mac SDK.
Linux/Windows host SDKs require their respective build hosts.
HELP
}

stop() { printf 'STOP: %s\n' "$*" >&2; exit 2; }
fresh() { [[ ! -e "$1" && ! -L "$1" ]] || stop "Output already exists: $1"; }

command=${1:-release}
case "$command" in
  -h|--help|help) usage; exit 0 ;;
  source|release|build|package|clean) (( $# == 0 )) || shift ;;
  *) usage >&2; exit 2 ;;
esac
venus_root=$(cd -- "$script_dir/../.." && pwd -P)
python=${PYTHON:-python3}
manifest="$script_dir/releases/3.44.9/manifest.json"
source_dir= output= flutter_root= from_checkout=
repository_args=() tool_args=() package_args=() receipts=() protect_args=()
while (( $# )); do
  key=$1
  [[ "$key" != --help ]] || { usage; exit 0; }
  (( $# >= 2 )) && [[ -n "$2" && "$2" != --* ]] || stop "Missing value: $key"
  case "$command:$key" in
    *:--manifest) manifest=$2 ;;
    *:--output) output=$2 ;;
    source:--repository|release:--repository|build:--repository|package:--repository) repository_args=(--repository "$2") ;;
    source:--flutter-root|package:--flutter-root) flutter_root=$2 ;;
    clean:--flutter-root) protect_args+=(--flutter-root "$2") ;;
    release:--source|build:--source|package:--source) source_dir=$2 ;;
    release:--from-checkout|build:--from-checkout) from_checkout=$2 ;;
    release:--nm|build:--nm|package:--nm|release:--readobj|build:--readobj|package:--readobj) tool_args+=("$key" "$2") ;;
    package:--build-receipt) receipts+=("$2") ;;
    package:--mingit-archive|package:--mingit-sha256) package_args+=("$key" "$2") ;;
    *) stop "Unsupported option for $command: $key" ;;
  esac
  shift 2
done
if [[ -n "${VENUS_RELEASE_DIR:-}" ]]; then
  source_dir=${source_dir:-"$VENUS_RELEASE_DIR/source"}
  case "$command" in
    source) output=${output:-"$VENUS_RELEASE_DIR/source"} ;;
    build) output=${output:-"$VENUS_RELEASE_DIR/build"} ;;
    release|clean) output=${output:-"$VENUS_RELEASE_DIR"} ;;
    package) output=${output:-"$VENUS_RELEASE_DIR/sdk"} ;;
  esac
fi
[[ -n "$output" ]] || stop "--output is required"
[[ "$command" == release || "$command" == clean ]] || fresh "$output"
[[ -f "$manifest" ]] || stop "Manifest missing: $manifest"
command -v "$python" >/dev/null || stop "Python executable missing: $python"
inject=("$python" "$script_dir/inject.py")
release=("$python" "$script_dir/release.py" --venus-root "$venus_root" --manifest "$manifest")

if [[ "$command" == clean ]]; then
  "${release[@]}" clean --output "$output" "${protect_args[@]}"
  exit 0
fi

if [[ "$command" == source ]]; then
  flutter_root=${flutter_root:-"$output/flutter"}
  if [[ -e "$flutter_root" || -L "$flutter_root" ]]; then
    "${inject[@]}" check --manifest "$manifest" --flutter-root "$flutter_root"
  fi
  mkdir -p -- "$output"
  if [[ ! -e "$flutter_root" && ! -L "$flutter_root" ]]; then
    "${inject[@]}" prepare --manifest "$manifest" --flutter-root "$flutter_root" "${repository_args[@]}"
  fi
  "${inject[@]}" snapshot --manifest "$manifest" --flutter-root "$flutter_root" \
    --bundle "$output/source.bundle" --receipt "$output/source.json"
  printf 'Shared source: %s\n' "$output"
  exit 0
fi

if [[ -n "$from_checkout" ]]; then
  source_dir=${source_dir:-"$output/source"}
  "${inject[@]}" snapshot --manifest "$manifest" --flutter-root "$from_checkout" \
    --bundle "$source_dir/source.bundle" --receipt "$source_dir/source.json" --reuse
fi

[[ -n "$source_dir" && -f "$source_dir/source.bundle" && -f "$source_dir/source.json" ]] || \
  stop "--source must contain source.bundle and source.json from ONE source run"
if [[ "$command" == release ]]; then
  fresh "$output/build"
  fresh "$output/sdk"
  bash "$script_dir/release_sdk.sh" build --manifest "$manifest" --source "$source_dir" \
    --output "$output/build" "${repository_args[@]}" "${tool_args[@]}"
  bash "$script_dir/release_sdk.sh" package --manifest "$manifest" --source "$source_dir" \
    --output "$output/sdk" --build-receipt "$output/build/artifacts/receipt.json" \
    --flutter-root "$output/build/flutter" "${repository_args[@]}" "${tool_args[@]}"
  exit 0
fi
if [[ "$command" == build ]]; then
  mkdir -p -- "$output"
  flutter_root="$output/flutter"
  "${inject[@]}" restore --manifest "$manifest" --flutter-root "$flutter_root" \
    --bundle "$source_dir/source.bundle" --receipt "$source_dir/source.json" "${repository_args[@]}"
  "${release[@]}" plan --flutter-root "$flutter_root" > "$output/plan.json"
  # Consume Python's actual host-to-groups contract; no Bash platform table.
  group_lines=$("$python" -c '
import json, sys
sys.stdout.reconfigure(encoding="utf-8", newline="\n")
with open(sys.argv[1]) as stream:
    plan = json.load(stream)
groups = plan["workers"].get(plan["current_host"])
if not groups:
    sys.exit("STOP: no official worker groups for " + plan["current_host"])
print("\n".join(groups))
' "$output/plan.json")
  group_args=()
  while IFS= read -r group; do group_args+=(--group "$group"); done <<< "$group_lines"
  "${release[@]}" build --flutter-root "$flutter_root" --source-receipt "$source_dir/source.json" \
    "${group_args[@]}" --output "$output/artifacts" "${tool_args[@]}"
  printf 'Build receipt: %s/artifacts/receipt.json\n' "$output"
else
  if (( ${#receipts[@]} == 0 )); then
    [[ -n "${VENUS_RELEASE_DIR:-}" ]] || stop "--build-receipt or VENUS_RELEASE_DIR is required"
    receipts=("$VENUS_RELEASE_DIR/build/artifacts/receipt.json")
    flutter_root=${flutter_root:-"$VENUS_RELEASE_DIR/build/flutter"}
  fi
  receipt_args=()
  for receipt in "${receipts[@]}"; do
    [[ -f "$receipt" ]] || stop "Build receipt missing: $receipt"
    receipt_args+=(--build-receipt "$receipt")
  done
  sdk_host=$("$python" -c 'import runpy,sys; sys.stdout.reconfigure(encoding="utf-8", newline="\n"); print(runpy.run_path(sys.argv[1])["host"]())' "$script_dir/release.py")
  [[ -z "$flutter_root" ]] || package_args+=(--flutter-root "$flutter_root")
  "${release[@]}" package --source-bundle "$source_dir/source.bundle" --source-receipt "$source_dir/source.json" \
    --host "$sdk_host" --output "$output" "${receipt_args[@]}" \
    "${repository_args[@]}" "${tool_args[@]}" "${package_args[@]}"
  printf 'SDK delivery: %s/delivery.json\n' "$output"
fi
