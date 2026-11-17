"socket_options.st -- the socket options of the image, on a UDP socket and on
 a TCP socket.  The answer has one line for each call, so a diff of the
 output of two VMs shows each option that the VMs handle in different ways.

   scripts/vmrun.sh eval cog  scripts/cogdiff/socket_options.st
   scripts/vmrun.sh eval ours scripts/cogdiff/socket_options.st

 The names are the table of the stock VM
 (plugins/SocketPlugin/src/common/SocketPluginImpl.c, socketOptions[]), two
 names that the table does not have (SO_PRIORITY, NO_SUCH_OPTION), and the
 calls that a multicast socket makes.

 Measured 2026-11-17 (Pharo 14 build 791, macOS 27): 72 lines.  Stock Cog and
 our VM give the same answer for 68 of them.  The 4 lines that differ are two
 defects of the stock VM, each on the UDP and on the TCP socket:
   set SO_RCVBUF := 65536   stock Cog answers #(0 0), and the option then
                            reads 8388608.  The stock VM reads an integer
                            only from a value of 4 characters or less, and
                            gives '65536' to setsockopt as 5 characters.
   set SO_LINGER := 5       stock Cog answers nil (the primitive fails): it
                            gives setsockopt one int, and the system needs a
                            struct linger.
 Before the repair of 2026-11-17 our VM knew 6 names and answered #(0 0) for
 each other name."
| names out try run udp tcp |
names := #( 'SO_DEBUG' 'SO_REUSEADDR' 'SO_DONTROUTE' 'SO_BROADCAST' 'SO_SNDBUF' 'SO_RCVBUF' 'SO_KEEPALIVE'
	'SO_OOBINLINE' 'SO_LINGER' 'IP_TTL' 'IP_HDRINCL' 'IP_MULTICAST_IF' 'IP_MULTICAST_TTL' 'IP_MULTICAST_LOOP'
	'IP_ADD_MEMBERSHIP' 'TCP_MAXSEG' 'TCP_NODELAY' 'SO_REUSEPORT' 'SO_PRIORITY' 'NO_SUCH_OPTION' ).
out := WriteStream on: String new.
try := [ :block | [ block value printString ] on: Error do: [ :e | 'error ' , e class name ] ].
run := [ :label :socket |
	names do: [ :each |
		out nextPutAll: label; nextPutAll: ' get '; nextPutAll: each; nextPutAll: ' -> ';
			nextPutAll: (try value: [ socket getOption: each ]); lf ].
	{ 'SO_REUSEADDR' -> true. 'SO_REUSEPORT' -> true. 'SO_BROADCAST' -> true. 'SO_KEEPALIVE' -> true.
	  'IP_TTL' -> 7. 'IP_MULTICAST_TTL' -> 3. 'IP_MULTICAST_LOOP' -> false.
	  'IP_MULTICAST_IF' -> #[ 127 0 0 1 ].
	  'IP_ADD_MEMBERSHIP' -> #[ 224 0 0 251 127 0 0 1 ].
	  'IP_DROP_MEMBERSHIP' -> #[ 224 0 0 251 127 0 0 1 ].
	  'TCP_NODELAY' -> 1. 'SO_SNDBUF' -> 8192. 'SO_RCVBUF' -> 65536. 'SO_LINGER' -> 5.
	  'SO_PRIORITY' -> 1. 'NO_SUCH_OPTION' -> 1 } do: [ :each |
		out nextPutAll: label; nextPutAll: ' set '; nextPutAll: each key; nextPutAll: ' := '; print: each value;
			nextPutAll: ' -> '; nextPutAll: (try value: [ socket setOption: each key value: each value ]);
			nextPutAll: '  then get -> '; nextPutAll: (try value: [ socket getOption: each key ]); lf ] ].
udp := Socket newUDP.
tcp := Socket newTCP.
[ run value: 'udp' value: udp.
  run value: 'tcp' value: tcp ] ensure: [ udp closeAndDestroy. tcp closeAndDestroy ].
out contents
