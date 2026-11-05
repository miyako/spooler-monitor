property _cli : cs._CLI

Class constructor($target : Text)
	This._cli:=cs._CLI.new("spooler-monitor"; $target)

Function list()->$result : Object
	$result:=This._decode(This._cli.execute(New collection("list")); "list")

Function get($queueID : Variant)->$result : Object
	$result:=This._queueOperation("get"; $queueID)

Function pause($queueID : Variant)->$result : Object
	$result:=This._queueOperation("pause"; $queueID)

Function resume($queueID : Variant)->$result : Object
	$result:=This._queueOperation("resume"; $queueID)

Function version()->$result : Object
	$result:=This._decode(This._cli.execute(New collection("--version")); "version")

Function capabilities()->$result : Object
	$result:=This._decode(This._cli.execute(New collection("capabilities")); "capabilities")

Function _queueOperation($operation : Text; $queueID : Variant)->$result : Object
	If (Not(This._validID($queueID)))
		return New object("ok"; False; "error"; New object("code"; "invalid_argument"; "message"; "Queue ID must be Text of 1 to 1024 UTF-8 bytes without control characters"; "outcome"; "not_submitted"))
	End if
	$result:=This._decode(This._cli.execute(New collection($operation; $queueID)); $operation)

Function _validID($value : Variant)->$valid : Boolean
	var $i; $code : Integer
	var $utf8 : Blob
	$valid:=False
	If (Value type($value)#Is text)
		return False
	End if
	If ((Length($value)=0) | (Length($value)>1024))
		return False
	End if
	TEXT TO BLOB($value; $utf8; UTF8 text without length)
	If (BLOB size($utf8)>1024)
		return False
	End if
	If (Position(Char(0); $value; *)>0)
		return False
	End if
	For ($i; 1; Length($value))
		$code:=Character code(Substring($value; $i; 1))
		If (($code<32) | (($code>=127) & ($code<=159)))
			return False
		End if
	End for
	$valid:=True

Function _integer($value : Variant)->$valid : Boolean
	$valid:=False
	If ((Value type($value)#Is real) & (Value type($value)#Is longint))
		return False
	End if
	$valid:=($value=Int($value))

Function _backend($value : Variant)->$valid : Boolean
	$valid:=False
	If (Value type($value)#Is text)
		return False
	End if
	$valid:=($value="cups") | ($value="winspool")

Function _error($value : Variant)->$valid : Boolean
	var $code : Text
	$valid:=False
	If (Value type($value)#Is object)
		return False
	End if
	If (Value type($value.code)#Is text)
		return False
	End if
	$code:=$value.code
	If (New collection("invalid_arguments"; "unknown_queue"; "access_denied"; "backend_unavailable"; "unsupported_operation"; "observation_failed"; "native_error").indexOf($code)<0)
		return False
	End if
	If (Value type($value.message)#Is text)
		return False
	End if
	If (Length($value.message)=0)
		return False
	End if
	If (Value type($value.backend)#Is null)
		If (Not(This._backend($value.backend)))
			return False
		End if
	End if
	If (Value type($value.nativeCode)#Is null)
		If (Not(This._integer($value.nativeCode)))
			return False
		End if
	End if
	If (Value type($value.outcome)#Is text)
		return False
	End if
	If (New collection("not_submitted"; "rejected"; "uncertain"; "accepted").indexOf($value.outcome)<0)
		return False
	End if
	$valid:=True

Function _queue($value : Variant)->$valid : Boolean
	var $reason : Variant
	$valid:=False
	If (Value type($value)#Is object)
		return False
	End if
	If (Not(This._validID($value.id)))
		return False
	End if
	If (Value type($value.displayName)#Is text)
		return False
	End if
	If (Not(This._backend($value.backend)))
		return False
	End if
	If (Value type($value.state)#Is text)
		return False
	End if
	If (New collection("idle"; "processing"; "stopped"; "unknown").indexOf($value.state)<0)
		return False
	End if
	If ((Value type($value.acceptingJobs)#Is null) & (Value type($value.acceptingJobs)#Is boolean))
		return False
	End if
	If (Value type($value.jobCount)#Is null)
		If (Not(This._integer($value.jobCount)))
			return False
		End if
		If ($value.jobCount<0)
			return False
		End if
	End if
	If (Value type($value.reasons)#Is null)
		If (Value type($value.reasons)#Is collection)
			return False
		End if
		For each ($reason; $value.reasons)
			If (Value type($reason)#Is text)
				return False
			End if
		End for each
	End if
	If ((Value type($value.message)#Is null) & (Value type($value.message)#Is text))
		return False
	End if
	If (Value type($value.observedAt)#Is text)
		return False
	End if
	If (Not(Match regex("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]+)?Z$"; $value.observedAt)))
		return False
	End if
	If (Value type($value.raw)#Is object)
		return False
	End if
	$valid:=True

Function _mutation($value : Variant)->$valid : Boolean
	$valid:=False
	If (Value type($value)#Is object)
		return False
	End if
	If (Value type($value.accepted)#Is boolean)
		return False
	End if
	If (Value type($value.outcome)#Is text)
		return False
	End if
	If ($value.accepted)
		If ($value.outcome#"accepted")
			return False
		End if
	Else
		If (New collection("not_submitted"; "rejected"; "uncertain").indexOf($value.outcome)<0)
			return False
		End if
	End if
	If (Value type($value.observation)#Is text)
		return False
	End if
	Case of
		: ($value.observation="succeeded")
			If (Not($value.accepted))
				return False
			End if
			If (Value type($value.error)#Is null)
				return False
			End if
		: ($value.observation="failed")
			If (Not(This._error($value.error)))
				return False
			End if
			If ($value.error.outcome#$value.outcome)
				return False
			End if
		Else
			return False
	End case
	$valid:=True

Function _invalidProtocol($result : Object)->$decoded : Object
	$result.ok:=False
	$result.error:=New object("code"; "invalid_protocol"; "message"; "CLI response violates spooler-monitor protocol 1")
	$decoded:=$result

Function _decode($result : Object; $field : Text)->$decoded : Object
	$decoded:=This._decodeResponse($result; $field)
	If (($field="pause") | ($field="resume"))
		If (Not($decoded.ok))
			If ($decoded.error.code="invalid_protocol")
				$decoded.error.outcome:="uncertain"
			End if
		End if
	End if

Function _decodeResponse($result : Object; $field : Text)->$decoded : Object
	var $payload; $queue : Variant
	var $previous; $operation; $stream : Text
	var $parseErrors; $operations : Collection
	var CLI_LastErrors : Collection
	var $mutation; $failure : Boolean
	var $expectedExit : Integer
	$operation:=$field
	If ($field="argument")
		$operation:="inspect-argument"
	End if
	$mutation:=($operation="pause") | ($operation="resume")
	$failure:=Not($result.ok)
	If ($failure)
		If ($result.error.code#"cli_error")
			If ($mutation)
				$result.error.outcome:="not_submitted"
				If (($result.error.code="process_error") | ($result.error.code="process_terminated"))
					$result.error.outcome:="uncertain"
				End if
			End if
			return $result
		End if
		$stream:=$result.stderr
	Else
		$stream:=$result.stdout
		If ($result.stderr#"")
			return This._invalidProtocol($result)
		End if
		If ($result.exitCode#0)
			return This._invalidProtocol($result)
		End if
	End if
	$previous:=Method called on error(ek local)
	CLI_LastErrors:=Null
	ON ERR CALL("_CaptureError"; ek local)
	$payload:=JSON Parse($stream)
	ON ERR CALL($previous; ek local)
	$parseErrors:=CLI_LastErrors
	CLI_LastErrors:=Null
	If ($parseErrors#Null)
		return This._invalidProtocol($result)
	End if
	If (Value type($payload)#Is object)
		return This._invalidProtocol($result)
	End if
	If (Not(This._integer($payload.protocol)))
		return This._invalidProtocol($result)
	End if
	If ($payload.protocol#1)
		return This._invalidProtocol($result)
	End if
	If (Value type($payload.ok)#Is boolean)
		return This._invalidProtocol($result)
	End if
	If ($payload.ok=$failure)
		return This._invalidProtocol($result)
	End if
	If (Value type($payload.operation)#Is text)
		return This._invalidProtocol($result)
	End if
	If ($payload.operation#$operation)
		If (Not($failure))
			return This._invalidProtocol($result)
		End if
		If ($payload.operation#"arguments")
			return This._invalidProtocol($result)
		End if
	End if
	If ($failure)
		If ($result.stdout#"")
			return This._invalidProtocol($result)
		End if
		If (Not(This._error($payload.error)))
			return This._invalidProtocol($result)
		End if
		$expectedExit:=New collection("invalid_arguments"; "unknown_queue"; "access_denied"; "backend_unavailable"; "unsupported_operation"; "observation_failed"; "native_error").indexOf($payload.error.code)+2
		If ($result.exitCode#$expectedExit)
			return This._invalidProtocol($result)
		End if
		If ($payload.operation="arguments")
			If ($payload.error.code#"invalid_arguments")
				return This._invalidProtocol($result)
			End if
		End if
		If (Value type($payload.mutation)#Is undefined)
			If (Not($mutation))
				return This._invalidProtocol($result)
			End if
			If (Not(This._mutation($payload.mutation)))
				return This._invalidProtocol($result)
			End if
			If ($payload.mutation.observation#"failed")
				return This._invalidProtocol($result)
			End if
			If ($payload.error.outcome#$payload.mutation.outcome)
				return This._invalidProtocol($result)
			End if
			If ($payload.error.code#$payload.mutation.error.code)
				return This._invalidProtocol($result)
			End if
			If ($payload.mutation.accepted)
				If ($payload.error.code#"observation_failed")
					return This._invalidProtocol($result)
				End if
				If (Value type($payload.queue)#Is null)
					return This._invalidProtocol($result)
				End if
			Else
				If ($payload.error.code="observation_failed")
					return This._invalidProtocol($result)
				End if
			End if
			$result.mutation:=$payload.mutation
		Else
			If ($mutation)
				If ($payload.operation#"arguments")
					return This._invalidProtocol($result)
				End if
			End if
			If (($payload.error.outcome="accepted") | ($payload.error.code="observation_failed"))
				return This._invalidProtocol($result)
			End if
		End if
		If (Value type($payload.queue)#Is undefined)
			If (Value type($payload.queue)#Is null)
				If (Not(This._queue($payload.queue)))
					return This._invalidProtocol($result)
				End if
			End if
			$result.queue:=$payload.queue
		End if
		$result.error:=$payload.error
		$result.value:=$payload
		return $result
	End if
	If (Value type($payload.error)#Is undefined)
		return This._invalidProtocol($result)
	End if
	Case of
		: ($operation="version")
			If (Value type($payload.version)#Is text)
				return This._invalidProtocol($result)
			End if
			If ($payload.version#"0.1.1")
				return This._invalidProtocol($result)
			End if
		: ($operation="capabilities")
			If (Not(This._backend($payload.backend)))
				return This._invalidProtocol($result)
			End if
			If (Value type($payload.operations)#Is collection)
				return This._invalidProtocol($result)
			End if
			$operations:=$payload.operations
			If (JSON Stringify($operations)#JSON Stringify(New collection("list"; "get"; "pause"; "resume")))
				return This._invalidProtocol($result)
			End if
			If (Value type($payload.mutationPolicy)#Is text)
				return This._invalidProtocol($result)
			End if
			If ($payload.mutationPolicy#"explicit-only")
				return This._invalidProtocol($result)
			End if
		: ($operation="inspect-argument")
			If (Value type($payload.argument)#Is text)
				return This._invalidProtocol($result)
			End if
		: ($operation="list")
			If (Value type($payload.queues)#Is collection)
				return This._invalidProtocol($result)
			End if
			For each ($queue; $payload.queues)
				If (Not(This._queue($queue)))
					return This._invalidProtocol($result)
				End if
			End for each
			$result.queues:=$payload.queues
		: ($operation="get")
			If (Not(This._queue($payload.queue)))
				return This._invalidProtocol($result)
			End if
			$result.queue:=$payload.queue
		: ($mutation)
			If (Not(This._mutation($payload.mutation)))
				return This._invalidProtocol($result)
			End if
			If ($payload.mutation.observation#"succeeded")
				return This._invalidProtocol($result)
			End if
			If (Not(This._queue($payload.queue)))
				return This._invalidProtocol($result)
			End if
			$result.mutation:=$payload.mutation
			$result.queue:=$payload.queue
		Else
			return This._invalidProtocol($result)
	End case
	$result.value:=$payload
	$decoded:=$result
