"""Fail-closed signing contracts. Mocked tools never replace hosted acceptance."""
import base64
from contextlib import ExitStack, nullcontext
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import sign_release as signing
import native_dependencies
from build_provenance import sha256, source_digest
import test_delivery

TEAM = "AB12345678"
FINGERPRINT = "A" * 40
IDENTITY = f'  1) {FINGERPRINT} "Developer ID Application: Example ({TEAM})"\n'
METADATA = (f"Authority=Developer ID Application: Example ({TEAM})\n"
            f"TeamIdentifier={TEAM}\nTimestamp=Nov 4, 2026 at 1:00:00 PM\n"
            "CodeDirectory v=20500 flags=0x10000(runtime)\nCDHash=" + "b" * 40 + "\n")
NOTARY = {"id": "01234567-89ab-cdef-0123-456789abcdef", "status": "Accepted"}
VALUES = {name: "private-test-value" for name in signing.SECRETS}
VALUES.update(APPLE_DEVELOPER_ID_CERTIFICATE=base64.b64encode(b"fake fixture p12").decode(),
              NOTARYTOOL_TEAM_ID=TEAM)


def notary_log(filename, digest, package="example-component.4dbase", cli="example-cli"):
    return {"jobId": NOTARY["id"], "status": "Accepted", "statusCode": 0,
            "archiveFilename": filename, "sha256": digest, "issues": None,
            "ticketContents": [
                {"path": filename + "/" + package + "/" + name,
                 "arch": arch, "digestAlgorithm": "SHA-256", "cdhash": "b" * 40}
                for name, architectures in ((f"Resources/bin/macOS/{cli}", ("arm64", "x86_64")),
                                            ("Libraries/lib4d-arm64.dylib", ("arm64",)))
                for arch in architectures]}


class SigningTests(unittest.TestCase):
    def test_require_all_six_credentials_and_team(self):
        with patch.dict(os.environ, VALUES, clear=True):
            self.assertEqual(signing.credentials(), VALUES)
        for name in signing.SECRETS:
            values = {key: value for key, value in VALUES.items() if key != name}
            with self.subTest(name=name), patch.dict(os.environ, values, clear=True):
                with self.assertRaisesRegex(ValueError, "missing signing secrets"):
                    signing.credentials()
        with patch.dict(os.environ, {**VALUES, "NOTARYTOOL_TEAM_ID": "invalid"}, clear=True):
            with self.assertRaisesRegex(ValueError, "invalid NOTARYTOOL_TEAM_ID"):
                signing.credentials()

    def test_exact_imported_identity_not_ad_hoc_installer_ambiguous_or_wrong_team(self):
        self.assertEqual(signing.select_identity(IDENTITY, TEAM), FINGERPRINT)
        for output in ("", "0 valid identities found", IDENTITY + IDENTITY,
                       IDENTITY.replace("Application:", "Installer:"), IDENTITY.rstrip() + " (CSSMERR)\n"):
            with self.subTest(output=output), self.assertRaises(ValueError):
                signing.select_identity(output, TEAM)
        with self.assertRaisesRegex(ValueError, "team mismatch"):
            signing.select_identity(IDENTITY, "ZZ12345678")

    def test_signature_requires_team_authority_timestamp_runtime(self):
        self.assertTrue(signing.signature_metadata(METADATA, TEAM)["hardened_runtime"])
        for output in (METADATA.replace(TEAM, "ZZ12345678"),
                       METADATA.replace("Authority=Developer ID Application:", "Authority=Other:"),
                       METADATA.replace("Timestamp=", "Signed Time="),
                       METADATA.replace("Nov 4, 2026 at 1:00:00 PM", "none"),
                       METADATA.replace("CDHash=", "Missing="),
                       METADATA.replace("(runtime)", "")):
            with self.subTest(output=output), self.assertRaises(ValueError):
                signing.signature_metadata(output, TEAM)

    def test_verify_every_architecture_and_apple_developer_id_requirement(self):
        calls = []

        def command(args):
            calls.append(args)
            if args[0] == "lipo":
                return "x86_64 arm64\n"
            return METADATA

        with patch.object(signing, "command", side_effect=command):
            result = signing.verify_code(Path("tool"), TEAM, notarized=True)
        self.assertEqual(set(result), {"arm64", "x86_64"})
        self.assertIn("--all-architectures", calls[0])
        self.assertIn("--strict", calls[0])
        self.assertIn("--check-notarization", calls[0])
        self.assertIn("anchor apple generic", calls[0][-2])
        self.assertEqual([call[-2] for call in calls[2:]], ["x86_64", "arm64"])

    def test_command_failures_never_expose_credential_arguments_or_diagnostics(self):
        result = subprocess.CompletedProcess([], 1, "secret stdout", "secret stderr")
        for error in (result, subprocess.TimeoutExpired(["secret"], 1), OSError("private")):
            with patch.object(signing.subprocess, "run") as run:
                if isinstance(error, subprocess.CompletedProcess):
                    run.return_value = error
                else:
                    run.side_effect = error
                with self.assertRaises(ValueError) as raised:
                    signing.command(["security", "private-password"])
                self.assertNotIn("private", str(raised.exception))
                self.assertNotIn("secret", str(raised.exception))

    def test_keychain_cleanup_on_success_import_identity_and_body_failures(self):
        for failure in (None, "import", "set-key-partition-list", "find-identity", "body"):
            calls = []
            paths = []

            def command(args):
                calls.append(args)
                if args[1] == "list-keychains" and "-s" not in args:
                    return '"/Users/fixture/Library/Keychains/login.keychain-db"\n'
                if args[1] == "import":
                    path = args[2]
                    paths.append(path)
                    if os.name != "nt":
                        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                    self.assertEqual(path.read_bytes(), b"fake fixture p12")
                if args[1] == failure:
                    raise ValueError("injected command failure")
                return IDENTITY if args[1] == "find-identity" else ""

            with self.subTest(failure=failure), patch.object(signing, "command", side_effect=command):
                try:
                    with signing.signing_keychain(VALUES) as (_, identity):
                        self.assertEqual(identity, FINGERPRINT)
                        if failure == "body":
                            raise ValueError("injected body failure")
                except ValueError:
                    self.assertIsNotNone(failure)
            self.assertEqual(calls[-1][1], "delete-keychain")
            self.assertEqual(calls[-2][1:5], ["list-keychains", "-d", "user", "-s"])
            self.assertTrue(all(not path.parent.exists() for path in paths))

    def test_cleanup_deletes_keychain_even_if_search_list_restore_fails(self):
        calls = []

        def command(args):
            calls.append(args)
            if args[1] == "list-keychains" and "-s" in args and len(args) == 5:
                raise ValueError("restore failed")
            return IDENTITY if args[1] == "find-identity" else ""

        with patch.object(signing, "command", side_effect=command), self.assertRaisesRegex(ValueError, "restore"):
            with signing.signing_keychain(VALUES):
                pass
        self.assertEqual(calls[-1][1], "delete-keychain")

    def test_invalid_certificate_never_imports_and_temp_files_removed(self):
        with patch.object(signing, "command", return_value="") as command:
            with self.assertRaisesRegex(ValueError, "base64"):
                with signing.signing_keychain({**VALUES, "APPLE_DEVELOPER_ID_CERTIFICATE": "!"}):
                    self.fail("invalid certificate was accepted")
        self.assertFalse(any(call.args[0][1] == "import" for call in command.call_args_list))

    def test_notary_requires_explicit_accepted_and_valid_id(self):
        self.assertEqual(signing.accepted_submission(json.dumps(NOTARY)), NOTARY)
        for value in ({}, {**NOTARY, "status": "Invalid"}, {**NOTARY, "status": "In Progress"},
                      {**NOTARY, "id": "-" * 36}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                signing.accepted_submission(json.dumps(value))

    def test_notary_uses_isolated_keychain_and_fails_on_rejection(self):
        log = notary_log("archive.zip", "a" * 64)
        with patch.object(signing, "command", side_effect=[
                "", json.dumps(NOTARY), json.dumps(log)]) as command, patch.object(
                signing, "sha256", return_value="a" * 64):
            self.assertEqual(signing.notarize(Path("archive.zip"), VALUES, Path("private.keychain")),
                             {**NOTARY, "log": log})
        submission = command.call_args_list[1].args[0]
        self.assertIn("--wait", submission)
        self.assertIn("--keychain", submission)
        self.assertEqual(submission[-3:], ["30m", "--output-format", "json"])
        self.assertEqual(command.call_args.args[0][1:3], ["notarytool", "log"])
        with patch.object(signing, "command", side_effect=["", '{"status":"Invalid"}']):
            with self.assertRaisesRegex(ValueError, "Accepted"):
                signing.notarize(Path("archive.zip"), VALUES, Path("private.keychain"))

    def test_notary_log_binds_job_status_archive_and_exact_signed_bytes(self):
        log = notary_log("archive.zip", "a" * 64)
        signing.validate_notary_log(log, NOTARY, "archive.zip", "a" * 64)
        for field in ("jobId", "status", "statusCode", "archiveFilename", "sha256", "ticketContents", "issues"):
            altered = {**log, field: "wrong"}
            with self.subTest(field=field), self.assertRaises(ValueError):
                signing.validate_notary_log(altered, NOTARY, "archive.zip", "a" * 64)
        code = {"Resources/bin/macOS/example-cli": {
            "architectures": {arch: {"cdhash": "b" * 40} for arch in ("arm64", "x86_64")}}}
        signing.verify_tickets(log, code, "example-component.4dbase")
        for field in ("arch", "digestAlgorithm", "cdhash", "path"):
            altered = json.loads(json.dumps(log))
            altered["ticketContents"][0][field] = "wrong"
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "ticket"):
                signing.verify_tickets(altered, code, "example-component.4dbase")

    def test_only_exact_spctl_not_an_app_diagnostic_is_unsupported(self):
        path = Path("bare-tool")
        expected = str(path) + ": rejected (the code is valid but does not seem to be an app)"
        with patch.object(signing, "command", side_effect=signing.ToolFailure("spctl", 3, expected)):
            self.assertEqual(signing.assess_cli(path)["status"], "not_run")
        for code, diagnostic in ((1, expected), (3, "rejected"), (3, expected + "\nother error")):
            with patch.object(signing, "command", side_effect=signing.ToolFailure("spctl", code, diagnostic)):
                with self.assertRaises(signing.ToolFailure):
                    signing.assess_cli(path)
        with patch.object(signing, "command", return_value="accepted\nsource=Developer ID"):
            with self.assertRaises(ValueError):
                signing.assess_cli(path)

    def test_publish_signs_verifies_and_releases_in_order(self):
        workflow = (ROOT / ".github/workflows/publish.yml").read_text()
        self.assertNotIn("pull_request:", workflow)
        self.assertNotIn("tags:", workflow)
        self.assertNotIn("continue-on-error", workflow)
        self.assertNotIn("CODESIGN_APPLE_ID", workflow)
        for name in signing.SECRETS:
            self.assertEqual(workflow.count("${{ secrets." + name + " }}"), 1)
        publish = workflow.split("  publish:", 1)[1]
        self.assertLess(publish.index("compile_component.py"), publish.index("sign_release.py --tool4d"))
        self.assertLess(publish.index("sign_release.py --tool4d"), publish.index("sign_release.py --verify-report"))
        self.assertLess(publish.index("sign_release.py --verify-report"), publish.index("gh release create"))

    def test_dylib_own_install_name_is_not_a_runtime_dependency(self):
        dependency = ("library.dylib:\n"
                      "\t@rpath/library.dylib (compatibility version 0.0.0, current version 0.0.0)\n"
                      "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1.0.0)\n")
        for extra in ("", "\t@rpath/unbundled.dylib (compatibility version 0.0.0)\n"):
            outputs = [subprocess.CompletedProcess([], 0, stdout=dependency + extra),
                       subprocess.CompletedProcess([], 0, stdout="library.dylib:\n@rpath/library.dylib\n")]
            with patch.object(native_dependencies.subprocess, "run", side_effect=outputs):
                if extra:
                    with self.assertRaisesRegex(ValueError, "non-system"):
                        native_dependencies.inspect(Path("library.dylib"), "macos-arm64", {})
                else:
                    self.assertEqual(native_dependencies.inspect(
                        Path("library.dylib"), "macos-arm64", {}), ["/usr/lib/libSystem.B.dylib"])


class SignedPackageTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_delivery.DeliveryTests(methodName="runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.component = self.fixture.component
        self.config = self.fixture.config
        (self.component / "Libraries/lib4d-arm64.dylib").write_bytes(b"\xcf\xfa\xed\xfe" + b"fixture")
        digest = source_digest(self.root)
        self.provenance = {
            "schema": 1, "phase": "pre-sign", "commit": None, "source_sha256": digest,
            "resources": {path.relative_to(self.component / "Resources/bin").as_posix(): sha256(path)
                          for path in (self.fixture.mac, self.fixture.windows)},
            "native": {target: {"target": target, "commit": None, "source_sha256": digest,
                                "execution": "passed", "binary_sha256": "a" * 64}
                       for target in signing.TARGETS}}
        self.manifest = self.component / "Resources/bin/assembly-manifest.json"
        self.manifest.write_text(json.dumps(self.provenance))
        self.fixture.write_status()
        self.archive = self.fixture.package()

    def test_pre_sign_provenance_stays_strict(self):
        self.assertEqual(signing.verify_assembly(self.root, self.component, self.config), self.provenance)
        for field in ("commit", "source_sha256", "resources"):
            modified = {**self.provenance, field: "wrong"}
            self.manifest.write_text(json.dumps(modified))
            with self.assertRaises(ValueError):
                signing.verify_assembly(self.root, self.component, self.config)
        for target in signing.TARGETS:
            modified = json.loads(json.dumps(self.provenance))
            modified["native"][target]["execution"] = "not_run"
            self.manifest.write_text(json.dumps(modified))
            with self.assertRaisesRegex(ValueError, "native execution"):
                signing.verify_assembly(self.root, self.component, self.config)

    def test_repack_changes_only_reviewed_code_preserves_windows_and_4dz(self):
        component = signing.unpack_for_test(self.root, self.archive)
        original = signing.inventory(component)
        paths = signing.code_paths(component, self.config["cli"])
        self.assertTrue(paths[0].startswith("Libraries/"))
        for name in paths:
            path = component / name
            path.write_bytes(path.read_bytes() + b"signed fixture")
        after = signing.repack(self.archive, component, original, paths)
        self.assertNotEqual(after[paths[0]], original[paths[0]])
        for name in original.keys() - set(paths):
            self.assertEqual(original[name], after[name])
        unpacked = signing.unpack_for_test(self.root, self.archive)
        self.assertEqual(signing.inventory(unpacked), after)
        with zipfile.ZipFile(self.archive) as packed:
            self.assertIn(component.name + "/Libraries/", packed.namelist())
        (unpacked / "Resources/logo.png").write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "non-code"):
            signing.repack(self.archive, unpacked, after, paths)

    def test_unreviewed_native_code_fails_closed(self):
        component = signing.unpack_for_test(self.root, self.archive)
        (component / "Libraries/extra.dylib").write_bytes(b"\xcf\xfa\xed\xfe")
        with self.assertRaisesRegex(ValueError, "unreviewed"):
            signing.code_paths(component, self.config["cli"])

    def run_pipeline(self, failure=None, unsupported_spctl=False):
        self.fixture.package()
        (self.root / "build/source-host-status.json").write_text('{"success":true}')
        contract = self.root / "tests/host/contract.json"
        contract.parent.mkdir(parents=True, exist_ok=True)
        contract.write_text("{}")
        report_path = self.archive.with_suffix(".signing.json")
        report_path.write_text('{"stale":true}')
        compile_bytes = self.fixture.marker.read_bytes()
        manifest_bytes = self.manifest.read_bytes()
        unsigned_hash = sha256(self.archive)

        def command(args):
            self.assertFalse(any(name in os.environ for name in signing.SECRETS))
            if args[0] == "codesign" and "--sign" in args:
                if failure == "codesign":
                    raise ValueError("codesign failed")
                path = Path(args[-1])
                self.assertIn("--timestamp", args)
                self.assertIn("runtime", args)
                self.assertNotIn("--deep", args)
                path.write_bytes(path.read_bytes() + b"signed fixture")
            if args[0] == "spctl":
                if failure == "gatekeeper":
                    raise ValueError("spctl failed")
                if unsupported_spctl:
                    raise signing.ToolFailure("spctl", 3, str(args[-1])
                        + ": rejected (the code is valid but does not seem to be an app)")
                return "accepted\nsource=Notarized Developer ID"
            return ""

        def verify(path, team, *, notarized=False):
            if notarized and failure == "online_ticket":
                raise ValueError("online ticket unavailable")
            architectures = {"arm64"} if path.suffix == ".dylib" else {"arm64", "x86_64"}
            return {arch: {"team": team, "timestamp": "Nov 4, 2026", "hardened_runtime": True,
                           "cdhash": "b" * 40}
                    for arch in architectures}

        def notarize(archive, values, keychain):
            if failure == "notary":
                raise ValueError("notary Invalid")
            return {**NOTARY, "log": notary_log(archive.name, sha256(archive))}

        with ExitStack() as stack:
            stack.enter_context(patch.dict(os.environ, {
                **VALUES, "GITHUB_REPOSITORY": "owner/example-component"}, clear=True))
            stack.enter_context(patch.object(signing.platform, "system", return_value="Darwin"))
            stack.enter_context(patch.object(signing.platform, "machine", return_value="arm64"))
            stack.enter_context(patch.object(signing, "signing_keychain",
                                             return_value=nullcontext((Path("isolated"), FINGERPRINT))))
            stack.enter_context(patch.object(signing, "command", side_effect=command))
            stack.enter_context(patch.object(signing, "verify_code", side_effect=verify))
            stack.enter_context(patch.object(signing, "inspect", return_value=["/usr/lib/libSystem.B.dylib"]))
            stack.enter_context(patch.object(signing, "validate_status"))
            stack.enter_context(patch.object(signing, "notarize", side_effect=notarize))
            host = stack.enter_context(patch.object(signing, "test_4d",
                **({"side_effect": ValueError("host failed")} if failure == "host"
                   else {"return_value": {"success": True, "finished": True, "compiled": True}})))
            if failure:
                with self.assertRaises(ValueError):
                    signing.sign_release(self.root, Path("tool4d"))
                self.assertFalse(report_path.exists())
                if failure != "host":
                    host.assert_not_called()
                return
            result = signing.sign_release(self.root, Path("tool4d"))
            self.assertEqual(signing.verify_report(self.root), result)
            self.assertEqual(self.fixture.marker.read_bytes(), compile_bytes)
            self.assertEqual(self.manifest.read_bytes(), manifest_bytes)
            self.assertEqual(result["pre_sign"]["archive_sha256"], unsigned_hash)
            self.assertNotEqual(result["artifact"]["sha256"], unsigned_hash)
            self.assertEqual(result["artifact"]["sha256"], sha256(self.archive))
            self.assertFalse(result["release_ready"])
            self.assertTrue(result["finished"])
            self.assertEqual(result["steps"]["gatekeeper_cli"]["status"],
                             "not_run" if unsupported_spctl else "passed")
            host.assert_called_once()
            component = host.call_args.args[2]
            self.assertEqual(signing.inventory(component), result["files"])
            for field in ("artifact", "notarization", "signed_code", "files"):
                tampered = {**result, field: {}}
                report_path.write_text(json.dumps(tampered))
                with self.subTest(field=field), self.assertRaises(ValueError):
                    signing.verify_report(self.root)

    def test_pipeline_separates_original_provenance_and_final_signed_hashes(self):
        self.run_pipeline()

    def test_bare_cli_spctl_is_not_run_but_online_tickets_and_real_host_are_required(self):
        self.run_pipeline(unsupported_spctl=True)

    def test_no_success_report_on_codesign_notary_gatekeeper_or_final_host_failure(self):
        for failure in ("codesign", "notary", "online_ticket", "gatekeeper", "host"):
            with self.subTest(failure=failure):
                self.run_pipeline(failure)


if __name__ == "__main__":
    unittest.main()
