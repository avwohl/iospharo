"Test driver selection logic"!

Object subclass: #DriverSelectionTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!DriverSelectionTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/driver_selection.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Driver Selection Test ==='; cr; cr.

    "Test 1: SDL2 availability"
    stream nextPutAll: '--- Test 1: SDL2 Availability ---'; cr.
    [stream nextPutAll: 'SDL2 isAvailable: '; print: SDL2 isAvailable; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: Check each driver's isSuitable"
    stream cr; nextPutAll: '--- Test 2: Each driver isSuitable ---'; cr.
    [OSWindowDriver subclassesDo: [:driver |
       stream nextPutAll: driver name; nextPutAll: ' isSuitable: '.
       [stream print: driver isSuitable]
       on: Error do: [:e | stream nextPutAll: 'ERROR: '; print: e messageText].
       stream cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: Check OSSDL2Driver isSuitable specifically"
    stream cr; nextPutAll: '--- Test 3: OSSDL2Driver isSuitable analysis ---'; cr.
    [| sdlAvail ffiBknd |
     sdlAvail := SDL2 isAvailable.
     stream nextPutAll: 'SDL2 isAvailable = '; print: sdlAvail; cr.
     ffiBknd := FFIBackend current.
     stream nextPutAll: 'FFIBackend current = '; print: ffiBknd; cr.
     stream nextPutAll: 'FFIBackend current class = '; print: ffiBknd class; cr.
     stream nextPutAll: 'FFIBackend current isAvailable = '; print: ffiBknd isAvailable; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: What does pickDriverClass return?"
    stream cr; nextPutAll: '--- Test 4: pickDriverClass ---'; cr.
    [| picked |
     picked := OSWindowDriver pickDriverClass.
     stream nextPutAll: 'pickDriverClass returns: '; print: picked; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: Why doesnt OSSDL2Driver get picked?"
    stream cr; nextPutAll: '--- Test 5: OSSDL2Driver analysis ---'; cr.
    [stream nextPutAll: 'OSSDL2Driver isSuitable source:'; cr.
     stream nextPutAll: (OSSDL2Driver class >> #isSuitable) sourceCode; cr; cr.
     stream nextPutAll: 'Evaluating SDL2 isAvailable inside block:'; cr.
     [stream nextPutAll: '  SDL2 isAvailable = '; print: SDL2 isAvailable; cr]
       on: Error do: [:e | stream nextPutAll: '  ERROR: '; print: e messageText; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 6: Try to load SDL2 library"
    stream cr; nextPutAll: '--- Test 6: SDL2 library loading ---'; cr.
    [| ffiLoader |
     stream nextPutAll: 'SDL2 libraryName = '.
     [stream print: SDL2 libraryName] on: Error do: [:e | stream nextPutAll: 'ERROR: '; print: e messageText].
     stream cr.
     stream nextPutAll: 'SDL2 moduleName = '.
     [stream print: SDL2 moduleName] on: Error do: [:e | stream nextPutAll: 'ERROR: '; print: e messageText].
     stream cr.
     stream nextPutAll: 'FFIMacLibraryFinder findAnyLibrary: SDL2 ffiLibrary ... '.
     [| lib |
      lib := FFIMacLibraryFinder findAnyLibrary: SDL2 ffiLibrary.
      stream print: lib]
     on: Error do: [:e | stream nextPutAll: 'ERROR: '; print: e messageText].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!DriverSelectionTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!DriverSelectionTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #DriverSelectionTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
