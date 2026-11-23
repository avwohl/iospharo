"Probe for the Cog PR socket-local-address-any-interface.
 Asks the VM primitive directly, without the image fallback to 127.0.0.1.
 Stock VM on macOS, or on Linux with predictable interface names:
   LOCAL-ADDRESS=#[0 0 0 0]  USABLE=false
 Patched VM on a machine with an interface that is up and not the loopback:
   LOCAL-ADDRESS=<address of that interface>  USABLE=true"
| address |
NetNameResolver initializeNetwork.
address := NetNameResolver primLocalAddress.
'LOCAL-ADDRESS=' , address printString , '  USABLE=' , (address asArray ~= #(0 0 0 0) and: [ address first ~= 127 ]) printString
