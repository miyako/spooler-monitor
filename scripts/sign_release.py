#!/usr/bin/env python3
"""Sign and notarize the staged compiled ZIP without rewriting build provenance."""
import argparse
import base64
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import subprocess
import sys
import tempfile
import zipfile

from build_provenance import commit, sha256, source_digest
from ci_config import ROOT, TARGETS, load_config
from native_dependencies import inspect
from package_component import package, unpack_for_test, validate_compile, validate_resources, write_zip
from test_4d import test_4d, validate_status


SECRETS = ("APPLE_DEVELOPER_ID_CERTIFICATE", "APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD",
           "KEYCHAIN_PASSWORD", "NOTARYTOOL_APPLE_ID", "NOTARYTOOL_PASSWORD",
           "NOTARYTOOL_TEAM_ID")
MACHO = {b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
         b"\xfe\xed\xfa\xce", b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"}


def command(args, *, timeout=120):
    """Never include credential-bearing argv or vendor diagnostics in exceptions."""
    try:
        result = subprocess.run([str(arg) for arg in args], capture_output=True,
                                text=True, encoding="utf-8", timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        raise ValueError(f"{Path(args[0]).name} could not complete") from None
    if result.returncode:
        raise ValueError(f"{Path(args[0]).name} failed (exit {result.returncode})")
    return result.stdout + result.stderr


def credentials():
    values = {name: os.environ.get(name, "") for name in SECRETS}
    missing = [name for name, value in values.items() if not value]
    if missing:
        raise ValueError("missing signing secrets: " + ", ".join(missing))
    if not re.fullmatch(r"[A-Z0-9]{10}", values["NOTARYTOOL_TEAM_ID"]):
        raise ValueError("invalid NOTARYTOOL_TEAM_ID")
    return values


def select_identity(output, team):
    identities = re.findall(r'^\s*\d+\)\s+([A-Fa-f0-9]{40}) "([^"]+)"\s*$',
                            output, flags=re.MULTILINE)
    if len(identities) != 1 or not identities[0][1].startswith("Developer ID Application: "):
        raise ValueError("require exactly one valid imported Developer ID Application identity")
    fingerprint, name = identities[0]
    if not name.endswith(f" ({team})"):
        raise ValueError("Developer ID Application identity team mismatch")
    return fingerprint


@contextmanager
def signing_keychain(values):
    with tempfile.TemporaryDirectory(prefix="spooler-sign-", dir=os.environ.get("RUNNER_TEMP")) as directory:
        directory = Path(directory)
        p12 = directory / "identity.p12"
        keychain = directory / "signing.keychain-db"
        password = values["KEYCHAIN_PASSWORD"]
        created = False
        search_list = shlex.split(command(["security", "list-keychains", "-d", "user"]))
        try:
            try:
                certificate = base64.b64decode(values["APPLE_DEVELOPER_ID_CERTIFICATE"], validate=True)
            except ValueError:
                raise ValueError("invalid base64 signing certificate") from None
            if not certificate:
                raise ValueError("empty signing certificate")
            p12.touch(mode=0o600)
            p12.write_bytes(certificate)
            command(["security", "create-keychain", "-p", password, keychain])
            created = True
            command(["security", "set-keychain-settings", "-lut", "21600", keychain])
            command(["security", "unlock-keychain", "-p", password, keychain])
            command(["security", "import", p12, "-P",
                     values["APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD"],
                     "-k", keychain, "-T", "/usr/bin/codesign"])
            p12.unlink()
            command(["security", "set-key-partition-list", "-S", "apple-tool:,apple:,codesign:",
                     "-s", "-k", password, keychain])
            identity = select_identity(command(
                ["security", "find-identity", "-v", "-p", "codesigning", keychain]),
                values["NOTARYTOOL_TEAM_ID"])
            yield keychain, identity
        finally:
            try:
                command(["security", "list-keychains", "-d", "user", "-s", *search_list])
            finally:
                if created:
                    command(["security", "delete-keychain", keychain])


def signature_metadata(output, team):
    fields = dict(re.findall(r"^([A-Za-z][A-Za-z ]*)=(.*)$", output, flags=re.MULTILINE))
    if fields.get("TeamIdentifier") != team:
        raise ValueError("signed code team mismatch")
    if not re.search(r"^Authority=Developer ID Application: .+ \(" + re.escape(team) + r"\)$",
                     output, flags=re.MULTILINE):
        raise ValueError("signed code requires Developer ID Application authority")
    timestamp = fields.get("Timestamp", "").strip()
    if not timestamp or timestamp.lower() in {"none", "not set"}:
        raise ValueError("signed code requires a secure timestamp")
    if not re.search(r"flags=.*\bruntime\b", output):
        raise ValueError("signed code requires hardened runtime")
    return {"team": team, "timestamp": timestamp, "hardened_runtime": True}


def verify_code(path, team):
    requirement = ('anchor apple generic and '
                   'certificate leaf[field.1.2.840.113635.100.6.1.13] exists and '
                   f'certificate leaf[subject.OU] = "{team}"')
    command(["codesign", "--verify", "--strict", "--all-architectures",
             "-R=" + requirement, path])
    architectures = command(["lipo", "-archs", path]).strip().split()
    if not architectures or set(architectures) - {"arm64", "x86_64"}:
        raise ValueError("unexpected signed Mach-O architectures")
    return {arch: signature_metadata(command(
        ["codesign", "--display", "--verbose=4", "--arch", arch, path]), team)
            for arch in architectures}


def inventory(component):
    result = {}
    for path in sorted(component.rglob("*")):
        if path.is_symlink() or not (path.is_dir() or path.is_file()):
            raise ValueError("signed package must contain only regular files/directories")
        if path.is_file():
            result[path.relative_to(component).as_posix()] = {
                "sha256": sha256(path), "bytes": path.stat().st_size,
                "mode": path.stat().st_mode & 0o777}
    return result


def code_paths(component, cli):
    expected = {f"Resources/bin/macOS/{cli}", "Libraries/lib4d-arm64.dylib"}
    found = {name for name in inventory(component)
             if (component / name).read_bytes()[:4] in MACHO}
    if found != expected:
        raise ValueError("unreviewed or missing Mach-O code objects: " + repr(sorted(found)))
    # 4D's CompiledCode is not native Mach-O; reject new embedded native objects.
    archive = component / (component.name.removesuffix(".4dbase") + ".4DZ")
    with zipfile.ZipFile(archive) as packed:
        if any(packed.read(name)[:4] in MACHO for name in packed.namelist() if not name.endswith("/")):
            raise ValueError("unreviewed native code inside .4DZ")
    return sorted(found, key=lambda name: (not name.startswith("Libraries/"), name))


def verify_assembly(root, component, config):
    mac, windows = validate_resources(component, config["cli"])
    base = component / "Resources/bin"
    status = json.loads((base / "assembly-manifest.json").read_text(encoding="utf-8"))
    if (status.get("schema") != 1 or status.get("phase") != "pre-sign"
            or status.get("commit") != commit(root)
            or status.get("source_sha256") != source_digest(root)):
        raise ValueError("stale pre-sign assembly provenance")
    if status.get("resources") != {path.relative_to(base).as_posix(): sha256(path)
                                   for path in (mac, windows)}:
        raise ValueError("pre-sign resource hashes mismatch")
    native = status.get("native", {})
    if set(native) != set(TARGETS):
        raise ValueError("all native pre-sign manifests are required")
    for target in TARGETS:
        item = native[target]
        if (item.get("target") != target or item.get("commit") != commit(root)
                or item.get("source_sha256") != source_digest(root)
                or item.get("execution") != "passed"
                or not re.fullmatch(r"[a-f0-9]{64}", item.get("binary_sha256", ""))):
            raise ValueError("native execution/provenance missing: " + target)
    return status


def repack(archive, component, original, signed_names):
    current = inventory(component)
    if current.keys() != original.keys() or any(
            current[name] != original[name] for name in current if name not in signed_names):
        raise ValueError("signing changed non-code package contents")
    entries = [(component.name + "/" + name, (component / name).read_bytes(), entry["mode"])
               for name, entry in current.items()]
    write_zip(archive, entries)
    return current


def accepted_submission(output):
    status = json.loads(output)
    if status.get("status") != "Accepted" or not re.fullmatch(
            r"[a-fA-F0-9]{8}(?:-[a-fA-F0-9]{4}){3}-[a-fA-F0-9]{12}", status.get("id", "")):
        raise ValueError("notarization did not return explicit Accepted status and submission ID")
    return {"status": "Accepted", "id": status["id"]}


def notarize(archive, values, keychain):
    profile = "spooler-monitor-notary"
    command(["xcrun", "notarytool", "store-credentials", profile,
             "--apple-id", values["NOTARYTOOL_APPLE_ID"], "--team-id", values["NOTARYTOOL_TEAM_ID"],
             "--password", values["NOTARYTOOL_PASSWORD"], "--keychain", keychain])
    return accepted_submission(command(
        ["xcrun", "notarytool", "submit", archive, "--keychain-profile", profile,
         "--keychain", keychain, "--wait", "--timeout", "30m", "--output-format", "json"],
        timeout=1900))


def sign_release(root, tool4d):
    root = Path(root).resolve()
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise ValueError("signed compiled acceptance requires native macOS ARM64")
    values = credentials()
    config = load_config(root, initialized=True, repository=os.environ.get("GITHUB_REPOSITORY"))
    archive = root / "dist" / (config["repository"] + ".zip")
    report_path = archive.with_suffix(".signing.json")
    report_path.unlink(missing_ok=True)
    source_component = root / "component" / config["repository"]
    validate_compile(root, source_component, config["repository"])
    provenance = verify_assembly(root, source_component, config)
    source_report = json.loads((root / "build/source-host-status.json").read_text())
    contract = json.loads((root / "tests/host/contract.json").read_text())
    validate_status(source_report, contract, False)
    unsigned_hash = sha256(archive)
    package(root, repository=os.environ["GITHUB_REPOSITORY"], tag="v" + config["version"])
    if sha256(archive) != unsigned_hash:
        raise ValueError("unsigned ZIP did not match freshly verified compilation/package inputs")
    component = unpack_for_test(root, archive)
    before = inventory(component)
    names = code_paths(component, config["cli"])
    code = {}
    with signing_keychain(values) as (keychain, identity):
        for name in names:
            path = component / name
            print("Signing reviewed code object: " + name, flush=True)
            dependencies = inspect(path, "macos-arm64", {})
            command(["codesign", "--force", "--sign", identity, "--keychain", keychain,
                     "--options", "runtime", "--timestamp", path])
            code[name] = {"architectures": verify_code(path, values["NOTARYTOOL_TEAM_ID"]),
                          "dependencies": dependencies, "status": "passed"}
        final_inventory = repack(archive, component, before, names)
        signed_hash = sha256(archive)
        print("Submitting exact signed ZIP to Apple notarization", flush=True)
        notary = notarize(archive, values, keychain)
        print("Apple notarization Accepted: " + notary["id"], flush=True)
    if sha256(archive) != signed_hash:
        raise ValueError("ZIP changed after notarization")
    component = unpack_for_test(root, archive)
    if inventory(component) != final_inventory:
        raise ValueError("final ZIP differs from signed byte inventory")
    for name in names:
        verify_code(component / name, values["NOTARYTOOL_TEAM_ID"])
    cli = component / "Resources/bin/macOS" / config["cli"]
    assessment = command(["spctl", "--assess", "--type", "execute", "--verbose=4", cli])
    if "source=Notarized Developer ID" not in assessment:
        raise ValueError("Gatekeeper did not confirm Notarized Developer ID")
    host = test_4d(root, tool4d, component)
    (root / "build/compiled-host-status.json").write_text(json.dumps(host, indent=2) + "\n")
    if sha256(archive) != signed_hash or inventory(component) != final_inventory:
        raise ValueError("final package changed during host validation")
    report = {
        "schema": 1, "result": "partial", "finished": True, "release_ready": False,
        "commit": commit(root), "source_sha256": source_digest(root),
        "artifact": {"path": "dist/" + archive.name, "sha256": signed_hash,
                     "bytes": archive.stat().st_size},
        "pre_sign": {"archive_sha256": unsigned_hash, "assembly": provenance,
                     "compile": json.loads((root / "build/compile-status.json").read_text())},
        "signed_code": code, "files": final_inventory, "notarization": notary,
        "ticket_delivery": "Apple online lookup; ZIP, bare CLI and dylib cannot be stapled",
        "steps": {
            "source_host_macos_arm64": {"status": "passed", "report": source_report},
            "compiled_host_macos_arm64": {"status": "passed", "report": host},
            "gatekeeper_cli": {"status": "passed", "assessment": assessment.strip()},
            "stapling": {"status": "not_run", "reason": "ZIP and bare code do not support stapling"},
            "macos_x64_4d": {"status": "not_run", "reason": "runtime unavailable"},
            "windows_4d": {"status": "not_run", "reason": "runtime unavailable"},
            "published_install": {"status": "not_run", "reason": "publication not performed"},
            "live_mutations": {"status": "not_run", "reason": "not authorized"},
        },
    }
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Final signed ZIP and compiled host verified: " + signed_hash, flush=True)
    return report


def verify_report(root):
    root = Path(root)
    config = load_config(root, initialized=True)
    archive = root / "dist" / (config["repository"] + ".zip")
    report = json.loads(archive.with_suffix(".signing.json").read_text())
    if (report.get("finished") is not True or report.get("commit") != commit(root)
            or report.get("source_sha256") != source_digest(root)
            or report.get("artifact") != {"path": "dist/" + archive.name, "sha256": sha256(archive),
                                          "bytes": archive.stat().st_size}
            or report.get("notarization", {}).get("status") != "Accepted"):
        raise ValueError("final signed artifact evidence mismatch")
    for step in ("source_host_macos_arm64", "compiled_host_macos_arm64", "gatekeeper_cli"):
        if report.get("steps", {}).get(step, {}).get("status") != "passed":
            raise ValueError("required signed package check missing: " + step)
    accepted_submission(json.dumps(report["notarization"]))
    contract = json.loads((root / "tests/host/contract.json").read_text())
    for step, compiled in (("source_host_macos_arm64", False), ("compiled_host_macos_arm64", True)):
        validate_status(report["steps"][step]["report"], contract, compiled)
    required = {f"Resources/bin/macOS/{config['cli']}": {"arm64", "x86_64"},
                "Libraries/lib4d-arm64.dylib": {"arm64"}}
    if set(report.get("signed_code", {})) != set(required):
        raise ValueError("signed code evidence missing")
    for name, architectures in required.items():
        item = report["signed_code"][name]
        if item.get("status") != "passed" or set(item.get("architectures", {})) != architectures:
            raise ValueError("signed architecture evidence missing: " + name)
        for metadata in item["architectures"].values():
            if (not re.fullmatch(r"[A-Z0-9]{10}", metadata.get("team", ""))
                    or not metadata.get("timestamp") or metadata.get("hardened_runtime") is not True):
                raise ValueError("signed identity/timestamp/runtime evidence missing")
    with zipfile.ZipFile(archive) as packed:
        files = {}
        prefix = config["repository"] + ".4dbase/"
        for entry in packed.infolist():
            if entry.is_dir():
                continue
            if not entry.filename.startswith(prefix):
                raise ValueError("unexpected signed ZIP member")
            name = entry.filename[len(prefix):]
            if name in files:
                raise ValueError("duplicate signed ZIP member")
            files[name] = {"sha256": hashlib.sha256(packed.read(entry)).hexdigest(),
                           "bytes": entry.file_size, "mode": (entry.external_attr >> 16) & 0o777}
    if files != report.get("files"):
        raise ValueError("final signed inventory mismatch")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--tool4d", type=Path)
    parser.add_argument("--verify-report", action="store_true")
    args = parser.parse_args()
    try:
        if args.verify_report:
            verify_report(args.root)
        elif args.tool4d:
            sign_release(args.root, args.tool4d)
        else:
            raise ValueError("--tool4d is required for signed final-package host validation")
    except (ValueError, OSError, KeyError, TypeError, zipfile.BadZipFile) as error:
        parser.exit(1, f"signed release failed: {error}\n")


if __name__ == "__main__":
    main()
