"Test world renderer with OSiOSDriver"!

Object subclass: #WorldRendererTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!WorldRendererTest class methodsFor: 'running'!
run
  | stream driver |
  stream := '/tmp/world_renderer_test.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== World Renderer Test ==='; cr; cr.

    "Step 1: Get or create OSiOSDriver"
    stream nextPutAll: '--- Step 1: Setup OSiOSDriver ---'; cr.
    driver := OSiOSDriver new.
    driver setupEventLoop.
    stream nextPutAll: 'OSiOSDriver created and event loop started'; cr.
    stream flush.

    "Step 2: Set as default driver"
    stream cr; nextPutAll: '--- Step 2: Set as default driver ---'; cr.
    [OSWindowDriver classVarNamed: 'Current' put: driver.
     stream nextPutAll: 'Set Current classVar to OSiOSDriver'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Error setting driver: '; print: ex messageText; cr].
    stream flush.

    "Step 3: Check WorldMorph"
    stream cr; nextPutAll: '--- Step 3: Check WorldMorph ---'; cr.
    [| world |
     world := World.
     stream nextPutAll: 'World: '; print: world class name; cr.
     stream nextPutAll: 'worldState: '; print: world worldState class name; cr.
     stream nextPutAll: 'worldRenderer: '; print: world worldState worldRenderer class name; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Error: '; print: ex messageText; cr].
    stream flush.

    "Step 4: Try to create OSWorldRenderer with our driver"
    stream cr; nextPutAll: '--- Step 4: Create OSWorldRenderer ---'; cr.
    [| renderer |
     renderer := OSWorldRenderer new.
     stream nextPutAll: 'OSWorldRenderer created: '; print: renderer class name; cr.
     renderer driver: driver.
     stream nextPutAll: 'Driver set to: '; print: driver class name; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Error: '; print: ex messageText; cr].
    stream flush.

    "Step 5: Check if events can be dispatched"
    stream cr; nextPutAll: '--- Step 5: Event dispatch test ---'; cr.
    [| map |
     map := OSiOSDriver windowMap.
     stream nextPutAll: 'WindowMap size: '; print: map size; cr.
     map keysAndValuesDo: [:k :v |
       stream nextPutAll: '  '; print: k; nextPutAll: ' -> '; print: v class name; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'Error: '; print: ex messageText; cr].
    stream flush.

    "Step 6: Test primitive 264"
    stream cr; nextPutAll: '--- Step 6: Primitive 264 ---'; cr.
    [| buf |
     buf := Array new: 8.
     1 to: 8 do: [:i | buf at: i put: 0].
     driver primGetNextEvent: buf.
     stream nextPutAll: 'Event buffer: '.
     buf do: [:v | stream print: v; space].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Error: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!WorldRendererTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!WorldRendererTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #WorldRendererTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
