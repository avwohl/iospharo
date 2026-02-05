SessionManager default registerSystemClassNamed: #DebugStartupHandler atPriority: 1.

DebugStartupHandler := Object subclass: #DebugStartupHandler
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'Debug'.

DebugStartupHandler class compile: 'startUp: isImageStarting
    "/tmp/iospharo-debug-startup.txt" asFileReference writeStreamDo: [ :s |
        s nextPutAll: ''startUp: called, isImageStarting=''.
        s nextPutAll: isImageStarting printString.
        s cr.
        s nextPutAll: ''commandLine args: ''.
        (1 to: 10) do: [ :i |
            | attr |
            attr := [ Smalltalk vm getSystemAttribute: i + 1 ] on: Error do: [ nil ].
            attr ifNotNil: [ s nextPutAll: i printString, ''->'', attr printString, '' '' ] ].
        s cr ].
    Smalltalk session addDeferredStartupAction: [
        "/tmp/iospharo-debug-deferred.txt" asFileReference writeStreamDo: [ :s |
            s nextPutAll: ''deferred action ran at: ''.
            s nextPutAll: DateAndTime now printString.
            s cr.
            s nextPutAll: ''isHeadless: '', Smalltalk isHeadless printString.
            s cr.
            s nextPutAll: ''isInteractiveGraphic: '', SmalltalkImage current isInteractiveGraphic printString.
            s cr ] ]'.
