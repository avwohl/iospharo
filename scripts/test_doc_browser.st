"Test the exact doc browser API flow step by step"!

SessionManager default registerUserClassNamed: #TestDocBrowser atPriority: 90!

Object subclass: #TestDocBrowser
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'TestDocBrowser'!

!TestDocBrowser class methodsFor: 'startup'!
startup: isImageStarting
  isImageStarting ifFalse: [^ self].
  [self runTest] fork! !

!TestDocBrowser class methodsFor: 'testing'!
runTest
  | f |
  (Delay forSeconds: 8) wait.
  f := (Smalltalk imageDirectory / 'doc_browser_test.txt') writeStream.
  [
    self testZnEasyGet: f.
    self testDocBrowserFlow: f.
  ] ensure: [f close].
  Smalltalk logStdout: '[TEST] doc_browser_test.txt written'! !

!TestDocBrowser class methodsFor: 'testing'!
testZnEasyGet: f
  | response entity |
  f nextPutAll: '=== ZnEasy basic test ==='; cr.
  [
    response := ZnEasy get: 'https://api.github.com/repos/pharo-project/pharo'.
    f nextPutAll: 'Response class: ', response class name; cr.
    f nextPutAll: 'Status: ', response statusLine printString; cr.
    f nextPutAll: 'Content-Type: ', response contentType printString; cr.
    entity := response entity.
    f nextPutAll: 'Entity class: ', entity class name; cr.
    f nextPutAll: 'Entity contents class: ', entity contents class name; cr.
    f nextPutAll: 'Entity contents size: ', entity contents size printString; cr.
    f nextPutAll: 'First 200 chars: ', (entity contents first: (200 min: entity contents size)); cr.
  ] on: Error do: [:e |
    f nextPutAll: 'ZnEasy ERROR: ', e printString; cr]! !

!TestDocBrowser class methodsFor: 'testing'!
testDocBrowserFlow: f
  | ref |
  f nextPutAll: '=== Doc browser flow test ==='; cr.
  [
    ref := MicGitHubRessourceReference new.
    f nextPutAll: 'MicGitHubRessourceReference class: ', ref class name; cr.
  ] on: Error do: [:e |
    f nextPutAll: 'Cannot create MicGitHubRessourceReference: ', e printString; cr.
    ^ self].
  [
    "Set it up like the doc browser does"
    ref user: 'pharo-project'.
    ref project: 'pharo'.
    ref docFolder: #('doc').
    f nextPutAll: 'user: ', ref user printString; cr.
    f nextPutAll: 'project: ', ref project printString; cr.
    f nextPutAll: 'docFolder: ', ref docFolder printString; cr.
  ] on: Error do: [:e |
    f nextPutAll: 'Setup ERROR: ', e printString; cr.
    ^ self].

  "Test githubPathLookup step by step"
  self testGithubPathLookup: ref log: f.

  "Test loadChildren"
  self testLoadChildren: ref log: f! !

!TestDocBrowser class methodsFor: 'testing'!
testGithubPathLookup: ref log: f
  | treesRequest api response tree docEntry docUrl docResponse |
  f nextPutAll: '--- githubPathLookup step by step ---'; cr.

  "Step 1: Build trees API request"
  [
    treesRequest := ref githubTreesApiRequest.
    f nextPutAll: 'treesApiRequest: ', treesRequest printString; cr.
  ] on: Error do: [:e |
    f nextPutAll: 'githubTreesApiRequest ERROR: ', e printString; cr.
    ^ self].

  "Step 2: Get via githubApi"
  [
    api := ref githubApi.
    f nextPutAll: 'githubApi class: ', api class name; cr.
    response := api get: treesRequest.
    f nextPutAll: 'API get response class: ', response class name; cr.
    response isDictionary ifTrue: [
      f nextPutAll: 'Response keys: ', response keys printString; cr].
    (response isKindOf: Collection) ifTrue: [
      f nextPutAll: 'Response size: ', response size printString; cr].
  ] on: Error do: [:e |
    f nextPutAll: 'githubApi get ERROR: ', e printString; cr.
    ^ self].

  "Step 3: Find tree entries"
  [
    (response includesKey: 'tree')
      ifTrue: [
        tree := response at: 'tree'.
        f nextPutAll: 'tree size: ', tree size printString; cr.
        tree do: [:e |
          f nextPutAll: '  ', (e at: 'path'), ' (', (e at: 'type'), ')'; cr]]
      ifFalse: [
        f nextPutAll: 'NO tree key in response, keys: ', response keys printString; cr.
        ^ self].
  ] on: Error do: [:e |
    f nextPutAll: 'tree extraction ERROR: ', e printString; cr.
    ^ self].

  "Step 4: Find doc entry"
  [
    docEntry := tree detect: [:e | (e at: 'path') = 'doc'] ifNone: [nil].
    docEntry
      ifNil: [f nextPutAll: 'NO doc entry found in tree'; cr. ^ self]
      ifNotNil: [
        f nextPutAll: 'doc entry url: ', (docEntry at: 'url') printString; cr.
        docUrl := (docEntry at: 'url') withoutPrefix: 'https://api.github.com/'.
        f nextPutAll: 'doc request path: ', docUrl; cr].
  ] on: Error do: [:e |
    f nextPutAll: 'doc entry ERROR: ', e printString; cr.
    ^ self].

  "Step 5: Fetch doc subtree"
  [
    | znResponse |
    znResponse := ZnEasy get: ('https://api.github.com/', docUrl).
    f nextPutAll: 'ZnEasy doc response status: ', znResponse statusLine printString; cr.
    f nextPutAll: 'ZnEasy doc entity size: ', znResponse entity contents size printString; cr.
    docResponse := STONJSON fromString: znResponse entity contents.
    f nextPutAll: 'Parsed doc response class: ', docResponse class name; cr.
    f nextPutAll: 'Parsed doc keys: ', docResponse keys printString; cr.
    (docResponse includesKey: 'tree')
      ifTrue: [
        | docTree |
        docTree := docResponse at: 'tree'.
        f nextPutAll: 'doc tree size: ', docTree size printString; cr.
        docTree do: [:e |
          f nextPutAll: '  DOC: ', (e at: 'path'), ' (', (e at: 'type'), ')'; cr]]
      ifFalse: [
        f nextPutAll: 'NO tree in doc response'; cr].
  ] on: Error do: [:e |
    f nextPutAll: 'doc subtree ERROR: ', e printString; cr]! !

!TestDocBrowser class methodsFor: 'testing'!
testLoadChildren: ref log: f
  | children |
  f nextPutAll: '--- loadChildren test ---'; cr.
  [
    children := ref loadChildren.
    f nextPutAll: 'loadChildren result class: ', children class name; cr.
    f nextPutAll: 'loadChildren size: ', children size printString; cr.
    children do: [:c |
      f nextPutAll: '  child: ', c printString; cr].
  ] on: Error do: [:e |
    f nextPutAll: 'loadChildren ERROR: ', e printString; cr.
    f nextPutAll: '  signalerContext: ', e signalerContext printString; cr]! !
