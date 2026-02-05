"Diagnose symbol interning issues"!

Object subclass: #SymbolDiagnostic
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!SymbolDiagnostic class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/symbol_diagnostic.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Symbol Diagnostic ==='; cr; cr.

    "Step 1: Check symbol identity"
    stream nextPutAll: '--- Step 1: Symbol identity ---'; cr.
    [| s1 s2 |
     s1 := #sdlInit:.
     s2 := 'sdlInit:' asSymbol.
     stream nextPutAll: '#sdlInit: == ''sdlInit:'' asSymbol: '; print: (s1 == s2); cr.
     stream nextPutAll: '#sdlInit: identityHash: '; print: s1 identityHash; cr.
     stream nextPutAll: '''sdlInit:'' asSymbol identityHash: '; print: s2 identityHash; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 2: Check SDL2 method dict keys"
    stream cr; nextPutAll: '--- Step 2: SDL2 method dict keys ---'; cr.
    [| md keys |
     md := SDL2 class methodDict.
     keys := md keys asArray.
     stream nextPutAll: 'Number of keys: '; print: keys size; cr.
     stream nextPutAll: 'First 20 keys:'; cr.
     1 to: (keys size min: 20) do: [:i |
       | k |
       k := keys at: i.
       stream nextPutAll: '  ['; print: i; nextPutAll: '] '.
       stream nextPutAll: k asString.
       stream nextPutAll: ' (class: '; print: k class name; nextPutAll: ')'; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 3: Check if any key matches sdlInit:"
    stream cr; nextPutAll: '--- Step 3: Find sdlInit: ---'; cr.
    [| md found |
     md := SDL2 class methodDict.
     found := false.
     md keysDo: [:k |
       (k asString = 'sdlInit:') ifTrue: [
         stream nextPutAll: 'Found key with string ''sdlInit:'': '; print: k; cr.
         stream nextPutAll: '  k == #sdlInit:: '; print: (k == #sdlInit:); cr.
         stream nextPutAll: '  k identityHash: '; print: k identityHash; cr.
         stream nextPutAll: '  #sdlInit: identityHash: '; print: #sdlInit: identityHash; cr.
         found := true]].
     found ifFalse: [
       stream nextPutAll: 'No key with string ''sdlInit:'' found'; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 4: Check OSSDL2Driver method dict"
    stream cr; nextPutAll: '--- Step 4: OSSDL2Driver method dict ---'; cr.
    [| md keys |
     md := OSSDL2Driver class methodDict.
     keys := md keys asArray.
     stream nextPutAll: 'Number of keys: '; print: keys size; cr.
     stream nextPutAll: 'All keys:'; cr.
     keys do: [:k |
       stream nextPutAll: '  '; nextPutAll: k asString; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 5: Check superclass method dict"
    stream cr; nextPutAll: '--- Step 5: OSWindowDriver method dict ---'; cr.
    [| md found |
     md := OSWindowDriver class methodDict.
     stream nextPutAll: 'OSWindowDriver class methodDict size: '; print: md size; cr.
     found := false.
     md keysDo: [:k |
       (k asString = 'isApplicableFor:') ifTrue: [
         stream nextPutAll: 'Found ''isApplicableFor:'' in OSWindowDriver class'; cr.
         stream nextPutAll: '  k == #isApplicableFor:: '; print: (k == #isApplicableFor:); cr.
         found := true]].
     found ifFalse: [
       stream nextPutAll: 'No ''isApplicableFor:'' in OSWindowDriver class'; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!SymbolDiagnostic class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!SymbolDiagnostic class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #SymbolDiagnostic)
  inCategory: SessionManager default userCategory
  atPriority: 90!
