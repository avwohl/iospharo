"Regression battery for the BitBlt, become and FFI-callback fixes.

 Files in the VM behaviour tests listed in scripts/README.md, runs them together
 with the stock image classes that cover the same primitives, and writes a
 one-line-per-class summary. Every class here failed, errored or ran quadratically
 on some VM in this repo's history, so the battery is the reproduction as much as
 the regression check.

 The test directory defaults to `scripts` under the working directory. Override it
 when the image is not started from the repo root:

     Smalltalk at: #VMFixTestDir put: '/path/to/repo/scripts'.
     'scripts/run_vm_fix_battery.st' asFileReference fileIn.

 Output goes to the file named by #VMFixTestOut, default /tmp/vm_fix_battery.txt.

 A class that is absent from the image is reported as such rather than skipped
 silently — an absent class and a passing one must not look alike in a summary."

| dir out s names |
dir := (Smalltalk at: #VMFixTestDir ifAbsent: [ 'scripts' ]) asFileReference.
out := Smalltalk at: #VMFixTestOut ifAbsent: [ '/tmp/vm_fix_battery.txt' ].
s := WriteStream on: String new.

"File in the behaviour tests. A file that will not compile is reported rather
 than swallowed: silently skipping it would show up later as an absent class,
 which reads like the image lacking a stock class rather than our own bug."
#( 'test_warpblt.st' 'test_pointsto.st' 'test_bitblt_lowdepth.st'
   'test_bitblt_depth.st' 'test_bitblt_translucent.st'
   'test_bitblt_depth_matrix.st' 'test_nonlocal_return.st'
   'test_directory_enumeration.st' 'test_deep_stack_unwind.st' )
  do: [ :n |
    | ref |
    ref := dir / n.
    ref exists
      ifTrue: [ [ ref fileIn ]
                  on: Error
                  do: [ :e | s nextPutAll: 'fileIn FAILED ', n, ': ',
                                           e class name, ' ', e messageText printString; lf ] ]
      ifFalse: [ s nextPutAll: 'fileIn MISSING ', ref fullName; lf ] ].

names := #( "our behaviour tests"
            #WarpBltTest #ObjectPointsToTest #BitBltLowDepthTest
            #BitBltDepthMatrixTest #NonLocalReturnActivationTest
            #DirectoryEnumerationTest #DeepStackUnwindTest
            "stock classes over the same primitives"
            #BitBltTest #FormTest #ColorTest #ColorFormTest
            #GIFReadWriterTest #BMPReadWriterTest #PNGReadWriterTest
            #BecomeTest #ObjectTest #MethodDictionaryTest
            #WeakKeyDictionaryTest #WeakSetTest
            #BlockClosureValueWithinDurationTest
            #FFICallbackTest #FFICalloutTest ).

names do: [ :c |
  | cls |
  cls := Smalltalk at: c ifAbsent: [ nil ].
  (cls notNil and: [ cls isClass and: [ cls inheritsFrom: TestCase ] ])
    ifTrue: [
      | r |
      r := cls suite run.
      s nextPutAll: c, ': ', r printString; lf.
      r failures do: [ :f | s nextPutAll: '   FAIL ', f selector; lf ].
      r errors do: [ :f | s nextPutAll: '   ERR  ', f selector; lf ] ]
    ifFalse: [ s nextPutAll: c, ': (absent)'; lf ] ].

"become was linear in pairs but each pair carried a whole heap scan, so this is
 a timing check rather than an assertion: 800 pairs took 5.5s before
 becomeForwardAll and about a tenth of a second after."
[ | a b ms |
  a := (1 to: 800) collect: [ :i | Array new: 2 ].
  b := (1 to: 800) collect: [ :i | Array new: 2 ].
  ms := [ a elementsForwardIdentityTo: b ] timeToRun asMilliSeconds.
  s nextPutAll: 'become 800 pairs: ', ms printString, ' ms'; lf ]
  on: Error
  do: [ :e | s nextPutAll: 'become benchmark FAILED: ', e class name; lf ].

out asFileReference writeStreamDo: [ :f | f nextPutAll: s contents ].
s contents
