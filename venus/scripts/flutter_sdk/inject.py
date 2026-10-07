#!/usr/bin/env python3
"""Prepare Flutter from Venus-owned patches; never overwrite an existing checkout."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

DEFAULT_MANIFEST = Path(__file__).parent / "releases/3.44.9/manifest.json"
GENERATED_FILES = {
    "release_entry": ("venus/release_sdk.sh", "100755"),
    "release_readme": ("venus/README.md", "100644"),
    "release_env_example": ("venus/release_sdk.env.example", "100644"),
    "release_ignore": ("venus/.gitignore", "100644"),
}
RELEASE_TOOLS = {"inject.py": "100644", "release.py": "100755", "release_sdk.sh": "100755"}
VERSION_FILES = {"release_entry": "release_entry.sh", "release_readme": "release_readme.md",
                 "release_env_example": "release_env.example", "release_ignore": "release_ignore"}


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def inside(root, relative):
    root = root.resolve()
    path = (root / relative).resolve()
    if not path.is_relative_to(root) or path == root:
        raise ValueError(f"Path escapes source: {relative}")
    return path


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def regular_input(root, relative):
    path = inside(root, relative)
    raw = root / relative
    while raw != root:
        if raw.is_symlink():
            raise ValueError(f"Unexpected symlink: {relative}")
        raw = raw.parent
    if not path.is_file():
        raise ValueError(f"Missing regular release input: {relative}")
    return path


def generated_inputs(manifest, manifest_path):
    """One fixed inventory for installation, Git staging and final identity checks."""
    prefix = "venus/scripts/flutter_sdk/"
    result = {manifest[key]["destination"]: (manifest_path.parent / manifest[key]["template"], manifest[key]["mode"])
              for key in GENERATED_FILES}
    result.update({prefix + name: (Path(__file__).parent / name, mode) for name, mode in RELEASE_TOOLS.items()})
    version = prefix + "releases/" + manifest["official_tag"] + "/"
    result.update({version + name: (manifest_path.parent / name, "100644")
                   for name in ("manifest.json", "native.patch", "skia.patch", *VERSION_FILES.values())})
    # A candidate manifest may have another basename; its installed name is fixed.
    result[version + "manifest.json"] = (manifest_path, "100644")
    return result


def check_whitespace(root, manifest, *revisions):
    # Raw patches contain valid single-space blank context lines. Their content
    # is pinned separately; all applied source and other generated files stay strict.
    prefix = "venus/scripts/flutter_sdk/releases/" + manifest["official_tag"] + "/"
    excluded = (manifest["native_patch"]["path"], manifest["dependencies"]["skia"]["patch_file"])
    git(root, "diff", "--check", *revisions, "--", ".", *(":(exclude)" + prefix + name for name in excluded))


def load_manifest(path):
    path = path.resolve()
    manifest = json.loads(path.read_text())
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", manifest["official_tag"]):
        raise ValueError("Invalid official tag")
    for value in (manifest["official_commit"], manifest["dependencies"]["skia"]["official_revision"]):
        if not re.fullmatch(r"[0-9a-f]{40}", value):
            raise ValueError("Invalid official commit")
    for spec, key, hash_key in ((manifest["native_patch"], "path", "sha256"),
                                (manifest["dependencies"]["skia"], "patch_file", "patch_sha256")):
        expected_name = "native.patch" if key == "path" else "skia.patch"
        if spec[key] != expected_name:
            raise ValueError("Unexpected release patch path")
        patch = regular_input(path.parent, spec[key])
        if sha256(patch) != spec[hash_key]:
            raise ValueError(f"Patch hash differs: {patch.name}")
    files = manifest["native_patch"]["files"]
    if not files or len(files) != manifest["native_patch"]["expected_file_count"]:
        raise ValueError("Native file inventory is incomplete")
    for spec in (manifest["native_patch"], manifest["dependencies"]["skia"]):
        for name in spec["files"]:
            inside(path.parent, name)
    for key, (destination, mode) in GENERATED_FILES.items():
        entry = manifest[key]
        if entry["template"] != VERSION_FILES[key]:
            raise ValueError(f"Unexpected {key} template path")
        template = inside(path.parent, entry["template"])
        if (path.parent / entry["template"]).is_symlink() or not template.is_file() or sha256(template) != entry["sha256"]:
            raise ValueError(f"{key.replace('_', ' ').capitalize()} template must be a regular file with the pinned hash")
        if entry["destination"] != destination or entry["mode"] != mode:
            raise ValueError(f"Unexpected generated {key} destination/mode")
    tools = manifest["release_tooling"]
    if set(tools) != set(RELEASE_TOOLS):
        raise ValueError("Release tool inventory differs")
    for name, mode in RELEASE_TOOLS.items():
        tool = regular_input(Path(__file__).parent, name)
        # Windows chmod has no POSIX execute bit; final Git tree modes are checked below.
        if tools[name] != {"sha256": sha256(tool), "mode": mode} or (sys.platform != "win32" and (
                "100755" if tool.stat().st_mode & 0o111 else "100644") != mode):
            raise ValueError(f"Release tool hash/mode differs: {name}")
    return manifest


def exact_repo(root):
    if Path(git(root, "rev-parse", "--show-toplevel")).resolve() != root:
        raise ValueError("Expected an independent checkout at the exact Flutter root")


def blob_differences(root, files, revision, field):
    differences = []
    for name, expected in files.items():
        record = git(root, "ls-tree", revision, "--", name).split()
        blob = record[2] if record else "0" * 40
        mode = record[0] if record else "000000"
        if blob != expected[field + "_blob"] or mode != expected.get(field + "_mode", "100644"):
            differences.append({"path": name, "expected_blob": expected[field + "_blob"],
                                "actual_blob": blob, "expected_mode": expected.get(field + "_mode", "100644"),
                                "actual_mode": mode})
    return differences


def check_blobs(root, files, revision, field):
    differences = blob_differences(root, files, revision, field)
    if differences:
        raise ValueError(f"Unexpected {field} blob/mode; AGENT_REQUIRED: " + json.dumps(differences))


def preflight(root, manifest, revision="HEAD", component="native"):
    """Compare only protected official file hashes saved in Venus; tag labels are metadata."""
    exact_repo(root)
    spec = manifest["native_patch"] if component == "native" else manifest["dependencies"]["skia"]
    repo = root if component == "native" else inside(root, "engine/src/flutter/third_party/skia")
    exact_repo(repo)
    target = git(repo, "rev-parse", "--verify", revision + "^{commit}")
    official_files = {name: record for name, record in spec["files"].items()
                      if record["base_blob"] != "0" * 40}
    differences = blob_differences(repo, official_files, target, "base")
    dependency_difference = None
    if component == "native":
        deps = git(repo, "show", target + ":DEPS")
        match = re.search(r"['\"]skia_revision['\"]\s*:\s*['\"]([0-9a-f]{40})['\"]", deps)
        actual = match.group(1) if match else None
        expected = manifest["dependencies"]["skia"]["official_revision"]
        if actual != expected:
            dependency_difference = {"path": "DEPS", "expected_revision": expected, "actual_revision": actual}
    local_changes = git(repo, "status", "--porcelain").splitlines()
    return {"decision": "AGENT_REQUIRED" if differences or dependency_difference or local_changes else "MATCHED",
            "component": component, "baseline_tag": manifest["official_tag"],
            "baseline_commit": manifest["official_commit"] if component == "native" else spec["official_revision"],
            "target_commit": target, "compared_files": len(official_files),
            "differences": differences, "dependency_difference": dependency_difference,
            "local_changes": local_changes}


def guard_preflight(report):
    if report["decision"] != "MATCHED":
        print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)
        raise ValueError("AGENT_REQUIRED: protected official files, dependency pin or local inputs changed; preserve the checkout, "
                         "follow the Venus flutter-venus-patch skill to migrate and validate before updating hashes")


def clean(root):
    if git(root, "status", "--porcelain") or git(root, "ls-files", "-u"):
        raise ValueError("Checkout has local changes; preserve them and use a fresh destination")


def check(root, manifest, manifest_path):
    exact_repo(root)
    clean(root)
    base = manifest["official_commit"]
    if git(root, "merge-base", base, "HEAD") != base or git(root, "rev-parse", "HEAD") == base:
        raise ValueError("Expected a customized descendant of the official tag")
    if git(root, "rev-parse", "refs/tags/" + manifest["official_tag"] + "^{commit}") != base:
        raise ValueError("Official tag identity differs")
    files = manifest["native_patch"]["files"]
    changed = set(git(root, "diff", "--name-only", base, "HEAD").splitlines())
    generated = generated_inputs(manifest, manifest_path)
    if changed != set(files) | {"docs/venus/source.json", *generated}:
        raise ValueError("Changed-file set differs; expected native patch, provenance and generated release files")
    provenance = json.loads((root / "docs/venus/source.json").read_text())
    locator = provenance.pop("venus_root", None)
    if not isinstance(locator, str) or not locator:
        raise ValueError("Missing Venus locator hint")
    expected = source_provenance(manifest, manifest_path)
    expected.pop("venus_root")
    if provenance != expected:
        raise ValueError("Flutter provenance differs from the canonical Venus manifest")
    for destination, (source, mode) in generated.items():
        current = regular_input(root, destination)
        current_mode = "100755" if current.stat().st_mode & 0o111 else "100644"
        if sha256(current) != sha256(source) or (sys.platform != "win32" and current_mode != mode):
            raise ValueError(f"Generated release file hash/mode differs: {destination}")
    check_blobs(root, {destination: {"patched_blob": git(root, "hash-object", str(source)),
                                    "patched_mode": mode}
                       for destination, (source, mode) in generated.items()}, "HEAD", "patched")
    check_blobs(root, files, base, "base")
    check_blobs(root, files, "HEAD", "patched")
    check_whitespace(root, manifest, base, "HEAD")
    return git(root, "rev-parse", "HEAD")


def skia(root, manifest, manifest_path, apply=False):
    exact_repo(root)
    spec = manifest["dependencies"]["skia"]
    pin = spec["official_revision"]
    deps_pin = re.search(r"['\"]skia_revision['\"]\s*:\s*['\"]([0-9a-f]{40})['\"]", (root / "DEPS").read_text())
    if not deps_pin or deps_pin.group(1) != pin:
        raise ValueError("Skia manifest differs from Flutter DEPS")
    repo = inside(root, "engine/src/flutter/third_party/skia")
    exact_repo(repo)
    if git(repo, "rev-parse", "HEAD") != pin:
        raise ValueError("Skia HEAD differs from the official dependency pin")
    if git(repo, "ls-files", "-u") or git(repo, "diff", "--cached", "--name-only"):
        raise ValueError("Skia has staged or unmerged changes")
    if git(repo, "ls-files", "--others", "--exclude-standard"):
        raise ValueError("Skia has untracked inputs")
    files = spec["files"]
    if not files or not set(git(repo, "diff", "HEAD", "--name-only").splitlines()) <= set(files):
        raise ValueError("Skia contains changes outside the patch")
    check_blobs(repo, files, "HEAD", "base")
    blobs = {}
    for name, expected in files.items():
        path = inside(repo, name)
        if (repo / name).is_symlink():
            raise ValueError(f"Unexpected symlink: {name}")
        mode = "100755" if path.stat().st_mode & 0o111 else "100644"
        if mode != expected["base_mode"]:
            raise ValueError(f"Unexpected file mode: {name}")
        blobs[name] = git(repo, "hash-object", str(path))
    if all(blobs[n] == e["patched_blob"] for n, e in files.items()):
        git(repo, "diff", "--check", "HEAD")
        return "already-applied"
    if not all(blobs[n] == e["base_blob"] for n, e in files.items()):
        raise ValueError("Skia is partially patched or contains another writer's changes")
    patch = inside(manifest_path.parent, spec["patch_file"])
    git(repo, "apply", "--check", str(patch))
    if not apply:
        return "pristine-patch-applicable"
    git(repo, "apply", str(patch))
    for name, expected in files.items():
        if git(repo, "hash-object", str(inside(repo, name))) != expected["patched_blob"]:
            raise ValueError(f"Applied Skia blob differs: {name}")
    git(repo, "diff", "--check", "HEAD")
    return "applied"


def clone_base(args, manifest):
    root = args.flutter_root.resolve()
    if root.exists():
        raise ValueError("Destination already exists; no existing checkout will be overwritten")
    branch = "venus/venus-stable-" + manifest["official_tag"]
    if args.branch and args.branch != branch:
        raise ValueError(f"Release branch is fixed: {branch}")
    subprocess.run(["git", "check-ref-format", "--branch", branch], check=True, stdout=subprocess.DEVNULL)
    root.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "clone", "--single-branch", "--branch", manifest["official_tag"],
                    args.repository or manifest["fork"], str(root)], check=True)
    exact_repo(root)
    if git(root, "rev-parse", "HEAD") != manifest["official_commit"]:
        raise ValueError("Cloned tag differs from the locked official commit; destination preserved")
    clean(root)
    return root, branch


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x") as stream:
        stream.write(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def source_provenance(manifest, manifest_path):
    return {"canonical_source": "Venus scripts/flutter_sdk", "official_tag": manifest["official_tag"],
            "official_commit": manifest["official_commit"], "manifest_sha256": sha256(manifest_path),
            "native_patch_sha256": manifest["native_patch"]["sha256"],
            "skia_patch_sha256": manifest["dependencies"]["skia"]["patch_sha256"],
            **{key + "_sha256": manifest[key]["sha256"] for key in GENERATED_FILES},
            "venus_root": str(Path(__file__).resolve().parents[2])}


def install_release_files(root, manifest, manifest_path):
    for name, (source, mode) in generated_inputs(manifest, manifest_path).items():
        destination = inside(root, name)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("xb") as stream:
            stream.write(source.read_bytes())
        destination.chmod(int(mode, 8) & 0o777)


def source_receipt(root, manifest, manifest_path):
    return {"schema_version": 1, "official_tag": manifest["official_tag"],
            "official_commit": manifest["official_commit"], "flutter_commit": check(root, manifest, manifest_path),
            "manifest_sha256": sha256(manifest_path), "native_patch_sha256": manifest["native_patch"]["sha256"],
            "skia_patch_sha256": manifest["dependencies"]["skia"]["patch_sha256"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("prepare", "restore", "check", "skia", "snapshot", "preflight"):
        command = sub.add_parser(name)
        command.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
        command.add_argument("--flutter-root", type=Path, required=True)
        if name in ("prepare", "restore"):
            command.add_argument("--repository")
            command.add_argument("--branch")
        if name == "skia":
            command.add_argument("--apply", action="store_true")
        if name == "preflight":
            command.add_argument("--revision", default="HEAD", help="Target official Git revision; read-only")
            command.add_argument("--component", choices=("native", "skia"), default="native")
        if name in ("restore", "snapshot"):
            command.add_argument("--bundle", type=Path, required=True)
            command.add_argument("--receipt", type=Path, required=True)
        if name == "snapshot":
            command.add_argument("--reuse", action="store_true", help="Reuse only an identical, complete snapshot")
    args = parser.parse_args()
    manifest_path = args.manifest.resolve()
    manifest = load_manifest(manifest_path)
    root = args.flutter_root.resolve()
    if args.command == "prepare":
        root, branch = clone_base(args, manifest)
        guard_preflight(preflight(root, manifest))
        git(root, "switch", "-c", branch)
        patch = inside(manifest_path.parent, manifest["native_patch"]["path"])
        git(root, "apply", "--check", str(patch))
        git(root, "apply", str(patch))
        write_json(root / "docs/venus/source.json", source_provenance(manifest, manifest_path))
        install_release_files(root, manifest, manifest_path)
        generated = generated_inputs(manifest, manifest_path)
        git(root, "add", "--", *manifest["native_patch"]["files"], "docs/venus/source.json", *generated)
        for name, (_, file_mode) in generated.items():
            mode = "+x" if file_mode == "100755" else "-x"
            git(root, "update-index", "--chmod=" + mode, "--", name)
        check_whitespace(root, manifest, "--cached")
        git(root, "commit", "-m", "feat(venus): 应用 Flutter " + manifest["official_tag"] + " 完整补丁")
        print(json.dumps(source_receipt(root, manifest, manifest_path), indent=2))
    elif args.command == "restore":
        receipt = json.loads(args.receipt.read_text())
        for key, expected in (("manifest_sha256", sha256(manifest_path)), ("bundle_sha256", sha256(args.bundle)),
                              ("official_commit", manifest["official_commit"])):
            if receipt.get(key) != expected:
                raise ValueError(f"Source snapshot identity differs: {key}")
        root, branch = clone_base(args, manifest)
        guard_preflight(preflight(root, manifest))
        git(root, "fetch", str(args.bundle.resolve()), "HEAD")
        if git(root, "rev-parse", "FETCH_HEAD") != receipt["flutter_commit"]:
            raise ValueError("Bundle revision differs from the source receipt")
        git(root, "switch", "-c", branch, "FETCH_HEAD")
        check(root, manifest, manifest_path)
        print("PASS: restored exact shared source commit")
    elif args.command == "snapshot":
        receipt = source_receipt(root, manifest, manifest_path)
        bundle = args.bundle.resolve()
        if args.bundle.is_symlink() or args.receipt.is_symlink() or bundle.is_relative_to(root) or args.receipt.resolve().is_relative_to(root):
            raise ValueError("Snapshot outputs must be new paths outside the Flutter checkout")
        if bundle.exists() or args.receipt.exists():
            if not args.reuse or not bundle.is_file() or not args.receipt.is_file():
                raise ValueError("Snapshot outputs must be new paths or a complete identical reusable snapshot")
            receipt["bundle_sha256"] = sha256(bundle)
            if json.loads(args.receipt.read_text()) != receipt:
                raise ValueError("Existing source snapshot differs from the checked checkout")
            if git(root, "bundle", "list-heads", str(bundle), "HEAD").split() != [receipt["flutter_commit"], "HEAD"]:
                raise ValueError("Existing source bundle HEAD differs from the checked checkout")
            print("PASS: identical source bundle and receipt reused")
            return
        bundle.parent.mkdir(parents=True, exist_ok=True)
        git(root, "bundle", "create", str(bundle), manifest["official_commit"] + "..HEAD")
        receipt["bundle_sha256"] = sha256(bundle)
        write_json(args.receipt, receipt)
        print("PASS: source bundle and receipt created")
    elif args.command == "skia":
        print("PASS: " + skia(root, manifest, manifest_path, args.apply))
    elif args.command == "preflight":
        report = preflight(root, manifest, args.revision, args.component)
        guard_preflight(report)
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print("PASS: exact native patch " + check(root, manifest, manifest_path))


if __name__ == "__main__":
    try:
        main()
    except (KeyError, OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f"STOP: {error}")
