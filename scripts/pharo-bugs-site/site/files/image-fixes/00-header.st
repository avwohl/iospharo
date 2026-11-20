"00-header.st -- the helper blocks for the entries files of this directory.
 scripts/gen-image-fixes.py puts this header in front of each entries file,
 so an entries file holds entries only.

 fix value: #Class value: #selector value: [ :source | <true when the defect
     is in the current source> ] value: '<new source of the method>'.
   Replaces one existing method.  Use #'Foo class' as the class symbol for a
   class-side method.
 add value: #Class value: #selector value: '<protocol>' value: '<source>'.
   Compiles a method that the class does not have yet.
 Each entry prints one line: FIXED, ADDED or SKIP with the reason."
| fix add resolve |
resolve := [ :clsName | | base |
	(clsName endsWith: ' class')
		ifTrue: [ base := Smalltalk globals at: (clsName allButLast: 6) asSymbol ifAbsent: [ nil ]. base ifNotNil: [ base class ] ]
		ifFalse: [ Smalltalk globals at: clsName asSymbol ifAbsent: [ nil ] ] ].
fix := [ :clsName :sel :guard :src | | cls |
	cls := resolve value: clsName.
	(cls notNil and: [ cls includesSelector: sel ])
		ifFalse: [ Stdio stdout nextPutAll: 'SKIP ', clsName, '>>', sel, ' (absent)'; lf; flush ]
		ifTrue: [
			(guard value: (cls >> sel) sourceCode)
				ifFalse: [ Stdio stdout nextPutAll: 'SKIP ', clsName, '>>', sel, ' (defect not present)'; lf; flush ]
				ifTrue: [
					cls compile: src classified: (cls >> sel) protocolName.
					Stdio stdout nextPutAll: 'FIXED ', clsName, '>>', sel; lf; flush ] ] ].
add := [ :clsName :sel :protocol :src | | cls |
	cls := resolve value: clsName.
	cls
		ifNil: [ Stdio stdout nextPutAll: 'SKIP ', clsName, '>>', sel, ' (class absent)'; lf; flush ]
		ifNotNil: [
			(cls includesSelector: sel)
				ifTrue: [ Stdio stdout nextPutAll: 'SKIP ', clsName, '>>', sel, ' (already present)'; lf; flush ]
				ifFalse: [
					cls compile: src classified: protocol.
					Stdio stdout nextPutAll: 'ADDED ', clsName, '>>', sel; lf; flush ] ] ].

