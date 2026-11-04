# Queue

The public host class is `cs.SpoolerMonitor.Queue`; component-internal code uses
`cs.Queue`. It wraps the bundled `spooler-monitor` process, not a native plug-in.

`Queue.new({target : Text})` accepts `"macOS-universal"` (or omitted on macOS)
and explicit `"Windows-x64"` on Windows. Windows ARM64 is unsupported.

| Method | Successful `result.value` fields in addition to `protocol:1, ok:true, operation` |
| --- | --- |
| `list() : Object` | `queues : Collection`, possibly empty |
| `get(queueID : Variant) : Object` | `queue : Object` |
| `pause(queueID : Variant) : Object` | `mutation : Object`, post-query `queue : Object` |
| `resume(queueID : Variant) : Object` | `mutation : Object`, post-query `queue : Object` |
| `version() : Object` | `version:"0.1.0"`, operation `"version"` |
| `capabilities() : Object` | `backend:"cups"\|"winspool"`, `operations:["list","get","pause","resume"]`, `mutationPolicy:"explicit-only"` |

`get`, `pause` and `resume` accept only actual Text IDs, 1–1024 UTF-8 bytes,
without C0, DEL or C1 controls (including embedded NUL). No scalar coercion,
trimming, display-name lookup or automatic retry is performed. IDs come from
`list().value.queues[].id`; a display name is not necessarily an ID.
Local rejection is `invalid_argument`, `outcome:"not_submitted"`, without an
invented process exit code. Unicode and whitespace are literal data.

## Scheduling: mandatory worker use on a dedicated client

Every method is **synchronous** and may wait up to the finite 30-second process
timeout. A spooler query or mutation can block. To avoid freezing a user
interface, use `CALL WORKER` on a **dedicated 4D client**, invoking these same
synchronous methods inside its live worker:

```4d
// UI/scheduler method; Queue_poll is an application method, not component setup.
CALL WORKER("spooler-monitor polling"; "Queue_poll")
```

```4d
// Application method Queue_poll, executed in that live worker.
var $target : Text
var $tool : cs.SpoolerMonitor.Queue
var $result : Object
$target:="macOS-universal"
If (Is Windows)
    $target:="Windows-x64"
End if
$tool:=cs.SpoolerMonitor.Queue.new($target)
$result:=$tool.list()
// Deliver a copied result to the application's existing UI-worker mechanism.
// Only an explicit authorized user action should call pause or resume.
```

Keep the worker alive for completion and isolate per-call state. The component
does not install a worker, callback registry, shared PID map, cancellation queue
or unsafe pseudo-asynchronous wrapper. Concurrent calls have separate CLI
processes and snapshots; there is no ordering or transactional guarantee.
Avoid overlapping polling and actions for the same queue at the application
level. `New process` is not a substitute for the live worker arrangement.

## Result envelope and observations

Successful calls return
`{ok:true,stdout:Text,stderr:"",exitCode:0,value:Object}`. The complete decoded
protocol payload is retained in `value`; `queues`, `queue` and `mutation` are
also exposed at the top level when present. Queue objects contain:

* `id`, `displayName`, `backend` Text; backend is `cups` or `winspool`.
* `state`: `idle`, `processing`, `stopped` or `unknown`.
* `acceptingJobs`: Boolean or Null; `jobCount`: nonnegative integer or Null.
* `reasons`: Collection of Text or Null; `message`: Text or Null.
* `observedAt`: UTC ISO Text (`YYYY-MM-DDTHH:MM:SS[.fraction]Z`);
  `raw`: Object containing backend-specific observation details.

Null means unavailable/unknown, not zero or false. Observations are snapshots,
not subscriptions. Pausing queue processing does not necessarily reject new
jobs. No job deletion, job-level operation or printer creation is provided.
Capabilities describes supported commands, not current permissions/backend
availability. Inspect `list`/`get` errors to diagnose an unavailable spooler.

Mutations are **explicit only**: construction, capabilities, version, list,
get and every host test never call pause/resume against a real printer.
An accepted mutation has
`{accepted:true,outcome:"accepted",observation:"succeeded",error:null}`.
The post-query queue is an observation, not proof of durable future state.
If the backend accepted a mutation but the follow-up query failed, exit 7 is
returned with `ok:false`, structured `error.outcome:"accepted"` and
`mutation:{accepted:true,outcome:"accepted",observation:"failed",error:Object}`.
The failed post-query must contain explicit `queue:null`; a successful mutation
must contain a complete queue Object, never Null or an omitted field.
The wrapper preserves that object and failure: **it is not a rollback**.
Do not automatically retry an accepted/uncertain action.
Unaccepted mutations retain `accepted:false`, `observation:"failed"` and matching
`not_submitted`, `rejected` or `uncertain` outcomes in the mutation and both error
objects. These do not claim a post-query queue.
Native pause/resume failures must include their mutation envelope; outer and
nested error codes must match, and the exit status must match that code.

## Protocol and failures

Commands are `list`, `get|pause|resume <queueID>`, `capabilities`, `--version`
and the harmless diagnostic `inspect-argument <text>`. Output is one compact
UTF-8 JSON object plus LF. A success has protocol 1, `ok:true` and the matching
operation. The diagnostic returns `argument` unchanged; it is not a printer
operation. Its wrapper decoder field selector is `"argument"`.

Failure stdout is empty and stderr contains one protocol-1 JSON object with
`ok:false`, operation (or `"arguments"` for invalid command syntax) and
`error:{code,message,backend,nativeCode,outcome}`.
Backend is `cups`, `winspool` or Null; nativeCode is integer or Null.
Outcome is `not_submitted`, `rejected`, `uncertain` or `accepted`.

| Exit | Structured code |
| --- | --- |
| 2 | `invalid_arguments` |
| 3 | `unknown_queue` |
| 4 | `access_denied` |
| 5 | `backend_unavailable` |
| 6 | `unsupported_operation` |
| 7 | `observation_failed` |
| 8 | `native_error` |

The exact command-syntax diagnostic message is
`expected list, get|pause|resume <queueID>, capabilities, inspect-argument <text>, or --version`.
The generic adapter's `cli_error` is decoded from stderr into this structured
error, preserving stdout, stderr, exitCode, `value` and any accepted mutation.
Transport errors retain generic codes (`launch_failed`, `process_error`,
`process_terminated`, missing resource/unsupported target, etc.). A prelaunch
mutation failure is `not_submitted`; timeout/abnormal termination/system-worker
failure is `uncertain` because submission cannot be ruled out.

The decoder rejects wrong protocol types/versions, operations, required fields,
queue/error/mutation shapes and error/exit-status mismatches as
`invalid_protocol`; it never fabricates a successful empty result. Invalid
responses after a mutation are not evidence of rollback: `invalid_protocol`
after pause/resume has `error.outcome:"uncertain"` and retains the original
streams and status. Known prelaunch failures remain `not_submitted`.
JSON parsing uses a
narrow local error handler, restored immediately. Underscore members are internal
by convention, not a 4D access-control boundary.
