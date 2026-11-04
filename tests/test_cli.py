"""Printer-independent native CLI contract tests; no live queue mutations."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
USAGE = (
    "expected list, get|pause|resume <queueID>, capabilities, "
    "inspect-argument <text>, or --version"
)
QUEUE = "世界 queue"


class CppCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        language = os.environ.get("TEST_CLI_LANGUAGE")
        if language == "none":
            raise unittest.SkipTest("TEST_CLI_LANGUAGE=none")
        if language not in (None, "cpp"):
            raise RuntimeError("this repository selects cpp, not " + language)
        if not shutil.which("cmake"):
            raise RuntimeError("selected C++ toolchain requires cmake")
        # Persistent ignored build directory: no external temporary files or caches.
        cls.build = ROOT / "build" / "cli-python-tests path"
        commands = [
            ["cmake", "-S", str(ROOT / "cli"), "-B", str(cls.build),
             "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON",
             *(["-A", "x64"] if os.name == "nt" else [])],
            ["cmake", "--build", str(cls.build), "--config", "Release"],
            ["ctest", "--test-dir", str(cls.build), "-C", "Release", "--output-on-failure"],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, timeout=180, cwd=ROOT)
            if result.returncode:
                raise RuntimeError(
                    repr(command) + "\n" +
                    result.stdout.decode("utf-8", errors="replace") +
                    result.stderr.decode("utf-8", errors="replace")
                )
        suffix = ".exe" if os.name == "nt" else ""
        binary_dir = cls.build / "Release" if (cls.build / "Release").is_dir() else cls.build
        # Exercise an executable filename containing spaces and Unicode as well.
        cls.executable = cls.build / ("CLI 世界 with spaces" + suffix)
        shutil.copy2(binary_dir / ("spooler-monitor" + suffix), cls.executable)
        cls.fixture = binary_dir / ("monitor_fixture" + suffix)
        cls.backend = "winspool" if os.name == "nt" else "cups"

    def run_cli(self, args, status=0, scenario=None):
        command = [str(self.executable), *args] if scenario is None else [
            str(self.fixture), scenario, *args
        ]
        result = subprocess.run(command, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, status, result.stderr)
        stream = result.stdout if status == 0 else result.stderr
        self.assertEqual(result.stderr if status == 0 else result.stdout, b"")
        self.assertTrue(stream.endswith(b"\n"))
        self.assertEqual(stream.count(b"\n"), 1)
        value = json.loads(stream.decode("utf-8", errors="strict"))
        # Enforce compact output framing, not just parseability.
        self.assertEqual(stream, (
            json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n"
        ).encode("utf-8"))
        self.assertIs(type(value["protocol"]), int)
        self.assertEqual(value["protocol"], 1)
        self.assertIs(value["ok"], status == 0)
        if status:
            error = value["error"]
            self.assertEqual(set(error), {"code", "message", "backend", "nativeCode", "outcome"})
            self.assertIsInstance(error["message"], str)
            self.assertTrue(error["message"])
            self.assertIn(error["backend"], (self.backend, None))
            self.assertTrue(error["nativeCode"] is None or type(error["nativeCode"]) is int)
            self.assertIn(error["outcome"], ("not_submitted", "rejected", "uncertain", "accepted"))
        return value

    def test_version(self):
        self.assertEqual(self.run_cli(["--version"]), {
            "protocol": 1, "ok": True, "operation": "version", "version": "0.1.0"
        })

    def test_capabilities(self):
        self.assertEqual(self.run_cli(["capabilities"]), {
            "protocol": 1, "ok": True, "operation": "capabilities", "backend": self.backend,
            "operations": ["list", "get", "pause", "resume"], "mutationPolicy": "explicit-only"
        })

    def test_argument_roundtrips(self):
        self.assertIn(" ", str(self.executable))
        values = [
            "", " ", "two words", " leading and trailing ", "世界 café 😀",
            'a"b\\c', "line\nbreak", "\t\r\b\f",
            "".join(chr(c) for c in range(1, 32)),
            "<>&/\u2028\u2029", "\u007f\u0080\u07ff\u0800\uffff\U0010ffff",
            "--version", "$(echo not-executed); `echo nope`", "x" * 16384,
        ]
        for literal in values:
            with self.subTest(literal=literal[:50]):
                self.assertEqual(self.run_cli(["inspect-argument", literal]), {
                    "protocol": 1, "ok": True, "operation": "inspect-argument", "argument": literal
                })

    def test_invalid_arguments(self):
        values = [
            [], [""], ["unknown"], ["greet", "World"], ["list", "extra"],
            ["get"], ["pause"], ["resume"], ["get", ""], ["get", "x\nb"],
            ["get", "x\tb"], ["get", "\x7f"], ["get", "\u0085"],
            ["get", "x" * 1025], ["pause", "x", "extra"],
            ["inspect-argument"], ["--version", "extra"], ["capabilities", "extra"],
            ["LIST"], ["--help"], ["inspect-argument", "x" * 16385],
        ]
        for args in values:
            with self.subTest(args=[x[:50] for x in args]):
                response = self.run_cli(args, 2)
                self.assertEqual(response, {
                    "protocol": 1, "ok": False, "operation": "arguments",
                    "error": {"code": "invalid_arguments", "message": USAGE,
                              "backend": None, "nativeCode": None, "outcome": "not_submitted"}
                })

    @unittest.skipUnless(os.name == "posix", "byte argv requires POSIX")
    def test_malformed_utf8(self):
        for value in [
            b"\x80", b"\xff", b"\xc0\xaf", b"\xc1\xbf", b"\xc2",
            b"\xe2\x82", b"\xe2(\xa1", b"\xe0\x80\x80", b"\xed\xa0\x80",
            b"\xf0\x80\x80\x80", b"\xf4\x90\x80\x80", b"\xf5\x80\x80\x80",
        ]:
            for args in ([b"inspect-argument", value], [value], [b"get", value]):
                with self.subTest(args=args):
                    self.assertEqual(self.run_cli(args, 2)["error"]["code"], "invalid_arguments")

    def assert_queue(self, value):
        self.assertEqual(set(value), {
            "id", "displayName", "backend", "state", "acceptingJobs", "jobCount",
            "reasons", "message", "observedAt", "raw"
        })
        self.assertEqual(value["id"], QUEUE)
        self.assertEqual(value["backend"], self.backend)
        self.assertEqual(value["displayName"], 'Mock "printer"')
        self.assertIn(value["state"], ("idle", "processing", "stopped", "unknown"))
        self.assertIs(value["acceptingJobs"], True)
        self.assertEqual(value["jobCount"], 2)
        self.assertRegex(value["observedAt"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")
        self.assertEqual(value["raw"], {"fixture": True})

    def test_mock_list_get(self):
        self.assertEqual(self.run_cli(["list"], scenario="empty")["queues"], [])
        response = self.run_cli(["list"], scenario="one")
        self.assertEqual(len(response["queues"]), 1)
        self.assert_queue(response["queues"][0])
        response = self.run_cli(["get", QUEUE], scenario="one")
        self.assert_queue(response["queue"])
        self.assertNotIn("mutation", response)

    def test_mock_explicit_mutations(self):
        for op, state in (("pause", "stopped"), ("resume", "idle")):
            response = self.run_cli([op, QUEUE], scenario="one")
            self.assertEqual(response["operation"], op)
            self.assertEqual(response["mutation"], {
                "accepted": True, "outcome": "accepted", "observation": "succeeded", "error": None
            })
            self.assert_queue(response["queue"])
            self.assertEqual(response["queue"]["state"], state)

    def test_mock_failures(self):
        for scenario, status, code in [
            ("denied", 4, "access_denied"), ("unavailable", 5, "backend_unavailable"),
            ("unsupported", 6, "unsupported_operation"), ("native-error", 8, "native_error")
        ]:
            response = self.run_cli(["pause", QUEUE], status, scenario)
            self.assertEqual(response["error"]["code"], code)
            self.assertIs(response["mutation"]["accepted"], False)
            self.assertEqual(response["error"]["outcome"],
                             "not_submitted" if scenario == "unavailable" else "rejected")
        response = self.run_cli(["get", QUEUE], 3, "get-failed")
        self.assertEqual(response["error"]["code"], "unknown_queue")
        self.assertNotIn("mutation", response)
        for op in ("get", "pause", "resume"):
            response = self.run_cli([op, "\\\\arbitrary-server\\queue"], 3, "one")
            self.assertEqual(response["error"]["outcome"], "not_submitted")

    def test_mock_accepted_observation_failure(self):
        for scenario in ("observation-failed", "unexpected"):
            response = self.run_cli(["pause", QUEUE], 7, scenario)
            self.assertEqual(response["error"]["code"], "observation_failed")
            self.assertEqual(response["error"]["outcome"], "accepted")
            self.assertIs(response["queue"], None)
            self.assertEqual(response["mutation"], {
                "accepted": True, "outcome": "accepted", "observation": "failed",
                "error": response["error"]
            })

    def test_mock_transport_uncertainty(self):
        response = self.run_cli(["pause", QUEUE], 5, "transport")
        self.assertEqual(response["error"]["outcome"], "uncertain")
        self.assertEqual(response["mutation"]["outcome"], "uncertain")
        self.assertNotIn("queue", response)

    def test_registered_contract(self):
        contract = json.loads((ROOT / "tests" / "cli-contract.json").read_text(encoding="utf-8"))
        self.assertEqual(contract["schema"], 1)
        for case in contract["cases"]:
            with self.subTest(args=case["args"]):
                result = subprocess.run([str(self.executable), *case["args"]],
                                        capture_output=True, timeout=10)
                self.assertEqual(result.returncode, case["exitCode"])
                for stream in ("stdout", "stderr"):
                    expected = case[stream]
                    if isinstance(expected, dict):
                        expected = json.dumps(expected, ensure_ascii=False, separators=(",", ":"))
                        expected = expected.replace("$VERSION", "0.1.0") + "\n"
                    self.assertEqual(getattr(result, stream), expected.encode("utf-8"))


if __name__ == "__main__":
    unittest.main()
