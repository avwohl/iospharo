"UnixResolverTest: the three tests skip when OSPlatform current isUnix is false.
 MacOSXPlatform is a MacOSPlatform, not a UnixPlatform, so isUnix is false on macOS.
 UnixResolver is plain POSIX code (HOME, TMPDIR, XDG files) and works on macOS; the
 test comments say they can run on any platform.  Skip only when neither Unix nor macOS."
fix value: #UnixResolverTest value: #testGetTempFromTMPDIR
	value: [ :s | (s includesSubstring: 'isUnix ifFalse:') ]
	value: 'testGetTempFromTMPDIR

	| expected |
	(OSPlatform current isUnix or: [ OSPlatform current isMacOS ]) ifFalse: [ ^ self skip ].
	
	expected := ''/tmp/foo''.
	OSEnvironment current setEnv: ''TMPDIR'' value: expected during: [ 
		| actual |
		actual := self createResolver temp.
		self assert: actual fullName equals: expected ]
'.
fix value: #UnixResolverTest value: #testXdgParseUserDirLineDocuments
	value: [ :s | (s includesSubstring: 'isUnix ifFalse:') ]
	value: 'testXdgParseUserDirLineDocuments
	"Ensure that a path of the form ''$HOME/Documents'' answers the expected value.
	Note that this test can be run on any platform."
	| stream path relativePath |
	
	(OSPlatform current isUnix or: [ OSPlatform current isMacOS ]) ifFalse: [ ^ self skip ].
	stream := ''"$HOME/Documents"'' readStream.
	path := resolver xdgParseUserDirLine: stream.
	relativePath := path relativeTo: resolver home.
	self assert: relativePath pathString equals: ''Documents''
'.
fix value: #UnixResolverTest value: #testXdgParseUserDirLineNotRoot
	value: [ :s | (s includesSubstring: 'isUnix ifFalse:') ]
	value: 'testXdgParseUserDirLineNotRoot
	"Ensure that a path of the form ''$HOME/'' doesn''t resolve to the root directory.
	Note that this test can be run on any platform."
	| stream path |

	(OSPlatform current isUnix or: [ OSPlatform current isMacOS ]) ifFalse: [ ^ self skip ].
	stream := ''"$HOME/"'' readStream.
	path := resolver xdgParseUserDirLine: stream.
	self deny: path isRoot
'.
