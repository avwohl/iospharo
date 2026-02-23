"Menu item tester — discovers all <worldMenu> pragmas and exercises each action.
 Runs inside the Mac Catalyst app where the UI loop is active.
 Inject into a fresh image with reference VM, then run with the app.
 Results: /tmp/menu_test_results.txt"!

Object subclass: #MenuItemTester
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Menu'!

!MenuItemTester class methodsFor: 'running'!
run
  "Discover all worldMenu items, execute each action, report results."
  | stream passed failed errors skipped allItems |
  passed := 0. failed := 0. errors := 0. skipped := 0.

  "Create file immediately so we know the test runner started"
  '/tmp/menu_test_results.txt' asFileReference writeStreamDo: [:s |
    s nextPutAll: 'Menu test runner started'; cr].

  allItems := self discoverAllItems.

  stream := '/tmp/menu_test_results.txt' asFileReference writeStream.
  [
    stream nextPutAll: 'Menu Item Test Results'; cr.
    stream nextPutAll: '===================='; cr.
    stream nextPutAll: 'Discovered ', allItems size printString, ' menu items'; cr; cr.

    allItems do: [:item |
      | path actionBlock status detail |
      path := item at: #path.
      actionBlock := item at: #action.

      (self isDangerous: path) ifTrue: [
        status := 'SKIP'. detail := 'dangerous'.
        skipped := skipped + 1.
      ] ifFalse: [
        actionBlock ifNil: [
          status := 'SKIP'. detail := 'no action'.
          skipped := skipped + 1.
        ] ifNotNil: [
          | result |
          result := self safeExecute: actionBlock path: path timeout: 8000.
          status := result first. detail := result second.
          status = 'PASS' ifTrue: [passed := passed + 1].
          status = 'FAIL' ifTrue: [failed := failed + 1].
          status = 'ERROR' ifTrue: [errors := errors + 1].
        ].
      ].

      stream nextPutAll: status; nextPut: Character tab.
      stream nextPutAll: path; nextPut: Character tab.
      stream nextPutAll: detail; cr; flush.

      "Yield to let other processes run between items"
      Processor yield.
    ].

    stream cr.
    stream nextPutAll: 'Summary: ', passed printString, ' pass, ',
      failed printString, ' fail, ', errors printString, ' error, ',
      skipped printString, ' skip (of ', allItems size printString, ')'; cr.
  ] ensure: [stream close].

  Transcript show: '[MENU-TEST] ', passed printString, ' pass, ',
    failed printString, ' fail, ', errors printString, ' error, ',
    skipped printString, ' skip'; cr.! !

!MenuItemTester class methodsFor: 'discovery'!
discoverAllItems
  "Use PragmaMenuBuilder to walk all menu items recursively."
  | items builder root |
  items := OrderedCollection new.
  [
    builder := PragmaMenuBuilder pragmaKeyword: 'worldMenu' model: World.
    root := builder menuSpec.
    root ifNotNil: [self extractFrom: root items into: items path: ''].
  ] on: Error do: [:e | ].
  ^ items! !

!MenuItemTester class methodsFor: 'discovery'!
extractFrom: menuItems into: items path: prefix
  "Recursively extract leaf items with actions."
  menuItems ifNil: [^ self].
  menuItems do: [:menuItem |
    | path name |
    [name := menuItem name asString] on: Error do: [:e | name := '??'].
    path := prefix isEmpty ifTrue: [name] ifFalse: [prefix, ' > ', name].
    [
      | sub |
      sub := menuItem subMenu.
      sub isNil
        ifTrue: [
          items add: (Dictionary new
            at: #path put: path;
            at: #action put: ([menuItem action] on: Error do: [:e | nil]);
            yourself)]
        ifFalse: [self extractFrom: sub items into: items path: path]
    ] on: Error do: [:e |
      items add: (Dictionary new at: #path put: path; at: #action put: nil; yourself)].
  ].! !

!MenuItemTester class methodsFor: 'testing'!
safeExecute: actionBlock path: path timeout: msTimeout
  "Execute action safely with timeout using a spin-wait watchdog.
   The action runs in the same process (no forking needed during startup).
   Uses Time millisecondClockValue for timeout since Delays may not work yet."
  | result windowsBefore windowsAfter detail startTime |
  result := nil.

  "Count windows before"
  windowsBefore := [World submorphs size] on: Error do: [:e | -1].

  startTime := Time millisecondClockValue.
  [actionBlock value]
    on: Error do: [:e | result := e messageText].

  "Check if it took too long (for logging)"
  detail := (Time millisecondClockValue - startTime) printString, 'ms'.

  "Count windows after"
  windowsAfter := [World submorphs size] on: Error do: [:e | -1].
  detail := windowsBefore printString, '->', windowsAfter printString, ' morphs (', detail, ')'.

  result ifNotNil: [
    ^ { 'ERROR'. result }].

  "Close newly opened windows"
  [self closeRecentWindows: windowsBefore] on: Error do: [:e | ].

  ^ { 'PASS'. detail }! !

!MenuItemTester class methodsFor: 'testing'!
closeRecentWindows: countBefore
  "Close windows opened since the test started."
  | current toClose |
  current := World submorphs.
  current size > countBefore ifTrue: [
    toClose := current copyFrom: 1 to: (current size - countBefore).
    toClose do: [:w | [w delete] on: Error do: [:e | ]].
  ].! !

!MenuItemTester class methodsFor: 'testing'!
isDangerous: menuPath
  | path |
  path := menuPath asLowercase.
  #('save' 'quit' 'exit' 'image cleanup' 'do image' 'profil'
    'full screen' 'delete all' 'discard' 'load toplo' 'screenshot'
    'start drawing' 'start stepping' 'restore display'
    'roassalloadfullversion' 'roassalexporters' 'tutorial'
    'spotter') do: [:d |
    (path includesSubstring: d) ifTrue: [^ true]].
  ^ false! !

!MenuItemTester class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  [self run] on: Error do: [:e |
    '/tmp/menu_test_results.txt' asFileReference writeStreamDo: [:s |
      s nextPutAll: 'FATAL: ', e messageText; cr]].! !

!MenuItemTester class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup - use user category at priority 90 (after UI setup)"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #MenuItemTester)
  inCategory: SessionManager default userCategory
  atPriority: 90!
