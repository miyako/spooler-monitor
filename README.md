# spooler-monitor

A real **4D component** for reading printer queue state and explicitly pausing or
resuming queue processing on **macOS and Windows**. A bundled C++17 executable
uses system libcups/IPP on macOS and Winspool on Windows. It does not submit
documents, restart a daemon/service, purge jobs, release held jobs, or silently
auto-resume queues.

| Identity | Value |
| --- | --- |
| Repository, project, component, catalog, package | `spooler-monitor` |
| Independent 4D namespace / public class | `SpoolerMonitor` / `Queue` |
| Host constructor | `cs.SpoolerMonitor.Queue.new(target)` |
| Native executable / language | `spooler-monitor` / C++17 |
| Targets | macOS 11+ universal arm64/x86_64; Windows x64 |
| 4D | Project compatibility 2060; pinned free tool4d 20.7 build 101905 |

This project was adopted from
[miyako/4d-component-template](https://github.com/miyako/4d-component-template).
The template's [AGENTS.md](AGENTS.md) retains the detailed build/acceptance
playbook; template adoption examples are not this component's feature contract.

## 4D API

```4d
var $queues : cs.SpoolerMonitor.Queue
var $result : Object
var $target : Text
$target:="macOS-universal"
If (Is Windows)
    $target:="Windows-x64"
End if
$queues:=cs.SpoolerMonitor.Queue.new($target)
$result:=$queues.list()
If ($result.ok)
    // Keep id separately from displayName; do not identify a queue by its label.
    For each ($queue; $result.value.queues)
        LOG EVENT($queue.id+": "+$queue.state; Into standard outputs)
    End for each
Else
    LOG EVENT($result.error.code+": "+$result.error.message; Into standard errors)
End if
```

`list()`, `get(queueID)`, `pause(queueID)` and `resume(queueID)` return an object
with `ok`, preserved process `stdout`/`stderr`/`exitCode` when available, and
decoded `value` on success or structured `error` on failure.
`version()` and `capabilities()` are harmless diagnostics.
See the [Queue class contract](component/spooler-monitor/Documentation/Classes/Queue.md)
and [native protocol](cli/README.md) for precise fields and error behavior.

Calls are synchronous with a finite **30-second isolated-process timeout**.
Winspool and CUPS calls may block; **do not call this API in the UI process**.
Run it in a live dedicated `CALL WORKER` worker on the **printing 4D Client**,
not the server. Keep that worker alive to receive work/results. Use per-call
state and deliver results back to the UI with worker messaging; no shared PID
map is needed. A worker method can create its own Queue instance and execute
one `get`/`resume` request. Do not repeatedly create workers without terminating
or reusing them.

`resume` means enable **processing**, not accept new jobs. Native command
acceptance is distinct from the subsequent state snapshot: an accepted resume
can immediately stop again. Check `mutation.accepted`, observation/error and
the new queue state. A timeout after submission has **uncertain outcome**;
query again before deciding whether to retry. No response proves paper was
printed, and a queue's normalized state is not a physical-printer health check.

## Queue state and platform boundaries

Every queue carries a native `id`, separate `displayName`, backend, normalized
`state` (`idle`, `processing`, `stopped`, `unknown`), `acceptingJobs`, `jobCount`,
`reasons`, `message`, UTC `observedAt` and backend `raw` fields. Missing or
unsupported fields are `null`, never silently converted to healthy values.
Windows has no reliable equivalent of CUPS job acceptance, so `acceptingJobs`
is `null`. Preserve the Windows status bitmask/flag map: flags coexist and
cannot be faithfully represented by one state string.

macOS talks only to the **local CUPS scheduler**, ignoring environment-selected
remote servers. It lists configured printers using CUPS-Get-Printers, not
Bonjour-discovered devices or printer classes. Queue URIs are constructed and
encoded locally. Pause-Printer and Resume-Printer use `/admin/`; Enable-Printer
is not substituted. No shell, sudo, credentials, password prompts, automatic
policy changes or privilege bypass is used. Administrative actions can fail
with `access_denied` under the caller's CUPS policy.

Windows enumerates locally installed printers/connections, resolves a queue ID
against that enumeration, and uses `OpenPrinterW` / `GetPrinterW` /
`SetPrinterW(PRINTER_CONTROL_PAUSE|RESUME)`. Printer administration permission
is requested for mutations. Configured remote connections may work under the
current Windows identity; v1 does not accept an arbitrary server endpoint.
Native Windows ARM64 is not supported.

IDs are scheduler/spooler identifiers, not immutable hardware identifiers:
renaming/recreating a queue can change them. Snapshots are not atomic across
queues and concurrent external administrators can change state between calls.

## Opt-in application policy

There is **no hidden watchdog**. A dedicated shipping client may explicitly
implement one: query a selected queue, require operator-approved recovery
policy, resume only an observed stopped queue, then re-query. Limit attempts
(for example three), use increasing delays (5, 15, 60 seconds), record both
native acceptance and observed state, and stop/escalate on authorization,
unknown queue, uncertain timeout or repeated stoppage. Never purge/restart as
an automatic fallback. Do not mistake `acceptingJobs:true` for processing.

## CLI

```text
spooler-monitor list
spooler-monitor get <queueID>
spooler-monitor pause <queueID>
spooler-monitor resume <queueID>
spooler-monitor capabilities
spooler-monitor --version
```

Arguments are literal UTF-8/UTF-16 data, never shell commands. Protocol 1 emits
one compact UTF-8 JSON object plus LF: success on stdout with exit 0; structured
failure on stderr with nonzero status. `inspect-argument <text>` is a harmless
literal round-trip diagnostic used by native 4D quoting tests.

| Exit | Stable error |
| --- | --- |
| 2 | `invalid_arguments` |
| 3 | `unknown_queue` |
| 4 | `access_denied` |
| 5 | `backend_unavailable` |
| 6 | `unsupported_operation` |
| 7 | `observation_failed` after native acceptance |
| 8 | `native_error` |

The 4D wrapper additionally reports argument, resource, launch, process timeout
and malformed-protocol errors. Native status, outcome and diagnostic streams
are preserved separately rather than flattened into a success fallback.

## Build and safe validation

Python 3.10+, CMake and a native C++17 compiler are required. No package manager
dependencies are downloaded for the helper. macOS links system libcups and
libc++; Windows links system Winspool and the static release Microsoft CRT.
Redistribution/system-runtime notes are in
[Resources/licenses](component/spooler-monitor/Resources/licenses).
The component is MIT licensed; retain [LICENSE](LICENSE).

```sh
python3 scripts/doctor.py --root .
python3 -m unittest discover -s tests -v
python3 scripts/generate_icons.py --check
python3 scripts/build_cli.py --root . --target macos-arm64
```

Use the native target `macos-arm64`, `macos-x64` or `windows-x64`.
CMake/CTest exercises injected backend behavior; Python tests exercise exact
CLI framing, Unicode, argument errors and harmless diagnostics. Tests never
pause/resume real printer queues, add printers, or change system configuration.
Read-only real enumeration can return a clear scheduler-unavailable error when
no scheduler is running. Mock results do not prove physical printing or
administrative permissions. Live mutations require a separately authorized,
disposable printer environment.

CI builds/tests all three native targets from one revision and uploads each
binary with `build-manifest.json`. Manifests bind exact source bytes, commit,
compiler identity and binary SHA256. Restore all three native CI artifact
directories under `build/` before local assembly; C++ cross mode is unsupported.
Native builds audit direct imports and reject unbundled non-system libraries.
Do not combine stale or mixed-source artifacts. Hosted runner images service
Xcode/MSVC patch versions; the actual configured compiler version is recorded,
not falsely described as an immutable compiler pin.

```sh
python3 scripts/ci_config.py download-tool4d --root .
python3 scripts/accept_local.py --root . --repository miyako/spooler-monitor \
  --tool4d .tools/tool4d.app/Contents/MacOS/tool4d
```

The downloader uses the fixed public URL/SHA256 in `package.json.tool`, accepts
only HTTPS redirects between `resources-download.4d.com` and the official
`builds2.4d.com` CDN, checks the pinned digest before extraction, and rejects
unsafe archive paths/symlinks. Free local unsigned compilation needs no vendor
activation, signing secrets or account changes.

Acceptance requires real source-host calls, fresh real 4D compilation,
source-stripped packaging, and loading the **exact unpacked ZIP** with compiled
mode true. The pair is `dist/spooler-monitor.zip` and
`dist/spooler-monitor.acceptance.json`; latest per-stage reports are in `build/`.
`passed`, `failed` and `not_run` are distinct. Local Apple Silicon validation
alone is **partial**, with `release_ready:false`; it is not native Intel/Windows
4D, dependency-manager download, signing or physical printing evidence.

Ordinary CI does not publish. The inherited release workflow requires an
authorized matching existing `v<version>` tag, explicit hosted 4D opt-in and
real final-package host checks. Do not create tags or publish as a build step.

## Installation

For an approved published version, add GitHub dependency
`miyako/spooler-monitor` through 4D's dependency manager (20 R6 or compatible
current LTS). Its asset must be exactly **`spooler-monitor.zip`**, containing
`spooler-monitor.4dbase/spooler-monitor.4DZ`. Namespace remains independently
`SpoolerMonitor`. For unsigned local testing, unpack the verified ZIP and copy
`spooler-monitor.4dbase` into the host's `Components/` folder. This is not
evidence of an actual published dependency-manager install.

## Backend references

[CUPS programming](https://openprinting.github.io/cups/doc/cupspm.html),
[CUPS IPP extensions](https://openprinting.github.io/cups/doc/spec-ipp.html),
[Microsoft SetPrinter](https://learn.microsoft.com/en-us/windows/win32/printdocs/setprinter).
