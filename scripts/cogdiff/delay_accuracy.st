"delay_accuracy.st -- how long a wait takes, against the time that was asked.

   scripts/vmrun.sh eval cog  scripts/cogdiff/delay_accuracy.st
   scripts/vmrun.sh eval ours scripts/cogdiff/delay_accuracy.st

 A wait must not end before its time: each 'shortest' value must be 0 or
 more.  The last line is a wait for a Duration of 99.98 milliseconds, which
 Duration>>asDelay of Pharo 14 build 791 makes a Delay of 99 milliseconds
 (asMilliSeconds rounds down)."
| out measure |
out := WriteStream on: String new.
measure := [ :label :requestedMicroseconds :waitBlock | | shortest longest |
	shortest := SmallInteger maxVal.
	longest := SmallInteger minVal.
	1 to: 40 do: [ :i | | start difference |
		start := Time microsecondClockValue.
		waitBlock value.
		difference := Time microsecondClockValue - start - requestedMicroseconds.
		shortest := shortest min: difference.
		longest := longest max: difference ].
	out nextPutAll: label; nextPutAll: ': shortest wait minus request = '; print: shortest;
		nextPutAll: ' us, longest = '; print: longest; nextPutAll: ' us'; lf ].
#( 1 10 99 ) do: [ :milliseconds |
	measure
		value: 'Delay forMilliseconds: ' , milliseconds printString
		value: milliseconds * 1000
		value: [ (Delay forMilliseconds: milliseconds) wait ] ].
measure
	value: '(Duration nanoSeconds: 99980000) wait'
	value: 99980
	value: [ (Duration nanoSeconds: 99980000) wait ].
out contents
