"load-pr.st -- put the change of a pull request branch into the image.
 scripts/upstream-pr/stage-pr.sh writes the stage directory and a load.st that
 sets the global UpstreamPrStageDir and files in this file.

 For each changed Tonel file the stage directory has the file at the merge
 base (base/) and at the head of the branch (pr/).  This file parses the two
 with TonelParser and loads, through MCPackageLoader, only the definitions
 that differ: added, changed and removed classes and methods.  The other
 changes of the branch base since the build of the image stay out.

 Output: <stage dir>/load-log.txt, one line per definition
   ADD | MODIFY | REMOVE <definition>
 then one line per method of the branch that was loaded
   CHECK-OK | CHECK-MISMATCH | CHECK-MISSING <Class>>><selector>
 and a last line DONE <errors> errors.  A LOAD-ERROR line names an error."
| dir log emit parse add modify remove loader errors |
dir := (Smalltalk globals at: #UpstreamPrStageDir) asFileReference.
log := (dir fullName , '/load-log.txt') asFileReference.
log ensureDelete.
emit := [ :text | log writeStreamDo: [ :o | o setToEnd; nextPutAll: text; nextPut: Character lf ] ].
parse := [ :name | | ref |
	ref := (dir fullName , '/' , name) asFileReference.
	ref exists
		ifTrue: [ (TonelParser parseString: ref contents) asOrderedCollection reject: [ :d | d isNil ] ]
		ifFalse: [ OrderedCollection new ] ].
add := OrderedCollection new.
modify := OrderedCollection new.
remove := OrderedCollection new.
errors := 0.
((dir fullName , '/files.txt') asFileReference contents lines reject: [ :l | l trimBoth isEmpty ]) do: [ :line |
	| path baseDefs prDefs |
	path := (line substrings: (String with: Character tab)) last trimBoth.
	baseDefs := parse value: 'base/' , path.
	prDefs := parse value: 'pr/' , path.
	prDefs do: [ :d | | old |
		old := baseDefs detect: [ :b | b description = d description ] ifNone: [ nil ].
		old
			ifNil: [ add add: d ]
			ifNotNil: [ old = d ifFalse: [ modify add: old -> d ] ] ].
	baseDefs do: [ :b |
		(prDefs anySatisfy: [ :d | d description = b description ]) ifFalse: [ remove add: b ] ] ].
add do: [ :d | emit value: 'ADD ' , d summary ].
modify do: [ :a | emit value: 'MODIFY ' , a value summary ].
remove do: [ :d | emit value: 'REMOVE ' , d summary ].
loader := MCPackageLoader new.
add do: [ :d | loader addDefinition: d ].
modify do: [ :a | loader modifyDefinition: a key to: a value ].
remove do: [ :d | loader removeDefinition: d ].
[ [ loader load ]
	on: Warning do: [ :w | emit value: 'WARNING ' , w class name , ': ' , (w messageText ifNil: [ '' ]). w resume ] ]
	on: Error do: [ :e | errors := errors + 1. emit value: 'LOAD-ERROR ' , e class name , ': ' , (e messageText ifNil: [ '' ]) ].
"Check: each added or changed method of the branch is in the image with the source of the branch."
(add , (modify collect: [ :a | a value ])) do: [ :d |
	d isMethodDefinition ifTrue: [ | cls m label |
		cls := Smalltalk globals at: d className ifAbsent: [ nil ].
		d classIsMeta ifTrue: [ cls := cls ifNotNil: [ :c | c classSide ] ].
		label := d className , (d classIsMeta ifTrue: [ ' class' ] ifFalse: [ '' ]) , '>>' , d selector.
		m := cls ifNotNil: [ :c | c compiledMethodAt: d selector ifAbsent: [ nil ] ].
		m
			ifNil: [ errors := errors + 1. emit value: 'CHECK-MISSING ' , label ]
			ifNotNil: [
				(m sourceCode lines collect: [ :l | l trimRight ]) = (d source lines collect: [ :l | l trimRight ])
					ifTrue: [ emit value: 'CHECK-OK ' , label ]
					ifFalse: [ errors := errors + 1. emit value: 'CHECK-MISMATCH ' , label ] ] ] ].
remove do: [ :d |
	d isMethodDefinition ifTrue: [ | cls label |
		cls := Smalltalk globals at: d className ifAbsent: [ nil ].
		d classIsMeta ifTrue: [ cls := cls ifNotNil: [ :c | c classSide ] ].
		label := d className , (d classIsMeta ifTrue: [ ' class' ] ifFalse: [ '' ]) , '>>' , d selector.
		(cls notNil and: [ cls includesSelector: d selector ])
			ifTrue: [ errors := errors + 1. emit value: 'CHECK-NOT-REMOVED ' , label ]
			ifFalse: [ emit value: 'CHECK-REMOVED ' , label ] ] ].
emit value: 'DONE ' , errors printString , ' errors'.
