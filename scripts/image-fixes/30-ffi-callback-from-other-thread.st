"TFUFFICallbackTest>>testCallbacksInOtherThread skips for the same-thread FFI
runner (2026-11-17).  The remark in the test says why: 'Using same thread
runner with callbacks from other threads produce a segmentation fault'.  Stock
Cog v12 enters the interpreter from the thread that made the callback, while
the VM thread runs.

Our VM runs such a callback in the VM thread.  docs/upstream has the change
for Cog (cog-same-thread-runner-callback-from-other-thread.patch).  The test
cannot run on a stock Cog without that change: the VM stops.  So the skip
stays for a VM that does not have the repair, and goes for a VM that has it.
The test asks the VM: our VM is known by its version text, and a Cog build
with the change is known by the environment variable
PHARO_FFI_CALLBACK_FROM_OTHER_THREAD, which scripts/vmrun.sh sets for the VM
name cogp."

add value: #TFUFFICallbackTest value: #sameThreadRunnerTakesCallbackFromOtherThread value: 'private'
	value:
'sameThreadRunnerTakesCallbackFromOtherThread
	"Stock Cog v12 enters the interpreter from the thread of the callback and stops with a segmentation fault.  A VM that runs such a callback in the VM thread says so: the iospharo VM by its version text, a Cog build with the repair by an environment variable."

	^ (Smalltalk vm interpreterSourceVersion includesSubstring: ''iospharo'')
		or: [ (OSEnvironment current at: ''PHARO_FFI_CALLBACK_FROM_OTHER_THREAD'' ifAbsent: [ nil ]) notNil ]'.

fix value: #TFUFFICallbackTest value: #testCallbacksInOtherThread
	value: [ :source | source includesSubstring: 'ffiLibrary = TFTestLibraryUsingSameThreadRunner ifTrue: [ ^ self skip ].' ]
	value: ((TFUFFICallbackTest >> #testCallbacksInOtherThread) sourceCode
		copyReplaceAll: 'ffiLibrary = TFTestLibraryUsingSameThreadRunner ifTrue: [ ^ self skip ].'
		with: '(ffiLibrary = TFTestLibraryUsingSameThreadRunner and: [ self sameThreadRunnerTakesCallbackFromOtherThread not ]) ifTrue: [ ^ self skip ].').
