"A block action holds its receiver strongly and there are no ephemerons, so a
 block subscription cannot be weak.  makeWeak keeps the subscription strong
 instead of raising an error, as testNoWeakBlock states."
fix value: #AnnouncementSubscription value: #makeWeak
	value: [ :s | s includesSubstring: 'missing ephemerons support' ]
	value: 'makeWeak

	"A block holds its receiver strongly and there are no ephemerons, so there is no weak block subscription: stay strong."
	action isBlock ifTrue: [ ^ self ].

	^ announcer replace: self with:
		(WeakAnnouncementSubscription new
			announcer: announcer;
			action: action asWeakMessageSend;
			subscriber: subscriber;
			announcementClass: announcementClass)'.
fix value: #WeakAnnouncerTest value: #testNoWeakBlock
	value: [ :s | s includesSubstring: '<expectedFailure>' ]
	value: 'testNoWeakBlock
	"There is no such thing as a weak block, these things never go away."

	| counter |

	self longTestCase.

	counter := 0.

	(announcer when: AnnouncementMockA do: [ :ann | counter := counter + 1 ] for: self) makeWeak.

	Smalltalk garbageCollect.
	announcer announce: AnnouncementMockA.
	self assert: counter equals: 1.

	Smalltalk garbageCollect.
	announcer announce: AnnouncementMockA.
	self assert: counter equals: 2'.
