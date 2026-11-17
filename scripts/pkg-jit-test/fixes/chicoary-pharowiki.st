"PharoWiki (chicoary/PharoWiki, main): seven tests skip, none for a machine reason.

1. PWikiGeneratorRegistryTest>>testGeneratorForChangesClass and
   testGeneratorForLibsClass skip when the Epicea log of the image has no
   EpClassAddition entry.  Epicea does not log code that Metacello loads
   (EpMonitor class>>shouldLogMetacelloLoading is false by default) and does not log
   during a test (EpMonitor>>subscribeToSystemAnnouncer), so in a loaded image the log
   is empty and both tests skip.  The tests become independent of the log of
   the image: each one installs an EpMonitor with a memory log that holds one
   EpClassAddition, and restores the monitor afterwards.

2. PWikiGeneratorTest has five tests with an unconditional self skip.  The
   tests work on Point and call the generator with the real
   SystemNavigation scan for implementors and senders of each selector sent in
   Point.  The tests use a stub extractor now (the scan is not under test), and
   testGeneratePointExtensionsSection does not expect the *Geometry
   extension asGVector of the machine of the author: it takes the last
   extension method that Point has in the image."

add value: #PWikiGeneratorRegistryTest value: #withEpiceaLogAdding:do: value: 'private'
	value:
'withEpiceaLogAdding: aClass do: aBlock
	"Run aBlock with an Epicea monitor whose log holds one EpClassAddition for aClass.
	 The log of the image is empty after a Metacello load, because Epicea does not log it."
	| saved |
	saved := EpMonitor hasCurrent ifTrue: [ EpMonitor current ].
	EpMonitor current: (EpMonitor newWithLog: (EpLog newWithEvents: { EpClassAddition class: aClass })).
	^ aBlock ensure: [ EpMonitor current: saved ]'.

fix value: #PWikiGeneratorRegistryTest value: #testGeneratorForChangesClass
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testGeneratorForChangesClass
	"generatorFor: returns changesGen for a project class registered in Epicea."
	| registry |
	registry := self buildRegistry.
	self
		withEpiceaLogAdding: PWikiClassPage
		do: [ self assert: (registry generatorFor: PWikiClassPage) == registry changesGen ]'.

fix value: #PWikiGeneratorRegistryTest value: #testGeneratorForLibsClass
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testGeneratorForLibsClass
	"generatorFor: returns libsGen for a class registered in Epicea that is not in a project package."
	| registry |
	registry := self buildRegistry.
	self
		withEpiceaLogAdding: Point
		do: [ self assert: (registry generatorFor: Point) == registry libsGen ]'.

add value: #PWikiGenerator value: #extractor: value: 'accessing'
	value:
'extractor: aDependencyExtractor
	extractor := aDependencyExtractor'.

add value: #PWikiGeneratorTest value: #stubExtractor value: 'private'
	value:
'stubExtractor
	"An extractor without the scan of the whole image: the tests check the generated pages, not SystemNavigation."
	| class |
	class := PWikiDependencyExtractor newAnonymousSubclass.
	class compile: ''implementorsOf: aSelector ^ #()''.
	class compile: ''sendersOf: aSelector ^ #()''.
	^ class new'.

fix value: #PWikiGeneratorTest value: #setUp
	value: [ :source | (source includesSubstring: 'extractor:') not ]
	value:
'setUp
	outputDir := FileSystem memory root.
	generator := PWikiSourcesGenerator outputDir: outputDir.
	generator extractor: self stubExtractor'.

fix value: #PWikiGeneratorTest value: #testExtensionsNotDuplicated
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testExtensionsNotDuplicated
	"Given a class with extension protocols (Point)
	 When rendering its page
	 Then the Extensions section appears exactly once"
	| page content occurrences |
	page := PWikiClassPage forClass: Point.
	self assert: page extensionProtocolNames isNotEmpty.
	content := generator renderPage: page.
	occurrences := 0.
	content linesDo: [ :line |
		(line trimmed = ''## Extensions'') ifTrue: [ occurrences := occurrences + 1 ] ].
	self assert: occurrences equals: 1'.

fix value: #PWikiGeneratorTest value: #testGeneratePointExtensionsSection
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testGeneratePointExtensionsSection
	"Given a PWikiSourcesGenerator writing to a memory filesystem
	 When generating the Point class page
	 Then the file ends with the Extensions section: the last method of the last extension protocol of Point"
	| page protocol method content expected |
	page := PWikiClassPage forClass: Point.
	protocol := page extensionProtocolNames reversed
		detect: [ :each | (page methodsInProtocol: each) isNotEmpty ].
	method := (page methodsInProtocol: protocol) last.
	generator generateForClass: Point.
	content := (outputDir / ''Kernel'' / ''Point.md'') readStream contents.
	expected := String streamContents: [ :s |
		s
			nextPutAll: method sourceCode; cr;
			nextPutAll: ''```''; cr ].
	self assert: (content includesSubstring: ''## Extensions'').
	self assert: (content includesSubstring: ''### '', protocol).
	self assert: (content includesSubstring: ''#### '', method selector).
	self assert: (content last: expected size) equals: expected'.

fix value: #PWikiGeneratorTest value: #testGeneratedFileContainsClassName
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testGeneratedFileContainsClassName
	"Given the generator
	 When generating for the class Point
	 Then the file contains the class name"
	| content |
	generator generateForClass: Point.
	content := (outputDir / ''Kernel'' / ''Point.md'') readStream contents.
	self assert: (content includesSubstring: ''Point'')'.

fix value: #PWikiGeneratorTest value: #testGeneratedFileNotEmpty
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testGeneratedFileNotEmpty
	"Given the generator
	 When generating for the class Point
	 Then the resulting file is not empty"
	| content |
	generator generateForClass: Point.
	content := (outputDir / ''Kernel'' / ''Point.md'') readStream contents.
	self assert: content isNotEmpty'.

fix value: #PWikiGeneratorTest value: #testGeneratesFileForPoint
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testGeneratesFileForPoint
	"Given the generator configured with an output dir
	 When generating for the class Point
	 Then a .md file exists for Point"
	generator generateForClass: Point.
	self assert: (outputDir / ''Kernel'' / ''Point.md'') exists'.

