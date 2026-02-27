"Patch Pharo test timeouts for non-JIT interpreter VM.

 Without JIT, tests run 5-10x slower than on Cog. The standard
 TestCase>>defaultTimeLimit (10s) and per-class overrides cause
 spurious timeouts. This script scales all test timeouts by a
 configurable factor.

 Usage (file in with stock Pharo, then run with custom VM):
   pharo Pharo.image eval --save [fileIn expression]
   ./build/test_load_image /path/to/Pharo.image

 Configure scale factor (default 10):
   echo 15 > /tmp/sunit_timeout_scale.txt

 Also integrated into run_sunit_tests.st (applied automatically)."!

| scale baseTimeout patched |
scale := 10.
[| f |
 f := '/tmp/sunit_timeout_scale.txt' asFileReference.
 f exists ifTrue: [
   | v |
   v := f contents trimBoth asInteger.
   v > 0 ifTrue: [scale := v]]]
on: Error do: [:e |].
baseTimeout := 10 * scale.

"Override TestCase >> defaultTimeLimit with scaled value.
 This is the timeout enforced by TestExecutionEnvironment watchdog."
TestCase compile: ('defaultTimeLimit
  ^ {1} seconds' format: {baseTimeout printString})
  classified: 'running'.

"Remove per-class overrides so all tests inherit the generous base.
 Some test classes define their own defaultTimeLimit (e.g. 1 minute,
 30 seconds). On a slow VM these are still too short, so we remove
 them and let the scaled base apply everywhere."
patched := OrderedCollection new.
TestCase allSubclasses do: [:cls |
  (cls includesSelector: #defaultTimeLimit) ifTrue: [
    patched add: cls name.
    cls removeSelector: #defaultTimeLimit]].

Transcript cr;
  show: 'Timeout patcher: scale='; show: scale printString;
  show: 'x, base='; show: baseTimeout printString;
  show: 's, '; show: patched size printString;
  show: ' class overrides removed'; cr.
patched do: [:name |
  Transcript show: '  '; show: name; cr]!
