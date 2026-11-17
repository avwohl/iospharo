"System-Time: a wait for a Duration can end before the Duration has passed
(2026-11-17).  Found by the package run: PharoMisc, Tasks,
TkThreadTest>>testPeriodFixedDuration, which failed on our VM only.

Duration>>asDelay makes a Delay for 'self asMilliSeconds' milliseconds, and
asMilliSeconds rounds down.  A wait for 99.98 milliseconds is a Delay of 99
milliseconds.  Duration>>wait uses asDelay.

Measured with scripts/cogdiff/delay_accuracy.st, 40 waits of
'(Duration nanoSeconds: 99980000) wait', shortest wait minus the Duration:
  our VM      -921 microseconds (the wait ends before its time)
  stock Cog   +4629 microseconds
The stock VM ends each Delay 1 to 6 milliseconds late on this machine, and
that hides the defect.  Our VM ends a Delay 0.1 to 0.4 milliseconds late.

The Tasks package waits 'dueTime - now' for each period.  With waits that
are 1 millisecond short, a task that repeats each 100 milliseconds for 300
milliseconds gets a fourth run before the 300 milliseconds end.

The repair rounds up: a part of a millisecond counts as one more
millisecond.  A negative Duration gives the same Delay as before (an error
in Delay>>setDelay:forSemaphore:).  The new test fails on the two VMs before
the repair."

fix value: #Duration value: #asDelay
	value: [ :source | source includesSubstring: 'Delay forMilliseconds: self asMilliSeconds' ]
	value:
'asDelay
	"A wait must not end before the receiver has passed.  A Delay counts milliseconds, so a part of a millisecond counts as one more millisecond.  (asMilliSeconds rounds down: a wait for 99.98 milliseconds ended after 99 milliseconds.)"

	| nanoseconds |
	nanoseconds := self asNanoSeconds.
	^ Delay forMilliseconds: (nanoseconds negative
			   ifTrue: [ nanoseconds // 1000000 ]
			   ifFalse: [ nanoseconds + 999999 // 1000000 ])'.

add value: #DurationTest value: #testAsDelayDoesNotRoundDown value: 'tests'
	value:
'testAsDelayDoesNotRoundDown
	"The Delay of a Duration must not be shorter than the Duration."

	self assert: (Duration nanoSeconds: 99980000) asDelay millisecondDelayDuration equals: 100.
	self assert: (Duration nanoSeconds: 100000000) asDelay millisecondDelayDuration equals: 100.
	self assert: (Duration nanoSeconds: 100000001) asDelay millisecondDelayDuration equals: 101.
	self assert: (Duration nanoSeconds: 1) asDelay millisecondDelayDuration equals: 1.
	self assert: Duration zero asDelay millisecondDelayDuration equals: 0'.
