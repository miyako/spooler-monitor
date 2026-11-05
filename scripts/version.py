#!/usr/bin/env python3
"""Compute the next release version from existing tags and write it to version files."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MODES = ("patch", "minor", "major")
RELEASE = re.compile(r"v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", re.ASCII)
# Files that carry the component/CLI version literally. The CLI reports the
# component version, and the 4D class and host fixtures assert it.
VERSION_FILES = (
    "template.json",
    "package.json",
    "cli/monitor.cpp",
    "cli/README.md",
    "component/{repository}/Project/Sources/Classes/Queue.4dm",
    "component/{repository}/Documentation/Classes/Queue.md",
    "tests/host/Project/Sources/Methods/Smoke.4dm",
    "tests/host/contract.json",
)


def parse_release(tag):
    """Return (major, minor, patch) for X.Y.Z or legacy vX.Y.Z; None for anything else."""
    match = RELEASE.fullmatch(tag) if isinstance(tag, str) else None
    return tuple(int(part) for part in match.groups()) if match else None


def latest(tags):
    releases = [version for version in map(parse_release, tags) if version]
    return max(releases) if releases else None


def bump(version, mode):
    major, minor, patch = version
    if mode == "major":
        return (major + 1, 0, 0)
    if mode == "minor":
        return (major, minor + 1, 0)
    if mode == "patch":
        return (major, minor, patch + 1)
    raise ValueError(f"mode must be one of {', '.join(MODES)}")


def text(version):
    return ".".join(map(str, version))


def configured(root):
    config = json.loads((root / "template.json").read_text(encoding="utf-8"))
    version = parse_release(config.get("version"))
    if not version or config["version"].startswith("v"):
        raise ValueError("template.json version must be a plain X.Y.Z release")
    return config, version


def git_tags(root):
    output = subprocess.run(["git", "tag", "--list"], cwd=root, check=True,
                            capture_output=True, text=True, encoding="utf-8").stdout
    return [line.strip() for line in output.splitlines() if line.strip()]


def next_version(root, mode, tags=None):
    tags = git_tags(root) if tags is None else tags
    _, current = configured(root)
    new = text(bump(latest(tags) or current, mode))
    if new in tags or "v" + new in tags:
        raise ValueError(f"tag {new} already exists")
    return new


def set_version(root, new):
    if not parse_release(new) or new.startswith("v"):
        raise ValueError("version must be a plain X.Y.Z release")
    config, current = configured(root)
    old = text(current)
    pattern = re.compile(r"(?<![0-9A-Za-z.])" + re.escape(old) + r"(?![0-9A-Za-z])")
    changed = []
    for name in VERSION_FILES:
        path = root / name.format(repository=config["repository"])
        content = path.read_text(encoding="utf-8")
        if not pattern.search(content):
            raise ValueError(f"{path.relative_to(root)} does not contain version {old}")
        if old != new:
            path.write_text(pattern.sub(new, content), encoding="utf-8", newline="")
            changed.append(path)
    for name in ("template.json", "package.json"):
        if json.loads((root / name).read_text(encoding="utf-8"))["version"] != new:
            raise ValueError(f"{name} version was not updated")
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--mode", choices=MODES, required=True)
    parser.add_argument("--write", action="store_true", help="also update version files")
    args = parser.parse_args()
    try:
        new = next_version(args.root, args.mode)
        if args.write:
            set_version(args.root, new)
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        print(f"version: {error}", file=sys.stderr)
        return 1
    print(new)
    if output := os.environ.get("GITHUB_OUTPUT"):
        with open(output, "a", encoding="utf-8") as stream:
            stream.write(f"version={new}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
