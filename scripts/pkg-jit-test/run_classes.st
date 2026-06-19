"Run a specific list of TestCase classes (one class name per line in
 /tmp/pkg_classnames.txt), in the listed order. For bisecting a cumulative-state
 JIT bug: the failing 'victim' goes LAST, the suspected poisoners before it.
 Emits RESULT + FAIL/ERR/TIMEOUT lines like run_pkg_tests.st."

| log names classes p f e to perTimeout |
log := Stdio stdout.
perTimeout := 50 seconds.
names := ('/tmp/pkg_classnames.txt' asFileReference contents lines
           collect: [:s | s trimBoth]) reject: [:s | s isEmpty].
classes := names collect: [:n | Smalltalk at: n asSymbol ifAbsent: [nil]].
log nextPutAll: 'CLASSES '; nextPutAll: (classes count: [:c | c notNil]) printString; lf; flush.

p := 0. f := 0. e := 0. to := 0.
classes do: [:cls | cls ifNotNil: [
  cls allTestSelectors do: [:sel |
    | res |
    res := [[ ([(cls selector: sel) runCase. #pass]
                valueWithin: perTimeout onTimeout: [#timeout])]
              on: TestFailure do: [:ex |
                f := f + 1.
                log nextPutAll: 'FAIL ', cls name, '>>', sel; lf. #fail]]
            on: Error do: [:ex |
              e := e + 1.
              log nextPutAll: 'ERR  ', cls name, '>>', sel,
                ' (', ex class name, ': ', ((ex messageText ifNil: ['']) copyFrom: 1 to: ((ex messageText ifNil: ['']) size min: 120)), ')'; lf. #error].
    res = #pass ifTrue: [p := p + 1].
    res = #timeout ifTrue: [to := to + 1. log nextPutAll: 'TIMEOUT ', cls name, '>>', sel; lf].
    log flush]]].
log nextPutAll: 'RESULT pass=', p printString, ' fail=', f printString,
  ' err=', e printString, ' timeout=', to printString; lf; flush.
