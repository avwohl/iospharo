"Ducasse/OSC: OSCParserTest>>testParseTimestamp compared the 8 time-tag bytes
of a bundle with the bytes of a second DateAndTime now, taken later.  The two
clock reads differ by microseconds, so the fraction bytes differ on every
machine and every VM.  The test also compared an Array (Interval>>collect:)
with a ByteArray, and Array = ByteArray is false in Pharo.  The test can never
pass.  The repair reads the time tag back as seconds and fraction and checks
that the tag is between (before + 50 ms) and (after + 50 ms).

DateAndTime>>addLatency: set nanos without carry.  Within 50 ms of a second
boundary nanos exceeds 10^9 and asNTPTimeStamp makes a fraction that does not
fit 4 bytes (about 5 percent of runs of any bundle print).  The repair adds a
Duration, which carries."

fix value: #OSCParserTest value: #testParseTimestamp
	value: [ :source | source includesSubstring: 'asNTPTimeStampWithLatency' ]
	value:
'testParseTimestamp
	| bundle byteStream bytes before after seconds fraction tagNanos lowNanos highNanos epochShift latency |
	latency := 50000000.
	epochShift := 2208988800 * 1000000000.
	bundle := OSCBundle new.
	byteStream := ByteArray new writeStream.
	before := DateAndTime now.
	bundle print: { OSCMessage for: { ''/test''. 99 } } onOSCStream: byteStream.
	after := DateAndTime now.
	bytes := byteStream contents.
	"bytes 1 to 8 are #bundle plus NUL, bytes 9 to 16 are the NTP time tag"
	seconds := (bytes copyFrom: 9 to: 12) asInteger.
	fraction := (bytes copyFrom: 13 to: 16) asInteger.
	tagNanos := seconds * 1000000000 + (fraction * 1000000000 // (2 raisedTo: 32)).
	lowNanos := (before - DateAndTime unixEpoch) asNanoSeconds + epochShift + latency.
	highNanos := (after - DateAndTime unixEpoch) asNanoSeconds + epochShift + latency.
	"the fraction is truncated to 2^-32 s: allow 1 microsecond"
	self assert: tagNanos >= (lowNanos - 1000).
	self assert: tagNanos <= (highNanos + 1000)'.

fix value: #DateAndTime value: #addLatency:
	value: [ :source | source includesSubstring: 'basicNew' ]
	value:
'addLatency: aLatencyInNanoSeconds
	"add nanoseconds to the current time; the sum carries into the seconds"

	^ self + (Duration nanoSeconds: aLatencyInNanoSeconds)'.

fix value: #DateAndTime value: #asNTPTimeStampWithLatency
	value: [ :source | source includesSubstring: 'latency :=' ]
	value:
'asNTPTimeStampWithLatency
	" convert DateAndTime to NTPTimestamp for OSC bundles and adds 50 milliseconds of latency, to schedule messages ahead of time in SuperCollider"

	^ (self addLatency: 50000000) asNTPTimeStamp'.
