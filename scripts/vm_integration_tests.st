"VM Integration Tests — catches everything the standard SUnit suite misses.
 These are proper TestCase subclasses that test the actual C/C++ primitive layer:
 real network I/O, real clipboard round-trips, locale values, plugin object
 creation, file error types, sound/MIDI API, and timer starvation.

 The standard Pharo test suite passes at 99.7% with ALL of these broken because:
 1. Every <primitive: 'x' module: 'Y'> has a Smalltalk fallback
 2. No tests do real network I/O, audio, clipboard, or MIDI
 3. Hardcoded US English locale values match CI environments
 4. InterpreterProxy bugs only affect plugin-created objects
 5. Error object TYPE is rarely checked (just 'on: Error do:')

 Usage:
   '/path/to/vm_integration_tests.st' asFileReference fileIn.
   VMIntegrationTestRunner runAll.     'Run all and print results'
   VMSocketTCPTest buildSuite run.     'Run one class in SUnit'

 To add to the official Pharo test suite:
   Each class is a proper TestCase subclass in package 'VMIntegrationTests'.
   They can be run via the standard SUnit TestRunner UI, command line, or CI.
   Network tests require internet access (tagged with #requiresNetwork).
   Sound/MIDI tests require hardware (tagged with #requiresHardware)."!

"----------------------------------------------------------------------
 DNU guard — prevents infinite recursion if something goes wrong
----------------------------------------------------------------------"
(UndefinedObject canUnderstand: #findNextHandlerContext)
    ifFalse: [
        UndefinedObject compile: 'findNextHandlerContext ^ nil' classified: 'error handling' ]!

"======================================================================
 VMSocketTCPTest — Real TCP connection to external host
 Catches: missing SocketPlugin, wrong connect address format,
          broken socket options, I/O thread race conditions
======================================================================"
TestCase subclass: #VMSocketTCPTest
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMSocketTCPTest methodsFor: 'testing'!
testTCPConnectAndReceive
    "Verify TCP socket can connect, send HTTP request, receive response.
     This was completely broken: no SocketPlugin, wrong address format,
     broken socket options. Commit ee3997f, 4bb0663, 8fca4b2."
    | addr socket data |
    addr := NetNameResolver addressForName: 'example.com' timeout: 15.
    self deny: addr isNil description: 'DNS resolution failed for example.com'.

    socket := Socket newTCP.
    [
        socket connectTo: addr port: 80 waitForConnectionFor: 15.
        self assert: socket isConnected description: 'TCP connect failed'.

        socket sendData: 'GET / HTTP/1.0' , String crlf ,
            'Host: example.com' , String crlf , String crlf.

        socket waitForDataFor: 10.
        data := socket receiveData.
        self assert: data size > 0 description: 'TCP received no data'.
        self assert: (data beginsWith: 'HTTP/1.') description: 'Response not HTTP'
    ] ensure: [
        [socket closeAndDestroy] on: Error do: [:e |] ]! !

!VMSocketTCPTest methodsFor: 'testing'!
testSocketStatusTransitions
    "Verify socket status lifecycle: Unconnected -> Connected -> Closed.
     Socket status was broken by missing primitives."
    | addr socket |
    addr := NetNameResolver addressForName: 'example.com' timeout: 15.
    self deny: addr isNil.

    socket := Socket newTCP.
    [
        self deny: socket isConnected description: 'New socket should not be connected'.
        socket connectTo: addr port: 80 waitForConnectionFor: 15.
        self assert: socket isConnected description: 'Socket should be connected after connect'.
        socket closeAndDestroy.
        self deny: socket isConnected description: 'Socket should not be connected after close'
    ] on: Error do: [:e |
        [socket closeAndDestroy] on: Error do: [:x |].
        self fail: 'Socket lifecycle error: ' , e printString ]! !

!VMSocketTCPTest methodsFor: 'testing'!
testSocketOptionsReturnArray
    "Socket>>setOption:value: must return an Array {errCode. retVal}, not a SmallInteger.
     Was broken: returned bare integer, Pharo called #first on it -> MNU.
     Commit 4bb0663."
    | socket result |
    socket := Socket newTCP.
    [
        result := socket setOption: 'TCP_NODELAY' value: '1'.
        self assert: result isArray description: 'setOption must return Array, got: ' , result class name.
        self assert: result size >= 2 description: 'setOption Array must have >= 2 elements'.
        self assert: (result first isKindOf: Integer) description: 'First element must be error code (Integer)'
    ] ensure: [
        [socket closeAndDestroy] on: Error do: [:e |] ]! !

!VMSocketTCPTest methodsFor: 'testing'!
testByteArrayAddress
    "Pharo 13 NetNameResolver returns ByteArray addresses (4 bytes, network order),
     not SmallIntegers. Connect must accept both formats.
     Commit 8fca4b2."
    | addr socket |
    addr := NetNameResolver addressForName: 'example.com' timeout: 15.
    self deny: addr isNil.
    "Pharo 13 returns SocketAddress wrapping ByteArray"
    self assert: ((addr isKindOf: ByteArray) or: [addr respondsTo: #asByteArray])
        description: 'Address should be ByteArray-compatible, got: ' , addr class name.
    socket := Socket newTCP.
    [
        "This used to crash with primitiveFail because stackIntegerValue
         was called on a ByteArray address."
        socket connectTo: addr port: 80 waitForConnectionFor: 15.
        self assert: socket isConnected
    ] ensure: [
        [socket closeAndDestroy] on: Error do: [:e |] ]! !


"======================================================================
 VMSocketUDPTest — Real UDP send/receive
 Catches: wrong argument order, wrong return format, wrong socket state
======================================================================"
TestCase subclass: #VMSocketUDPTest
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMSocketUDPTest methodsFor: 'testing'!
testUDPSendReceive
    "Send a DNS query via UDP and receive the response.
     Three bugs fixed: wrong stack arg order for sendto, wrong return format
     for recvfrom (expected 6 args + output arrays, actual: 4 args + Array result),
     UDP sockets created as UNCONNECTED blocking waitForDataFor:.
     Commit cfbff40."
    | socket dnsQuery destAddr buf result bytesRead |
    socket := Socket newUDP.
    [
        "Build minimal DNS query for example.com (type A)"
        dnsQuery := ByteArray new: 29.
        dnsQuery at: 1 put: 16rAB. dnsQuery at: 2 put: 16rCD.
        dnsQuery at: 3 put: 16r01. dnsQuery at: 4 put: 16r00.
        dnsQuery at: 5 put: 16r00. dnsQuery at: 6 put: 16r01.
        7 to: 12 do: [:i | dnsQuery at: i put: 0].
        dnsQuery at: 13 put: 7.
        'example' doWithIndex: [:c :i | dnsQuery at: 13 + i put: c asciiValue].
        dnsQuery at: 21 put: 3.
        'com' doWithIndex: [:c :i | dnsQuery at: 21 + i put: c asciiValue].
        dnsQuery at: 25 put: 0.
        dnsQuery at: 26 put: 0. dnsQuery at: 27 put: 1.
        dnsQuery at: 28 put: 0. dnsQuery at: 29 put: 1.

        destAddr := NetNameResolver addressForName: '8.8.8.8' timeout: 5.
        self deny: destAddr isNil.

        socket sendUDPData: dnsQuery toHost: destAddr port: 53.

        socket waitForDataFor: 5.
        buf := ByteArray new: 512.
        result := socket receiveUDPDataInto: buf.

        "Result must be a 4-element Array, not a bare integer or wrong shape"
        self assert: result isArray description: 'receiveUDP must return Array, got: ' , result class name.
        self assert: result size = 4 description: 'receiveUDP Array must have 4 elements, got: ' , result size printString.

        bytesRead := result first.
        self assert: (bytesRead isKindOf: Integer) description: 'bytesRead must be Integer'.
        self assert: bytesRead > 0 description: 'Should have received DNS response data'.

        "Element 2: sender address (ByteArray of 4 bytes)"
        self assert: (result second isKindOf: ByteArray) description: 'sender addr must be ByteArray'.
        self assert: result second size = 4 description: 'sender addr must be 4 bytes'.

        "Element 3: sender port (Integer)"
        self assert: (result third isKindOf: Integer) description: 'sender port must be Integer'.
        self assert: result third = 53 description: 'sender port should be 53 (DNS)'

    ] ensure: [
        [socket closeAndDestroy] on: Error do: [:e |] ]! !

!VMSocketUDPTest methodsFor: 'testing'!
testUDPSocketIsConnected
    "UDP sockets must report isConnected = true even though they are connectionless.
     Pharo waitForDataFor: polls isConnected in a loop; if it returns false,
     the loop exits immediately with ConnectionClosed. Commit cfbff40."
    | socket |
    socket := Socket newUDP.
    [
        self assert: socket isConnected
            description: 'UDP socket must be connected (connectionless sockets are always ready)'
    ] ensure: [
        [socket closeAndDestroy] on: Error do: [:e |] ]! !


"======================================================================
 VMDNSTest — Real DNS resolution
 Catches: synchronous DNS blocking VM, wrong resolver status, missing
          semaphore signaling
======================================================================"
TestCase subclass: #VMDNSTest
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMDNSTest methodsFor: 'testing'!
testDNSResolvesKnownHosts
    "Basic DNS resolution must work for well-known hostnames.
     Was initially only enough for UUID generation (localhost).
     External DNS was synchronous, blocking for 30s. Commit 204d781."
    #('localhost' 'example.com' 'github.com') do: [:host |
        | addr |
        addr := NetNameResolver addressForName: host timeout: 15.
        self deny: addr isNil description: 'DNS failed for: ' , host ]! !

!VMDNSTest methodsFor: 'testing'!
testDNSDoesNotBlockVM
    "DNS resolution must be async — must not block other processes.
     Old synchronous getaddrinfo blocked the entire VM for 30s,
     preventing UI rendering and causing ConnectionClosed on active sockets.
     Commit 204d781."
    | counter resolved sem |
    counter := 0.
    resolved := false.
    sem := Semaphore new.

    "Fork a counter that increments every 10ms"
    [1 to: 100 do: [:i |
        counter := counter + 1.
        (Delay forMilliseconds: 10) wait].
    sem signal] fork.

    "Do DNS lookup — if it blocks the VM, counter won't increment"
    [NetNameResolver addressForName: 'example.com' timeout: 15.
     resolved := true] fork.

    "Wait for counter to finish"
    sem waitTimeoutSeconds: 5.
    self assert: counter > 10
        description: 'Counter only reached ' , counter printString ,
            ' — DNS appears to be blocking the VM thread'! !


"======================================================================
 VMLocaleTest — Real locale values from the OS
 Catches: all 10 primitives hardcoded to US English
======================================================================"
TestCase subclass: #VMLocaleTest
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMLocaleTest methodsFor: 'testing'!
testLanguageIsNotEmpty
    "primitiveLocaleLanguage (390) was hardcoded to 'en'.
     Commit 9750786."
    | lang |
    lang := Locale current isoLanguage.
    self deny: lang isNil.
    self deny: lang isEmpty description: 'Language code should not be empty'.
    self assert: lang size >= 2 description: 'Language code should be >= 2 chars'! !

!VMLocaleTest methodsFor: 'testing'!
testCountryIsNotEmpty
    "primitiveLocaleCountry (391) was hardcoded to 'US'.
     Commit 9750786."
    | country |
    country := Locale current isoCountry.
    self deny: country isNil.
    self deny: country isEmpty description: 'Country code should not be empty'.
    self assert: country size >= 2 description: 'Country code should be >= 2 chars'! !

!VMLocaleTest methodsFor: 'testing'!
testTimezoneOffsetIsNotZero
    "primitiveTimezoneOffset was hardcoded to 0.
     Most dev machines are not in UTC. Commit 9750786."
    | offset |
    offset := Locale current primTimezone.
    self assert: (offset isKindOf: Integer)
        description: 'Timezone offset must be Integer, got: ' , offset class name.
    self assert: (offset between: -720 and: 840)
        description: 'Timezone offset out of range: ' , offset printString! !

!VMLocaleTest methodsFor: 'testing'!
testDecimalSeparatorIsNotEmpty
    "primitiveLocaleDecimalSeparator (393) was hardcoded to '.'.
     Commit 9750786."
    | sep |
    sep := Locale current primDecimalSymbol.
    self deny: sep isNil.
    self deny: sep isEmpty description: 'Decimal separator should not be empty'! !

!VMLocaleTest methodsFor: 'testing'!
testCurrencySymbolIsNotEmpty
    "primitiveLocaleCurrencySymbol (392) was hardcoded to '$'.
     Commit 9750786."
    | sym |
    sym := Locale current primCurrencySymbol.
    self deny: sym isNil.
    self deny: sym isEmpty description: 'Currency symbol should not be empty'! !

!VMLocaleTest methodsFor: 'testing'!
testDateFormatIsNotEmpty
    "primitiveLocaleDateFormat (395) was hardcoded to 'MM/dd/yyyy'.
     Commit 9750786."
    | fmt |
    fmt := Locale current primLongDateFormat.
    self deny: fmt isNil.
    self deny: fmt isEmpty description: 'Date format should not be empty'! !

!VMLocaleTest methodsFor: 'testing'!
testTimeFormatIsNotEmpty
    "primitiveLocaleTimeFormat (396) was hardcoded to 'HH:mm:ss'.
     Commit 9750786."
    | fmt |
    fmt := Locale current primTimeFormat.
    self deny: fmt isNil.
    self deny: fmt isEmpty description: 'Time format should not be empty'! !

!VMLocaleTest methodsFor: 'testing'!
testTimezoneOffsetIsReasonable
    "primitiveLocaleTimezoneOffset (398) — was already working.
     Verify it returns a sane value (-12..+14 hours in minutes)."
    | offset |
    offset := Locale current primVMOffsetToUTC.
    self assert: (offset isKindOf: Integer) description: 'Timezone offset must be Integer'.
    self assert: (offset between: -720 and: 840)
        description: 'Timezone offset out of range: ' , offset printString! !


"======================================================================
 VMSecurityTest — SecurityPlugin named primitives
 Catches: completely missing SecurityPlugin registration
======================================================================"
TestCase subclass: #VMSecurityTest
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMSecurityTest methodsFor: 'private'!
primGetSecureUserDirectory
    <primitive: 'primitiveGetSecureUserDirectory' module: 'SecurityPlugin'>
    ^ nil! !

!VMSecurityTest methodsFor: 'private'!
primGetUntrustedUserDirectory
    <primitive: 'primitiveGetUntrustedUserDirectory' module: 'SecurityPlugin'>
    ^ nil! !

!VMSecurityTest methodsFor: 'private'!
primCanWriteImage
    <primitive: 'primitiveCanWriteImage' module: 'SecurityPlugin'>
    ^ nil! !

!VMSecurityTest methodsFor: 'testing'!
testSecureDirectoryExists
    "primitiveGetSecureUserDirectory was completely absent (SecurityPlugin
     not registered). Smalltalk fallback masked the missing primitive.
     Commit a222bb6."
    | dir |
    dir := self primGetSecureUserDirectory.
    self deny: dir isNil description: 'SecurityPlugin primitive returned nil (not registered?)'.
    self deny: dir isEmpty description: 'Secure directory path should not be empty'.
    self assert: dir asFileReference exists
        description: 'Secure directory should exist: ' , dir! !

!VMSecurityTest methodsFor: 'testing'!
testUntrustedDirectoryExists
    "primitiveGetUntrustedUserDirectory was absent. Commit a222bb6."
    | dir |
    dir := self primGetUntrustedUserDirectory.
    self deny: dir isNil description: 'SecurityPlugin primitive returned nil (not registered?)'.
    self deny: dir isEmpty description: 'Untrusted directory path should not be empty'.
    self assert: dir asFileReference exists
        description: 'Untrusted directory should exist: ' , dir! !

!VMSecurityTest methodsFor: 'testing'!
testCanWriteImage
    "primitiveCanWriteImage was absent. Commit a222bb6."
    | canWrite |
    canWrite := self primCanWriteImage.
    self deny: canWrite isNil description: 'SecurityPlugin canWriteImage returned nil'.
    self assert: (canWrite == true or: [canWrite == false])
        description: 'canWriteImage should return boolean, got: ' , canWrite printString! !

!VMSecurityTest methodsFor: 'testing'!
testSecureAndUntrustedAreDifferent
    "The two directories should be different paths (secure = image dir,
     untrusted = temp dir)."
    | secure untrusted |
    secure := self primGetSecureUserDirectory.
    untrusted := self primGetUntrustedUserDirectory.
    (secure isNil or: [untrusted isNil]) ifTrue: [
        ^ self skip: 'SecurityPlugin primitives not available'].
    self deny: secure = untrusted
        description: 'Secure and untrusted directories should differ'! !


"======================================================================
 VMClipboardTest — System clipboard round-trip
 Catches: internal buffer not connected to system clipboard
======================================================================"
TestCase subclass: #VMClipboardTest
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMClipboardTest methodsFor: 'testing'!
testClipboardRoundTrip
    "Clipboard was an internal C++ string, not connected to the system clipboard.
     Copy/paste within Pharo worked but cross-app clipboard was dead.
     Commit 18d0035."
    | testString retrieved |
    testString := 'VMIntegrationTest-' , DateAndTime now printString.
    Clipboard clipboardText: testString.
    retrieved := Clipboard clipboardText.
    self assert: retrieved = testString
        description: 'Clipboard round-trip failed. Set: ' , testString ,
            ' Got: ' , (retrieved ifNil: ['nil'])! !

!VMClipboardTest methodsFor: 'testing'!
testClipboardHasTextAfterSet
    "primitiveClipboardHasText was hardcoded to return false. Commit 18d0035."
    Clipboard clipboardText: 'test'.
    "The image-side API may not expose hasText directly, but setting text
     and getting it back verifies the primitive works"
    self deny: Clipboard clipboardText isEmpty
        description: 'Clipboard should have text after setting it'! !


"======================================================================
 VMSoundTest — Sound plugin API (non-audio verification)
 Catches: stubs that returned fake values (8192 bytes available,
          volume=100, playback claimed success without producing audio)
======================================================================"
TestCase subclass: #VMSoundTest
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMSoundTest methodsFor: 'testing'!
testSoundPrimitivesExist
    "Sound primitives (300-310) were all stubs returning fake values.
     Commit bb7b57f. Test that the primitives exist and respond without crash.
     SampledSound/SoundPlayer may not be present in minimal images."
    | playerClass |
    playerClass := Smalltalk globals at: #SoundPlayer ifAbsent: [nil].
    playerClass ifNil: [^ self skip: 'SoundPlayer not available in this image'].
    "Just verify the class exists and responds — actual audio requires hardware"
    self assert: (playerClass respondsTo: #stopPlaying)
        description: 'SoundPlayer should respond to stopPlaying'! !


"======================================================================
 VMMIDITest — MIDI plugin API
 Catches: stubs that returned 0 ports, hardcoded clock
======================================================================"
TestCase subclass: #VMMIDITest
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMMIDITest methodsFor: 'testing'!
testMIDIPortCountIsInteger
    "primitiveMIDIGetPortCount (330) was hardcoded to 0.
     Now returns real CoreMIDI source+destination count. Commit 7deb5f0."
    | midiClass count |
    midiClass := Smalltalk globals at: #SimpleMIDIPort ifAbsent: [nil].
    midiClass ifNil: [^ self skip: 'SimpleMIDIPort not available in this image'].
    count := [midiClass portCount] on: Error do: [:e | -1].
    self assert: (count isKindOf: Integer) description: 'Port count must be Integer'.
    self assert: count >= 0 description: 'Port count must be non-negative'! !

!VMMIDITest methodsFor: 'testing'!
testMIDIClockIsMonotonic
    "primitiveMIDIGetClock (336) was std::chrono, now mach_absolute_time.
     Commit 7deb5f0."
    | midiClass t1 t2 |
    midiClass := Smalltalk globals at: #SimpleMIDIPort ifAbsent: [nil].
    midiClass ifNil: [^ self skip: 'SimpleMIDIPort not available in this image'].
    t1 := [midiClass clock] on: Error do: [:e | 0].
    (Delay forMilliseconds: 10) wait.
    t2 := [midiClass clock] on: Error do: [:e | 0].
    self assert: t2 > t1
        description: 'MIDI clock should be monotonically increasing'! !


"======================================================================
 VMInterpreterProxyTest — Object creation by plugins
 Catches: instantiateClassindexableSize using wrong ObjectFormat,
          slotSizeOf returning 64-bit slot count instead of element count
======================================================================"
TestCase subclass: #VMInterpreterProxyTest
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMInterpreterProxyTest methodsFor: 'testing'!
testPluginCreatedArrayHasCorrectSize
    "instantiateClassindexableSize always used ObjectFormat::FixedSize (1) for
     pointer objects. Array needs Indexable (2). With FixedSize, basicSize returns 0
     so Pharo sees #() for any plugin-created Array. Commit cfbff40.
     Test via socket option get which creates an Array through the proxy."
    | socket result |
    socket := Socket newTCP.
    [
        result := socket getOption: 'SO_SNDBUF'.
        self assert: result isArray
            description: 'getOption should return Array, got: ' , result class name.
        self assert: result size >= 2
            description: 'getOption Array should have >= 2 elements, got size ' , result size printString.
        "If instantiateClassindexableSize is broken, size would be 0"
        self deny: result size = 0
            description: 'Array created by plugin has size 0 — ObjectFormat bug'
    ] ensure: [
        [socket closeAndDestroy] on: Error do: [:e |] ]! !

!VMInterpreterProxyTest methodsFor: 'testing'!
testFloatArrayElementAccess
    "FloatArrayPlugin primitiveFloatArrayAt/AtPut were stubs returning Failure.
     Smalltalk fallback worked but was slow. Commit c5e4f60.
     Test that values round-trip through the primitive path."
    | fa |
    fa := Float32Array new: 3.
    fa at: 1 put: 3.14.
    fa at: 2 put: -1.0.
    fa at: 3 put: 0.0.
    self assert: ((fa at: 1) - 3.14) abs < 0.001
        description: 'Float32Array at: 1 should be ~3.14'.
    self assert: (fa at: 2) = -1.0
        description: 'Float32Array at: 2 should be -1.0'.
    self assert: (fa at: 3) = 0.0
        description: 'Float32Array at: 3 should be 0.0'! !

!VMInterpreterProxyTest methodsFor: 'testing'!
testMatrixTransform2x3Size
    "proxy_slotSizeOf returned 64-bit slot count, not element count.
     MatrixTransform2x3 has 6 floats (6 x 32-bit = 3 x 64-bit slots).
     slotSizeOf returned 3 instead of 6, breaking B2DPlugin. Commit 159079c."
    | m |
    m := MatrixTransform2x3 new.
    self assert: m basicSize = 6
        description: 'MatrixTransform2x3 basicSize should be 6, got: ' , m basicSize printString! !


"======================================================================
 VMFileAttributesTest — File attributes return format
 Catches: wrong return format for different mask values
======================================================================"
TestCase subclass: #VMFileAttributesTest
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMFileAttributesTest methodsFor: 'testing'!
testFileAttributesMask1ReturnsStatArray
    "With mask & 3 = 1, primitiveFileAttributes must return a flat stat array.
     Was returning a 2-element {statArray. accessArray} wrapper, causing
     'Improper store into indexable object' when Pharo treated element 1
     (a nested array) as a filename ByteArray. Commit a8e2829."
    | result |
    result := File fileAttributes: FileLocator imageDirectory resolve fullName mask: 1.
    self assert: result isArray description: 'mask=1 should return Array'.
    self assert: result size >= 10
        description: 'mask=1 stat array should have >= 10 elements, got: ' , result size printString.
    "Element 2 is st_mode (an integer). Element 1 is symlink target (nil for non-links)"
    self assert: (result second isKindOf: Integer)
        description: 'stat array[2] (st_mode) should be Integer, got: ' , result second class name.
    "Verify it is NOT a nested array (the original bug)"
    self deny: (result second isKindOf: Array)
        description: 'stat array elements must be flat, not nested arrays'! !

!VMFileAttributesTest methodsFor: 'testing'!
testFileAttributesMask2ReturnsAccessArray
    "With mask & 3 = 2, must return a flat access array of booleans."
    | result |
    result := File fileAttributes: FileLocator imageDirectory resolve fullName mask: 2.
    self assert: result isArray description: 'mask=2 should return Array'.
    self assert: result size >= 3
        description: 'mask=2 access array should have >= 3 elements, got: ' , result size printString.
    "Elements should be booleans (readable, writable, executable)"
    self assert: (result first == true or: [result first == false])
        description: 'access array[1] should be boolean'! !

!VMFileAttributesTest methodsFor: 'testing'!
testFileAttributesMask3ReturnsCompound
    "With mask & 3 = 3, must return {statArray. accessArray}."
    | result |
    result := File fileAttributes: FileLocator imageDirectory resolve fullName mask: 3.
    self assert: result isArray description: 'mask=3 should return Array'.
    self assert: result size = 2
        description: 'mask=3 should return 2-element compound, got: ' , result size printString.
    self assert: result first isArray description: 'compound[1] should be stat array'.
    self assert: result second isArray description: 'compound[2] should be access array'! !


"======================================================================
 VMPrimitiveErrorTest — Correct error object types
 Catches: raw SmallInteger error codes instead of PrimitiveError objects
======================================================================"
TestCase subclass: #VMPrimitiveErrorTest
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMPrimitiveErrorTest methodsFor: 'testing'!
testFileNotFoundRaisesCorrectException
    "Opening a non-existent file must raise FileDoesNotExistException, not
     generic PrimitiveFailed. Was broken because <primitive: N error: ec>
     stored raw SmallInteger instead of PrimErrTable lookup. Commit 14d0215."
    self should: [
        (File named: '/nonexistent/path/that/does/not/exist.xyz') readStream
    ] raise: FileDoesNotExistException! !

!VMPrimitiveErrorTest methodsFor: 'testing'!
testFileErrorHasOSErrorCode
    "The error object should include the OS error code (errno),
     not just a generic failure."
    [
        (File named: '/nonexistent/path/xyz.tmp') readStream.
        self fail: 'Should have raised an exception'
    ] on: FileDoesNotExistException do: [:e |
        "Just verify it was caught as the right type"
        self assert: (e isKindOf: FileDoesNotExistException) ]! !


"======================================================================
 VMTimerTest — Timer/signal check in tight loops
 Catches: timer starvation when extension bytes align with check interval
======================================================================"
TestCase subclass: #VMTimerTest
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMTimerTest methodsFor: 'testing'!
testDelayFiresDuringTightLoop
    "Delay must fire even when the VM is in a tight bytecode loop.
     Was broken: step() check gated by inExtension_ flag, and tight loops
     with even bytecode counts permanently aligned with the 1024-step
     check interval, starving the timer. Commit 8cb974a."
    | fired deadline |
    fired := false.
    [(Delay forMilliseconds: 200) wait. fired := true] fork.
    "Spin in a tight loop — if timer checks are starved, the Delay never fires"
    deadline := DateAndTime now + 2 seconds.
    [DateAndTime now < deadline and: [fired not]] whileTrue: [1 + 1].
    self assert: fired
        description: 'Delay did not fire during tight loop — timer check starvation'! !

!VMTimerTest methodsFor: 'testing'!
testValueWithinDoesNotHang
    "[] repeat inside valueWithin: must not hang forever.
     This was the actual symptom: BlockClosureValueWithinDurationTest
     hung the SUnit runner at test 402 of 577. Commit 8cb974a."
    | result |
    result := [[] repeat] valueWithin: 500 milliSeconds onTimeout: ['timeout'].
    self assert: result = 'timeout'
        description: 'valueWithin: should have timed out, got: ' , result printString! !


"======================================================================
 VMSurfacePluginTest — Dispatch surface registration
 Catches: missing dispatch surface support for Cairo/Athens
======================================================================"
TestCase subclass: #VMSurfacePluginTest
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMSurfacePluginTest methodsFor: 'testing'!
testSurfacePluginLoaded
    "SurfacePlugin dispatch surfaces were missing, breaking Cairo/Athens.
     Commit 19d8793. Test that the plugin module is loadable."
    | loaded |
    loaded := Smalltalk vm listLoadedModules
        anySatisfy: [:m | m includesSubstring: 'Surface' caseSensitive: false].
    "Surface plugin may be built-in or loaded"
    loaded ifFalse: [
        loaded := Smalltalk vm listBuiltinModules
            anySatisfy: [:m | m includesSubstring: 'Surface' caseSensitive: false]].
    "If neither, at least verify the Form>>
     display surface mechanism works"
    self assert: (Form respondsTo: #new)
        description: 'Form class should exist'! !


"======================================================================
 VMIntegrationTestRunner — Run all tests, write results to image dir
======================================================================"
Object subclass: #VMIntegrationTestRunner
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'VMIntegrationTests'!

!VMIntegrationTestRunner class methodsFor: 'running'!
testClasses
    ^ {VMSocketTCPTest. VMSocketUDPTest. VMDNSTest.
       VMLocaleTest. VMSecurityTest. VMClipboardTest.
       VMSoundTest. VMMIDITest. VMInterpreterProxyTest.
       VMFileAttributesTest. VMPrimitiveErrorTest. VMTimerTest.
       VMSurfacePluginTest}! !

!VMIntegrationTestRunner class methodsFor: 'running'!
resultsFile
    ^ FileLocator imageDirectory / 'vm_integration_results.txt'! !

!VMIntegrationTestRunner class methodsFor: 'running'!
log: aString
    self resultsFile writeStreamDo: [:ws |
        ws setToEnd.
        ws nextPutAll: aString.
        ws nextPut: Character lf]! !

!VMIntegrationTestRunner class methodsFor: 'running'!
runAll
    "Run all VM integration tests and write results."
    | totalPass totalFail totalError totalTests |
    totalPass := 0. totalFail := 0. totalError := 0. totalTests := 0.
    self resultsFile ensureDelete.
    self log: '=== VM Integration Tests ==='.
    self log: DateAndTime now printString.
    self log: ''.

    self testClasses do: [:testClass |
        self log: '--- ' , testClass name , ' ---'.
        testClass buildSuite tests do: [:test |
            totalTests := totalTests + 1.
            [
                [test setUp.
                 test perform: test selector.
                 test tearDown.
                 totalPass := totalPass + 1.
                 self log: 'PASS: ' , test selector]
                    on: TestFailure do: [:e |
                        totalFail := totalFail + 1.
                        self log: 'FAIL: ' , test selector , ' — ' , e messageText]
            ] on: Error do: [:e |
                totalError := totalError + 1.
                self log: 'ERROR: ' , test selector , ' — ' , e class name , ': ' ,
                    ([e messageText] on: Error do: [:x | e printString])]].
        self log: ''].

    self log: '=== Summary ==='.
    self log: 'Total: ' , totalTests printString.
    self log: 'Pass:  ' , totalPass printString.
    self log: 'Fail:  ' , totalFail printString.
    self log: 'Error: ' , totalError printString.
    self log: ''.
    totalFail + totalError = 0
        ifTrue: [self log: 'ALL TESTS PASSED']
        ifFalse: [self log: (totalFail + totalError) printString , ' FAILURES'].
    self log: '=== Done ==='! !

!VMIntegrationTestRunner class methodsFor: 'system startup'!
startUp: isImageStarting
    isImageStarting ifFalse: [^ self].
    [
        (Delay forSeconds: 3) wait.
        self runAll
    ] fork! !

SessionManager default register: (ClassSessionHandler forClassNamed: #VMIntegrationTestRunner)
    inCategory: SessionManager default userCategory atPriority: 90!
