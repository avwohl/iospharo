"Generic loaded-package SUnit runner — runs identically on our custom VM and on
 stock Cog, so the two outputs can be diffed for JIT-correctness parity.

 Selects every non-abstract TestCase subclass whose package name starts with any
 prefix listed in /tmp/pkg_prefixes.txt (one prefix per line, e.g. 'Neo-JSON',
 'Math-Tests', 'STON').  Runs each test selector with a per-test wall-clock
 timeout, and prints one machine-readable RESULT line plus FAIL/ERR detail lines.

 Fairness: the prefixes live in an EXTERNAL file, never as literals in this
 script, so neither VM's literal pool biases reflective senders — same contract
 as the kernel-suite runners.

 Output contract (the driver scripts depend on it): tests write to stdout without
 a trailing newline, so this runner prints a newline BEFORE every CLASSES, RUN,
 FAIL, ERR, TIMEOUT and RESULT line.  A RUN line (the word RUN, then the class
 name, two greater-than signs and the selector) is printed before each test, so
 when a VM gives no RESULT, the last RUN line names the test that was running
 when the run ended.  Keep the comments free of text that looks like such a
 line: a failed compile echoes this source into the log."

| log prefixes classes p f e to perTimeout envp |
log := Stdio stdout.
perTimeout := 50 seconds.
"Prefixes come from the PKG_PREFIXES env var (comma-separated) when set, else the
 /tmp/pkg_prefixes.txt file.  The env path lets parallel workers each pass their
 own prefixes without racing on the shared file.  Either way the prefixes live
 OUTSIDE this script (the fairness contract), so neither VM's literal pool is
 biased."
envp := [ Smalltalk os environment at: 'PKG_PREFIXES' ifAbsent: [ '' ] ]
          on: Error do: [:e | '' ].
prefixes := (envp notNil and: [ envp notEmpty ])
  ifTrue: [ ((envp substrings: ',') collect: [:s | s trimBoth]) reject: [:s | s isEmpty] ]
  ifFalse: [ ('/tmp/pkg_prefixes.txt' asFileReference exists)
      ifTrue: [ ('/tmp/pkg_prefixes.txt' asFileReference contents lines
                  collect: [:s | s trimBoth]) reject: [:s | s isEmpty] ]
      ifFalse: [ #() ] ].
log nextPutAll: 'PREFIXES '; nextPutAll: prefixes printString; lf; flush.

"Optional prelude (the headless fake GUI) named by the PKG_PRELUDE env var.  It is
 filed in HERE, after the temp declaration above: the driver must not paste it in
 front of this script, because Pharo 14 rejects a temp declaration that follows a
 statement (Variable or expression expected).  A prelude that is named but
 missing is an error that stops the run at once with a PRELUDE-MISSING line and a
 failure exit (our VM eval mode still exits 0, so the summary then shows NO-RESULT): running the tests without the GUI would give wrong results."
envp := [ Smalltalk os environment at: 'PKG_PRELUDE' ifAbsent: [ '' ] ]
          on: Error do: [:ex | '' ].
(envp notNil and: [ envp notEmpty ]) ifTrue: [
  envp asFileReference exists
    ifTrue: [ envp asFileReference fileIn ]
    ifFalse: [
      log lf; nextPutAll: 'PRELUDE-MISSING '; nextPutAll: envp; lf; flush.
      Smalltalk exitFailure ] ].

classes := (TestCase allSubclasses select: [:c |
  c isAbstract not and: [
    prefixes anySatisfy: [:pre | c package name beginsWith: pre]]])
  asSortedCollection: [:a :b | a name < b name].
log lf; nextPutAll: 'CLASSES '; nextPutAll: classes size printString; lf; flush.

p := 0. f := 0. e := 0. to := 0.
classes do: [:cls |
  cls allTestSelectors do: [:sel |
    | res |
    log lf; nextPutAll: 'RUN ', cls name, '>>', sel; lf; flush.
    res := [[ ([(cls selector: sel) runCase. #pass]
                  valueWithin: perTimeout onTimeout: [#timeout])]
              on: TestFailure do: [:ex |
                f := f + 1.
                log lf; nextPutAll: 'FAIL ', cls name, '>>', sel,
                  ' [', ((ex messageText ifNil: ['']) copyFrom: 1 to: ((ex messageText ifNil: ['']) size min: 200)), ']'; lf.
                #fail]]
            "Halt is an Exception, not an Error, so without it here a test that sends
             #halt (or #haltIf:) leaves the headless eval with an unhandled Halt and
             the whole run ends with no RESULT.  Count it as an ERR like any other
             test that stops with an exception."
            on: Error, Halt do: [:ex |
              e := e + 1.
              log lf; nextPutAll: 'ERR  ', cls name, '>>', sel,
                ' (', ex class name, ': ', ((ex messageText ifNil: ['']) copyFrom: 1 to: ((ex messageText ifNil: ['']) size min: 160)), ')'; lf.
              #error].
    res = #pass ifTrue: [p := p + 1].
    res = #timeout ifTrue: [to := to + 1.
      log lf; nextPutAll: 'TIMEOUT ', cls name, '>>', sel; lf].
    log flush]].

log lf; nextPutAll: 'RESULT pass=', p printString, ' fail=', f printString,
  ' err=', e printString, ' timeout=', to printString,
  ' classes=', classes size printString; lf; flush.
