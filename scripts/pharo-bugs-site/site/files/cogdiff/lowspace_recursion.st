"lowspace_recursion.st -- a recursion with no end, in an image with the low
 space watcher.  The answer is the class and the text of the error that ends
 the recursion.

   scripts/vmrun.sh eval --old-space 1024 cog  scripts/cogdiff/lowspace_recursion.st
   scripts/vmrun.sh eval --old-space 1024 cogp scripts/cogdiff/lowspace_recursion.st
   scripts/vmrun.sh eval --old-space 1024 ours scripts/cogdiff/lowspace_recursion.st

 Measured 2026-11-17 with the three commands above:
   stock Cog   no answer: the VM stops with 'Could not allocate new object in
               the old space' when the old space reaches its limit, and then
               with a segmentation fault in the code that prints the report.
   patched Cog 'OutOfMemory: ' (cogp with the change of
               docs/upstream/cog-low-space-before-old-space-limit.patch;
               before that change cogp did the same as stock Cog)
   our VM      before the change 'Error: stack overflow: recursion deeper than
               56000 frames' (a limit that Cog does not have,
               the iospharo defect list); after the change 'OutOfMemory: '
 The answer that the image is made for: OutOfMemory, from the low space
 watcher."
| recurse |
Smalltalk installLowSpaceWatcher.
recurse := nil.
recurse := [ :n | (recurse value: n + 1) + 1 ].
[ recurse value: 1. 'the recursion returned' ]
	on: Error
	do: [ :e | e class name , ': ' , ((e messageText ifNil: [ '' ]) asString first: (((e messageText ifNil: [ '' ]) asString size) min: 80)) ]
