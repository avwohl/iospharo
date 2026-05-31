"Full-coverage, crash-resilient SUnit runner for the custom iOS Pharo VM.

Goal: run ALL concrete TestCase subclasses (not just a curated 565), and keep
making forward progress even when a test class hard-hangs or segfaults the VM.

Robustness model (no dependence on in-image Delay/watchdog, which is buggy on
this VM): an external shell driver relaunches the VM repeatedly. State lives in
/tmp files so progress survives a crash:
   /tmp/sunit_done.txt        classes whose result is durably recorded
   /tmp/sunit_blacklist.txt   classes that crashed/hung the VM (skipped)
   /tmp/sunit_current.txt     class currently running (crash marker)
   /tmp/sunit_test_detail.txt per-class CLS line + FAIL/ERR lines (appended, flushed)
   /tmp/sunit_test_results.txt final totals
   /tmp/sunit_ALL_DONE.txt    sentinel: every class processed
Each launch: startUp: fires runAllTests, which blacklists the previous crasher
(named in sunit_current.txt but not in sunit_done.txt), then runs every class
not in done|blacklist, recording each durably before moving on."

"=== compat shims (mirrors the old harness) ==="
ClassDescription compile: 'methodsFor: aCategory
  ^ self' classified: '*chunk-format-compat'!

"FileReference>>lines: our VM mishandles the upstream non-local-return form."
FileReference compile: 'lines
  | content |
  content := self readStreamDo: [:s | s contents].
  ^ content lines' classified: '*sunit-runner-shim'!

"Terminate handler-context chain so exception handling does not DNU-loop."
UndefinedObject compile: 'findNextHandlerContext
  ^ nil' classified: 'private'!

Object subclass: #SUnitRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

"--- file helpers ---"
SUnitRunner class compile: 'readLines: path
  | ref raw |
  ref := path asFileReference.
  ref exists ifFalse: [^ OrderedCollection new].
  raw := ref readStreamDo: [:s | s upToEnd].
  ^ (raw lines collect: [:l | l trimBoth]) select: [:l | l notEmpty]' classified: 'io'!

SUnitRunner class compile: 'appendLine: aString to: path
  path asFileReference appendStreamDo: [:s | s nextPutAll: aString; lf]' classified: 'io'!

SUnitRunner class compile: 'writeFile: path contents: aString
  | ref |
  ref := path asFileReference.
  ref exists ifTrue: [ref ensureDelete].
  ref writeStreamDo: [:s | s nextPutAll: aString]' classified: 'io'!

SUnitRunner class compile: 'nextRunNumber
  | file num |
  file := ''/tmp/sunit_run_number.txt'' asFileReference.
  num := [file contents asInteger] on: Error do: [:e | 0].
  num := num + 1.
  self writeFile: ''/tmp/sunit_run_number.txt'' contents: num printString.
  ^ num' classified: 'running'!

"--- class list: curated-first, then ALL concrete TestCase subclasses sorted ---"
SUnitRunner class compile: 'allTestClassNames
  | names seen |
  names := OrderedCollection new.
  seen := Set new.
  (self readLines: ''/tmp/sunit_test_classes.txt'') do: [:n |
    (seen includes: n) ifFalse: [seen add: n. names add: n]].
  ((TestCase allSubclasses reject: [:c | c isAbstract])
     asSortedCollection: [:a :b | a name <= b name]) do: [:c |
       (seen includes: c name) ifFalse: [seen add: c name. names add: c name]].
  "optional restriction: only run names listed in sunit_class_names.txt"
  (''/tmp/sunit_class_names.txt'' asFileReference exists) ifTrue: [
    | only |
    only := Set withAll: (self readLines: ''/tmp/sunit_class_names.txt'').
    only notEmpty ifTrue: [names := names select: [:n | only includes: n]]].
  ^ names' classified: 'running'!

"--- run one class, durably record its result ---"
SUnitRunner class compile: 'runOneClass: cls named: n
  | result p f e sk |
  p := f := e := sk := 0.
  [ | suite |
    suite := cls suite.
    result := suite run.
    p := result passedCount. f := result failureCount.
    e := result errorCount. sk := result skippedCount.
    ''/tmp/sunit_test_detail.txt'' asFileReference appendStreamDo: [:s |
      s nextPutAll: ''CLS ''; nextPutAll: n; nextPutAll: '' P ''; print: p;
        nextPutAll: '' F ''; print: f; nextPutAll: '' E ''; print: e;
        nextPutAll: '' S ''; print: sk; lf.
      result failures do: [:t | s nextPutAll: ''FAIL ''; nextPutAll: n;
        nextPutAll: ''>>''; nextPutAll: t selector asString; lf].
      result errors do: [:t | s nextPutAll: ''ERR ''; nextPutAll: n;
        nextPutAll: ''>>''; nextPutAll: t selector asString; lf]] ]
  on: Error do: [:ex |
    ''/tmp/sunit_test_detail.txt'' asFileReference appendStreamDo: [:s |
      s nextPutAll: ''SUITEERR ''; nextPutAll: n; nextPutAll: '' '';
        nextPutAll: ex class name; lf]]' classified: 'running'!

"--- totals from the detail log ---"
SUnitRunner class compile: 'writeTotals: runNum
  | tp tf te ts nc |
  tp := tf := te := ts := nc := 0.
  (self readLines: ''/tmp/sunit_test_detail.txt'') do: [:line |
    (line beginsWith: ''CLS '') ifTrue: [
      | parts |
      parts := line substrings: '' ''.
      parts size >= 10 ifTrue: [
        nc := nc + 1.
        tp := tp + (parts at: 4) asInteger.
        tf := tf + (parts at: 6) asInteger.
        te := te + (parts at: 8) asInteger.
        ts := ts + (parts at: 10) asInteger]]].
  self writeFile: ''/tmp/sunit_test_results.txt'' contents:
    (String streamContents: [:s |
      s nextPutAll: ''run ''; print: runNum; lf;
        nextPutAll: ''classes ''; print: nc; lf;
        nextPutAll: ''passed ''; print: tp; lf;
        nextPutAll: ''failed ''; print: tf; lf;
        nextPutAll: ''errors ''; print: te; lf;
        nextPutAll: ''skipped ''; print: ts; lf;
        nextPutAll: ''total ''; print: tp + tf + te + ts; lf])' classified: 'running'!

"--- main entry ---"
SUnitRunner class compile: 'runAllTests
  | names done current runNum |
  runNum := self nextRunNumber.
  names := self allTestClassNames.
  done := Set withAll: (self readLines: ''/tmp/sunit_done.txt'').
  "previous crasher: named in current.txt but never recorded as done"
  current := self readLines: ''/tmp/sunit_current.txt''.
  (current notEmpty and: [(done includes: current first) not]) ifTrue: [
    self appendLine: current first to: ''/tmp/sunit_blacklist.txt''].
  (self readLines: ''/tmp/sunit_blacklist.txt'') do: [:n | done add: n].
  names do: [:n | | cls |
    (done includes: n) ifFalse: [
      cls := Smalltalk globals at: n asSymbol ifAbsent: [nil].
      cls isNil
        ifTrue: [self appendLine: n to: ''/tmp/sunit_done.txt''. done add: n]
        ifFalse: [
          self writeFile: ''/tmp/sunit_current.txt'' contents: n.
          self runOneClass: cls named: n.
          self appendLine: n to: ''/tmp/sunit_done.txt''. done add: n.
          ''/tmp/sunit_current.txt'' asFileReference ensureDelete]]].
  self writeTotals: runNum.
  self writeFile: ''/tmp/sunit_ALL_DONE.txt'' contents: ''done run '', runNum printString' classified: 'running'!

SUnitRunner class compile: 'startUp: isStarting
  self appendLine: (''startUp: isStarting='' , isStarting printString) to: ''/tmp/sunit_startup_trace.txt''.
  isStarting == true ifTrue: [
    [ self runAllTests ]
      on: Error
      do: [:e |
        self appendLine:
          (''STARTUP-ERR '' , e class name , '': '' ,
            ((e messageText ifNil: [''(nil msg)'']) , '' @ '' ,
             (e signalerContext ifNil: [''(no ctx)''] ifNotNil: [:c | c printString])))
          to: ''/tmp/sunit_startup_error.txt''] ]' classified: 'running'!

"=== register startup handler so tests auto-run on image resume (idempotent) ==="
| h |
[SessionManager default unregisterClassNamed: #SUnitRunner] on: Error do: [:e | ].
h := ClassSessionHandler forClassNamed: #SUnitRunner.
SessionManager default register: h inCategory: SessionManager default userCategory.
Transcript showln: 'SUnitRunner (full-coverage) registered'.!
