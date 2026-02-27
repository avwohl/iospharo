"Window test: try opening every major Pharo tool, catch errors, write results.
 Inject into a fresh Pharo 13 image with:
   pharo Pharo.image eval --save
     'CodeImporter evaluateFileNamed: ''scripts/test_open_windows.st'''
 Then run the image with our VM and check /tmp/window_test_results.txt"!

Object subclass: #WindowTester
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'TestRunner'!

!WindowTester class methodsFor: 'system startup'!
startUp: isImageStarting
  isImageStarting ifFalse: [^ self].
  [ (Delay forSeconds: 3) wait. self runTests ] fork! !

!WindowTester class methodsFor: 'system startup'!
shutDown: isQuitting! !

!WindowTester class methodsFor: 'testing'!
tryOpen: aName with: aBlock results: aCollection
  [aBlock value.
   aCollection add: aName , ': OK']
    on: Error
    do: [:e | aCollection add: aName , ': ERROR - ' , e messageText]! !

!WindowTester class methodsFor: 'testing'!
runTests
  | results stream |
  results := OrderedCollection new.
  results add: '=== Window Open Tests ==='.
  results add: DateAndTime now printString.

  "--- Core Development Tools ---"
  self tryOpen: 'System Browser' with: [ClyFullBrowserMorph open] results: results.
  self tryOpen: 'Playground' with: [StPlaygroundPresenter open] results: results.
  self tryOpen: 'Transcript' with: [Transcript open] results: results.
  self tryOpen: 'Test Runner (DrTests)' with: [DrTests open] results: results.

  "--- Inspectors ---"
  self tryOpen: 'Inspector (42)' with: [42 inspect] results: results.
  self tryOpen: 'Inspector (String)' with: ['hello' inspect] results: results.
  self tryOpen: 'Inspector (Dictionary)' with: [(Dictionary new at: 'a' put: 1; yourself) inspect] results: results.

  "--- System Tools ---"
  self tryOpen: 'Settings Browser' with: [SettingBrowser open] results: results.
  self tryOpen: 'Iceberg (Git)' with: [IceTipRepositoriesBrowser new open] results: results.
  self tryOpen: 'Process Browser' with: [StProcessBrowser open] results: results.
  self tryOpen: 'Finder' with: [StFinderPresenter open] results: results.

  "--- Morphic ---"
  self tryOpen: 'Color inspect' with: [Color red inspect] results: results.
  self tryOpen: 'Morph openInWorld' with: [Morph new color: Color blue; extent: 50@50; openInWorld] results: results.

  "--- Network ---"
  self tryOpen: 'Socket Create TCP' with: [| s | s := Socket newTCP. s destroy] results: results.
  self tryOpen: 'DNS Lookup' with: [NetNameResolver addressForName: 'example.com' timeout: 5] results: results.
  self tryOpen: 'HTTP GET' with: [ZnClient new timeout: 10; url: 'http://example.com'; get] results: results.
  self tryOpen: 'HTTPS GET' with: [ZnClient new timeout: 10; url: 'https://example.com'; get] results: results.

  "--- Clipboard ---"
  self tryOpen: 'Clipboard write' with: [Clipboard clipboardText: 'test from pharo'] results: results.
  self tryOpen: 'Clipboard read' with: [Clipboard clipboardText] results: results.

  results add: '=== Done ==='.

  stream := '/tmp/window_test_results.txt' asFileReference writeStream.
  results do: [:r | stream nextPutAll: r; lf].
  stream close.

  Transcript show: 'Window test complete - see /tmp/window_test_results.txt'; cr.
  results do: [:r | Transcript show: r; cr]! !

"Register as startup handler"
SessionManager default registerGuiClassNamed: #WindowTester!
