"pillar-markup-microdown: repairs for the 21 tests that do not pass
(1 failure, 3 errors, 17 skips; 1605 tests; same on Cog and on our VM).
The record of the repair: docs/upstream/package-repairs.md

Cause of each entry, in plain words:

A. MicMetaDataBlockTest>>testCreatingAnInstanceInitializeTheMetadatDictionary
   (error 'only integers should be used as indices').  The entry 20 of
   scripts/image-fixes (our own repair of the upstream 'self skip' test) calls
   'metadata at: ''authors''' on a MicMetaDataBlock.  The block has no #at:
   (Object>>#at: is the indexed access); the accessor is #atKey:.  The
   repair of the repair.  The coordinator should also correct
   scripts/image-fixes/20-MicMetaDataBlockTest.st.

B. MicSyncTesterStrategy>>verify: (2 errors, MicMethodBodySyncTest
   testCodeblockIsNotInSyncCheckingResultAttributes and
   testReportsCodeblockIsNotInSync).  The package calls
   'Smalltalk image lastUpdateString'.  That method was removed from the
   Pharo image (no implementor in Pharo 14; MicDesynchronizedCodeResult
   already has the default '>12' for this field).  The repair writes the
   version of the running image with 'SystemVersion current printString'.

C. MicBookTesterVisitorTest>>testExampleEmptyCodeblockWithTwoBrackets
   (failure).  The test expects the message '#key was sent to nil', which the
   old compiler gave for an empty source.  The Pharo 14 compiler
   (Behavior>>compile:methodInstaller: / OCInteractiveAPI) signals
   'The source code should not be empty' first.  The test pinned an accident
   of an older image.  The repair makes the test expect the message of the
   image that runs.

D. Skips that only say 'self skip' with a reason that is true on the Github
   build server and false here (MicBlogCreatorTest, 8 tests: the CSS files
   are copied from FileSystem workingDirectory / pharo-local / iceberg / ...;
   on the server the working directory has no such clone; the pkg driver
   copies pharo-local to the working directory of the test run) and
   MicTextualMicrodownExporterTest>>testEnvironmentWithInnerBlock (the
   comment says the environment does not parse inner blocks; the package
   parses them now, MicEnvironmentBlock>>bodyFromLine:).  The repair removes
   the skip.

E. MicReferenceCheckerTest, 5 skips.
   testDuplicatedAnchorDir and testReportingUnknownAnchorDir: the tests do
   'self deny: (visitor checkDirectory: dir)' but checkDirectory: answers
   the checker, not a Boolean (the comment of the method says it answers
   'visitor is ok': the API changed), and the tests write to a real
   directory 'myDirectory' of the OS working directory and leave it behind.
   The repair uses the memory directory 'dir' of the test class and asks
   'checker isOkay' and the result labels.
   testReportingSTONFormat*: MicAnalysisReportWriter>>buildReportSTONFormatOn:
   (1) writes all results under each group header (the loop variable v is
   not used), (2) puts the FileReference of the source into the STON text
   (STON has no string form for it: FileReference{...}), (3) has a dead
   'String streamContents:' expression.  The repair writes one STON array
   for each group with the full name of the file as a String.
   The test testReportingSTONFormatDuplicatedAnchors expects 2 entries for a
   file with 3 duplicated anchors (testDuplicatedAnchors in the same class
   says 3).  testReportingSTONFormatDuplicatedFigure is a copy of
   testReportingSTONFormatUnknownAnchorDir (same body, same expectation, a
   figure is never used).  The repair gives it two figures with one anchor.

I. MicUnreferencedFigureResult (error SubclassResponsibility in
   MicReferenceCheckerTest>>testReportingSTONFormatDuplicatedFigure).  The
   class lacks the class-side #errorType and the instance-side
   #stonFormatExplanation that its sister result classes have; MicResult
   answers 'subclassResponsibility' for both.  The STON report asks the
   class for #errorType of each group, so any report with an unreferenced
   figure signalled the error.  The repair adds the two methods (the entry is
   a label and the source file, as for the anchor results).  The test
   document in entry E now also refers to the figure (*@fig1@*), so the only
   result is the duplicated anchor that the test is about.

F. MicDumperTest>>testTable and MicRealDumper>>visitTable:.  The visitor
   method was a stub ('aTable children first accept: self': a table has no
   children, its cells are in #rows) and the test expected the string
   'MicTableBlock' from 'accept:'.  The repair writes the dumper method in
   the style of the other visit methods and makes the test compare the
   contents of the dumper.

G. MicHTMLConfigurationTest>>testWriteDocumentCSS.  The method has only
   'self skip' and the old test in a comment ('Need to think about creating
   referenced CSS files on the fly').  The CSS files are in the repository
   (ressource/), and a linked CSS needs no file: writeCssLinked only writes
   a link tag.  The repair writes the test: the tag is there, and its href
   is the cssPath of the framework.

H. MicParsesAndRendersAllCommentsTest>>testSmokeSignalAndChoke.  Skipped
   with 'I take too long for normal circumstances'.  The default limit of
   a test is 10 s.  The repair removes the skip and gives the test class a
   limit of 15 minutes (class-side #defaultTimeLimit).  The test is a
   robustness test of the parser: a class whose comment breaks the parser
   or the renderer is a defect to repair after the run shows the list."

"--- A ---"
fix value: #MicMetaDataBlockTest value: #testCreatingAnInstanceInitializeTheMetadatDictionary
	value: [ :s | (s includesSubstring: 'metadata at: ') or: [ s includesSubstring: 'self skip' ] ]
	value:
'testCreatingAnInstanceInitializeTheMetadatDictionary

	"MicStartStopMarkupBlock>>bodyFromLine: collects the lines of the block in body, starting from nil (the initialization to a SmallDictionary is commented out in MicMetaDataBlock>>initialize).  closeMe turns the collected text into the metadata dictionary.  So the dictionary exists after closeMe, not after new.  The accessor of the dictionary is #atKey:, the block has no #at:."

	| metadata |
	metadata := MicMetaDataBlock new.
	metadata bodyFromLine: ''"authors" : "S. Ducasse"''.
	metadata closeMe.
	self assert: metadata body isNotNil.
	self assert: (metadata atKey: ''authors'') equals: ''S. Ducasse''.
	self deny: metadata bogusParsing'.

"--- B, C, D: replace one piece of the source of a method ---
Each entry is { class. selector. old text. new text }.  The guard is that the
old text is in the current source, so a second file-in changes nothing."
{
	{ #MicSyncTesterStrategy. #verify:.
		'Smalltalk image lastUpdateString'. 'SystemVersion current printString' }.
	{ #MicBookTesterVisitorTest. #testExampleEmptyCodeblockWithTwoBrackets.
		'#key was sent to nil'. 'Error: The source code should not be empty' }.
	{ #MicBlogCreatorTest. #testCreateAllHtmlFile. 'self skip.'. '' }.
	{ #MicBlogCreatorTest. #testCreateFromTo. 'self skip.'. '' }.
	{ #MicBlogCreatorTest. #testCreateHtmlFileToReplace. 'self skip.'. '' }.
	{ #MicBlogCreatorTest. #testCreateHtmlGroupFileAt. 'self skip.'. '' }.
	{ #MicBlogCreatorTest. #testCreateHtmlSummarize. 'self skip.'. '' }.
	{ #MicBlogCreatorTest. #testDownloadCSS. 'self skip.'. '' }.
	{ #MicBlogCreatorTest. #testInitializeMonthList. 'self skip.'. '' }.
	{ #MicBlogCreatorTest. #testWriteTo. 'self skip.'. '' }.
	{ #MicTextualMicrodownExporterTest. #testEnvironmentWithInnerBlock. 'self skip.'. '' }.
	{ #MicTextualMicrodownExporterTest. #testEnvironmentWithInnerBlock.
		'"The parsing of environment is plain wrong. It should recursively parse subelements"'. '' }.
	{ #MicParsesAndRendersAllCommentsTest. #testSmokeSignalAndChoke.
		'self skip. "I take too long for normal circumstances"'. '' }.
	{ #MicReferenceCheckerTest. #testReportingSTONFormatUnknownAnchorDir. 'self skip.'. '' }.
} do: [ :entry | | cls selector |
	cls := Smalltalk globals at: entry first ifAbsent: [ nil ].
	selector := entry at: 2.
	(cls notNil and: [ cls includesSelector: selector ])
		ifTrue: [
			fix value: entry first value: selector
				value: [ :source | source includesSubstring: (entry at: 3) ]
				value: ((cls >> selector) sourceCode copyReplaceAll: (entry at: 3) with: (entry at: 4)) ]
		ifFalse: [ Stdio stdout nextPutAll: 'SKIP ', entry first, '>>', selector, ' (absent)'; lf; flush ] ].

"--- E: reference checker tests ---"
fix value: #MicReferenceCheckerTest value: #testDuplicatedAnchorDir
	value: [ :s | s includesSubstring: 'self deny: ( visitor checkDirectory: dir )' ]
	value:
'testDuplicatedAnchorDir

	| file1 file2 checker |
	file1 := dir / ''file1.txt''.
	file1 writeStreamDo: [ :stream | stream nextPutAll: ''# Section
@anchorSection0

# Section 1
@anchorSection1


'' ].
	file1 ensureCreateFile.

	file2 := dir / ''file2.txt''.
	file2 writeStreamDo: [ :stream | stream nextPutAll: ''# Section
@anchorSection3

# Section 4
@anchorSection1


'' ].
	file2 ensureCreateFile.

	checker := MicReferenceChecker new.
	checker checkDirectory: dir.
	self deny: checker isOkay.
	self assert: (self anchorNames: checker) asArray equals: #( ''anchorSection1'' ''anchorSection1'' )'.

fix value: #MicReferenceCheckerTest value: #testReportingUnknownAnchorDir
	value: [ :s | s includesSubstring: 'self deny: (visitor checkDirectory: dir)' ]
	value:
'testReportingUnknownAnchorDir

	| file1 file2 checker |
	file1 := dir / ''file1.txt''.
	file1 writeStreamDo: [ :stream | stream nextPutAll: ''# Section
@anchorSection0

# Section 1
@anchorSection1


'' ].
	file1 ensureCreateFile.

	file2 := dir / ''file2.txt''.
	file2 writeStreamDo: [ :stream | stream nextPutAll: '' See *@anchorSection1@* and *@anchorSection2@*'' ].
	file2 ensureCreateFile.

	checker := MicReferenceChecker new.
	checker checkDirectory: dir.
	self deny: checker isOkay.
	self assert: (self anchorNames: checker) asArray equals: #( ''anchorSection2'' )'.

fix value: #MicReferenceCheckerTest value: #testReportingSTONFormatDuplicatedAnchors
	value: [ :s | s includesSubstring: 'self skip.' ]
	value:
'testReportingSTONFormatDuplicatedAnchors

	| defAncS0TripleAncS1RefAncS1AncS0 checker |
	defAncS0TripleAncS1RefAncS1AncS0 := self defAncS0TripleAncS1RefAncS1AncS0.
	checker := MicReferenceChecker new.
	checker checkList: { defAncS0TripleAncS1RefAncS1AncS0 }.

	"three definitions of ancS1: see testDuplicatedAnchors"
	self
		assert: checker reportSTONFormat
		equals: ''[''''type'''':''''DuplicatedAnchor'''',[''''ancS1'''':''''/myDirectory/defAncS0TripleAncS1RefAncS1AncS0.md'''',''''ancS1'''':''''/myDirectory/defAncS0TripleAncS1RefAncS1AncS0.md'''',''''ancS1'''':''''/myDirectory/defAncS0TripleAncS1RefAncS1AncS0.md'''']]''
'.

fix value: #MicReferenceCheckerTest value: #testReportingSTONFormatDuplicatedFigure
	value: [ :s | s includesSubstring: 'self skip.' ]
	value:
'testReportingSTONFormatDuplicatedFigure

	| doubleFigure checker |
	(dir / ''figures'' / ''f.png'') ensureCreateFile.
	doubleFigure := dir / ''doubleFigure.md''.
	doubleFigure writeStreamDo: [ :stream |
		stream nextPutAll: ''# Section
@ancS0

![a caption 1](figures/f.png anchor=fig1)

![a caption 2](figures/f.png anchor=fig1)

See *@fig1@*.
'' ].
	doubleFigure ensureCreateFile.
	checker := MicReferenceChecker new.
	checker rootDirectory: dir.
	checker checkList: { doubleFigure }.

	self
		assert: checker reportSTONFormat
		equals: ''[''''type'''':''''DuplicatedAnchor'''',[''''fig1'''':''''/myDirectory/doubleFigure.md'''',''''fig1'''':''''/myDirectory/doubleFigure.md'''']]''
'.

fix value: #MicAnalysisReportWriter value: #buildReportSTONFormatOn:
	value: [ :s | s includesSubstring: 'Association new key:' ]
	value:
'buildReportSTONFormatOn: str
	"One STON array for each kind of result: the type, then the entries of that kind.  Groups are on separate lines."

	| dict isFirst |
	dict := results groupedBy: [ :each | each class ].
	isFirst := true.
	dict keysAndValuesDo: [ :kind :ofKind |
		isFirst ifFalse: [ str cr ].
		isFirst := false.
		STON
			put: { (''type'' -> kind errorType). (ofKind collect: [ :each | self stonEntryFor: each ]) asArray }
			onStream: str ]'.

add value: #MicAnalysisReportWriter value: #stonEntryFor: value: 'reporting' value:
'stonEntryFor: aResult
	"The entry of a result for the STON report.  An anchor result answers label -> file reference.  STON writes a file reference as an object (FileReference{...}), so the report holds the full name of the file."

	| entry |
	entry := aResult stonFormatExplanation.
	^ (entry isKindOf: Association)
		ifTrue: [ entry key -> (entry value isString ifTrue: [ entry value ] ifFalse: [ entry value fullName ]) ]
		ifFalse: [ entry ]'.

"--- F: dumper ---"
fix value: #MicRealDumper value: #visitTable:
	value: [ :s | s includesSubstring: 'children first' ]
	value:
'visitTable: aTable

	self
		element: aTable
		during: [
			stream nextPutAll: ''rows: { ''.
			aTable rows
				do: [ :row |
					stream nextPutAll: ''{ ''.
					row
						do: [ :cell |
							stream nextPutAll: ''{ ''.
							cell
								do: [ :each | each accept: self ]
								separatedBy: [ stream nextPutAll: '' . '' ].
							stream nextPutAll: '' }'' ]
						separatedBy: [ stream nextPutAll: '' . '' ].
					stream nextPutAll: '' }'' ]
				separatedBy: [ stream nextPutAll: '' . '' ].
			stream nextPutAll: '' }; hasHeader: ''.
			stream nextPutAll: aTable hasHeader printString.
			stream nextPutAll: ''; yourself'' ]'.

fix value: #MicDumperTest value: #testTable
	value: [ :s | s includesSubstring: 'self skip' ]
	value:
'testTable

	| micTable |
	micTable := (parser parse: factory tableSample) children first.
	visitor visit: micTable.
	self
		assert: visitor contents
		equals: ''(MicTableBlock new rows: { { { (MicTextBlock new substring: ''''aaab'''') } . { (MicTextBlock new substring: ''''jkhjh'''') } } . { { (MicTextBlock new substring: ''''bar'''') } . { (MicTextBlock new substring: ''''rab'''') } } }; hasHeader: true; yourself)''
'.

"--- G: HTML configuration CSS ---"
fix value: #MicHTMLConfigurationTest value: #testWriteDocumentCSS
	value: [ :s | s includesSubstring: 'self skip' ]
	value:
'testWriteDocumentCSS

	| framework parsedContents linkElements link |
	framework := doc addCSSNamed: self sampleMicCSSName.
	configuration writeDocumentCSS.
	self deny: doc contents isEmpty.
	parsedContents := (XMLHTMLParser on: doc contents) parseDocument.
	linkElements := parsedContents descendantElementsNamed: ''link''.
	self assert: linkElements size equals: 1.
	link := linkElements first.
	self assert: (link attributeAt: ''rel'') equals: ''stylesheet''.
	self assert: (link attributeAt: ''href'') equals: framework cssPath'.

"--- H: time limit for the smoke test over all class comments ---"
add value: #'MicParsesAndRendersAllCommentsTest class' value: #defaultTimeLimit value: 'timeouts' value:
'defaultTimeLimit
	"The test renders the comment of each class of the image: more than the 10 seconds of the default."

	^ 15 minutes'.

"--- I: the result of an unreferenced figure in the STON report ---"
add value: #'MicUnreferencedFigureResult class' value: #errorType value: 'kinds' value:
'errorType

	^ ''UnreferencedFigure'''.

add value: #MicUnreferencedFigureResult value: #stonFormatExplanation value: 'accessing' value:
'stonFormatExplanation
	"As the other results: a label and the file that holds the figure."

	^ Association new key: anchorLabel value: source'.
