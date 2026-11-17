"rko281-restoreforpharo: three groups of entries.

1. SQLite3Library text binding (dependency rko281/Pharo-SQLite3).  The binding
   of a text value passes the UTF-8 ByteArray of the text to sqlite3_bind_text
   with the destructor argument 0 (SQLITE_STATIC).  SQLITE_STATIC means that
   SQLite keeps the pointer and reads the bytes later, at sqlite3_step.  The
   ByteArray is a temporary of SQLite3Library>>with:at:putText: and nothing keeps
   the ByteArray alive (the cache 'bindings' of the statement keeps the String,
   not the encoded bytes).  After the bind call the ByteArray is garbage; its
   memory can be moved or freed and reused by a collection before the step.
   SSWReStoreTypeTest>>testVeryLargeString allocates 10 MB arrays between the
   bind of the small varchar and the step, so the stock Cog collector reuses
   the bytes of the small text, the row stores a wrong varchar, and the later
   detect: finds no row (NotFound).  The repair passes SQLITE_TRANSIENT (-1),
   with which SQLite copies the bytes in the bind call.  The destructor
   argument becomes sqlite3_int64, because -1 must reach the 64-bit pointer
   register intact (a C int leaves the upper 32 bits of the register
   undefined).

2. SSWReStoreBlockIfNilQueryTest>>isExpectedFailure.  The mark says that
   testSelectSimpleObjectWithFunctionIfNil fails.  The test passes on both VMs
   (unexpected pass).  The mark is stale: the repair removes the mark.

3. SSWReStoreGeneralTest>>test01Events.  The test needs a list that triggers
   the event #listChanged when the list changes (Dolphin ListModel).  Pharo has
   no ListModel, and SSWReStoreTest class>>listModelClass answers
   OrderedCollection, which triggers no event.  So the last assertion
   (counter = #(1 2)) can never hold, and the test was marked as an expected
   failure.  The repair triggers #listChanged by hand after each removal, which
   is what ListModel does, and keeps all four assertions.  The mark is removed
   in the same file."

fix value: #SQLite3Library value: #with:at:putText:
	value: [ :source | source includesSubstring: 'with: 0' ]
	value:
'with: aStatement at: aColumn putText: aString

	| bytes |

	bytes := self pharoStringToUTF8: aString.

	"SQLITE_TRANSIENT (-1): SQLite copies the bytes in this call.  With 0
	(SQLITE_STATIC) SQLite reads the bytes at sqlite3_step, when the garbage
	collector can have moved or reused the memory of the temporary ByteArray."
	^ self
		  apiBindText: aStatement
		  atColumn: aColumn
		  with: bytes
		  with: bytes size
		  with: -1'.

fix value: #SQLite3Library value: #apiBindText:atColumn:with:with:with:
	value: [ :source | source includesSubstring: 'int anotherInteger' ]
	value:
'apiBindText: aStatement atColumn: aColumn with: aString with: anInteger with: anotherInteger
	"int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void(*)(void*))
	The last argument is the destructor.  It is a pointer, so it is a 64-bit integer here (SQLITE_TRANSIENT is -1)."

	^ self ffiCall: #(int sqlite3_bind_text (sqlite3_stmt* aStatement, int aColumn, String aString, int anInteger, sqlite3_int64 anotherInteger))'.

fix value: #SSWReStoreBlockIfNilQueryTest value: #isExpectedFailure
	value: [ :source | source includesSubstring: 'self selector =' ]
	value:
'isExpectedFailure

	"No test of this class is an expected failure.  One test was marked, and it passes."

	^ super isExpectedFailure'.

fix value: #SSWReStoreGeneralTest value: #expectedFailures
	value: [ :source | source includesSubstring: '#(' ]
	value:
'expectedFailures

	"No test of this class is an expected failure now."

	^ super expectedFailures'.

fix value: #SSWReStoreGeneralTest value: #test01Events
	value: [ :source | (source includesSubstring: 'triggerEvent') not ]
	value:
'test01Events

	"Check persistency of events"

	| t counter |

	reStore refreshAllObjects.
	t := (OwnerTest storedInstancesIn: reStore) last.
	counter := OrderedCollection new.

	reStore beginTransaction.
	t single1 when: #something send: #yourself to: t.
	t single1 name: ''blah''.
	t list when: #listChanged send: #add: to: counter with: 1.
	t ownedList when: #listChanged send: #add: to: counter with: 2.
	"A Dolphin ListModel triggers #listChanged when it changes; an OrderedCollection does not, so the test triggers the event after each change"
	t list removeLast.
	t list triggerEvent: #listChanged.
	t ownedList removeLast.
	t ownedList triggerEvent: #listChanged.
	reStore rollbackTransaction.

	self assert: (t single1 getEvents includesKey: #something).
	self assert: (t list getEvents includesKey: #listChanged).
	self assert: (t ownedList getEvents includesKey: #listChanged).
	self assert: (counter asSortedCollection asArray = #(1 2))'.
