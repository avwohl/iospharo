"Add Character>>bitShift: to unblock SHA1>>processBuffer: when the
primSecureHash primitive is absent (Pharo 13 expects either the prim
or stock-VM behaviour where ByteString>>at: returns Integer; our VM
returns Character per spec, so the pure-Smalltalk fallback needs the
conversion)."

Character compile: 'bitShift: anInteger
	^ self asInteger bitShift: anInteger' classified: 'pharo-vm patch'!

Character compile: 'bitAnd: anInteger
	^ self asInteger bitAnd: anInteger' classified: 'pharo-vm patch'!

Character compile: 'bitOr: anInteger
	^ self asInteger bitOr: anInteger' classified: 'pharo-vm patch'!

Character compile: 'bitXor: anInteger
	^ self asInteger bitXor: anInteger' classified: 'pharo-vm patch'!

"Numeric coercion: when Integer + Character is sent, Integer asks the
Character to adaptTo... — without this Character becomes a DNU.
Adapt by converting self to Integer."
Character compile: 'adaptToInteger: rcvr andSend: selector
	^ rcvr perform: selector with: self asInteger' classified: 'pharo-vm patch'!

Character compile: 'adaptToFloat: rcvr andSend: selector
	^ rcvr perform: selector with: self asInteger asFloat' classified: 'pharo-vm patch'!

Character compile: 'adaptToNumber: rcvr andSend: selector
	^ rcvr perform: selector with: self asInteger' classified: 'pharo-vm patch'!

"Smoke-test."
Transcript show: 'Character>>bitShift: 8 yields '; show: ($A bitShift: 8) printString; cr.
Transcript show: '10 + $A yields '; show: (10 + $A) printString; cr!
