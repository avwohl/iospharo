"Install OSiOSDriver as the active window driver"!

Object subclass: #IOSDriverInstaller
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!IOSDriverInstaller class methodsFor: 'installation'!
install
  | stream driverClass driver |
  stream := '/tmp/ios_driver_install.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== iOS Driver Installation ==='; cr; cr.

    "Step 1: Check if OSiOSDriver exists"
    stream nextPutAll: '--- Step 1: Check OSiOSDriver ---'; cr.
    driverClass := Smalltalk at: #OSiOSDriver ifAbsent: [nil].
    driverClass
      ifNil: [
        stream nextPutAll: 'OSiOSDriver not found - creating it'; cr.
        self createDriverClass.
        driverClass := Smalltalk at: #OSiOSDriver]
      ifNotNil: [stream nextPutAll: 'OSiOSDriver already exists'; cr].
    stream flush.

    "Step 2: Create driver instance"
    stream cr; nextPutAll: '--- Step 2: Create driver instance ---'; cr.
    driver := driverClass new.
    stream nextPutAll: 'Driver created: '; print: driver class name; cr.
    stream flush.

    "Step 3: Set as current driver"
    stream cr; nextPutAll: '--- Step 3: Set as current driver ---'; cr.
    [OSWindowDriver setAsDefault: driver.
     stream nextPutAll: 'Driver set as default'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'setAsDefault failed: '; print: ex messageText; cr.
      "Try alternative method"
      [OSWindowDriver classVarNamed: 'Current' put: driver.
       stream nextPutAll: 'Set via classVar directly'; cr]
      on: Error do: [:ex2 |
        stream nextPutAll: 'classVar set also failed: '; print: ex2 messageText; cr]].
    stream flush.

    "Step 4: Setup event loop"
    stream cr; nextPutAll: '--- Step 4: Setup event loop ---'; cr.
    [driver setupEventLoop.
     stream nextPutAll: 'Event loop started'; cr.
     (Delay forMilliseconds: 100) wait.
     stream nextPutAll: 'EventLoopProcess: '; print: driverClass eventLoopProcess; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'setupEventLoop failed: '; print: ex messageText; cr].
    stream flush.

    "Step 5: Verify current driver"
    stream cr; nextPutAll: '--- Step 5: Verify ---'; cr.
    [| current |
     current := OSWindowDriver current.
     stream nextPutAll: 'OSWindowDriver current: '; print: current class name; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Verification failed: '; print: ex messageText; cr].
    stream flush.

    "Step 6: Try to create a window"
    stream cr; nextPutAll: '--- Step 6: Test window creation ---'; cr.
    [| win |
     win := OSWindow new.
     stream nextPutAll: 'OSWindow created: '; print: win class name; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Window creation failed: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Installation Complete ==='; cr]
  ensure: [stream close]! !

!IOSDriverInstaller class methodsFor: 'installation'!
createDriverClass
  "Create OSiOSDriver if it does not exist - assumes create_ios_driver.st was already loaded"
  self error: 'OSiOSDriver must be loaded before this script'! !

!IOSDriverInstaller class methodsFor: 'running'!
run
  self install.
  Smalltalk exitSuccess! !

!IOSDriverInstaller class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!IOSDriverInstaller class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #IOSDriverInstaller)
  inCategory: SessionManager default userCategory
  atPriority: 90!
