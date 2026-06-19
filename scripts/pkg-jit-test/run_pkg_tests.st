"Generic loaded-package SUnit runner — runs identically on our custom VM and on
 stock Cog, so the two outputs can be diffed for JIT-correctness parity.

 Selects every non-abstract TestCase subclass whose package name starts with any
 prefix listed in /tmp/pkg_prefixes.txt (one prefix per line, e.g. 'Neo-JSON',
 'Math-Tests', 'STON').  Runs each test selector with a per-test wall-clock
 timeout, and prints one machine-readable RESULT line plus FAIL/ERR detail lines.

 Fairness: the prefixes live in an EXTERNAL file, never as literals in this
 script, so neither VM's literal pool biases reflective senders — same contract
 as the kernel-suite runners."

| log prefixes classes p f e to perTimeout |
log := Stdio stdout.
perTimeout := 50 seconds.
prefixes := ('/tmp/pkg_prefixes.txt' asFileReference exists)
  ifTrue: [ ('/tmp/pkg_prefixes.txt' asFileReference contents lines
              collect: [:s | s trimBoth]) reject: [:s | s isEmpty] ]
  ifFalse: [ #() ].
log nextPutAll: 'PREFIXES '; nextPutAll: prefixes printString; lf; flush.

classes := (TestCase allSubclasses select: [:c |
  c isAbstract not and: [
    prefixes anySatisfy: [:pre | c package name beginsWith: pre]]])
  asSortedCollection: [:a :b | a name < b name].
log nextPutAll: 'CLASSES '; nextPutAll: classes size printString; lf; flush.

p := 0. f := 0. e := 0. to := 0.
classes do: [:cls |
  cls allTestSelectors do: [:sel |
    | res |
    res := [[ ([(cls selector: sel) runCase. #pass]
                  valueWithin: perTimeout onTimeout: [#timeout])]
              on: TestFailure do: [:ex |
                f := f + 1.
                log nextPutAll: 'FAIL ', cls name, '>>', sel,
                  ' [', ((ex messageText ifNil: ['']) copyFrom: 1 to: ((ex messageText ifNil: ['']) size min: 200)), ']'; lf.
                #fail]]
            on: Error do: [:ex |
              e := e + 1.
              log nextPutAll: 'ERR  ', cls name, '>>', sel,
                ' (', ex class name, ': ', ((ex messageText ifNil: ['']) copyFrom: 1 to: ((ex messageText ifNil: ['']) size min: 160)), ')'; lf.
              #error].
    res = #pass ifTrue: [p := p + 1].
    res = #timeout ifTrue: [to := to + 1.
      log nextPutAll: 'TIMEOUT ', cls name, '>>', sel; lf].
    log flush]].

log nextPutAll: 'RESULT pass=', p printString, ' fail=', f printString,
  ' err=', e printString, ' timeout=', to printString,
  ' classes=', classes size printString; lf; flush.
