"Hyperspace (ba-st/Hyperspace): ZnUrlHyperspaceExtensionsTest>>testStartLimit
asserts that the same URL equals both '?limit=10&start=1' and
'?start=1&limit=10'.  The query of a ZnUrl is a ZnMultiValueDictionary, and
that is a subclass of OrderedDictionary.  OrderedDictionary>>= of Pharo 14
(Collections-Sequenceable/OrderedDictionary.class.st line 90) compares
indexOfKey: for each key, so the order of the keys matters.  ZnUrl>>equals:
(Zinc-Resource-Meta-Core/ZnUrl.class.st line 324) uses 'self query = url query'.
The two assertions cannot both be true; the first one fails with 'Got
...?limit=10&start=1 instead of ...?start=1&limit=10' (start:limit: sets start
first).  Same cause as the Buoy OrderedDictionaryExtensionsTest>>testEquals
repair.

The repair: the first assertion compares the query as an unordered set of
associations (the intent: both parameters are set with the given values); the
second assertion (exact order, start first) stays."

fix value: #ZnUrlHyperspaceExtensionsTest value: #testStartLimit
	value: [ :source | (source includesSubstring: 'associations asSet') not ]
	value:
'testStartLimit
	| url |
	url := ''https://api.example.com/pets'' asUrl start: 1 limit: 10.
	"Pharo 14: the order of the query parameters matters for ZnUrl equality"
	self
		assert: url query associations asSet
		equals: ''https://api.example.com/pets?limit=10&start=1'' asUrl query associations asSet;
		assert: url
		equals: ''https://api.example.com/pets?start=1&limit=10'' asUrl'.
