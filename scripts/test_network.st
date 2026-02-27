"Network Test Jig — tests DNS, TCP, and UDP from within the Pharo image.
 Inject into a fresh image and run with our VM.
 Results written to network_test_results.txt in the image directory."!

"Ensure DNU does not infinite-loop if something goes wrong"
(UndefinedObject canUnderstand: #findNextHandlerContext)
    ifFalse: [
        UndefinedObject compile: 'findNextHandlerContext ^ nil' classified: 'error handling' ]!

Object subclass: #NetworkTestRunner
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'NetworkTests'!

!NetworkTestRunner class methodsFor: 'logging'!
resultsFile
    ^ FileLocator imageDirectory / 'network_test_results.txt'! !

!NetworkTestRunner class methodsFor: 'logging'!
log: aString
    self resultsFile writeStreamDo: [ :ws |
        ws setToEnd.
        ws nextPutAll: aString.
        ws nextPut: Character lf ]! !

!NetworkTestRunner class methodsFor: 'tests'!
testDNS
    | addr startTime elapsed |
    self log: ''.
    self log: '=== Test 1: DNS Resolution ==='.

    #('localhost' 'example.com' 'github.com') do: [ :host |
        self log: 'Resolving ' , host , ' ...'.
        startTime := Time millisecondClockValue.
        [ addr := NetNameResolver addressForName: host timeout: 15 ]
            on: Error do: [ :e | addr := nil. self log: '  Exception: ' , e printString ].
        elapsed := Time millisecondClockValue - startTime.
        addr
            ifNil: [ self log: 'FAIL: DNS ' , host , ' — nil (' , elapsed printString , 'ms)' ]
            ifNotNil: [ self log: 'PASS: DNS ' , host , ' — ' , addr printString , ' (' , elapsed printString , 'ms)' ] ]! !

!NetworkTestRunner class methodsFor: 'tests'!
testTCP
    | addr socket startTime elapsed request data firstLine |
    self log: ''.
    self log: '=== Test 2: TCP Connection + HTTP GET ==='.

    self log: 'Resolving example.com...'.
    [ addr := NetNameResolver addressForName: 'example.com' timeout: 15 ]
        on: Error do: [ :e | addr := nil ].
    addr ifNil: [
        self log: 'FAIL: TCP — DNS failed'.
        ^ self ].

    socket := Socket newTCP.
    [
        startTime := Time millisecondClockValue.
        socket connectTo: addr port: 80 waitForConnectionFor: 15.
        elapsed := Time millisecondClockValue - startTime.
        socket isConnected
            ifFalse: [
                self log: 'FAIL: TCP connect (' , elapsed printString , 'ms)'.
                ^ self ].
        self log: 'PASS: TCP connect (' , elapsed printString , 'ms)'.

        request := 'GET / HTTP/1.0' , String crlf ,
                   'Host: example.com' , String crlf ,
                   String crlf.
        socket sendData: request.
        self log: 'PASS: TCP send (' , request size printString , ' bytes)'.

        self log: 'Waiting for response...'.
        socket waitForDataFor: 10.
        data := socket receiveData.
        data size > 0
            ifTrue: [
                firstLine := data lines first.
                firstLine size > 60 ifTrue: [ firstLine := firstLine first: 60 ].
                self log: 'PASS: TCP receive (' , data size printString , ' bytes)'.
                self log: '  Response: ' , firstLine ]
            ifFalse: [ self log: 'FAIL: TCP receive — no data' ]
    ] on: Error do: [ :e |
        self log: 'FAIL: TCP — ' , e printString ].
    [ socket closeAndDestroy ] on: Error do: [ :x | ]! !

!NetworkTestRunner class methodsFor: 'tests'!
testUDP
    | socket dnsQuery destAddr startTime elapsed buf result bytesRead |
    self log: ''.
    self log: '=== Test 3: UDP Send/Receive ==='.

    socket := Socket newUDP.
    [
        "Build minimal DNS query for example.com (type A)"
        dnsQuery := ByteArray new: 29.
        dnsQuery at: 1 put: 16r12. dnsQuery at: 2 put: 16r34.
        dnsQuery at: 3 put: 16r01. dnsQuery at: 4 put: 16r00.
        dnsQuery at: 5 put: 16r00. dnsQuery at: 6 put: 16r01.
        7 to: 12 do: [ :i | dnsQuery at: i put: 0 ].
        dnsQuery at: 13 put: 7.
        'example' doWithIndex: [ :c :i | dnsQuery at: 13 + i put: c asciiValue ].
        dnsQuery at: 21 put: 3.
        'com' doWithIndex: [ :c :i | dnsQuery at: 21 + i put: c asciiValue ].
        dnsQuery at: 25 put: 0.
        dnsQuery at: 26 put: 0. dnsQuery at: 27 put: 1.
        dnsQuery at: 28 put: 0. dnsQuery at: 29 put: 1.

        "Resolve 8.8.8.8"
        destAddr := NetNameResolver addressForName: '8.8.8.8' timeout: 5.
        destAddr ifNil: [
            self log: 'FAIL: UDP — cannot resolve 8.8.8.8'.
            ^ self ].

        self log: 'Sending DNS query to 8.8.8.8:53...'.
        startTime := Time millisecondClockValue.
        socket sendUDPData: dnsQuery toHost: destAddr port: 53.
        self log: 'PASS: UDP send (29 bytes)'.

        self log: 'Waiting for response...'.
        socket waitForDataFor: 5.
        elapsed := Time millisecondClockValue - startTime.
        buf := ByteArray new: 512.
        result := socket receiveUDPDataInto: buf.
        "result is Array: {bytesRead, senderAddr, senderPort, moreFlag}"
        bytesRead := result first.
        bytesRead > 0
            ifTrue: [ self log: 'PASS: UDP receive (' , bytesRead printString , ' bytes, ' , elapsed printString , 'ms)' ]
            ifFalse: [ self log: 'FAIL: UDP receive — no data (' , elapsed printString , 'ms)' ]
    ] on: Error do: [ :e |
        self log: 'FAIL: UDP — ' , e printString ].
    [ socket closeAndDestroy ] on: Error do: [ :x | ]! !

!NetworkTestRunner class methodsFor: 'running'!
runAll
    self log: '=== Network Test Jig ==='.
    self log: DateAndTime now printString.

    [ self testDNS ] on: Error do: [ :e |
        self log: 'DNS CRASH: ' , e printString ].
    [ self testTCP ] on: Error do: [ :e |
        self log: 'TCP CRASH: ' , e printString ].
    [ self testUDP ] on: Error do: [ :e |
        self log: 'UDP CRASH: ' , e printString ].

    self log: ''.
    self log: '=== All tests complete ==='! !

!NetworkTestRunner class methodsFor: 'system startup'!
startUp: isImageStarting
    isImageStarting ifFalse: [ ^ self ].
    [
        (Delay forSeconds: 3) wait.
        self resultsFile ensureDelete.
        self runAll.
    ] fork! !

SessionManager default register: (ClassSessionHandler forClassNamed: #NetworkTestRunner)
    inCategory: SessionManager default userCategory atPriority: 90!
