"Leak test: open and close each window type 10 times.
 Results: /tmp/leak_test_results.txt"!

Object subclass: #LeakTestRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'LeakTest'!

!LeakTestRunner class methodsFor: 'running'!
closeWindow: window
  "Close a window/presenter. Morphs understand #close directly.
   Spec2 presenters need #window close."
  (window respondsTo: #close)
    ifTrue: [window close]
    ifFalse: [(window respondsTo: #window)
      ifTrue: [window window close]]! !

!LeakTestRunner class methodsFor: 'running'!
runLeakTest
  | stream |
  '/tmp/leak_test_results.txt' asFileReference ensureDelete.
  stream := '/tmp/leak_test_results.txt' asFileReference writeStream.
  [
  stream nextPutAll: '=== Window Leak Test ==='; lf. stream flush.
  stream nextPutAll: 'Date: '; nextPutAll: DateAndTime now printString; lf. stream flush.
  stream lf. stream flush.

  self testWindow: 'System Browser' open: [ClyFullBrowserMorph open] times: 10 on: stream.
  self testWindow: 'Playground' open: [StPlaygroundPresenter open] times: 10 on: stream.
  self testWindow: 'Transcript' open: [StTranscriptPresenter open] times: 10 on: stream.
  self testWindow: 'Inspector' open: [StInspectorPresenter inspect: 42] times: 10 on: stream.
  self testWindow: 'Settings' open: [SettingBrowser open] times: 10 on: stream.
  self testWindow: 'Dr Test' open: [DrTests open] times: 10 on: stream.
  self testWindow: 'Finder' open: [StFinderPresenter open] times: 10 on: stream.
  "Spotter and Critique Browser skipped: Spotter kills the Delay scheduler (empty Heap),
   Critique Browser hangs without display. Both need live UI interaction."
  self testWindow: 'Process Browser' open: [StProcessBrowser open] times: 10 on: stream.

  stream lf. stream flush.
  stream nextPutAll: '=== Done ==='; lf. stream flush
  ] ensure: [stream close]! !

!LeakTestRunner class methodsFor: 'running'!
testWindow: name open: aBlock times: n on: stream
  stream nextPutAll: 'Testing: '; nextPutAll: name; nextPutAll: ' x'; nextPutAll: n printString; lf. stream flush.
  1 to: n do: [:i |
    | window |
    [window := aBlock value.
     10 timesRepeat: [Processor yield].
     window ifNotNil: [[self closeWindow: window] on: Error do: [:e |
       stream nextPutAll: '  close-err '; nextPutAll: i printString; nextPutAll: ': '; nextPutAll: (e messageText copyFrom: 1 to: (e messageText size min: 200)); lf. stream flush]].
     5 timesRepeat: [Processor yield]]
    on: Error do: [:e |
      stream nextPutAll: '  open-err '; nextPutAll: i printString; nextPutAll: ': '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: (e messageText copyFrom: 1 to: (e messageText size min: 200)); lf. stream flush]].
  stream nextPutAll: '  Done: '; nextPutAll: name; lf; lf. stream flush! !

!LeakTestRunner class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  [(Delay forSeconds: 5) wait.
   self runLeakTest] forkAt: 40 named: 'LeakTest'! !

!LeakTestRunner class methodsFor: 'startup'!
shutDown: quitting! !

SessionManager default
  register: (ClassSessionHandler forClassNamed: #LeakTestRunner)
  inCategory: SessionManager default userCategory
  atPriority: 90!
