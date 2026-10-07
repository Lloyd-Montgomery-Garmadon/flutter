#!/usr/bin/env python3
"""Build official Flutter engine recipes and package a Venus-injected full SDK.

This file belongs to Venus. Native patches and the injector are read from Venus;
the Flutter checkout is a generated build input. See README.md for the workflow.
Only fresh build/package directories are written; clean removes verified intermediates.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile


GROUPS = {
    "linux": ("linux_host_engine", "linux_host_desktop_engine", "linux_arm_host_engine"),
    "windows": ("windows_host_engine", "windows_arm_host_engine"),
    "apple": ("mac_host_engine", "mac_ios_engine"),
    "android": ("linux_android_debug_engine", "linux_android_aot_engine"),
    "mac-android": ("mac_android_aot_engine",),
    "windows-android": ("windows_android_aot_engine",),
    "web": ("linux_web_engine_build",),
}
WORKERS = {
    "linux-x64": ("linux", "android", "web"),
    "windows-x64": ("windows", "windows-android"),
    "macos-x64": ("apple", "android", "web"),
    "macos-arm64": ("apple", "android", "web"),
}
HOSTS = ("macos-x64", "macos-arm64", "linux-x64", "windows-x64")
MODES = ("debug", "profile", "release")
ABIS = ("arm", "arm64", "x64")
STAMP_NAMES = ("flutter_sdk", "flutter_web_sdk", "engine_stamp", "android-sdk",
               "android-internal-build-artifacts", "ios-sdk", "macos-sdk", "linux-sdk", "windows-sdk", "font-subset")
RUNTIMES = {"libflutter.so", "libflutter_linux_gtk.so", "flutter_windows.dll",
            "Flutter", "FlutterMacOS", "libflutter_engine.so", "libflutter_engine.dylib"}


class Failure(RuntimeError):
    pass


def need(ok, message):
    if not ok:
        raise Failure(message)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_json(path, data):
    path = Path(path)
    need(not path.exists(), f"Refuse to overwrite receipt: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def run(argv, cwd=None, env=None, capture=False):
    print("+ " + subprocess.list2cmdline([str(x) for x in argv]), file=sys.stderr)
    p = subprocess.run([str(x) for x in argv], cwd=cwd, env=env, text=True,
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.PIPE if capture else None)
    need(p.returncode == 0, f"Command failed ({p.returncode}): {argv}\n{p.stderr or ''}")
    return p.stdout.strip() if capture else ""


def relative(name):
    name = name.replace("\\", "/")
    p = PurePosixPath(name)
    need(not p.is_absolute() and bool(p.parts) and ".." not in p.parts and ":" not in name,
         f"Unsafe relative path: {name}")
    return p


def inside(root, name):
    p = Path(root).joinpath(*relative(name).parts)
    need(p.resolve().is_relative_to(Path(root).resolve()), f"Path escapes root: {name}")
    return p


def host():
    os_name = {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}.get(platform.system())
    arch = {"x86_64": "x64", "AMD64": "x64", "arm64": "arm64", "aarch64": "arm64"}.get(platform.machine())
    need(os_name and arch, "Unsupported build host")
    return f"{os_name}-{arch}"


def context(args):
    venus = Path(args.venus_root).resolve() if args.venus_root else Path(__file__).resolve().parents[2]
    manifest = Path(args.manifest).resolve() if args.manifest else venus / "scripts/flutter_sdk/releases/3.44.9/manifest.json"
    need(manifest.is_file(), f"Missing Venus manifest: {manifest}")
    need(manifest.is_relative_to(venus), "Manifest must belong to the explicit Venus root")
    spec = read_json(manifest)
    matrix = spec["release_matrix"]
    matrix_hosts = {f'{item["os"]}-{item["arch"]}' for item in matrix["sdk_hosts"]}
    need(matrix_hosts == set(HOSTS), "Official SDK host matrix changed; adapt the Venus packager")
    expected_recipes = {"linux": set(GROUPS["linux"] + GROUPS["android"] + GROUPS["web"]),
                        "macos": set(GROUPS["apple"] + GROUPS["mac-android"]),
                        "windows": set(GROUPS["windows"] + GROUPS["windows-android"])}
    need({name: set(values) for name, values in matrix["recipe_groups"].items()} == expected_recipes,
         "Official recipe matrix changed; adapt the Venus packager")
    targets = matrix["application_targets"]
    architectures = {"android": set(ABIS), "ios": {"arm64"}, "macos": {"arm64", "x64"},
                     "windows": {"arm64", "x64"}, "linux": {"arm64", "x64"}, "web": {"javascript", "wasm"}}
    need(set(targets) == set(architectures) and all(set(targets[name]["architectures"]) == values and
         set(targets[name]["modes"]) == set(MODES) for name, values in architectures.items()) and
         set(targets["ios"]["debug_simulator_architectures"]) == {"arm64", "x64"},
         "Official application target matrix changed; adapt the Venus packager")
    need(all(item["format"] == ("tar.xz" if item["os"] == "linux" else "zip") for item in matrix["sdk_hosts"]),
         "Official archive format changed; adapt the Venus packager")
    for part, name, sha in ((spec["native_patch"], "path", "sha256"),
                            (spec["dependencies"]["skia"], "patch_file", "patch_sha256")):
        p = inside(manifest.parent, part[name])
        need(p.is_file() and digest(p) == part[sha], f"Canonical patch hash mismatch: {p}")
    injector = venus / "scripts/flutter_sdk/inject.py"
    need(injector.is_file(), f"Missing Venus injector: {injector}")
    return venus, manifest, spec, injector


def source_identity(args, manifest):
    receipt = read_json(args.source_receipt)
    spec = read_json(manifest)
    need(receipt["manifest_sha256"] == digest(manifest), "Source receipt uses another manifest")
    need(re.fullmatch(r"[0-9a-f]{40}", receipt["flutter_commit"]), "Invalid source commit")
    for key, expected in (("official_tag", spec["official_tag"]), ("official_commit", spec["official_commit"]),
                          ("native_patch_sha256", spec["native_patch"]["sha256"]),
                          ("skia_patch_sha256", spec["dependencies"]["skia"]["patch_sha256"])):
        need(receipt[key] == expected, f"Source receipt identity mismatch: {key}")
    return receipt


def inject(injector, manifest, root, action, apply=False):
    command = [sys.executable, injector, action, "--manifest", manifest, "--flutter-root", root]
    if apply:
        command.append("--apply")
    run(command)


def recipes(root, groups, sdk_host=None):
    result = []
    for group in groups:
        for name in GROUPS[group]:
            path = Path(root) / f"engine/src/flutter/ci/builders/{name}.json"
            spec = read_json(path)
            # riscv64 is experimental and not a Flutter 3.44.9 supported SDK ABI.
            spec["builds"] = [b for b in spec["builds"] if "riscv64" not in b["name"]]
            if sdk_host and sdk_host.startswith("macos-"):
                adapt_macos_recipe(name, spec)
            result.append((group, path, spec))
    return result


def adapt_macos_recipe(name, spec):
    """Keep official sources unchanged; select their local Darwin producers."""
    for build in spec["builds"]:
        config = build["ninja"]["config"]
        targets = build["ninja"].setdefault("targets", [])
        if name == "mac_host_engine" and config in {"ci/host_debug", "ci/host_release"}:
            # Official Apple recipes omit the common Dart platform archives.
            targets.append("flutter/build/archives:flutter_patched_sdk")
            filename = "flutter_patched_sdk_product.zip" if config.endswith("release") else "flutter_patched_sdk.zip"
            base = f"out/{config}/zip_archives/"
            build.setdefault("archives", []).append({"base_path": base, "include_paths": [base + filename]})
        elif name == "linux_android_aot_engine":
            # The public Darwin producer merges x64/arm64 host snapshots.
            build["ninja"]["targets"] = [target for target in targets if target not in {
                "clang_x64/gen_snapshot", "flutter/shell/platform/android:analyze_snapshot"}]
            build["ninja"]["targets"].append("flutter/shell/platform/android:gen_snapshot")
            for archive in build.get("archives", []):
                archive["include_paths"] = [source.replace("/linux-x64.zip", "/darwin-x64.zip")
                                            for source in archive["include_paths"]
                                            if "/analyze-snapshot-linux-x64.zip" not in source]


def gn_arguments(build):
    # Official local postsubmit arguments avoid LUCI's remote execution service.
    args = list(build.get("postsubmit_overrides", {}).get("gn", build["gn"]))
    args = ["--no-rbe" if x == "--rbe" else "--no-goma" if x == "--goma" else x for x in args]
    for flag in ("--no-rbe", "--no-goma"):
        if flag not in args:
            args.append(flag)
    return args


def required_exports(root):
    text = (Path(root) / "engine/src/flutter/shell/platform/common/public/flutter_venus_text_layout.h").read_text()
    exports = sorted(set(re.findall(r"FLUTTER_EXPORT\s+\w+\s+(FlutterVenus\w+)\s*\(", text)))
    need(len(exports) >= 3, "Venus native ABI export declarations are missing")
    return exports


def binary_arches(path):
    with Path(path).open("rb") as f:
        data = f.read(4096)
        if data[:4] == b"\x7fELF":
            endian = "<" if data[5] == 1 else ">"
            machine = struct.unpack_from(endian + "H", data, 18)[0]
            return {3: {"x86"}, 40: {"arm"}, 62: {"x64"}, 183: {"arm64"}}.get(machine, set())
        if data[:2] == b"MZ":
            offset = struct.unpack_from("<I", data, 60)[0]
            f.seek(offset)
            pe = f.read(6)
            need(pe[:4] == b"PE\0\0", f"Invalid PE header: {path}")
            return {0x14C: {"x86"}, 0x8664: {"x64"}, 0xAA64: {"arm64"}}.get(struct.unpack("<H", pe[4:])[0], set())
        thin = {b"\xcf\xfa\xed\xfe": "<", b"\xfe\xed\xfa\xcf": ">", b"\xce\xfa\xed\xfe": "<", b"\xfe\xed\xfa\xce": ">"}
        cpu = {7: "x86", 12: "arm", 0x1000007: "x64", 0x100000C: "arm64"}
        if data[:4] in thin:
            return {cpu.get(struct.unpack_from(thin[data[:4]] + "I", data, 4)[0], "unknown")}
        fat = {b"\xca\xfe\xba\xbe": (">", 20), b"\xbe\xba\xfe\xca": ("<", 20), b"\xca\xfe\xba\xbf": (">", 32), b"\xbf\xba\xfe\xca": ("<", 32)}
        if data[:4] in fat:
            endian, size = fat[data[:4]]
            count = struct.unpack_from(endian + "I", data, 4)[0]
            need(count <= 32, f"Invalid fat Mach-O header: {path}")
            return {cpu.get(struct.unpack_from(endian + "I", data, 8 + n * size)[0], "unknown") for n in range(count)}
    raise Failure(f"Not a supported native binary: {path}")


def find_tool(root, supplied, name):
    if supplied:
        result = Path(supplied).resolve()
        need(result.is_file(), f"Missing explicitly supplied {name}: {result}")
        return result
    suffix = ".exe" if platform.system() == "Windows" else ""
    actual_host = host().replace("macos", "mac").replace("windows", "win")
    pinned = Path(root) / f"engine/src/flutter/buildtools/{actual_host}/clang/bin/{name}{suffix}"
    if pinned.is_file():
        return pinned
    found_on_path = shutil.which(name)
    need(found_on_path, f"Missing {name}; supply its path explicitly")
    return Path(found_on_path)


def fat_slices(path):
    formats = {b"\xca\xfe\xba\xbe": (">", 20), b"\xbe\xba\xfe\xca": ("<", 20),
               b"\xca\xfe\xba\xbf": (">", 32), b"\xbf\xba\xfe\xca": ("<", 32)}
    with Path(path).open("rb") as stream:
        header = stream.read(8)
        if header[:4] not in formats:
            return []
        endian, stride = formats[header[:4]]
        count = struct.unpack_from(endian + "I", header, 4)[0]
        need(count <= 32, f"Invalid universal slice count: {path}")
        table = stream.read(count * stride)
        result = []
        for n in range(count):
            entry = n * stride
            offset, length = struct.unpack_from(endian + ("II" if stride == 20 else "QQ"), table, entry + 8)
            need(offset >= 8 + count * stride and length > 0 and offset + length <= Path(path).stat().st_size,
                 f"Invalid universal slice range: {path}")
            result.append((offset, length))
        return result


def contains_revision(path, revision, offset=0, length=None):
    needle = revision.encode("ascii")
    tail = b""
    with Path(path).open("rb") as stream:
        stream.seek(offset)
        remaining = Path(path).stat().st_size - offset if length is None else length
        while remaining > 0:
            block = stream.read(min(1024 * 1024, remaining))
            if not block:
                break
            remaining -= len(block)
            data = tail + block
            if needle in data:
                return True
            tail = data[-len(needle):]
    return False


def inspect_native(path, exports, nm, readobj, expected=None, revision=None):
    arches = binary_arches(path)
    need(arches and "unknown" not in arches, f"Unknown binary architecture: {path}")
    if expected:
        need(arches == set(expected), f"Wrong architecture {arches}; expected {expected}: {path}")
    if revision:
        slices = fat_slices(path) or [(0, Path(path).stat().st_size)]
        need(all(contains_revision(path, revision, offset, length) for offset, length in slices),
             f"Native runtime does not embed the expected commit {revision} in every slice: {path}")
    if Path(path).suffix.lower() == ".dll":
        symbols = run([readobj, "--coff-exports", path], capture=True)
    else:
        # Universal Mach-O must export the ABI in every slice, not only one.
        with Path(path).open("rb") as stream:
            magic = stream.read(4)
        fat = magic in (b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca")
        outputs = [run([nm, "--defined-only", "--extern-only", f"--arch={dict(x64='x86_64', x86='i386').get(arch, arch)}", path], capture=True)
                   for arch in sorted(arches)] if fat else [run([nm, "--defined-only", "--extern-only", *(["--dynamic"] if magic == b"\x7fELF" else []), path], capture=True)]
        symbols = "\n".join(outputs)
        for output in outputs:
            for symbol in exports:
                need(re.search(r"(?<![\w])_?" + re.escape(symbol) + r"(?![\w])", output), f"Missing exported ABI {symbol}: {path}")
    for symbol in exports:
        need(re.search(r"(?<![\w])_?" + re.escape(symbol) + r"(?![\w])", symbols), f"Missing exported ABI {symbol}: {path}")
    return {"sha256": digest(path), "architectures": sorted(arches), "exports": exports}


def expected_arch(build):
    args = gn_arguments(build)
    for flag in ("--android-cpu", "--simulator-cpu", "--mac-cpu", "--linux-cpu", "--windows-cpu"):
        for index, arg in enumerate(args):
            if arg.startswith(flag + "="):
                value = arg.split("=", 1)[1]
            elif arg == flag:
                need(index + 1 < len(args) and not args[index + 1].startswith("--"), f"Missing GN CPU value: {flag}")
                value = args[index + 1]
            else:
                continue
            need(value in {"arm", "arm64", "x86", "x64"}, f"Unsupported GN CPU value: {flag} {value}")
            return value
    return "arm" if "--android" in args else "arm64" if "--ios" in args and "--simulator" not in args else "x64"


def plan(args):
    _, manifest, spec, _ = context(args)
    root = Path(args.flutter_root).resolve()
    groups = args.group or WORKERS.get(host(), ())
    data = {"official_tag": spec["official_tag"], "manifest_sha256": digest(manifest), "current_host": host(),
            "official_sdk_archives": list(HOSTS), "additional_desktop_target_architectures": ["x64", "arm64"],
            "workers": WORKERS,
            "constraints": ["Apple frameworks require macOS/Xcode", "Windows targets require Windows/Visual Studio",
                            "macOS builds Apple/Android/Web locally using pinned Xcode, Android/JDK and Emscripten toolchains",
                            "Darwin Android AOT tools are universal x64/arm64",
                            "Linux/Windows arm64 desktop targets are included; this tag shipped no arm64 host SDK archives"],
            "recipes": []}
    for group, path, recipe in recipes(root, groups, sdk_host=host()):
        data["recipes"].append({"group": group, "path": path.relative_to(root).as_posix(), "sha256": digest(path),
                                "builds": [{"name": b["name"], "gn": gn_arguments(b), "ninja": b["ninja"],
                                            "gclient_variables": b.get("gclient_variables", {}),
                                            "drone_dimensions": b.get("drone_dimensions", [])} for b in recipe["builds"]]})
    print(json.dumps(data, indent=2))


def build(args):
    _, manifest, spec, injector = context(args)
    root = Path(args.flutter_root).resolve()
    identity = source_identity(args, manifest)
    need(run(["git", "-C", root, "rev-parse", "HEAD"], capture=True) == identity["flutter_commit"], "Wrong source HEAD")
    inject(injector, manifest, root, "check")
    groups = args.group
    actual_host = host()
    need(set(groups) <= set(WORKERS.get(actual_host, ())), f"Groups {groups} cannot run on {actual_host}; see plan")
    src = root / "engine/src"
    need(not (src / "out").exists(), "Fresh engine/src/out required; old native outputs are never reused")
    output = Path(args.output).resolve()
    need(not output.exists(), f"Fresh build output required: {output}")
    output.mkdir(parents=True)
    recipes_to_run = recipes(root, groups, sdk_host=actual_host)
    configs = [b["ninja"]["config"] for _, _, recipe in recipes_to_run for b in recipe["builds"]]
    need(len(configs) == len(set(configs)), "Repeated recipe/output config; use separate fresh worker checkouts")
    custom_vars = {"setup_githooks": False, "use_rbe": False}
    # One sync loads the union of dependencies needed by this worker's groups.
    for _, _, recipe in recipes_to_run:
        for b in recipe["builds"]:
            for key, value in b.get("gclient_variables", {}).items():
                if key != "use_rbe":
                    need(isinstance(value, bool), f"Non-boolean official gclient variable needs explicit adaptation: {key}")
                    custom_vars[key] = bool(custom_vars.get(key, False) or value)
    if "android" in groups:
        custom_vars.update(download_android_deps=True, download_jdk=True)
    gclient = shutil.which("gclient")
    need(gclient, "depot_tools gclient must be on PATH")
    config = root / ".gclient"
    need(not config.exists(), "Existing .gclient ownership is ambiguous; use a fresh injector/bundle checkout")
    config.write_text("solutions = " + repr([{"name": ".", "url": spec["upstream"], "managed": False,
                                             "deps_file": "DEPS", "custom_deps": {}, "custom_vars": custom_vars}]) + "\n")
    env = dict(os.environ)
    for key in ("FLUTTER_PREBUILT_ENGINE_VERSION", "FLUTTER_REALM", "FLUTTER_STORAGE_BASE_URL", "DART_SDK_BASE_URL"):
        env.pop(key, None)
    run([gclient, "sync", "--no-history"], cwd=root, env=env)
    inject(injector, manifest, root, "check")
    inject(injector, manifest, root, "skia", apply=True)
    exports = required_exports(root)
    nm = find_tool(root, args.nm, "llvm-nm")
    readobj = find_tool(root, args.readobj, "llvm-readobj")
    ninja = root / ("third_party/ninja/ninja.exe" if platform.system() == "Windows" else "third_party/ninja/ninja")
    need(ninja.is_file(), f"DEPS ninja missing: {ninja}")
    receipt = {"schema_version": 1, "source": identity, "host": actual_host, "groups": groups,
               "fresh_outputs": True, "started_ns": time.time_ns(), "gclient_variables": custom_vars,
               "recipes": {}, "builds": [], "archives": [],
               "inspection_tools": {"llvm_nm": {"sha256": digest(nm), "version": run([nm, "--version"], capture=True)},
                                    "llvm_readobj": {"sha256": digest(readobj), "version": run([readobj, "--version"], capture=True)}}}
    for group, recipe_path, recipe in recipes_to_run:
        receipt["recipes"][recipe_path.relative_to(root).as_posix()] = digest(recipe_path)
        for b in recipe["builds"]:
            config_path = inside(src, "out/" + b["ninja"]["config"])
            need(not config_path.exists(), f"Output collision/old output: {config_path}")
            started = time.time_ns()
            gn = gn_arguments(b)
            run([sys.executable, src / "flutter/tools/gn", *gn], cwd=src, env=env)
            actual_args = (config_path / "args.gn").read_text()
            need(f'engine_version = "{identity["flutter_commit"]}"' in actual_args, "GN embedded engine identity mismatch")
            run([ninja, "-C", config_path, *b["ninja"].get("targets", [])], cwd=src, env=env)
            native = {}
            for p in config_path.rglob("*"):
                if p.is_file() and not p.is_symlink() and p.name in RUNTIMES:
                    native[p.relative_to(src).as_posix()] = inspect_native(p, exports, nm, readobj, [expected_arch(b)], identity["flutter_commit"])
                elif p.is_file() and p.name in {"flutter_tester", "flutter_tester.exe"}:
                    # flutter_tester links internal engine code, not the public
                    # embedding ABI. Its architecture and embedded revision still matter.
                    native[p.relative_to(src).as_posix()] = inspect_native(p, [], nm, readobj, [expected_arch(b)], identity["flutter_commit"])
            if group == "web":
                for name in ("canvaskit", "skwasm"):
                    wasm = config_path / f"flutter_web_sdk/canvaskit/{name}.wasm"
                    need(wasm.is_file() and wasm.read_bytes()[:8] == b"\0asm\x01\0\0\0", f"Missing/invalid freshly built Web runtime: {wasm}")
                    native[wasm.relative_to(src).as_posix()] = {"sha256": digest(wasm), "architectures": ["wasm32"], "exports": []}
            receipt["builds"].append({"name": b["name"], "group": group, "gn": gn,
                                       "args_gn_sha256": digest(config_path / "args.gn"), "ninja_targets": b["ninja"].get("targets", []), "started_ns": started,
                                       "finished_ns": time.time_ns(), "native": native})
        # Reuse official Apple framework/snapshot postprocessors. LUCI docs,
        # upload and official export-whitelist steps do not produce SDK artifacts.
        for task in recipe.get("generators", {}).get("tasks", []):
            if Path(task["script"]).name not in {"create_ios_framework.py", "create_macos_framework.py", "create_macos_gen_snapshots.py", "create_embedder_framework.py"}:
                continue
            run([sys.executable, inside(src, task["script"]), *task.get("parameters", [])], cwd=src, env=env)
        archives = []
        for b in recipe["builds"]:
            for archive in b.get("archives", []):
                for source in archive["include_paths"]:
                    destination = PurePosixPath(source).relative_to(PurePosixPath(archive["base_path"])).as_posix()
                    archives.append((source, destination))
        for archive in recipe.get("archives", []):
            if group == "apple":
                archives.append((archive["source"], archive["destination"]))
        for source, destination in archives:
            p = inside(src, source)
            need(p.exists(), f"Official recipe did not produce {p}")
            # Maven archive entries are directories; bundle without changing contents.
            file_name = f"{len(receipt['archives']):04d}.zip"
            target = output / file_name
            if p.is_dir():
                with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
                    for f in sorted(p.rglob("*")):
                        if f.is_file():
                            z.write(f, f.relative_to(p).as_posix())
            else:
                shutil.copyfile(p, target)
            receipt["archives"].append({"destination": destination, "file": file_name,
                                         "sha256": digest(target), "group": group, "source": source})
    inject(injector, manifest, root, "check")
    inject(injector, manifest, root, "skia")
    receipt["finished_ns"] = time.time_ns()
    write_json(output / "receipt.json", receipt)
    print(output / "receipt.json")


def extract_zip(archive, root):
    """Reject traversal and conflicting files; preserve safe framework symlinks."""
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    links = []
    with zipfile.ZipFile(archive) as z:
        for entry in z.infolist():
            destination = inside(root, entry.filename)
            mode = entry.external_attr >> 16
            if entry.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            data = z.read(entry)
            if stat.S_ISLNK(mode):
                link = data.decode("utf-8")
                need(not Path(link).is_absolute() and (destination.parent / link).resolve().is_relative_to(root.resolve()), "Unsafe archive symlink")
                links.append((destination, link))
            else:
                need(not destination.is_symlink(), f"Archive collision with symlink: {destination}")
                if destination.exists():
                    identical = destination.is_file() and hashlib.sha256(data).hexdigest() == digest(destination)
                    # Android embedding JARs repeat in each ABI's Maven archive.
                    # ZIP timestamps can differ; every entry's bytes must agree.
                    if not identical and destination.suffix == ".jar":
                        import io
                        with zipfile.ZipFile(io.BytesIO(data)) as incoming, zipfile.ZipFile(destination) as existing:
                            identical = jar_contents(incoming) == jar_contents(existing)
                    need(identical, f"Conflicting artifact file: {destination}")
                else:
                    destination.write_bytes(data)
                    destination.chmod(mode & 0o777 or 0o644)
        for destination, link in links:
            if destination.exists() or destination.is_symlink():
                need(destination.is_symlink() and os.readlink(destination) == link, f"Conflicting symlink: {destination}")
            else:
                destination.symlink_to(link)


def artifact_map(receipt_paths, identity):
    artifacts = {}
    receipts = []
    for path in receipt_paths:
        path = Path(path).resolve()
        receipt = read_json(path)
        need(receipt["fresh_outputs"] is True and receipt["finished_ns"] > receipt["started_ns"], "Incomplete or reused native build")
        need(receipt["source"] == identity, "Mixed source/engine/manifest identities in native inputs")
        need(receipt.get("builds") and all(b["finished_ns"] > b["started_ns"] for b in receipt["builds"]), "Missing build provenance")
        receipts.append({"sha256": digest(path), "host": receipt["host"], "groups": receipt["groups"]})
        for item in receipt["archives"]:
            p = inside(path.parent, item["file"])
            need(p.is_file() and digest(p) == item["sha256"], f"Artifact hash mismatch: {p}")
            destination = relative(item["destination"]).as_posix()
            artifacts.setdefault(destination, []).append(p)
    return artifacts, receipts


def required_artifacts(sdk_host):
    os_name, arch = sdk_host.split("-")
    artifact_host = "darwin" if os_name == "macos" else os_name
    snapshot_host = {"macos": "darwin-x64", "linux": "linux-x64", "windows": "windows-x64"}[os_name]
    required = {"flutter_patched_sdk.zip": "artifacts/engine/common", "flutter_patched_sdk_product.zip": "artifacts/engine/common",
                "sky_engine.zip": "pkg", "flutter_gpu.zip": "pkg", "flutter-web-sdk.zip": "flutter_web_sdk",
                f"dart-sdk-{artifact_host}-{arch}.zip": "", f"{artifact_host}-{arch}/artifacts.zip": f"artifacts/engine/{'darwin-x64' if os_name == 'macos' else sdk_host}",
                f"{artifact_host}-{arch}/font-subset.zip": f"artifacts/engine/{'darwin-x64' if os_name == 'macos' else sdk_host}"}
    for abi in (*ABIS, "x86"):
        for mode in (MODES if abi != "x86" else ("debug",)):
            directory = f"android-{abi}" + (f"-{mode}" if mode != "debug" else "")
            required[f"{directory}/artifacts.zip"] = f"artifacts/engine/{directory}"
            if mode != "debug":
                required[f"{directory}/{snapshot_host}.zip"] = f"artifacts/engine/{directory}/{snapshot_host}"
    if os_name == "macos":
        for mode in MODES:
            suffix = "" if mode == "debug" else f"-{mode}"
            required[f"ios{suffix}/artifacts.zip"] = f"artifacts/engine/ios{suffix}"
            required[f"darwin-x64{suffix}/framework.zip"] = f"artifacts/engine/darwin-x64{suffix}"
            required[f"darwin-x64{suffix}/gen_snapshot.zip"] = f"artifacts/engine/darwin-x64{suffix}"
            if mode != "debug":
                required[f"darwin-x64{suffix}/artifacts.zip"] = f"artifacts/engine/darwin-x64{suffix}"
    else:
        for target_arch in ("x64", "arm64"):
            for mode in MODES:
                directory = f"{os_name}-{target_arch}" + (f"-{mode}" if mode != "debug" else "")
                source_directory = f"{os_name}-{target_arch}-{mode}"
                name = f"{os_name}-{target_arch}-flutter" + ("-gtk" if os_name == "linux" else "") + ".zip"
                required[f"{source_directory}/{name}"] = f"artifacts/engine/{directory}"
            if os_name == "windows":
                required[f"windows-{target_arch}/flutter-cpp-client-wrapper.zip"] = f"artifacts/engine/windows-{target_arch}"
    return required


def replace_once(root, path, old, new):
    p = inside(root, path)
    text = p.read_text(encoding="utf-8")
    need(text.count(old) == 1, f"SDK injection context changed: {path}; manually adapt the Venus packager")
    p.write_text(text.replace(old, new), encoding="utf-8")
    return digest(p)


def sdk_overlays(root, revision):
    hashes = {}
    plugin = "packages/flutter_tools/gradle/src/main/kotlin/FlutterPlugin.kt"
    old = '            } else {\n                "$hostedRepository/${engineRealm}download.flutter.io"\n            }'
    new = '            } else {\n                val bundledRepository = File(flutterRoot!!, "bin/cache/venus-maven")\n                if (!bundledRepository.isDirectory) {\n                    throw GradleException("Venus SDK Maven artifacts are missing; reinstall the complete SDK")\n                }\n                bundledRepository.toURI().toString()\n            }'
    hashes[plugin] = replace_once(root, plugin, old, new)
    resolver = "packages/flutter_tools/gradle/resolve_dependencies.gradle.kts"
    hashes[resolver] = replace_once(root, resolver,
        '        url = uri("$storageUrl/${engineRealm}download.flutter.io")',
        '        val bundledRepository = java.io.File(flutterRoot, "bin/cache/venus-maven")\n        require(bundledRepository.isDirectory) { "Venus SDK Maven artifacts are missing" }\n        url = uri(bundledRepository)')
    web = "packages/flutter_tools/lib/src/runner/flutter_command.dart"
    hashes[web] = replace_once(root, web,
        '      FlutterOptions.kWebResourcesCdnFlag,\n      defaultsTo: true,',
        '      FlutterOptions.kWebResourcesCdnFlag,\n      defaultsTo: false,')
    version = root / "bin/internal/engine.version"
    version.write_text(revision + "\n")
    hashes[version.relative_to(root).as_posix()] = digest(version)
    return hashes


def jar_contents(archive):
    names = archive.namelist()
    need(len(names) == len(set(names)), "Duplicate entries in JAR")
    return {name: hashlib.sha256(archive.read(name)).hexdigest() for name in names if not name.endswith("/")}


def jar_runtime(path, abi, exports, nm, readobj, revision):
    android_abi = {"arm": "armeabi-v7a", "arm64": "arm64-v8a", "x64": "x86_64", "x86": "x86"}[abi]
    entry = f"lib/{android_abi}/libflutter.so"
    with zipfile.ZipFile(path) as jar:
        jar_contents(jar)  # Reject duplicate members before selecting a runtime.
        libraries = [name for name in jar.namelist() if name.endswith("/libflutter.so")]
        need(libraries == [entry], f"Wrong/missing Android JAR runtime {entry}: {path}")
        data = jar.read(entry)
    with tempfile.TemporaryDirectory(prefix="venus-sdk-jar-") as tmp:
        p = Path(tmp) / "libflutter.so"
        p.write_bytes(data)
        return inspect_native(p, exports, nm, readobj, [abi], revision)


def check_maven(root, revision, exports, nm, readobj, cache_native):
    artifacts = [f"flutter_embedding_{mode}" for mode in MODES]
    artifacts += [f"{abi}_{mode}" for abi in ("armeabi_v7a", "arm64_v8a", "x86_64") for mode in MODES]
    artifacts += ["x86_debug"]
    for artifact in artifacts:
        directory = root / "bin/cache/venus-maven/io/flutter" / artifact / f"1.0.0-{revision}"
        for suffix in ("jar", "pom"):
            p = directory / f"{artifact}-1.0.0-{revision}.{suffix}"
            need(p.is_file(), f"Missing bundled Android Maven artifact: {p}")
            if suffix == "pom":
                need(f"1.0.0-{revision}" in p.read_text(), f"Wrong Maven engine identity: {p}")
        if artifact.startswith("flutter_embedding_"):
            with zipfile.ZipFile(directory / f"{artifact}-1.0.0-{revision}.jar") as jar:
                need(any(name.endswith("FlutterJNI.class") for name in jar.namelist()), f"Missing Flutter Java embedding: {artifact}")
            need((directory / f"{artifact}-1.0.0-{revision}-sources.jar").is_file(), f"Missing embedding source JAR: {artifact}")
        else:
            prefix, mode = artifact.rsplit("_", 1)
            abi = {"armeabi_v7a": "arm", "arm64_v8a": "arm64", "x86_64": "x64", "x86": "x86"}[prefix]
            result = jar_runtime(directory / f"{artifact}-1.0.0-{revision}.jar", abi, exports, nm, readobj, revision)
            need(result["sha256"] == cache_native[f"android-{abi}-{mode}"]["sha256"],
                 f"Maven/cache native bytes disagree: {artifact}")


def install_mingit(root, output, archive=None, sha256=None):
    text = (root / "dev/bots/prepare_package/common.dart").read_text()
    match = re.search(r"const String mingitForWindowsUrl\s*=\s*((?:'[^']*'\s*)+);", text)
    need(match, "Official MinGit URL declaration changed; adapt the packager")
    url = "".join(re.findall(r"'([^']*)'", match.group(1)))
    need(url.startswith("https://storage.googleapis.com/flutter_infra_release/mingit/"), "Unexpected official MinGit URL")
    if archive:
        source = Path(archive).resolve()
        need(sha256 and digest(source) == sha256, "Explicit MinGit archive needs a matching --mingit-sha256")
    else:
        source = output / "mingit.zip"
        with urllib.request.urlopen(url, timeout=60) as response, source.open("xb") as stream:
            shutil.copyfileobj(response, stream)
    extract_zip(source, root / "bin/mingit")
    need((root / "bin/mingit/cmd/git.exe").is_file(), "Incomplete official MinGit archive")
    need(binary_arches(root / "bin/mingit/cmd/git.exe") == {"x64"}, "Official MinGit host architecture mismatch")
    return {"official_url": url, "sha256": digest(source)}


def engine_content_hash(root):
    # Exactly the official content hash inputs, intentionally at the captured
    # custom HEAD rather than a development branch's remote merge-base.
    data = subprocess.check_output(["git", "-C", str(root), "ls-tree", "HEAD", "--",
                                    "DEPS", "engine", "bin/internal/release-candidate-branch.version"])
    return subprocess.check_output(["git", "hash-object", "--stdin"], input=data).decode().strip()


def verify_sdk(root, sdk_host, revision, exports, nm, readobj):
    root = Path(root)
    cache = root / "bin/cache"
    need((root / "bin/internal/engine.version").read_text().strip() == revision, "SDK engine.version differs from custom native identity")
    for name in ("engine", "engine-dart-sdk", *STAMP_NAMES):
        need((cache / f"{name}.stamp").read_text().strip() == revision, f"Wrong/missing cache stamp: {name}")
    for path in ("pkg/sky_engine/lib/ui/ui.dart", "pkg/flutter_gpu/lib/gpu.dart", "artifacts/engine/common/flutter_patched_sdk/platform_strong.dill",
                 "artifacts/engine/common/flutter_patched_sdk_product/platform_strong.dill", "flutter_web_sdk/kernel/amd-canvaskit/dart_sdk.js",
                 "flutter_web_sdk/kernel/dart2js_platform.dill", "flutter_web_sdk/kernel/dart2wasm_platform.dill",
                 "flutter_web_sdk/canvaskit/canvaskit.wasm", "flutter_web_sdk/canvaskit/skwasm.wasm"):
        need((cache / path).is_file(), f"Missing SDK build input: {path}")
    native = {}
    for p in (cache / "artifacts/engine").rglob("*"):
        if p.is_file() and not p.is_symlink() and p.name in RUNTIMES:
            rel = p.relative_to(cache).as_posix()
            expected = None
            for match in re.finditer(r"(?:android|linux|windows)-(arm64|x64|x86|arm)(?:-|/)", rel):
                expected = [match.group(1)]
            if "/darwin-x64" in rel and p.name == "FlutterMacOS":
                expected = ["arm64", "x64"]
            if re.search(r"/ios(?:-profile|-release)?/", rel) and p.name == "Flutter":
                expected = ["arm64", "x64"] if "simulator" in rel else ["arm64"]
            native[rel] = inspect_native(p, exports, nm, readobj, expected, revision)
    need(native, "SDK contains no verified custom runtime")
    os_name, arch = sdk_host.split("-")
    for mode in MODES:
        suffix = "" if mode == "debug" else f"-{mode}"
        if os_name == "macos":
            directory = cache / f"artifacts/engine/darwin-x64{suffix}/FlutterMacOS.framework"
            need(any(p.is_file() and p.name == "FlutterMacOS" for p in directory.rglob("*")), f"Missing macOS {mode} framework")
            ios = cache / f"artifacts/engine/ios{suffix}/Flutter.xcframework"
            need((ios / "ios-arm64/Flutter.framework/Flutter").is_file(), f"Missing iOS {mode} device framework")
            need((ios / "ios-arm64_x86_64-simulator/Flutter.framework/Flutter").is_file(), f"Missing iOS {mode} simulator framework")
            ios_snapshot = ios.parent / "gen_snapshot_arm64"
            need(ios_snapshot.is_file() and binary_arches(ios_snapshot) == {"arm64", "x64"}, f"Missing/wrong iOS snapshot tool: {ios_snapshot}")
            for tool_arch in ("arm64", "x64"):
                tool = cache / f"artifacts/engine/darwin-x64{suffix}/gen_snapshot_{tool_arch}"
                # The suffix selects the target CPU; official generators copy
                # tools lipo-built for both macOS host architectures.
                need(tool.is_file() and binary_arches(tool) == {"arm64", "x64"}, f"Missing/wrong macOS snapshot tool: {tool}")
        else:
            for target_arch in ("arm64", "x64"):
                directory = cache / f"artifacts/engine/{os_name}-{target_arch}{suffix}"
                runtime = directory / ("flutter_windows.dll" if os_name == "windows" else "libflutter_linux_gtk.so")
                need(runtime.is_file(), f"Missing desktop runtime: {runtime}")
                snapshot = directory / ("gen_snapshot.exe" if os_name == "windows" else "gen_snapshot")
                # Windows ARM64's official recipe builds its cross-snapshot
                # tool for the x64 host; Linux GTK snapshots are target-native.
                snapshot_arch = "x64" if os_name == "windows" else target_arch
                need(snapshot.is_file() and binary_arches(snapshot) == {snapshot_arch}, f"Missing/wrong desktop snapshot tool: {snapshot}")
    android = {}
    for abi in (*ABIS, "x86"):
        for mode in (MODES if abi != "x86" else ("debug",)):
            directory = f"android-{abi}" + (f"-{mode}" if mode != "debug" else "")
            jar = cache / f"artifacts/engine/{directory}/flutter.jar"
            need(jar.is_file(), f"Missing Android cache JAR: {jar}")
            android[f"android-{abi}-{mode}"] = jar_runtime(jar, abi, exports, nm, readobj, revision)
    check_maven(root, revision, exports, nm, readobj, android)
    native.update(android)
    host_directory = cache / f"artifacts/engine/{'darwin-x64' if os_name == 'macos' else sdk_host}"
    for name in ("frontend_server_aot.dart.snapshot", "icudtl.dat", "isolate_snapshot.bin", "vm_isolate_snapshot.bin"):
        need((host_directory / name).is_file(), f"Missing host build input: {name}")
    need((host_directory / "shader_lib").is_dir() and any(p.is_file() for p in (host_directory / "shader_lib").rglob("*")),
         "Missing Impeller shader library")
    for name in (("path_ops.dll", "libtessellator.dll") if os_name == "windows" else
                 ("libpath_ops.dylib", "libtessellator.dylib") if os_name == "macos" else ("libpath_ops.so", "libtessellator.so")):
        library = host_directory / name
        need(library.is_file() and binary_arches(library) == {arch}, f"Missing/wrong host library: {library}")
    for name in ("flutter_tester", "impellerc", "font-subset"):
        executable = host_directory / (name + (".exe" if os_name == "windows" else ""))
        need(executable.is_file() and binary_arches(executable) == {arch}, f"Missing/wrong host tool: {executable}")
        if name == "flutter_tester":
            native[executable.relative_to(cache).as_posix()] = inspect_native(executable, [], nm, readobj, [arch], revision)
    for name in ("dart", "dartaotruntime"):
        dart = cache / "dart-sdk/bin" / (name + (".exe" if sdk_host.startswith("windows") else ""))
        need(dart.is_file() and binary_arches(dart) == {sdk_host.split("-")[1]}, "Host Dart SDK architecture mismatch")
    for abi in ABIS:
        for mode in ("profile", "release"):
            host_dir = "darwin-x64" if sdk_host.startswith("macos") else sdk_host
            tool = cache / f"artifacts/engine/android-{abi}-{mode}/{host_dir}/gen_snapshot"
            if sdk_host.startswith("windows"):
                tool = tool.with_suffix(".exe")
            tool_arches = {"arm64", "x64"} if os_name == "macos" else {"x64"}
            need(tool.is_file() and binary_arches(tool) == tool_arches, f"Missing/wrong AOT host tool: {tool}")
    return native


def archive_path(output, tag, sdk_host, revision):
    extension = ".tar.xz" if sdk_host.startswith("linux") else ".zip"
    return Path(output) / f"flutter_venus_{tag}_{sdk_host}_{revision[:12]}{extension}"


def package(args):
    _, manifest, spec, injector = context(args)
    identity = source_identity(args, manifest)
    revision = identity["flutter_commit"]
    sdk_host = args.host
    need(host() == sdk_host, "Package/warm/test must run on its actual SDK host architecture")
    bundle = Path(args.source_bundle).resolve()
    need(digest(bundle) == identity["bundle_sha256"], "Source bundle hash mismatch")
    artifacts, input_receipts = artifact_map(args.build_receipt, identity)
    required = required_artifacts(sdk_host)
    missing = sorted(set(required) - set(artifacts))
    need(not missing and "download.flutter.io" in artifacts, f"Incomplete native SDK inputs: {missing}; Maven directory is also required")
    output = Path(args.output).resolve()
    need(not output.exists(), f"Fresh package output required: {output}")
    output.mkdir(parents=True)
    root = output / "flutter"
    run([sys.executable, injector, "restore", "--manifest", manifest, "--flutter-root", root,
         "--bundle", bundle, "--receipt", args.source_receipt, "--repository", args.repository or spec["upstream"]])
    inject(injector, manifest, root, "check")
    exports = required_exports(root)
    nm = find_tool(args.flutter_root or root, args.nm, "llvm-nm")
    readobj = find_tool(args.flutter_root or root, args.readobj, "llvm-readobj")
    cache = root / "bin/cache"
    used = {}
    for destination, target in required.items():
        candidates = artifacts[destination]
        need(len({digest(p) for p in candidates}) == 1, f"Conflicting archives for {destination}")
        extract_zip(candidates[0], inside(cache, target) if target else cache)
        used[destination] = digest(candidates[0])
    for p in artifacts["download.flutter.io"]:
        extract_zip(p, cache / "venus-maven")
    overlays = sdk_overlays(root, revision)
    mingit = install_mingit(root, output, args.mingit_archive, args.mingit_sha256) if sdk_host.startswith("windows") else None
    for name in ("engine", "engine-dart-sdk", *STAMP_NAMES):
        (cache / f"{name}.stamp").write_text(revision + "\n")
    (cache / "engine.realm").write_text("")
    now = time.time()
    write_json(cache / "engine_stamp.json", {"build_date": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now)),
        "build_time_ms": int(now * 1000), "git_revision": revision,
        "git_revision_date": run(["git", "-C", root, "show", "-s", "--format=%cI", revision], capture=True),
        "content_hash": engine_content_hash(root)})
    native = verify_sdk(root, sdk_host, revision, exports, nm, readobj)
    env = dict(os.environ)
    for key in ("FLUTTER_PREBUILT_ENGINE_VERSION", "FLUTTER_REALM", "FLUTTER_STORAGE_BASE_URL", "DART_SDK_BASE_URL", "FLUTTER_ENGINE", "FLUTTER_ENGINE_SRC_PATH", "FLUTTER_TOOL_ARGS"):
        env.pop(key, None)
    env["PUB_CACHE"] = str(output / "pub-cache")
    env["PATH"] = str(root / "bin") + os.pathsep + env.get("PATH", "")
    flutter = root / ("bin/flutter.bat" if sdk_host.startswith("windows") else "bin/flutter")
    before = {p.relative_to(cache).as_posix(): digest(p) for p in cache.rglob("*") if p.is_file() and not p.is_symlink()}
    # Bootstrap tool snapshot from the newly installed Dart SDK. With native
    # stamps already exact, this command must not fetch official engine outputs.
    run([flutter, "--version", "--machine"], cwd=root, env=env)
    version = json.loads(run([flutter, "--version", "--machine"], cwd=root, env=env, capture=True))
    need(version["engineRevision"] == revision, "Normal Flutter tool resolved another engine")
    platform_flags = ["--android", "--web", "--ios", "--macos"] if sdk_host.startswith("macos") else ["--android", "--web", "--" + sdk_host.split("-")[0]]
    run([flutter, "precache", *platform_flags], cwd=root, env=env)
    # Ordinary Web build verifies the cache resolver and locally bundled Web resources.
    smoke = output / "smoke"
    run([flutter, "create", "--platforms=web", "--project-name=venus_sdk_smoke", smoke], env=env)
    run([flutter, "build", "web", "--release"], cwd=smoke, env=env)
    need((smoke / "build/web/canvaskit").is_dir(), "Default Web build did not bundle custom resources")
    run([flutter, "build", "web", "--release", "--wasm"], cwd=smoke, env=env)
    need(list((smoke / "build/web").rglob("*skwasm*.wasm")), "Default Wasm build did not bundle custom skwasm")
    for name, sha in before.items():
        need(digest(cache / name) == sha, f"Flutter bootstrap/build replaced packaged artifact: {name}")
    verify_sdk(root, sdk_host, revision, exports, nm, readobj)
    # Match official package layout: root flutter/, Git identity, warmed tool,
    # Dart SDK and artifacts. No machine-specific pub package_config is shipped.
    for p in (root / "packages").glob("*/.dart_tool"):
        shutil.rmtree(p)
    preload = root / ".pub-preload-cache"
    preload.mkdir()
    for pubspec in (output / "pub-cache/hosted/pub.dev").glob("*/pubspec.yaml"):
        package_root = pubspec.parent
        archive = preload / f"{package_root.name}.tar.gz"
        with tarfile.open(archive, "w:gz") as t:
            for p in sorted(package_root.iterdir()):
                t.add(p, arcname=p.name)
    delivery = {"schema_version": 1, "source": identity, "host": sdk_host, "input_receipts": input_receipts,
                "used_archives": used, "sdk_source_overlays": overlays, "native": native,
                "version": version, "default_web_build": "PASS", "cross_platform_execution": "NOT_RUN",
                "mingit": mingit,
                "android_native_inputs": "bundled local Maven; actual APK build requires installed Android SDK/JDK",
                "official_native_downloads_used": False}
    write_json(root / "bin/cache/venus-sdk.json", delivery)
    archive = archive_path(output, spec["official_tag"], sdk_host, revision)
    if not sdk_host.startswith("linux"):
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            for p in sorted(root.rglob("*")):
                if p.is_symlink():
                    info = zipfile.ZipInfo(p.relative_to(output).as_posix())
                    info.create_system = 3
                    info.external_attr = (stat.S_IFLNK | 0o777) << 16
                    z.writestr(info, os.readlink(p))
                elif p.is_file():
                    z.write(p, p.relative_to(output).as_posix())
    else:
        with tarfile.open(archive, "w:xz") as t:
            t.add(root, arcname="flutter")
    write_json(output / "delivery.json", {"sdk_archive": archive.name, "sha256": digest(archive), "sdk_receipt": digest(root / "bin/cache/venus-sdk.json")})
    print(archive)


def clean(args):
    """Delete a fixed set of intermediates only after all delivery checks pass."""
    output = Path(args.output).absolute()
    for part in (output, *output.parents):
        need(not part.is_symlink(), f"Refuse cleanup through symlink: {part}; use its physical path")
    need(not output.is_symlink() and output.is_dir(), f"Not a real release directory: {output}")
    output = output.resolve()
    need(output != Path(output.anchor) and output != Path.home().resolve() and
         not (output / ".git").exists() and not (output / "bin/flutter").exists(),
         "Refuse to clean a filesystem root, home, repository or SDK root")

    def checked(name, directory=False):
        path = output / name
        need(path.is_relative_to(output), f"Path escapes release directory: {name}")
        for part in (path, *path.parents):
            if part == output:
                break
            need(not part.is_symlink(), f"Refuse cleanup through symlink: {part}")
        need(path.is_dir() if directory else path.is_file(), f"Missing delivery path: {path}")
        return path

    checked("sdk", directory=True)
    flutter = checked("sdk/flutter", directory=True)
    checked("sdk/flutter/bin/flutter")
    marker = checked("sdk/delivery.json")
    receipt_path = checked("sdk/flutter/bin/cache/venus-sdk.json")
    delivery = read_json(marker)
    name = relative(delivery["sdk_archive"])
    need(len(name.parts) == 1 and name.name not in {"delivery.json", "flutter"}, "Unsafe SDK archive name")
    archive = checked("sdk/" + name.name)
    need(digest(archive) == delivery["sha256"], "Final SDK archive hash mismatch")
    need(digest(receipt_path) == delivery["sdk_receipt"], "Final SDK receipt hash mismatch")
    receipt = read_json(receipt_path)
    need(receipt["schema_version"] == 1 and receipt["host"] in HOSTS and
         receipt["default_web_build"] == "PASS" and receipt["official_native_downloads_used"] is False,
         "SDK packaging did not complete with local engines")
    identity = receipt["source"]
    need(isinstance(identity, dict) and identity.get("schema_version") == 1 and
         re.fullmatch(r"[0-9a-f]{40}", identity.get("flutter_commit", "")), "Invalid packaged source identity")
    protected = [flutter, Path.cwd().resolve(),
                 Path(args.venus_root).resolve() if args.venus_root else Path(__file__).resolve().parents[2]]
    protected.extend(Path(path).resolve() for path in (args.flutter_root or []))
    candidates = []
    for name in ("source", "build", "sdk/smoke", "sdk/pub-cache"):
        path = output / name
        if not path.exists() and not path.is_symlink():
            continue
        path = checked(name, directory=True)
        need(not any(p.is_relative_to(path) for p in protected), f"Cleanup contains a protected checkout/SDK/cwd: {path}")
        need(not any(p.is_relative_to(path) for p in (marker, receipt_path, archive)), f"Cleanup overlaps final delivery: {path}")
        for checkout in (path, path / "flutter"):
            need(not (checkout / ".git").is_file(), f"Refuse to remove registered Git worktree: {checkout}")
        candidates.append(path)
    if output / "source" in candidates:
        source = checked("source/source.json")
        need(read_json(source) == identity, "Source directory belongs to another SDK")
        bundle = checked("source/source.bundle")
        need(digest(bundle) == identity["bundle_sha256"], "Source bundle hash mismatch")
    if output / "build" in candidates:
        build = checked("build/artifacts/receipt.json")
        need(read_json(build)["source"] == identity, "Build directory belongs to another SDK")
        build_sha = digest(build)
        need(any(item["sha256"] == build_sha for item in receipt["input_receipts"]),
             "Build receipt was not used in this SDK")
    # All preflight checks precede deletion. rmtree does not follow inner symlinks.
    # An I/O error during deletion can leave partial intermediates; report it.
    for path in candidates:
        shutil.rmtree(path)
        print(f"Removed: {path}")
    print(f"Kept SDK: {flutter}\nKept archive: {archive}\nKept delivery: {marker}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--venus-root")
    parser.add_argument("--manifest")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("plan", help="Read official recipes and print the real worker/platform matrix")
    p.add_argument("--flutter-root", required=True)
    p.add_argument("--group", choices=GROUPS, action="append")
    b = sub.add_parser("build", help="Build selected official recipes in a fresh injected checkout")
    b.add_argument("--flutter-root", required=True)
    b.add_argument("--source-receipt", required=True)
    b.add_argument("--group", choices=GROUPS, action="append", required=True)
    b.add_argument("--output", required=True)
    q = sub.add_parser("package", help="Package/warm/smoke-test a complete custom SDK on its actual host")
    q.add_argument("--source-bundle", required=True)
    q.add_argument("--source-receipt", required=True)
    q.add_argument("--build-receipt", action="append", required=True)
    q.add_argument("--host", choices=HOSTS, required=True)
    q.add_argument("--output", required=True)
    q.add_argument("--flutter-root", help="Optional current builder checkout to locate its llvm tools")
    q.add_argument("--repository", help="Official tag source URL or readonly local mirror; default manifest upstream")
    q.add_argument("--mingit-archive", help="Optional offline copy of the official MinGit ZIP, Windows packaging only")
    q.add_argument("--mingit-sha256", help="Required SHA256 when --mingit-archive is supplied")
    c = sub.add_parser("clean", help="Verify final delivery and remove fixed packaging intermediates")
    c.add_argument("--output", required=True, help="Release root containing sdk/delivery.json")
    c.add_argument("--flutter-root", action="append", help="Protected source checkout; may repeat")
    for item in (b, q):
        item.add_argument("--nm", help="Pinned llvm-nm for native export checks")
        item.add_argument("--readobj", help="Pinned llvm-readobj for PE export checks")
    args = parser.parse_args()
    try:
        {"plan": plan, "build": build, "package": package, "clean": clean}[args.command](args)
    except (Failure, OSError, KeyError, ValueError, struct.error, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        print(f"STOP: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
