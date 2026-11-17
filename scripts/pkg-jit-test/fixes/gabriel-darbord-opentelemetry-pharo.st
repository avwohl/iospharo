"gabriel-darbord-opentelemetry-pharo: two defects, both of the way the tests meet the GRPC package.

1. OTOtlpGrpcSpanExporterTest>>testExportUsesGrpcTransportStackEndToEnd skips
   because OTGrpcTestSupport class>>hasActualGrpcTransportStack needs the class
   GRPCHTTP2MockStream.  That class is in the package GRPC-Tests of grpc-pharo.
   BaselineOfOpenTelemetry loads the baseline GRPC with its default group
   (core: GRPC-Core, GRPC-HTTP2), so GRPC-Tests is not in the image.  The first
   part of this file defines GRPCHTTP2MockStream, copied from GRPC-Tests of
   grpc-pharo v0.1.0 (the clone under iceberg/Gabriel-Darbord/grpc-pharo).
   Loading the group tests of BaselineOfGRPC from that clone is the alternative.

2. The four integration test classes need an OpenTelemetry collector on
   127.0.0.1:4317 (ci/otel-collector-otlp-grpc.yaml).  The class
   OTLPGrpcCollectorDouble is a gRPC/HTTP2 receiver in the image.  It uses the
   frame reader and writer, the HPACK codec and the message framer of the GRPC
   package for the server side, and the OTProtobufReader of the OpenTelemetry
   package to decode the request into the JSON that the file exporter of the
   collector writes.  The setUp of OTOtlpGrpcIntegrationTest starts the double and
   sets the environment variables that the tests read.  The assertions of the
   tests are not changed.  Evidence and limits: docs/upstream/package-repairs.md."

(Smalltalk globals includesKey: #GRPCHTTP2MockStream) ifFalse: [
	(Smalltalk globals includesKey: #GRPCHTTP2Transport) ifTrue: [
		(Object << #GRPCHTTP2MockStream
			slots: { #readStream. #writeStream. #timeoutSeconds. #closed };
			package: 'GRPC-Tests';
			yourself) install ] ].

add value: #'GRPCHTTP2MockStream class' value: #'readingBytes:' value: 'instance creation' value:
'readingBytes: aByteArray

^ self new
	readingBytes: aByteArray;
	yourself'.

add value: #'GRPCHTTP2MockStream' value: #'atEnd' value: 'testing' value:
'atEnd

^ readStream atEnd'.

add value: #'GRPCHTTP2MockStream' value: #'close' value: 'initialization' value:
'close

closed := true'.

add value: #'GRPCHTTP2MockStream' value: #'closed' value: 'accessing' value:
'closed

^ closed'.

add value: #'GRPCHTTP2MockStream' value: #'flush' value: 'stream out' value:
'flush

^ self'.

add value: #'GRPCHTTP2MockStream' value: #'initialize' value: 'initialization' value:
'initialize

super initialize.
readStream := #[ ] readStream.
writeStream := ByteArray new writeStream.
timeoutSeconds := nil.
closed := false'.

add value: #'GRPCHTTP2MockStream' value: #'next' value: 'stream in' value:
'next

^ readStream next'.

add value: #'GRPCHTTP2MockStream' value: #'next:' value: 'stream in' value:
'next: anInteger

^ readStream next: anInteger'.

add value: #'GRPCHTTP2MockStream' value: #'nextPut:' value: 'stream out' value:
'nextPut: anInteger

writeStream nextPut: anInteger'.

add value: #'GRPCHTTP2MockStream' value: #'nextPutAll:' value: 'stream out' value:
'nextPutAll: aCollection

writeStream nextPutAll: aCollection'.

add value: #'GRPCHTTP2MockStream' value: #'readingBytes:' value: 'initialization' value:
'readingBytes: aByteArray

readStream := aByteArray readStream'.

add value: #'GRPCHTTP2MockStream' value: #'timeout:' value: 'accessing' value:
'timeout: anInteger

timeoutSeconds := anInteger'.

add value: #'GRPCHTTP2MockStream' value: #'timeoutSeconds' value: 'accessing' value:
'timeoutSeconds

^ timeoutSeconds'.

add value: #'GRPCHTTP2MockStream' value: #'writtenBytes' value: 'accessing' value:
'writtenBytes

^ writeStream contents'.


"--- the collector double ---"

(Smalltalk globals includesKey: #OTLPGrpcCollectorDouble) ifFalse: [
	(Smalltalk globals includesKey: #GRPCHTTP2FrameReader) ifTrue: [
		(Object << #OTLPGrpcCollectorDouble
			sharedVariables: { #Listener. #Acceptor. #Lock. #Documents. #Failures. #Received. #Schema };
			package: 'OTLP-Grpc-Collector-Double';
			yourself) install ] ].

add value: #'OTLPGrpcCollectorDouble class' value: #'port' value: 'configuration' value:
'port
	"The port of the OpenTelemetry collector in ci/otel-collector-otlp-grpc.yaml."
	^ 4317'.

add value: #'OTLPGrpcCollectorDouble class' value: #'ensureStarted' value: 'lifecycle' value:
'ensureStarted
	"Called by setUp of the OTLP/gRPC integration tests.  Starts the listener on
	 127.0.0.1:4317 when it does not run, and sets the environment variables that
	 the tests read.  A setUp error here (port in use) is the right signal: another
	 collector listens on the port."
	(Acceptor notNil and: [ Acceptor isTerminated not and: [ Listener notNil and: [ Listener isValid ] ] ])
		ifFalse: [ self start ].
	self publishEnvironment'.

add value: #'OTLPGrpcCollectorDouble class' value: #'start' value: 'lifecycle' value:
'start
	self stop.
	Lock := Mutex new.
	Documents := Dictionary new.
	Failures := OrderedCollection new.
	Received := 0.
	#( #traces #logs #metrics ) do: [ :each | (self outputFileFor: each) ensureDelete ].
	Listener := ZnNetworkingUtils serverSocketOn: self port interface: #[ 127 0 0 1 ].
	Acceptor := [ self acceptLoop ] forkAt: Processor lowIOPriority named: ''OTLP gRPC collector double'''.

add value: #'OTLPGrpcCollectorDouble class' value: #'stop' value: 'lifecycle' value:
'stop
	Acceptor ifNotNil: [ Acceptor terminate ].
	Acceptor := nil.
	Listener ifNotNil: [ [ Listener destroy ] on: Error do: [ :error | nil ] ].
	Listener := nil'.

add value: #'OTLPGrpcCollectorDouble class' value: #'failures' value: 'accessing' value:
'failures
	"Errors that ended a connection or a request, as strings.  Empty on a clean run."
	^ Failures ifNil: [ #() ]'.

add value: #'OTLPGrpcCollectorDouble class' value: #'received' value: 'accessing' value:
'received
	"The number of Export requests that the double accepted."
	^ Received ifNil: [ 0 ]'.

add value: #'OTLPGrpcCollectorDouble class' value: #'publishEnvironment' value: 'lifecycle' value:
'publishEnvironment
	| environment prefix |
	environment := Smalltalk os environment.
	prefix := ''OTEL_PHARO_OTLP_GRPC_INTEGRATION_''.
	environment at: prefix, ''ENABLED'' put: ''true''.
	environment at: prefix, ''TRACE_OUTPUT_FILE'' put: (self outputPathFor: #traces).
	environment at: prefix, ''LOG_OUTPUT_FILE'' put: (self outputPathFor: #logs).
	environment at: prefix, ''METRIC_OUTPUT_FILE'' put: (self outputPathFor: #metrics)'.

add value: #'OTLPGrpcCollectorDouble class' value: #'outputFileFor:' value: 'files' value:
'outputFileFor: aSignalSymbol
	| directory |
	directory := FileLocator temp / ''otlp-grpc-collector-double''.
	directory ensureCreateDirectory.
	^ (directory / (''otlp-grpc-'', aSignalSymbol, ''.json'')) resolve'.

add value: #'OTLPGrpcCollectorDouble class' value: #'outputPathFor:' value: 'files' value:
'outputPathFor: aSignalSymbol
	^ (self outputFileFor: aSignalSymbol) fullName'.

add value: #'OTLPGrpcCollectorDouble class' value: #'acceptLoop' value: 'server' value:
'acceptLoop
	[ | socket |
	  Listener isValid ifFalse: [ ^ self ].
	  socket := Listener waitForAcceptFor: 1.
	  socket ifNotNil: [ self serveSocket: socket ] ] repeat'.

add value: #'OTLPGrpcCollectorDouble class' value: #'serveSocket:' value: 'server' value:
'serveSocket: aSocket
	[ | stream |
	  stream := ZnNetworkingUtils socketStreamOn: aSocket.
	  [ self serveConnection: stream ]
		on: Error
		do: [ :error | self noteFailure: error ].
	  [ stream close ] on: Error do: [ :error | nil ].
	  [ aSocket destroy ] on: Error do: [ :error | nil ] ]
		forkAt: Processor lowIOPriority
		named: ''OTLP gRPC collector double connection'''.

add value: #'OTLPGrpcCollectorDouble class' value: #'noteFailure:' value: 'server' value:
'noteFailure: anError
	Lock critical: [ Failures add: anError class name, '': '', anError messageText printString ]'.

add value: #'OTLPGrpcCollectorDouble class' value: #'serveConnection:' value: 'server' value:
'serveConnection: aStream
	"One HTTP/2 connection: the client preface, the server SETTINGS, then frames
	 until the client closes (peek answers nil then).  Every request is answered at the end of its stream."
	| reader writer streams |
	reader := GRPCHTTP2FrameReader new.
	writer := GRPCHTTP2FrameWriter new.
	streams := Dictionary new.
	reader readClientConnectionPrefaceFrom: aStream.
	writer writeFrame: (GRPCHTTP2SettingsFrame settings: GRPCHTTP2Settings new) on: aStream.
	aStream flush.
	[ aStream peek notNil ] whileTrue: [ | frame |
		frame := reader readNextFrameFrom: aStream.
		frame ifNotNil: [ self handleFrame: frame streams: streams writer: writer on: aStream ] ]'.

add value: #'OTLPGrpcCollectorDouble class' value: #'handleFrame:streams:writer:on:' value: 'server' value:
'handleFrame: aFrame streams: streams writer: writer on: aStream
	| entry |
	aFrame class = GRPCHTTP2SettingsFrame ifTrue: [
		aFrame acknowledgment ifFalse: [
			writer writeFrame: GRPCHTTP2SettingsFrame acknowledgment on: aStream.
			aStream flush ].
		^ self ].
	aFrame class = GRPCHTTP2PingFrame ifTrue: [
		aFrame acknowledgment ifFalse: [
			writer
				writeFrame: (GRPCHTTP2PingFrame ack: true payload1: aFrame payload1 payload2: aFrame payload2)
				on: aStream.
			aStream flush ].
		^ self ].
	aFrame class = GRPCHTTP2HeadersFrame ifTrue: [
		entry := Array with: aFrame headers with: (WriteStream on: ByteArray new).
		aFrame endStream
			ifTrue: [ self respondTo: entry streamId: aFrame streamId writer: writer on: aStream ]
			ifFalse: [ streams at: aFrame streamId put: entry ].
		^ self ].
	aFrame class = GRPCHTTP2DataFrame ifTrue: [
		entry := streams at: aFrame streamId ifAbsent: [ ^ self ].
		entry second nextPutAll: aFrame payload.
		aFrame payload isEmpty ifFalse: [
			writer
				writeFrame: (GRPCHTTP2WindowUpdateFrame streamId: 0 increment: aFrame payload size)
				on: aStream.
			aFrame endStream ifFalse: [
				writer
					writeFrame: (GRPCHTTP2WindowUpdateFrame streamId: aFrame streamId increment: aFrame payload size)
					on: aStream ].
			aStream flush ].
		aFrame endStream ifTrue: [
			streams removeKey: aFrame streamId.
			self respondTo: entry streamId: aFrame streamId writer: writer on: aStream ].
		^ self ].
	aFrame class = GRPCHTTP2RstStreamFrame ifTrue: [ streams removeKey: aFrame streamId ifAbsent: [ nil ] ]'.

add value: #'OTLPGrpcCollectorDouble class' value: #'servicesByPath' value: 'server' value:
'servicesByPath
	"The gRPC path of each Export method -> the signal and the root message."
	^ Dictionary new
		at: ''/opentelemetry.proto.collector.trace.v1.TraceService/Export'' put: #( #traces #ExportTraceServiceRequest );
		at: ''/opentelemetry.proto.collector.logs.v1.LogsService/Export'' put: #( #logs #ExportLogsServiceRequest );
		at: ''/opentelemetry.proto.collector.metrics.v1.MetricsService/Export'' put: #( #metrics #ExportMetricsServiceRequest );
		yourself'.

add value: #'OTLPGrpcCollectorDouble class' value: #'respondTo:streamId:writer:on:' value: 'server' value:
'respondTo: anEntry streamId: anInteger writer: writer on: aStream
	"anEntry is (request headers, request body stream).  The answer is the one
	 of a real collector: HEADERS, one empty response message, trailers with
	 grpc-status 0.  An unknown path or a body that does not decode gets a
	 trailers-only answer with grpc-status 12 or 3."
	| headers path service document frame |
	headers := anEntry first.
	path := (headers detect: [ :each | each key = '':path'' ] ifNone: [ nil ])
		ifNotNil: [ :each | each value asString ].
	service := path ifNotNil: [ self servicesByPath at: path ifAbsent: [ nil ] ].
	service ifNil: [
		^ self answerWithStatus: 12 message: ''unknown method '', path printString streamId: anInteger writer: writer on: aStream ].
	[ frame := GRPCMessageFramer readSingleFrameFrom: anEntry second contents readStream.
	  frame isCompressed ifTrue: [ Error signal: ''compressed request messages are not supported by the double'' ].
	  document := self decode: frame payload as: service second ]
		on: Error
		do: [ :error |
			self noteFailure: error.
			^ self answerWithStatus: 3 message: error messageText asString streamId: anInteger writer: writer on: aStream ].
	self record: document signal: service first.
	writer
		writeFrame: (GRPCHTTP2HeadersFrame
			streamId: anInteger
			headers: { '':status'' -> ''200''. ''content-type'' -> ''application/grpc+proto'' }
			endStream: false)
		on: aStream.
	writer
		writeFrame: (GRPCHTTP2DataFrame
			streamId: anInteger
			payload: (GRPCMessageFramer frameMessage: #[ ] compressed: false)
			endStream: false)
		on: aStream.
	writer
		writeFrame: (GRPCHTTP2HeadersFrame streamId: anInteger headers: { ''grpc-status'' -> ''0'' } endStream: true)
		on: aStream.
	aStream flush'.

add value: #'OTLPGrpcCollectorDouble class' value: #'answerWithStatus:message:streamId:writer:on:' value: 'server' value:
'answerWithStatus: aCode message: aString streamId: anInteger writer: writer on: aStream
	writer
		writeFrame: (GRPCHTTP2HeadersFrame
			streamId: anInteger
			headers: {
				'':status'' -> ''200''.
				''content-type'' -> ''application/grpc+proto''.
				''grpc-status'' -> aCode printString.
				''grpc-message'' -> (aString copyReplaceAll: String lf with: '' '') }
			endStream: true)
		on: aStream.
	aStream flush'.

add value: #'OTLPGrpcCollectorDouble class' value: #'record:signal:' value: 'output' value:
'record: aDocument signal: aSignalSymbol
	"One JSON line for each Export request, as the file exporter of the real
	 collector writes it, then the whole file again (the writes are rare)."
	| lines text |
	Lock critical: [
		lines := Documents at: aSignalSymbol ifAbsentPut: [ OrderedCollection new ].
		lines add: (STON toJsonString: aDocument).
		Received := Received + 1.
		text := String streamContents: [ :out |
			lines do: [ :each | out nextPutAll: each; nextPut: Character lf ] ].
		(self outputFileFor: aSignalSymbol) ensureDelete.
		(self outputFileFor: aSignalSymbol) binaryWriteStreamDo: [ :out | out nextPutAll: text utf8Encoded ] ]'.

add value: #'OTLPGrpcCollectorDouble class' value: #'decode:as:' value: 'decoder' value:
'decode: someBytes as: aMessageName
	"Decode a protobuf message by the table of schema into a Dictionary with the
	 field names of the OTLP JSON mapping.  A field that the table does not name
	 is kept as unknown<number> (hex), so nothing is dropped silently."
	| reader fields result |
	reader := OTProtobufReader on: someBytes.
	fields := self schema at: aMessageName.
	result := Dictionary new.
	[ reader atEnd ] whileFalse: [ | tag number wire raw spec |
		tag := reader readTag.
		number := tag first.
		wire := tag second.
		raw := self readWire: wire from: reader.
		spec := fields at: number ifAbsent: [ nil ].
		spec
			ifNil: [
				result
					at: ''unknown'', number printString
					put: (raw isInteger ifTrue: [ raw printString ] ifFalse: [ raw hex ]) ]
			ifNotNil: [ self store: raw spec: spec into: result ] ].
	result keys do: [ :key |
		(result at: key) class = OrderedCollection ifTrue: [ result at: key put: (result at: key) asArray ] ].
	^ result'.

add value: #'OTLPGrpcCollectorDouble class' value: #'readWire:from:' value: 'decoder' value:
'readWire: aWireType from: aReader
	aWireType = 0 ifTrue: [ ^ aReader readVarint ].
	aWireType = 1 ifTrue: [ ^ aReader readFixed64 ].
	aWireType = 2 ifTrue: [ ^ aReader readBytes ].
	aWireType = 5 ifTrue: [ ^ aReader readFixed32 ].
	Error signal: ''Unsupported protobuf wire type '', aWireType printString'.

add value: #'OTLPGrpcCollectorDouble class' value: #'store:spec:into:' value: 'decoder' value:
'store: raw spec: spec into: aDictionary
	| name value |
	name := spec first asString.
	value := self convert: raw kind: (spec at: 2) nested: (spec at: 3).
	(spec at: 4)
		ifTrue: [ (aDictionary at: name ifAbsentPut: [ OrderedCollection new ]) add: value ]
		ifFalse: [ aDictionary at: name put: value ]'.

add value: #'OTLPGrpcCollectorDouble class' value: #'convert:kind:nested:' value: 'decoder' value:
'convert: raw kind: aKind nested: aMessageName
	"The wire type must fit the kind: bytes for str, hex, bytes and msg, a number for the rest."
	| isBytes |
	isBytes := raw isInteger not.
	(#( #str #hex #bytes #msg ) includes: aKind)
		ifTrue: [ isBytes ifFalse: [ Error signal: ''field of kind '', aKind, '' has a numeric wire type'' ] ]
		ifFalse: [ isBytes ifTrue: [ Error signal: ''field of kind '', aKind, '' has a length-delimited wire type'' ] ].
	aKind = #str ifTrue: [ ^ raw utf8Decoded ].
	aKind = #hex ifTrue: [ ^ raw hex ].
	aKind = #bytes ifTrue: [ ^ raw base64Encoded ].
	aKind = #msg ifTrue: [ ^ self decode: raw as: aMessageName ].
	aKind = #int ifTrue: [ ^ raw ].
	aKind = #fx32 ifTrue: [ ^ raw ].
	aKind = #bool ifTrue: [ ^ raw ~= 0 ].
	aKind = #fx64 ifTrue: [ ^ raw printString ].
	(aKind = #i64 or: [ aKind = #sfx64 ])
		ifTrue: [ ^ (raw >= (1 bitShift: 63) ifTrue: [ raw - (1 bitShift: 64) ] ifFalse: [ raw ]) printString ].
	aKind = #dbl ifTrue: [ ^ Float fromIEEE64Bit: raw ].
	Error signal: ''no such kind in the table: '', aKind printString'.

add value: #'OTLPGrpcCollectorDouble class' value: #'schema' value: 'decoder' value:
'schema
	"message name -> (field number -> #(json name, kind, nested message, repeated)).
	 The field numbers are those of opentelemetry-proto, which the encoders of the
	 package write (OTOtlpProtobufTraceEncoder and its log and metric relatives).
	 Kinds: str hex bytes msg int bool i64 (varint as signed decimal string)
	 fx64 (unsigned decimal string) sfx64 (signed decimal string) fx32 dbl."
	^ Schema ifNil: [ Schema := self buildSchema ]'.

add value: #'OTLPGrpcCollectorDouble class' value: #'buildSchema' value: 'decoder' value:
'buildSchema
	| table |
	table := Dictionary new.
	#(
	#( #ExportTraceServiceRequest #( 1 #resourceSpans #msg #ResourceSpans true ) )
	#( #ExportLogsServiceRequest #( 1 #resourceLogs #msg #ResourceLogs true ) )
	#( #ExportMetricsServiceRequest #( 1 #resourceMetrics #msg #ResourceMetrics true ) )
	#( #Resource #( 1 #attributes #msg #KeyValue true ) #( 2 #droppedAttributesCount #int nil false ) )
	#( #Scope #( 1 #name #str nil false ) #( 2 #version #str nil false ) #( 3 #attributes #msg #KeyValue true ) #( 4 #droppedAttributesCount #int nil false ) )
	#( #KeyValue #( 1 #key #str nil false ) #( 2 #value #msg #AnyValue false ) )
	#( #AnyValue #( 1 #stringValue #str nil false ) #( 2 #boolValue #bool nil false ) #( 3 #intValue #i64 nil false ) #( 4 #doubleValue #dbl nil false ) #( 5 #arrayValue #msg #ArrayValue false ) #( 6 #kvlistValue #msg #KeyValueList false ) #( 7 #bytesValue #bytes nil false ) )
	#( #ArrayValue #( 1 #values #msg #AnyValue true ) )
	#( #KeyValueList #( 1 #values #msg #KeyValue true ) )
	#( #ResourceSpans #( 1 #resource #msg #Resource false ) #( 2 #scopeSpans #msg #ScopeSpans true ) #( 3 #schemaUrl #str nil false ) )
	#( #ScopeSpans #( 1 #scope #msg #Scope false ) #( 2 #spans #msg #Span true ) #( 3 #schemaUrl #str nil false ) )
	#( #Span #( 1 #traceId #hex nil false ) #( 2 #spanId #hex nil false ) #( 3 #traceState #str nil false ) #( 4 #parentSpanId #hex nil false ) #( 5 #name #str nil false ) #( 6 #kind #int nil false ) #( 7 #startTimeUnixNano #fx64 nil false ) #( 8 #endTimeUnixNano #fx64 nil false ) #( 9 #attributes #msg #KeyValue true ) #( 10 #droppedAttributesCount #int nil false ) #( 11 #events #msg #SpanEvent true ) #( 12 #droppedEventsCount #int nil false ) #( 13 #links #msg #SpanLink true ) #( 14 #droppedLinksCount #int nil false ) #( 15 #status #msg #SpanStatus false ) #( 16 #flags #fx32 nil false ) )
	#( #SpanEvent #( 1 #timeUnixNano #fx64 nil false ) #( 2 #name #str nil false ) #( 3 #attributes #msg #KeyValue true ) #( 4 #droppedAttributesCount #int nil false ) )
	#( #SpanLink #( 1 #traceId #hex nil false ) #( 2 #spanId #hex nil false ) #( 3 #traceState #str nil false ) #( 4 #attributes #msg #KeyValue true ) #( 5 #droppedAttributesCount #int nil false ) #( 6 #flags #fx32 nil false ) )
	#( #SpanStatus #( 2 #message #str nil false ) #( 3 #code #int nil false ) )
	#( #ResourceLogs #( 1 #resource #msg #Resource false ) #( 2 #scopeLogs #msg #ScopeLogs true ) #( 3 #schemaUrl #str nil false ) )
	#( #ScopeLogs #( 1 #scope #msg #Scope false ) #( 2 #logRecords #msg #LogRecord true ) #( 3 #schemaUrl #str nil false ) )
	#( #LogRecord #( 1 #timeUnixNano #fx64 nil false ) #( 2 #severityNumber #int nil false ) #( 3 #severityText #str nil false ) #( 5 #body #msg #AnyValue false ) #( 6 #attributes #msg #KeyValue true ) #( 7 #droppedAttributesCount #int nil false ) #( 8 #flags #fx32 nil false ) #( 9 #traceId #hex nil false ) #( 10 #spanId #hex nil false ) #( 11 #observedTimeUnixNano #fx64 nil false ) #( 12 #eventName #str nil false ) )
	#( #ResourceMetrics #( 1 #resource #msg #Resource false ) #( 2 #scopeMetrics #msg #ScopeMetrics true ) #( 3 #schemaUrl #str nil false ) )
	#( #ScopeMetrics #( 1 #scope #msg #Scope false ) #( 2 #metrics #msg #Metric true ) #( 3 #schemaUrl #str nil false ) )
	#( #Metric #( 1 #name #str nil false ) #( 2 #description #str nil false ) #( 3 #unit #str nil false ) #( 5 #gauge #msg #Gauge false ) #( 7 #sum #msg #Sum false ) )
	#( #Gauge #( 1 #dataPoints #msg #NumberDataPoint true ) )
	#( #Sum #( 1 #dataPoints #msg #NumberDataPoint true ) #( 2 #aggregationTemporality #int nil false ) #( 3 #isMonotonic #bool nil false ) )
	#( #NumberDataPoint #( 2 #startTimeUnixNano #fx64 nil false ) #( 3 #timeUnixNano #fx64 nil false ) #( 4 #asDouble #dbl nil false ) #( 6 #asInt #sfx64 nil false ) #( 7 #attributes #msg #KeyValue true ) #( 8 #flags #int nil false ) ) )
		do: [ :message | | fields |
			fields := Dictionary new.
			message allButFirst do: [ :spec | fields at: spec first put: spec allButFirst ].
			table at: message first put: fields ].
	^ table'.


"--- the tests start the double ---"

fix value: #OTOtlpGrpcIntegrationTest value: #setUp
	value: [ :source | (source includesSubstring: 'OTLPGrpcCollectorDouble') not ]
	value:
'setUp
	"The collector double listens on 127.0.0.1:4317 and the tests find its output files through the environment."
	OTLPGrpcCollectorDouble ensureStarted.
	super setUp.
	self snapshotOtelEnvironment.
	self clearNonIntegrationOtelEnvironmentValues.
	OTGrpcTestSupport removeFakeGrpcResolvers.
	self resetTelemetryConfiguration'.
