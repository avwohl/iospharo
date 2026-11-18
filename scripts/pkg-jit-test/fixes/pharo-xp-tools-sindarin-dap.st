"Sindarin-DAP (Pharo-XP-Tools/Sindarin-DAP): DAPHistoryTest>>
testMessagesShouldReturnInReverseChronologicalOrder fails on stock Cog with
'Got 7 instead of 8'.

Cause, in the package: DAPHistory>>messages sorts requests, events and
responses by the time stamp that addRequest:, addEvent: and addResponse: write
(DateAndTime now) with the sort block [ tA >= tB ].  Two messages that are
added in the same clock tick have equal time stamps.  The sort block then
answers true for both orders, and the order of the two messages is left to the
sort algorithm.  The result depends on the resolution of the clock and on the
speed of the VM: a VM that adds messages in a different tick from each other
passes by accident, a VM that adds two messages in one tick can fail.  It is
not a defect of a primitive.

The repair: addRequest:, addEvent: and addResponse: also store the arrival
number of the message (how many messages the history holds before the add), and
messages sorts by time stamp and, for equal time stamps, by arrival number, so
the newest message is first on every clock."

fix value: #DAPHistory value: #addEvent:
	value: [ :source | (source includesSubstring: 'arrival') not ]
	value:
'addEvent: eventAsDict
	
	eventAsDict at: #arrival put: requests size + events size + responses size.
	eventAsDict at: #timestamp put: DateAndTime now.
	events add: eventAsDict.
	self notifyObserversEvent: eventAsDict'.

fix value: #DAPHistory value: #addRequest:
	value: [ :source | (source includesSubstring: 'arrival') not ]
	value:
'addRequest: requestAsDict

	requestAsDict at: #arrival put: requests size + events size + responses size.
	requestAsDict at: #timestamp put: DateAndTime now.
	requests add: requestAsDict.
	self notifyObserversRequest: requestAsDict'.

fix value: #DAPHistory value: #addResponse:
	value: [ :source | (source includesSubstring: 'arrival') not ]
	value:
'addResponse: responseAsDict
	
	responseAsDict at: #arrival put: requests size + events size + responses size.
	responseAsDict at: #timestamp put: DateAndTime now.
	responses add: responseAsDict.
	self notifyObserversResponse: responseAsDict'.

fix value: #DAPHistory value: #messages
	value: [ :source | (source includesSubstring: 'arrival') not ]
	value:
'messages

	| all |
	all := requests , events , responses.

	"Newest first.  Equal time stamps (same clock tick) are ordered by arrival number."
	^ all sort: [ :a :b |
			  | tA tB |
			  tA := a at: #timestamp ifAbsent: [ DateAndTime now ].
			  tB := b at: #timestamp ifAbsent: [ DateAndTime now ].
			  tA = tB
				  ifTrue: [ (a at: #arrival ifAbsent: [ 0 ]) >= (b at: #arrival ifAbsent: [ 0 ]) ]
				  ifFalse: [ tA > tB ] ]'.
