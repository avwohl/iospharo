Object subclass: #TFFITestRunner instanceVariableNames: '' classVariableNames: '' package: 'TFFITest'!

!TFFITestRunner class methodsFor: 'system startup' stamp: 'test'!
startUp: isImageStarting
    isImageStarting ifFalse: [ ^ self ].
    [ self runTest ] fork! !

!TFFITestRunner class methodsFor: 'running' stamp: 'test'!
runTest
    | log |
    log := '/tmp/tffi_test_results.txt' asFileReference.
    log ensureDelete.
    [
        log writeStreamDo: [:s |
            s nextPutAll: 'TFFI Test Results'; cr; cr.

            s nextPutAll: 'Step 1: Invalidating all TFBasicType handles...'; cr.
            TFBasicType allInstances do: [:t |
                t instVarNamed: 'handle' put: ExternalAddress new
            ].

            s nextPutAll: 'Step 2: Validating basic types (primitiveFillBasicType)...'; cr.
            TFBasicType allInstances do: [:t |
                [ t validate.
                  s nextPutAll: '  OK: ', t typeName, ' typeCode=', t typeCode printString, ' byteSize=', t byteSize printString; cr
                ] on: Error do: [:e |
                  s nextPutAll: '  FAIL: ', t typeName, ' - ', e messageText; cr
                ]
            ].

            s nextPutAll: 'Step 3: Testing primitiveGetSameThreadRunnerAddress...'; cr.
            [ | runner addr |
              TFSameThreadRunner allInstances do: [:r |
                r instVarNamed: 'handle' put: ExternalAddress new
              ].
              runner := TFSameThreadRunner new.
              runner doInitialize.
              addr := runner instVarNamed: 'handle'.
              s nextPutAll: '  Runner handle: ', addr printString; cr.
              s nextPutAll: '  Runner handle null?: ', addr isNull printString; cr
            ] on: Error do: [:e |
              s nextPutAll: '  FAIL: ', e messageText; cr
            ].

            s nextPutAll: 'Step 4: Testing primitiveDefineFunction...'; cr.
            [ | def sint32Type |
              sint32Type := TFBasicType sint32.
              sint32Type validate.
              def := TFFunctionDefinition parameterTypes: #() returnType: sint32Type.
              def validate.
              s nextPutAll: '  Function definition created'; cr.
              s nextPutAll: '  CIF handle null?: ', (def instVarNamed: 'handle') isNull printString; cr
            ] on: Error do: [:e |
              s nextPutAll: '  FAIL: ', e messageText; cr
            ].

            s cr. s nextPutAll: 'Done.'; cr
        ]
    ] on: Error do: [:e |
        log writeStreamDo: [:s |
            s nextPutAll: 'FATAL ERROR: ', e messageText; cr
        ]
    ]! !

SessionManager default registerUserClassNamed: #TFFITestRunner!
