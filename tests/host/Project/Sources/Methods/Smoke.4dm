//%attributes = {"invisible":true}
var $ignored : Integer
var $parameter; $name; $target : Text
var $folderPath : Text
var $request; $result; $report : Object
var $tool : cs.SpoolerMonitor.Queue
var $requestFile; $statusFile : 4D.File
var $renamedExecutable : 4D.File
var $cases : Collection
var $invalid; $payload : Variant
var $queue; $error; $mutation; $envelope : Object
var $i; $status : Integer
var HOST_StatusFile : 4D.File
var HOST_Phase : Text
var HOST_PreviousHandler : Text
$ignored:=Get database parameter(User param value; $parameter)
If (Is Windows)
	$requestFile:=File($parameter; fk platform path)
Else
	$requestFile:=File($parameter; fk posix path)
End if
$request:=JSON Parse($requestFile.getText())
If (Is Windows)
	$statusFile:=File($request.status; fk platform path)
Else
	$statusFile:=File($request.status; fk posix path)
End if
$statusFile.setText("{\"success\":false,\"phase\":\"starting\"}")
HOST_StatusFile:=$statusFile
HOST_Phase:="constructor"
HOST_PreviousHandler:=Method called on error(ek local)
ON ERR CALL("Host_error"; ek local)
$target:="macOS-universal"
If (Is Windows)
	$target:="Windows-x64"
End if
$tool:=cs.SpoolerMonitor.Queue.new($target)
$cases:=New collection("world"; "path with spaces"; "quote \" and \\ slash"; "line\nbreak"; "日本語"; "$() ; & % !")
$report:=New object("success"; True; "finished"; False; "cases"; New collection)
$report.compiled:=$tool._cli.compiled
HOST_Phase:="renamed executable"
If (Is Windows)
	$renamedExecutable:=File($request.executable; fk platform path)
Else
	$renamedExecutable:=File($request.executable; fk posix path)
End if
$report.renamedExecutable:=cs.SpoolerMonitor._Process.new().execute($renamedExecutable; New collection("--version"))
If (Not($report.renamedExecutable.ok))
	$report.success:=False
End if
HOST_Phase:="feature cases"
If ($request.expectCompiled#$report.compiled)
	$report.success:=False
End if
$report.capabilities:=$tool.capabilities()
$report.version:=$tool.version()
$report.cases.push($report.capabilities; $report.version)
If (Not($report.capabilities.ok) | Not($report.version.ok))
	$report.success:=False
End if
$report.arguments:=New collection
For each ($name; $cases)
	$result:=$tool._decode($tool._cli.execute(New collection("inspect-argument"; $name)); "argument")
	$report.arguments.push($result)
	If (Not($result.ok))
		$report.success:=False
	Else
		If ($result.value.argument#$name)
			$report.success:=False
		End if
	End if
End for each
$result:=$tool._decode($tool._cli.execute(New collection("not-a-command")); "list")
$report.invalid:=$result
If ($result.ok | ($result.exitCode#2))
	$report.success:=False
End if
HOST_Phase:="read-only list"
$result:=$tool.list()
$report.readOnly:=$result
$report.readOnlyValid:=True
$report.readOnlyMode:="success"
If (Not($result.ok))
	$report.readOnlyMode:="backend_failure"
	If (New collection("access_denied"; "backend_unavailable"; "unsupported_operation"; "native_error").indexOf($result.error.code)<0)
		$report.readOnlyValid:=False
		$report.success:=False
	End if
	If ($result.value=Null)
		$report.readOnlyValid:=False
		$report.success:=False
	End if
End if
HOST_Phase:="queue ID guards (no mutation submitted)"
$report.invalidIDs:=New collection
$name:=""
For ($i; 1; 1025)
	$name+="x"
End for
For each ($invalid; New collection(123; 1.25; True; False; Null; Current date; Current time; New collection; New object; ""; Char(0)+"prefix"; "middle"+Char(0)+"suffix"; "trailing"+Char(0); "line"+Char(10)+"break"; "tab"+Char(9); Char(127); Char(159); $name))
	For each ($name; New collection("get"; "pause"; "resume"))
		$result:=$tool._queueOperation($name; $invalid)
		$report.invalidIDs.push($result)
		If ($result.ok | ($result.error.code#"invalid_argument"))
			$report.success:=False
		End if
	End for each
End for each
$report.idLimits:=New collection
$name:=""
For ($i; 1; 1024)
	$name+="x"
End for
$report.idLimits.push($tool._validID($name); $tool._validID($name+"x"))
$name:=""
For ($i; 1; 341)
	$name+="日"
End for
$report.idLimits.push($tool._validID($name); $tool._validID($name+"日"))
HOST_Phase:="missing executable"
$result:=cs.SpoolerMonitor._CLI.new("missing-cli"; $target).execute(New collection("--version"))
$report.missing:=$result
If ($result.error.code#"missing_executable")
	$report.success:=False
End if
$result:=cs.SpoolerMonitor.Queue.new("unsupported-arm-target").version()
$report.unsupported:=$result
If ($result.error.code#"unsupported_architecture")
	$report.success:=False
End if
HOST_Phase:="launch failure"
$result:=cs.SpoolerMonitor._Process.new().execute($requestFile; New collection)
$report.launch:=$result
If ($result.error.code#"launch_failed")
	$report.success:=False
End if
HOST_Phase:="malformed protocol"
$report.protocolInvalid:=New collection
For each ($name; New collection("not json"; "{\"protocol\":true,\"ok\":true,\"operation\":\"version\",\"version\":\"0.1.0\"}"; "{\"protocol\":false}"; "{\"protocol\":null}"; "true"; "false"; "null"; "1"; "[]"; "{\"protocol\":1,\"ok\":true,\"operation\":\"version\",\"version\":true}"; "{\"protocol\":1,\"ok\":true,\"operation\":\"list\",\"queues\":[]}"; "{\"protocol\":2,\"ok\":true,\"operation\":\"version\",\"version\":\"0.1.0\"}"))
	$result:=$tool._decode(New object("ok"; True; "stdout"; $name; "stderr"; ""; "exitCode"; 0); "version")
	$report.protocolInvalid.push($result)
	If ($result.ok | ($result.error.code#"invalid_protocol"))
		$report.success:=False
	End if
End for each
HOST_Phase:="synthetic queue schemas (no native mutations)"
$queue:=New object("id"; "fixture"; "displayName"; "Fixture"; "backend"; "cups"; "state"; "idle"; "acceptingJobs"; True; "jobCount"; 0; "reasons"; New collection; "message"; Null; "observedAt"; "2026-11-04T00:00:00Z"; "raw"; New object)
$report.syntheticSuccesses:=New collection
For each ($name; New collection("list"; "get"; "pause"; "resume"))
	$payload:=New object("protocol"; 1; "ok"; True; "operation"; $name)
	If ($name="list")
		$payload.queues:=New collection($queue)
	Else
		$payload.queue:=$queue
		If (($name="pause") | ($name="resume"))
			$payload.mutation:=New object("accepted"; True; "outcome"; "accepted"; "observation"; "succeeded"; "error"; Null)
		End if
	End if
	$result:=$tool._decode(New object("ok"; True; "stdout"; JSON Stringify($payload); "stderr"; ""; "exitCode"; 0); $name)
	$report.syntheticSuccesses.push($result)
	If (Not($result.ok))
		$report.success:=False
	End if
End for each
$report.schemaInvalid:=New collection
$payload:=JSON Parse(JSON Stringify(New object("protocol"; 1; "ok"; True; "operation"; "get"; "queue"; $queue)))
$payload.queue.id:=""
For ($i; 1; 256)
	$payload.queue.id+="x"
End for
$report.longQueue:=$tool._decode(New object("ok"; True; "stdout"; JSON Stringify($payload); "stderr"; ""; "exitCode"; 0); "get")
If (Not($report.longQueue.ok))
	$report.success:=False
End if
For ($i; 1; 14)
	$payload:=JSON Parse(JSON Stringify(New object("protocol"; 1; "ok"; True; "operation"; "get"; "queue"; $queue)))
	Case of
		: ($i=1)
			$payload.queue.id:=123
		: ($i=2)
			$payload.queue.displayName:=False
		: ($i=3)
			$payload.queue.backend:="other"
		: ($i=4)
			$payload.queue.state:=Null
		: ($i=5)
			$payload.queue.acceptingJobs:=1
		: ($i=6)
			$payload.queue.jobCount:=True
		: ($i=7)
			$payload.queue.jobCount:=-1
		: ($i=8)
			$payload.queue.reasons:=New collection(123)
		: ($i=9)
			$payload.queue.message:=New object
		: ($i=10)
			$payload.queue.observedAt:="not UTC"
		: ($i=11)
			$payload.queue.raw:=Null
		: ($i=12)
			$payload.queue.jobCount:=1.5
		: ($i=13)
			$payload.queue:=Null
		: ($i=14)
			$payload.ok:=1
	End case
	$result:=$tool._decode(New object("ok"; True; "stdout"; JSON Stringify($payload); "stderr"; ""; "exitCode"; 0); "get")
	$report.schemaInvalid.push($result)
	If ($result.ok | ($result.error.code#"invalid_protocol"))
		$report.success:=False
	End if
End for
$error:=New object("code"; "native_error"; "message"; "Synthetic native failure"; "backend"; "cups"; "nativeCode"; 123; "outcome"; "rejected")
$payload:=New object("protocol"; 1; "ok"; False; "operation"; "get"; "error"; $error)
$report.nativeError:=$tool._decode(New object("ok"; False; "stdout"; ""; "stderr"; JSON Stringify($payload); "exitCode"; 8; "error"; New object("code"; "cli_error")); "get")
$report.errorSchemaInvalid:=New collection
For ($i; 1; 8)
	$payload:=JSON Parse(JSON Stringify(New object("protocol"; 1; "ok"; False; "operation"; "get"; "error"; $error)))
	Case of
		: ($i=1)
			$payload.error.nativeCode:=True
		: ($i=2)
			$payload.error.nativeCode:=1.5
		: ($i=3)
			$payload.error.backend:=1
		: ($i=4)
			$payload.error.outcome:="rollback"
		: ($i=5)
			$payload.error.message:=Null
		: ($i=6)
			$payload.error.code:="bogus"
		: ($i=7)
			$payload.operation:="pause"
		: ($i=8)
			$payload.error:=Null
	End case
	$result:=$tool._decode(New object("ok"; False; "stdout"; ""; "stderr"; JSON Stringify($payload); "exitCode"; 8; "error"; New object("code"; "cli_error")); "get")
	$report.errorSchemaInvalid.push($result)
	If ($result.ok | ($result.error.code#"invalid_protocol"))
		$report.success:=False
	End if
End for
$error:=New object("code"; "observation_failed"; "message"; "Synthetic post-query failure"; "backend"; "cups"; "nativeCode"; Null; "outcome"; "accepted")
$mutation:=New object("accepted"; True; "outcome"; "accepted"; "observation"; "failed"; "error"; $error)
$payload:=New object("protocol"; 1; "ok"; False; "operation"; "pause"; "error"; $error; "mutation"; $mutation; "queue"; Null)
$report.acceptedFailure:=$tool._decode(New object("ok"; False; "stdout"; ""; "stderr"; JSON Stringify($payload); "exitCode"; 7; "error"; New object("code"; "cli_error")); "pause")
$report.mutationInvalid:=New collection
For ($i; 1; 10)
	$payload:=JSON Parse(JSON Stringify(New object("protocol"; 1; "ok"; False; "operation"; "pause"; "error"; $error; "mutation"; $mutation; "queue"; Null)))
	Case of
		: ($i=1)
			$payload.mutation.accepted:=False
		: ($i=2)
			$payload.mutation.accepted:=1
		: ($i=3)
			$payload.mutation.outcome:="rollback"
		: ($i=4)
			$payload.mutation.observation:="succeeded"
		: ($i=5)
			$payload.mutation.error:=Null
		: ($i=6)
			$payload.error.outcome:="rejected"
		: ($i=7)
			$payload:=New object("protocol"; 1; "ok"; False; "operation"; "pause"; "error"; $error; "mutation"; $mutation)
		: ($i=8)
			$payload.queue:=$queue
		: ($i=9)
			$payload.mutation.error.outcome:="uncertain"
		: ($i=10)
			$payload.mutation.outcome:="uncertain"
	End case
	$result:=$tool._decode(New object("ok"; False; "stdout"; ""; "stderr"; JSON Stringify($payload); "exitCode"; 7; "error"; New object("code"; "cli_error")); "pause")
	$report.mutationInvalid.push($result)
	If ($result.ok | ($result.error.code#"invalid_protocol"))
		$report.success:=False
	End if
End for
$report.mutationQueueInvalid:=New collection
For each ($name; New collection("pause"; "resume"))
	For ($i; 1; 2)
		$payload:=New object("protocol"; 1; "ok"; True; "operation"; $name; "mutation"; New object("accepted"; True; "outcome"; "accepted"; "observation"; "succeeded"; "error"; Null))
		If ($i=1)
			$payload.queue:=Null
		End if
		$result:=$tool._decode(New object("ok"; True; "stdout"; JSON Stringify($payload); "stderr"; ""; "exitCode"; 0); $name)
		$report.mutationQueueInvalid.push($result)
		If ($result.ok | ($result.error.code#"invalid_protocol"))
			$report.success:=False
		End if
	End for
End for each
$report.malformedMutation:=New collection
For each ($name; New collection("pause"; "resume"))
	$result:=$tool._decode(New object("ok"; True; "stdout"; "not json"; "stderr"; ""; "exitCode"; 0); $name)
	$report.malformedMutation.push($result)
	$result:=$tool._decode(New object("ok"; False; "stdout"; ""; "stderr"; "not json"; "exitCode"; 8; "error"; New object("code"; "cli_error")); $name)
	$report.malformedMutation.push($result)
End for each
$report.rejectedMutations:=New collection
$i:=0
For each ($name; New collection("not_submitted"; "rejected"; "uncertain"; "not_submitted"; "rejected"))
	$i+=1
	$status:=8
	$error:=New object("code"; "native_error"; "message"; "Synthetic unaccepted mutation"; "backend"; "cups"; "nativeCode"; 123; "outcome"; $name)
	If ($i=4)
		$status:=3
		$error.code:="unknown_queue"
	End if
	If ($i=5)
		$status:=4
		$error.code:="access_denied"
	End if
	$mutation:=New object("accepted"; False; "outcome"; $name; "observation"; "failed"; "error"; $error)
	$payload:=New object("protocol"; 1; "ok"; False; "operation"; "resume"; "error"; $error; "mutation"; $mutation)
	$result:=$tool._decode(New object("ok"; False; "stdout"; ""; "stderr"; JSON Stringify($payload); "exitCode"; $status; "error"; New object("code"; "cli_error")); "resume")
	$report.rejectedMutations.push($result)
	If ($result.ok | ($result.error.code#$error.code))
		$report.success:=False
	End if
End for each
$report.mutationErrorMismatch:=New collection
For ($i; 1; 2)
	$payload:=JSON Parse(JSON Stringify(New object("protocol"; 1; "ok"; False; "operation"; "pause"; "error"; $error; "mutation"; $mutation)))
	If ($i=1)
		$payload.mutation.error.code:="unknown_queue"
	Else
		$payload:=New object("protocol"; 1; "ok"; False; "operation"; "pause"; "error"; $error)
	End if
	$result:=$tool._decode(New object("ok"; False; "stdout"; ""; "stderr"; JSON Stringify($payload); "exitCode"; 4; "error"; New object("code"; "cli_error")); "pause")
	$report.mutationErrorMismatch.push($result)
	If ($result.ok | ($result.error.code#"invalid_protocol"))
		$report.success:=False
	End if
End for
$report.transport:=New collection
For each ($name; New collection("launch_failed"; "process_error"; "process_terminated"))
	$result:=$tool._decode(New object("ok"; False; "error"; New object("code"; $name; "message"; "Synthetic transport failure")); "resume")
	$report.transport.push($result)
End for each
HOST_Phase:="File argument"
$result:=$tool._decode($tool._cli.execute(New collection("inspect-argument"; $requestFile)); "argument")
$report.fileArgument:=$result
If (Not($result.ok))
	$report.success:=False
Else
	If ($result.value.argument#$parameter)
		$report.success:=False
	End if
End if
$report.rejectedArguments:=New collection
HOST_Phase:="scalar arguments"
For each ($invalid; New collection(123; 1.25; True; False; Null; Current date; Current time; New collection; New object))
	$result:=cs.SpoolerMonitor._Process.new().execute($requestFile; New collection($invalid))
	$report.rejectedArguments.push(New object("type"; Value type($invalid); "result"; $result))
	If ($result.ok | ($result.error.code#"invalid_argument"))
		$report.success:=False
	End if
End for each
$report.rejectedNul:=New collection
HOST_Phase:="NUL arguments"
For each ($name; New collection(Char(0)+"prefix"; "middle"+Char(0)+"suffix"; "trailing"+Char(0)))
	$result:=cs.SpoolerMonitor._Process.new().execute($requestFile; New collection($name))
	$report.rejectedNul.push($result)
	If ($result.ok | ($result.error.code#"invalid_argument"))
		$report.success:=False
	End if
End for each
$report.missingFile:=cs.SpoolerMonitor._Process.new().execute(Null; New collection)
$report.missingArguments:=cs.SpoolerMonitor._Process.new().execute($requestFile; Null)
If ($report.missingFile.ok | ($report.missingFile.error.code#"invalid_argument"))
	$report.success:=False
End if
If ($report.missingArguments.ok | ($report.missingArguments.error.code#"invalid_argument"))
	$report.success:=False
End if
HOST_Phase:="Folder argument"
$result:=$tool._decode($tool._cli.execute(New collection("inspect-argument"; $requestFile.parent)); "argument")
$report.folderArgument:=$result
$folderPath:=$requestFile.parent.path
If (Is Windows)
	$folderPath:=$requestFile.parent.platformPath
End if
If (Not($result.ok))
	$report.success:=False
Else
	If ($result.value.argument#$folderPath)
		$report.success:=False
	End if
End if
$report.finished:=True
ON ERR CALL(HOST_PreviousHandler; ek local)
$statusFile.setText(JSON Stringify($report))
HOST_StatusFile:=Null
HOST_Phase:=""
HOST_PreviousHandler:=""
QUIT 4D
