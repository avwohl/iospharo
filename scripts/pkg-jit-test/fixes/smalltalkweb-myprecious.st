"smalltalkweb-myprecious: one defect of MyPrecious, three symptoms.

MpConnection>>startReceiving starts a TKTParameterizableService that runs
'self receiveObject' with stepDelay 0 milliSecond.  When the other side
closes a connection, SocketStream>>next: does not signal: its documented
rule is that a short answer means a closed connection (SocketStream>>
readInto:startingAt:count: swallows ConnectionClosed).  So receiveBytes
answers an empty ByteArray, asInteger gives 0, and receiveObject schedules a
task that tries to materialize 0 bytes.  That task fails in a TaskIt worker,
and the default TaskIt handler (TKTExceptionHandler>>handleException:) sends
'debug'.  The service then runs again with a 0 ms Delay, so it never ends:
each round makes one more failing task.  Nothing stops the service,
because a closed connection is not a reason to stop in receiveObject: the
'on: Exception do: [ ^self ]' there is meant for the read timeout of an
idle connection and also swallows the closed connection.

  Our VM, testReconnectsIfOtherSideIsDisconnected in MpWebMiddlewareTest:
  the failing tasks are unhandled in forked processes during the test, and
  ProcessMonitorTestService makes the test fail with TestFailedByForkedProcess.
  Stock Cog, same test in MpTcpMiddlewareTest, then MpWebMiddlewareTest: the
  Delay of 0 ms does not wait and Cog has no time slicing, so the service at
  priority 30 never lets the listener of the other middleware (priority 30)
  run.  The retries fail (MpCouldNotDeliverMessageError after 20 s) and the
  queue of failing tasks grows until old space is full.

The first entry makes receiveBytes signal MpConnectionFailed when fewer
bytes than asked come back.  The second entry makes receiveObject stop the
reception service of a connection that is closed (the exception is
ConnectionClosed or MpConnectionFailed, or the connection cannot send any
more); a read timeout of an open connection still goes on.  The test
checks that the reception service of B stops when A closes the connection."

add value: #MpSocketStreamConnection value: #receiveExactly: value: 'receiving' value:
'receiveExactly: count
	"Read count bytes.  SocketStream>>next: answers fewer bytes, and does not signal, when the other side has closed the connection: that is the end of the connection."

	| bytes |
	bytes := socketStream next: count.
	bytes size < count ifTrue: [ ^ MpConnectionFailed signal: ''Connection closed by the other side'' ].
	^ bytes'.

fix value: #MpSocketStreamConnection value: #receiveBytes
	value: [ :source | source includesSubstring: 'socketStream next: self byteCountArraySize' ]
	value:
'receiveBytes
	"Read one message: the size of the size, the size, then the bytes."

	| bytesCountSize bytesCount |
	bytesCountSize := (self receiveExactly: self byteCountArraySize) asInteger.
	bytesCount := (self receiveExactly: bytesCountSize) asInteger.
	^ self receiveExactly: bytesCount'.

fix value: #MpConnection value: #receiveObject
	value: [ :source | source includesSubstring: 'on: Exception do: [ ^self ]' ]
	value:
'receiveObject
	| receivedBytes receivedObject |
	receivedBytes := [ self receiveBytes ] on: Exception do: [ :exception |
		"A read timeout of an open connection goes on.  A closed connection can never receive again: stop the reception service, else it runs again at once and for ever."
		((exception isKindOf: ConnectionClosed)
			or: [ (exception isKindOf: MpConnectionFailed)
			or: [ self isSendingPossible not ] ])
				ifTrue: [ self stopReceiving ].
		^ self ].
	self schedule: [
		receivedObject := self materializeAndUnmarshal: receivedBytes.
		receivedObject deliverBy: self
	]'.

add value: #MpTcpMiddlewareTest value: #testStopsReceivingWhenOtherSideClosesConnection value: 'tests' value:
'testStopsReceivingWhenOtherSideClosesConnection
	"When A closes its end of a connection, the reception service of the connection in B must stop.  It used to run again at once and for ever."

	| valueHolderA remoteReferenceA connectionA connectionB |
	valueHolderA := self newValueHolder.
	remoteReferenceA := middlewareA export: valueHolderA.
	middlewareB send: #contents to: remoteReferenceA.
	connectionA := middlewareA transporter connections first.
	connectionB := middlewareB transporter connections first.
	self assert: connectionB isReceptionStarted.
	connectionA disconnect.
	self waitWhile: [ connectionB isReceptionStarted ] timeout: 2 seconds.
	self deny: connectionB isReceptionStarted'.
