"socket_bind_in_use.st -- a second socket takes a port that a first socket
 has, for UDP (setPort:) and for TCP (listenOn:backlogSize:).

   scripts/vmrun.sh eval cog  scripts/cogdiff/socket_bind_in_use.st
   scripts/vmrun.sh eval cogp scripts/cogdiff/socket_bind_in_use.st
   scripts/vmrun.sh eval ours scripts/cogdiff/socket_bind_in_use.st

 The system refuses the second bind (Address already in use).  The answer
 shows if the image gets that error, and which port the second socket has.

 Measured 2026-11-17 (Pharo 14 build 791):
   stock Cog   UDP: no error, and the second socket does not have the port.
               TCP: SocketError: Address already in use.
   our VM      UDP and TCP: SocketError: Address already in use.
 The stock VM does not test the result of bind in
 sqSocketListenOnPortBacklogSizeInterface, which Socket>>setPort: uses."
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
  out nextPutAll: 'TCP: the first socket has a port: '; print: port > 0; lf.
  out nextPutAll: 'TCP: second socket, listenOn: of the same port -> ';
	nextPutAll: (try value: [ second listenOn: port backlogSize: 4. nil ]); lf.
  out nextPutAll: 'TCP: the second socket has the port of the first: ';
	nextPutAll: (try value: [ (second localPort = port) printString ]); lf.
  out nextPutAll: 'TCP: the second socket waits for a connection: ';
	nextPutAll: (try value: [ second isWaitingForConnection printString ]); lf ]
	ensure: [ first closeAndDestroy. second closeAndDestroy ].
out contents
