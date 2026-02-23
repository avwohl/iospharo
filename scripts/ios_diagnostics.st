"Simple diagnostic via stderr.
 Uses Stdio stderr for output.
 In chunk format, exclamation marks in strings must be doubled."!

Object subclass: #IOSDiagnostics
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'IOSDiag'!

!IOSDiagnostics class methodsFor: 'system startup'!
startUp: isResuming
  isResuming ifFalse: [^ self].
  Stdio stderr nextPutAll: '[DIAG] IOSDiagnostics startUp called'; lf; flush.
  [self runDiagnostics] on: Error do: [:e |
    Stdio stderr nextPutAll: '[DIAG] ERROR: '; nextPutAll: e messageText; lf; flush.
  ].! !

!IOSDiagnostics class methodsFor: 'system startup'!
shutDown: isQuitting! !

!IOSDiagnostics class methodsFor: 'running'!
runDiagnostics
  | err display world |
  err := Stdio stderr.
  err nextPutAll: '[DIAG] runDiagnostics starting'; lf; flush.

  display := Smalltalk globals at: #Display ifAbsent: [nil].
  world := Smalltalk globals at: #World ifAbsent: [nil].

  display ifNotNil: [
    err nextPutAll: '[DIAG] Display extent: '; nextPutAll: display extent printString; lf.
    err nextPutAll: '[DIAG] Display depth: '; nextPutAll: display depth printString; lf.
  ] ifNil: [
    err nextPutAll: '[DIAG] Display is nil'; lf.
  ].

  world ifNotNil: [
    err nextPutAll: '[DIAG] World extent: '; nextPutAll: world extent printString; lf.
    err nextPutAll: '[DIAG] World submorphs: '; nextPutAll: world submorphs size printString; lf.
    world submorphs doWithIndex: [:m :i |
      err nextPutAll: '[DIAG]   '; nextPutAll: i printString;
          nextPutAll: ': '; nextPutAll: m class name;
          nextPutAll: ' '; nextPutAll: m bounds printString; lf.
    ].
  ] ifNil: [
    err nextPutAll: '[DIAG] World is nil'; lf.
  ].

  err nextPutAll: '[DIAG] MenubarMorph instances: ';
      nextPutAll: MenubarMorph allInstances size printString; lf.
  MenubarMorph allInstances do: [:mb |
    err nextPutAll: '[DIAG]   bounds='; nextPutAll: mb bounds printString;
        nextPutAll: ' visible='; nextPutAll: mb visible printString;
        nextPutAll: ' owner='; nextPutAll: (mb owner ifNil: ['nil'] ifNotNil: [:o | o class name]); lf.
  ].

  err nextPutAll: '[DIAG] TaskbarMorph instances: ';
      nextPutAll: TaskbarMorph allInstances size printString; lf.

  err nextPutAll: '[DIAG] Platform: ';
      nextPutAll: OSPlatform current class name; lf.

  err nextPutAll: '[DIAG] Done'; lf.
  err flush.

  "Also try file output"
  [
    | stream |
    stream := '/tmp/ios_diagnostics.txt' asFileReference writeStream.
    [stream nextPutAll: 'Written from Smalltalk'; cr] ensure: [stream close].
  ] on: Error do: [:e | "ignore" ].! !

SessionManager default registerToolClassNamed: #IOSDiagnostics!
