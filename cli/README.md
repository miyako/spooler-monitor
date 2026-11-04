# spooler-monitor native CLI

C++17, component/CLI version **0.1.0**, output protocol **1**. macOS 11+
uses the system libcups; Windows x64 uses Winspool and the static MSVC CRT.
There are no third-party source dependencies, JSON parser, shell invocations,
runtime downloads, credential prompts, or implicit queue modifications.
System libraries are linked, not copied into the component; platform/runtime
redistribution review remains part of final packaging.

## Commands and framing

```
spooler-monitor list
spooler-monitor get <queueID>
spooler-monitor pause <queueID>
spooler-monitor resume <queueID>
spooler-monitor capabilities
spooler-monitor --version
spooler-monitor inspect-argument <literal>
```

Only pause/resume mutate. No command changes job acceptance, cancels/deletes
jobs, adds queues, or prints. Queue IDs are case-sensitive exact strings from
`list`, not display names. An ID is stable as the native configured queue name,
not an immutable UUID: deletion/rename/recreation can change its identity.
Concurrent administrators may change a queue between enumeration, submission
and observation; no transaction, rollback, or exclusive access is promised.

Success: exit 0, empty stderr, exactly one compact UTF-8 JSON object plus LF
on stdout. Every success starts with `protocol:1,ok:true,operation:<command>`.
`--version` uses operation `version` and adds `version:"0.1.0"`.
`capabilities` adds `backend:"cups"|"winspool"`,
`operations:["list","get","pause","resume"]`, `mutationPolicy:"explicit-only"`.
These describe the API, **not** per-printer permissions or guaranteed server
support. `inspect-argument` adds `argument` containing the exact input text and
never initializes a backend. Empty diagnostic text is allowed, as are literal
newlines, quotes, Unicode and shell metacharacters (escaped only for JSON).

`list` adds `queues:[]` (possibly empty); `get` adds `queue:{...}`.
Pause/resume add both:

```json
{"mutation":{"accepted":true,"outcome":"accepted","observation":"succeeded","error":null},"queue":{"id":"example","displayName":"Example","backend":"cups","state":"stopped","acceptingJobs":true,"jobCount":0,"reasons":["paused"],"message":"","observedAt":"2026-11-04T00:00:00Z","raw":{}}}
```

That snippet shows the added fields, not a complete response. Acceptance means
the native API accepted the request; it does not guarantee lasting state.
The observation is a subsequent query, and may already show a different state.
Pause stops queue processing, **not** job submission; resume restarts processing.

If submission succeeds but the subsequent query or serialization fails, exit
**7** with empty stdout and a stderr error envelope containing
`mutation:{accepted:true,outcome:"accepted",observation:"failed",error:<structured error>}`
and `queue:null`. Do not retry blindly, discard those fields, or report rejection.

## Failure contract

Failures have empty stdout and one compact JSON+LF on stderr:

```json
{"protocol":1,"ok":false,"operation":"get","error":{"code":"unknown_queue","message":"queue is not locally configured","backend":"cups","nativeCode":null,"outcome":"not_submitted"}}
```

All five error fields are present. `backend` is null for argument errors;
`nativeCode` is the integer IPP status / Windows error when available, otherwise
null. Human messages are descriptive, not stable identifiers.

| Exit | Stable error code |
|---|---|
| 2 | `invalid_arguments` |
| 3 | `unknown_queue` |
| 4 | `access_denied` |
| 5 | `backend_unavailable` |
| 6 | `unsupported_operation` |
| 7 | `observation_failed` |
| 8 | `native_error` |

Argument failures use operation `arguments` and the exact message:

```
expected list, get|pause|resume <queueID>, capabilities, inspect-argument <text>, or --version
```

Mutation errors additionally include `mutation` with `accepted`, `outcome`,
`observation:"failed"` and `error`. `outcome` distinguishes:

* `not_submitted`: failure during preflight or before native command submission
  (for example opening an administrative handle or connecting to the scheduler).
* `rejected`: the native backend explicitly rejected the mutation (`accepted:false`).
* `uncertain`: transport/native failure without a reliable acknowledgement.
  `accepted:false` in this case means acceptance was **not confirmed**, not that
  rejection or absence of side effects was established.
* `accepted`: mutation acknowledged, but observation failed (`accepted:true`,
  exit 7 and `queue:null`).

A caller-enforced process timeout/termination may have **no complete JSON**
response and must be treated as unknown outcome. Never roll back automatically.
Calls are synchronous, one request per process; there is no shared mutable
process map. The 4D process adapter's finite 30-second timeout remains necessary,
especially for synchronous Windows RPC. The CLI itself has no portable
whole-process deadline.

## Queue schema and normalization

All queue fields are always present:

| Field | Type / meaning |
|---|---|
| `id` | Native configured queue ID, Text |
| `displayName` | Text; CUPS printer-info falling back to ID, Windows native name |
| `backend` | `cups` or `winspool` |
| `state` | `idle`, `processing`, `stopped`, `unknown` |
| `acceptingJobs` | Boolean or null (Windows unavailable; paused does not imply false) |
| `jobCount` | Nonnegative integer or null |
| `reasons` | Text array or null (Windows unavailable) |
| `message` | Text or null (Windows unavailable) |
| `observedAt` | Observation serialization time, UTC `YYYY-MM-DDTHH:MM:SSZ` |
| `raw` | Object preserving native observations; unavailable properties null |

CUPS states 3/4/5 map to idle/processing/stopped. Missing or unrecognized
printer-state maps to unknown, **never** idle. Raw keys: `printer-state`,
`printer-state-reasons`, `printer-state-message`, `printer-is-accepting-jobs`,
`queued-job-count`, `printer-uri-supported` (array), `operations-supported`
(integer array), `printer-type`, `printer-is-temporary`.

Windows retains numeric `raw.status` and **all** named `PRINTER_STATUS_*` flags
as Booleans under `raw.flags`, including conflicting flags, SERVER_OFFLINE and
`DRIVER_UPDATE_NEEDED`. Unknown bits remain in the numeric status.
Paused/error, pending deletion, paper jam/out/problem, offline, output-bin-full,
not-available, no-toner, user-intervention, out-of-memory and
door-open and server-offline states conservatively dominate printing and map to stopped.
Otherwise PRINTING or PROCESSING maps to processing. WARMING_UP alone maps to
unknown; alongside PRINTING/PROCESSING it maps to processing unless a stopped
condition dominates. Status 0 maps to idle. **Windows zero
means absence of reported status flags, not proof of printer readiness.**
Other combinations map to unknown. Raw also preserves numeric attributes,
server/share/port/driver names, comment and location (nullable).
Spooler observations are not physical printer or network reachability probes.

## Local scheduler / configured target boundary

macOS primarily connects to the fixed system Unix-domain socket
`/private/var/run/cupsd` with `httpConnect2`. This is libcups' supported
absolute-path host form and permits launchd socket activation even while the
CUPS TCP listener sleeps. Only if that connection fails does it attempt the
explicit fallback `localhost:631`, only during connection establishment before
any request submission. Both attempts share one monotonic **3-second total**
connection budget; the fallback receives only the remaining milliseconds and
is skipped when the budget is exhausted. Connected HTTP I/O has a 5-second
timeout. There is one `cupsDoRequest` invocation: no application-level request
retry or transport fallback after authentication/protocol/transport failures,
especially after a possibly submitted mutation. The outer 4D process deadline
remains 30 seconds. If both endpoints fail,
the backend-unavailable diagnostic names both attempts and their errno values;
no mutation was submitted. It never calls `cupsServer()`
or `ippPort()`, never uses `CUPS_SERVER` / client.conf to select a scheduler or port,
and does not perform Bonjour discovery. It issues **CUPS-Get-Printers** only,
excluding class/discovered/implicit types in the request and response and
temporary queues in the response. A missing printer-type cannot establish
configured status and fails closed. CUPS not-found for an empty configured
printer set is `queues:[]`.

Get uses Get-Printer-Attributes; mutation uses standard Pause-Printer or
Resume-Printer at HTTP resource `/admin/`. Printer URIs are constructed as
`ipp://localhost:631/printers/<percent-encoded ID>`, never taken from returned
printer-uri-supported. Slash/backslash and dot path segments are rejected in
CUPS native IDs. Returned URI data is preserved only as raw observation.
Socket selection does not change these local printer URIs or `/admin/`.
`cupsSetPasswordCB2` always returns null: no credential callback/UI prompting.
CUPS may use existing platform credentials; no credentials are collected or
stored by this CLI.

Windows enumerates only `PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS`
with EnumPrintersW level 2. OpenPrinterW is called only after exact membership
in that enumeration, including user-configured UNC connections; an arbitrary
new UNC target is never opened. GetPrinterW uses level 2; SetPrinterW uses
level 0 and PRINTER_CONTROL_PAUSE/RESUME with admin access. Query access is
PRINTER_ACCESS_USE. Every successful handle open has a RAII ClosePrinter.
Unicode argv uses `wmain`, strict UTF-16/UTF-8 conversion and binary output.
No native Windows ARM64 support is claimed.

## Resource bounds

At most two command arguments (excluding executable), 16,384 UTF-8 bytes per
argument, IDs 1–1,024 UTF-8 bytes, strict UTF-8 with no NUL. IDs additionally
reject C0/C1/DEL control characters. Diagnostic arguments permit controls except
NUL. At most 4,096 queues, 1,024 entries in a native multivalue attribute,
16,384 bytes per native CUPS string / Windows UTF-16 code units per string.
Serialized output is bounded at 16 MiB. Windows native buffers are bounded at
16 MiB with at most three retries for size races; CUPS attributes are bounded
after libcups receives/decodes the response (libcups owns wire allocation).
Limit violations are explicit native errors, not truncated output or partial
success. Windows remote configured connections may still block RPC; callers
must enforce their process deadline. A 16 MiB output limit also covers every
aggregate JSON object/array; transient native allocations are not a process
memory sandbox.

## Build and tests

```sh
cmake -S cli -B build/native-cli -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-cli --config Release
ctest --test-dir build/native-cli -C Release --output-on-failure
TEST_CLI_LANGUAGE=cpp python3 -m unittest discover -s tests -p test_cli.py -v
```

CTest runs portable protocol/mock checks plus actual CUPS IPP-object request and
decode tests plus injected fixed-socket/fallback/deadline and no-resubmission tests on macOS, or
injected Winspool function-table tests on Windows.
Release builds retain test assertions. `monitor_fixture` is a test-only CLI,
not the shipped executable; it enables deterministic Python protocol/error/
mutation tests with no real printer interaction. Production has no mock
environment variable or switch. Python also tests real offline version,
capabilities, invalid arguments and Unicode/control/quote literal round trips,
and exact vectors in `tests/cli-contract.json`.

Native tests **never mutate actual queues** or depend on any printers being
configured. A separately authorized read-only `list` smoke can validate native
connectivity but does not prove mutation permissions or Windows acceptance.
The parent pipeline owns same-source artifact manifests, component resources,
4D host/compiled-package checks and release readiness. A macOS compile does
not validate Windows, native Intel execution or a final component package.
