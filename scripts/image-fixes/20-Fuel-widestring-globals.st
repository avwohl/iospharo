"Fuel-widestring-globals.  Fuel wrote the name of a global (a class name, a
 global key) with encodeString:, which stores one byte per character.  A name
 with a character above 255 (WideString, WideSymbol) lost its high bytes, so
 the materializer looked up a wrong name (FLGlobalNotFound, FLClassNotFound).
 Global names are now written as UTF-8 and read back as UTF-8.  For an ASCII
 name the bytes are the same as before, so old files still load.  A name with
 bytes 128-255 that is not valid UTF-8 (a file from before this change) is
 read as Latin-1, as before."
add value: #FLGlobalCluster value: #encodeGlobalName:on: value: 'serialization'
	value:
'encodeGlobalName: aName on: anEncoder
	"Write the name of a global as UTF-8.  Same bytes as encodeString: for an ASCII name."

	anEncoder encodeString: aName asString utf8Encoded asString'.
add value: #FLGlobalCluster value: #nextGlobalNameFrom: value: 'serialization'
	value:
'nextGlobalNameFrom: aDecoder
	"Read a global name written by #encodeGlobalName:on:.  Bytes that are not valid UTF-8 come from an older file and are Latin-1."

	| string |
	string := aDecoder nextEncodedString.
	^ ([ string asByteArray utf8Decoded ]
		on: Error
		do: [ :error | string ]) asSymbol'.
fix value: #FLGlobalCluster value: #serializeGlobalClass:on:
	value: [ :source | source includesSubstring: 'anEncoder encodeString: aClass name' ]
	value:
'serializeGlobalClass: aClass on: anEncoder
	self encodeGlobalName: aClass name on: anEncoder'.
fix value: #FLGlobalCluster value: #serializeGlobalAssociationKeyed:on:
	value: [ :source | source includesSubstring: 'anEncoder encodeString: aSymbol' ]
	value:
'serializeGlobalAssociationKeyed: aSymbol on: anEncoder

	self encodeGlobalName: aSymbol on: anEncoder'.
fix value: #FLGlobalCluster value: #materializeGlobalAssociationFrom:
	value: [ :source | source includesSubstring: 'nextEncodedString asSymbol' ]
	value:
'materializeGlobalAssociationFrom: aDecoder

	| globalName |
	globalName := self nextGlobalNameFrom: aDecoder.
	^ aDecoder globalEnvironment
		associationAt: globalName
		ifAbsent: [ FLGlobalNotFound signalWithName: globalName ]'.
fix value: #FLGlobalCluster value: #materializeGlobalClassFrom:
	value: [ :source | source includesSubstring: 'nextEncodedString asSymbol' ]
	value:
'materializeGlobalClassFrom: aDecoder

	| className |
	className := self nextGlobalNameFrom: aDecoder.
	^ aDecoder classNamed: className'.
"testWideStringGlobal put a WideString into a SystemEnvironment as key, which
 only accepts Symbols (SystemEnvironment>>at:put:), and then expected the
 materialized VALUE to be the KEY.  The key is now a WideSymbol and the value
 expected is the global value, the object found by that key."
fix value: #FLBasicSerializationTest value: #testWideStringGlobal
	value: [ :source | source includesSubstring: 'yourself.' ]
	value:
'testWideStringGlobal
	| global globalValue |
	global := (''Foox'' asWideString
		at: 4 put: (Character value: 265);
		yourself) asSymbol.
	globalValue := global asString , ''value''.
	self environmentOfTest
		at: global
		put: globalValue.
	addedGlobals add: global.
	self serializer lookUpInGlobals: global.

	self shouldnt: [ self serialize: ''bar'' -> globalValue ] raise: Error.
	self assert: self materialized class equals: Association.
	self assert: self materialized value identicalTo: globalValue'.
"The three tests pass now: no expected failures remain."
fix value: #FLSerializationTest value: #expectedFailures
	value: [ :source | source includesSubstring: 'testWideStringGlobal' ]
	value:
'expectedFailures
	^ #()'.
