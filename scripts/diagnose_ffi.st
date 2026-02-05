"Diagnose FFI method resolution issue"!

Object subclass: #FFIDiagnostic
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!FFIDiagnostic class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/ffi_diagnostic.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== FFI Diagnostic ==='; cr; cr.

    "Step 1: Check FFICalloutMethodBuilder class hierarchy"
    stream nextPutAll: '--- Step 1: FFICalloutMethodBuilder class hierarchy ---'; cr.
    [| cls |
     cls := Smalltalk at: #FFICalloutMethodBuilder ifAbsent: [nil].
     cls
       ifNil: [stream nextPutAll: 'FFICalloutMethodBuilder NOT FOUND'; cr]
       ifNotNil: [
         stream nextPutAll: 'FFICalloutMethodBuilder exists'; cr.
         stream nextPutAll: 'Superclass chain: '.
         [cls notNil] whileTrue: [
           stream print: cls name; nextPutAll: ' -> '.
           cls := cls superclass].
         stream nextPutAll: 'nil'; cr.

         "Check if createFFICalloutLiteralFromSpec: is defined"
         stream nextPutAll: 'Looking for createFFICalloutLiteralFromSpec:'; cr.
         cls := Smalltalk at: #FFICalloutMethodBuilder.
         (cls canUnderstand: #createFFICalloutLiteralFromSpec:)
           ifTrue: [stream nextPutAll: '  canUnderstand: TRUE'; cr]
           ifFalse: [stream nextPutAll: '  canUnderstand: FALSE (BUG)'; cr].

         "Find which class actually implements it"
         stream nextPutAll: 'Searching for implementation:'; cr.
         (cls withAllSuperclasses) do: [:c |
           (c methodDict includesKey: #createFFICalloutLiteralFromSpec:)
             ifTrue: [stream nextPutAll: '  Found in: '; print: c name; cr]]]]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 1 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 2: Check available FFI classes"
    stream cr; nextPutAll: '--- Step 2: FFI-related classes ---'; cr.
    [#(FFICalloutMethodBuilder FFIBackend FFIThreadedFFIBackend TFCalloutMethodBuilder
       FFICallout TFExternalFunction ExternalLibraryFunction) do: [:name |
         | cls |
         cls := Smalltalk at: name ifAbsent: [nil].
         stream nextPutAll: '  '; print: name; nextPutAll: ': '.
         cls
           ifNil: [stream nextPutAll: 'NOT FOUND']
           ifNotNil: [stream nextPutAll: 'exists, super='; print: cls superclass name].
         stream cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 2 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 3: Try to trace the actual error"
    stream cr; nextPutAll: '--- Step 3: Try FFI callout ---'; cr.
    [| backend |
     backend := FFIBackend current.
     stream nextPutAll: 'FFIBackend current: '; print: backend class name; cr.
     stream nextPutAll: 'backend calloutMethodBuilderClass: ';
       print: backend calloutMethodBuilderClass name; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 3 ERROR: '; print: ex class name; nextPutAll: ' - '; print: ex messageText; cr].
    stream flush.

    "Step 4: Check method lookup basics"
    stream cr; nextPutAll: '--- Step 4: Method lookup sanity ---'; cr.
    [stream nextPutAll: 'Object >> class: '; print: (Object canUnderstand: #class); cr.
     stream nextPutAll: 'Object >> subclassResponsibility: '; print: (Object canUnderstand: #subclassResponsibility); cr.
     stream nextPutAll: 'Array >> new: '; print: (Array class canUnderstand: #new:); cr.
     stream nextPutAll: 'OrderedCollection >> add: '; print: (OrderedCollection canUnderstand: #add:); cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 4 ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 5: Check if OSSDL2Driver can be used"
    stream cr; nextPutAll: '--- Step 5: OSSDL2Driver ---'; cr.
    [| driverClass |
     driverClass := Smalltalk at: #OSSDL2Driver ifAbsent: [nil].
     driverClass
       ifNil: [stream nextPutAll: 'OSSDL2Driver NOT FOUND'; cr]
       ifNotNil: [
         stream nextPutAll: 'OSSDL2Driver exists'; cr.
         stream nextPutAll: 'isApplicableFor: '; print: (driverClass isApplicableFor: nil); cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'Step 5 ERROR: '; print: ex class name; nextPutAll: ' - '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!FFIDiagnostic class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!FFIDiagnostic class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #FFIDiagnostic)
  inCategory: SessionManager default userCategory
  atPriority: 90!
