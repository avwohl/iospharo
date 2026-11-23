"socket-listen-bind-result.st -- a second UDP socket takes a port that a first
 socket has (Socket>>setPort:).  The UDP lines are the proof: stock Cog gives
 no error on macOS, patched Cog signals SocketError (Address already in use).
 The TCP lines are a CONTROL only.  Socket>>listenOn:backlogSize: uses
 primitiveSocketBindToPort, which already tests bind(), so the TCP answer is
 the same on both VMs.  The answer is a short text."
| out try first second port |
out := WriteStream on: String new.
try := [ :block |
	[ block value ifNil: [ 'no error' ] ]
		on: Error
		do: [ :e | e class name , ': ' , (e messageText ifNil: [ '' ]) asString ] ].
first := Socket newUDP.
second := Socket newUDP.
[ first setPort: 0.
  port := first localPort.
  out nextPutAll: 'UDP: the first socket has a port: '; print: port > 0; lf.
  out nextPutAll: 'UDP: second socket, setPort: of the same port -> ';
	nextPutAll: (try value: [ second setPort: port. nil ]); lf.
  out nextPutAll: 'UDP: the second socket has the port of the first: ';
	nextPutAll: (try value: [ (second localPort = port) printString ]); lf ]
	ensure: [ first closeAndDestroy. second closeAndDestroy ].
first := Socket newTCP.
second := Socket newTCP.
[ first listenOn: 0 backlogSize: 4.
  port := first localPort.
  out nextPutAll: 'TCP control: the first socket has a port: '; print: port > 0; lf.
  out nextPutAll: 'TCP control: second socket, listenOn: of the same port -> ';
	nextPutAll: (try value: [ second listenOn: port backlogSize: 4. nil ]); lf.
  out nextPutAll: 'TCP control: the second socket has the port of the first: ';
	nextPutAll: (try value: [ (second localPort = port) printString ]); lf.
  out nextPutAll: 'TCP control: the second socket waits for a connection: ';
	nextPutAll: (try value: [ second isWaitingForConnection printString ]); lf ]
	ensure: [ first closeAndDestroy. second closeAndDestroy ].
out contents
