"HTTPS test runner — verifies SqueakSSL data integrity via ZnClient"!

Object subclass: #HttpsTestRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Https'!

!HttpsTestRunner class methodsFor: 'running'!
runTests
  | fs |
  fs := (Smalltalk imageDirectory / 'https_test_results.txt') writeStream.
  [
    fs nextPutAll: '=== HTTPS Test ==='; cr.
    fs nextPutAll: 'Date: ', DateAndTime now printString; cr.

    "Test 1: HTTPS GET from GitHub API"
    fs nextPutAll: 'Test 1: HTTPS GET github api contents/doc'; cr.
    [ | client |
      client := ZnClient new.
      client url: 'https://api.github.com/repos/pharo-project/pharo/contents/doc'.
      client headerAt: 'User-Agent' put: 'Pharo/13'.
      client headerAt: 'Accept' put: 'application/vnd.github.v3+json'.
      client timeout: 30.
      client get.
      fs nextPutAll: '  Status: ', client response statusLine printString; cr.
      fs nextPutAll: '  Body size: ', client contents size printString; cr.
      fs nextPutAll: '  First 500 chars: '; cr.
      fs nextPutAll: (client contents copyFrom: 1 to: (500 min: client contents size)); cr.
      fs nextPutAll: '  PASS'; cr.
      client close.
    ] on: Error do: [ :e |
      fs nextPutAll: '  ERROR: ', e printString; cr.
      fs nextPutAll: '  FAIL'; cr.
    ].

    "Test 2: Simple HTTPS fetch"
    fs nextPutAll: 'Test 2: HTTPS GET httpbin.org/get'; cr.
    [ | client |
      client := ZnClient new.
      client url: 'https://httpbin.org/get'.
      client headerAt: 'User-Agent' put: 'Pharo/13'.
      client timeout: 30.
      client get.
      fs nextPutAll: '  Status: ', client response statusLine printString; cr.
      fs nextPutAll: '  Body size: ', client contents size printString; cr.
      fs nextPutAll: '  First 300 chars: '; cr.
      fs nextPutAll: (client contents copyFrom: 1 to: (300 min: client contents size)); cr.
      fs nextPutAll: '  PASS'; cr.
      client close.
    ] on: Error do: [ :e |
      fs nextPutAll: '  ERROR: ', e printString; cr.
      fs nextPutAll: '  FAIL'; cr.
    ].

    fs nextPutAll: '=== DONE ==='; cr.
  ] ensure: [ fs close ].
  Smalltalk snapshot: false andQuit: true! !

!HttpsTestRunner class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self runTests! !

!HttpsTestRunner class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #HttpsTestRunner)
  inCategory: SessionManager default userCategory
  atPriority: 90!
