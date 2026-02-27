"HTTP test with detailed error diagnostics"!

Object subclass: #HttpTester
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'TestRunner'!

!HttpTester class methodsFor: 'system startup'!
startUp: isImageStarting
  isImageStarting ifFalse: [^ self].
  [ (Delay forSeconds: 3) wait. self runTests ] fork! !

!HttpTester class methodsFor: 'system startup'!
shutDown: isQuitting! !

!HttpTester class methodsFor: 'testing'!
runTests
  | results stream cr |
  results := OrderedCollection new.
  cr := String cr.
  results add: '=== HTTP Test ==='.
  results add: DateAndTime now printString.

  "Test 1: Raw socket connect"
  [
    | sock addr |
    sock := Socket newTCP.
    results add: 'Socket created: ' , sock printString.
    results add: 'Socket status: ' , sock statusString.
    addr := NetNameResolver addressForName: 'example.com' timeout: 10.
    results add: 'DNS resolved: ' , addr printString.
    sock connectTo: addr port: 80 waitForConnectionFor: 10.
    results add: 'Connected, status: ' , sock statusString.
    sock sendData: 'GET / HTTP/1.0' , cr , cr , 'Host: example.com' , cr , cr , cr , cr.
    results add: 'Sent request'.
    (Delay forSeconds: 3) wait.
    sock isDataAvailable ifTrue: [
      | data |
      data := sock receiveData.
      results add: 'Received ' , data size printString , ' bytes'.
      results add: 'First 200 chars: ' , (data copyFrom: 1 to: (data size min: 200))
    ] ifFalse: [
      results add: 'No data available after 3s wait'
    ].
    sock close.
    results add: 'Socket closed OK'
  ] on: Error do: [:e |
    results add: 'Socket test ERROR: ' , e class name , ' - ' , e messageText
  ].

  "Test 2: ZnClient HTTP"
  [
    | response |
    response := ZnClient new
      timeout: 15;
      url: 'http://example.com';
      get.
    results add: 'ZnClient HTTP response length: ' , response size printString.
    results add: 'First 100: ' , (response copyFrom: 1 to: (response size min: 100))
  ] on: Error do: [:e |
    results add: 'ZnClient HTTP ERROR: ' , e class name , ' - ' , e messageText
  ].

  "Test 3: ZnClient HTTPS"
  [
    | response |
    response := ZnClient new
      timeout: 15;
      url: 'https://example.com';
      get.
    results add: 'ZnClient HTTPS response length: ' , response size printString.
    results add: 'First 100: ' , (response copyFrom: 1 to: (response size min: 100))
  ] on: Error do: [:e |
    results add: 'ZnClient HTTPS ERROR: ' , e class name , ' - ' , e messageText
  ].

  results add: '=== Done ==='.

  stream := '/tmp/http_test_results.txt' asFileReference writeStream.
  results do: [:r | stream nextPutAll: r; lf].
  stream close.

  Transcript show: 'HTTP test complete'; cr! !

"Register as startup handler"
SessionManager default registerGuiClassNamed: #HttpTester!
