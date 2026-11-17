"pharo-contributions/OSSubprocess (master): causes of the 26 errors, one entry group each.

1. OldFileStream class>>forceNewFileNamed: (and OldStandardFileStream, OldMultiByteFileStream)
 of the package send SessionManager default currentSession isReadOnlyAccessMode.
 Pharo 14 WorkingSession has no such method (the read-only session mode was removed),
 so each file-based subprocess that redirects a stream (temp file for stdout, stderr or
 stdin) raises MessageNotUnderstood: 18 tests, and the two testStopWaiting tests of the
 file-based class, where the error happens in the forked process (TestFailedByForkedProcess).
 The entry adds WorkingSession>>isReadOnlyAccessMode answering false: a Pharo 14 image
 session is never read-only.

2. OSSUnixSubprocessTest>>withNonAsciiDirectoryDo: sends UUIDGenerator next.
 Pharo 14 UUIDGenerator has no class-side next; the supported API is UUID new.
 4 tests (two methods, two test classes).  The entry uses UUID new asString.

3. testSigTerm and testSigTermInsideLoop send Smalltalk image changesName.
 Pharo 14 SmalltalkImage has no changesName (PharoFilesOpener>>changesName answers only
 the base name).  The error is inside the forked block for testSigTerm
 (TestFailedByForkedProcess) and in the test body for testSigTermInsideLoop.  The entry
 uses FileLocator changes fullName, the full path of the changes file, so tail -f finds
 the file whatever the working directory is.

Each entry has a guard that is true only for the old text, so a second file-in changes nothing."

add value: #WorkingSession value: #isReadOnlyAccessMode value: 'testing'
	value:
'isReadOnlyAccessMode
	"The package OSSubprocess (OldFileStream) asks the session whether files may be written.
	A Pharo 14 session is never read-only."

	^ false'.

fix value: #OSSUnixSubprocessTest value: #withNonAsciiDirectoryDo:
	value: [ :source | source includesSubstring: 'UUIDGenerator next asString' ]
	value: ((Smalltalk globals at: #OSSUnixSubprocessTest ifAbsent: [ nil ])
		ifNil: [ '' ]
		ifNotNil: [ :cls | (cls includesSelector: #withNonAsciiDirectoryDo:)
			ifTrue: [ (cls >> #withNonAsciiDirectoryDo:) sourceCode copyReplaceAll: 'UUIDGenerator next asString' with: 'UUID new asString' ]
			ifFalse: [ '' ] ]).
fix value: #OSSUnixSubprocessTest value: #testSigTerm
	value: [ :source | source includesSubstring: 'Smalltalk image changesName' ]
	value: ((Smalltalk globals at: #OSSUnixSubprocessTest ifAbsent: [ nil ])
		ifNil: [ '' ]
		ifNotNil: [ :cls | (cls includesSelector: #testSigTerm)
			ifTrue: [ (cls >> #testSigTerm) sourceCode copyReplaceAll: 'Smalltalk image changesName' with: 'FileLocator changes fullName' ]
			ifFalse: [ '' ] ]).
fix value: #OSSUnixSubprocessTest value: #testSigTermInsideLoop
	value: [ :source | source includesSubstring: 'Smalltalk image changesName' ]
	value: ((Smalltalk globals at: #OSSUnixSubprocessTest ifAbsent: [ nil ])
		ifNil: [ '' ]
		ifNotNil: [ :cls | (cls includesSelector: #testSigTermInsideLoop)
			ifTrue: [ (cls >> #testSigTermInsideLoop) sourceCode copyReplaceAll: 'Smalltalk image changesName' with: 'FileLocator changes fullName' ]
			ifFalse: [ '' ] ]).
fix value: #OSSFileBasedUnixSubprocessTest value: #testSigTerm
	value: [ :source | source includesSubstring: 'Smalltalk image changesName' ]
	value: ((Smalltalk globals at: #OSSFileBasedUnixSubprocessTest ifAbsent: [ nil ])
		ifNil: [ '' ]
		ifNotNil: [ :cls | (cls includesSelector: #testSigTerm)
			ifTrue: [ (cls >> #testSigTerm) sourceCode copyReplaceAll: 'Smalltalk image changesName' with: 'FileLocator changes fullName' ]
			ifFalse: [ '' ] ]).
