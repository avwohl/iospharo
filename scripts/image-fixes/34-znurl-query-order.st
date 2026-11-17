"Zinc: two URLs with the same query fields in another order are not equal in
Pharo 14 (2026-11-17).

Symptom: tests of three packages of the 200-package run compare a URL with a
text whose query fields are in another order than the code makes them
(ba-st/Hyperspace ZnUrlHyperspaceExtensionsTest>>testStartLimit, ba-st/Stargate
PetsRESTfulControllerTest>>testComplexPagination, four tests of
ba-st/Superluminal): 'Got https://pets.example.com/pets?limit=2&start=3 instead
of https://pets.example.com/pets?start=3&limit=2'.  The tests pass on Pharo 13.

Cause: ZnUrl>>equals: compares the queries with 'self query = url query'.  The
query is a ZnMultiValueDictionary, a subclass of OrderedDictionary, in Pharo 13
and in Pharo 14.  OrderedDictionary>>= of Pharo 14 also compares the position
of each key (OrderedDictionaryTest>>testEqualsWhenKeyArentInTheSameOrder).  So
the equality of two URLs changed with it, and nothing says that this was
meant: the hash of a ZnUrl does not look at the query, and Zinc has no test
for it.

Measured with the same expression
('https://example.com/pets?start=3&limit=2' asZnUrl =
 'https://example.com/pets?limit=2&start=3' asZnUrl):
  Pharo 13 build 750, stock Cog   true
  Pharo 14 build 791, stock Cog   false
  Pharo 14 build 791, our VM      false

The repair: ZnUrl>>equals: compares the query fields with no regard to their
order, as in Pharo 13.  The values of one field (a field that is there more
than one time) keep their order.  The new test fails on the two VMs before the
repair."

fix value: #ZnUrl value: #equals:
	value: [ :source | source includesSubstring: 'self query = url query' ]
	value:
'equals: url
	self scheme = url scheme ifFalse: [ ^ false ].
	self host = url host ifFalse: [ ^ false ].
	self portOrDefault = url portOrDefault ifFalse: [ ^ false ].
	((self isSlash and: [ url isSlash ]) or: [ self segments = url segments ]) ifFalse: [ ^ false ].
	(self hasQuery or: [ url hasQuery ])
		ifTrue: [
			"The same fields with the same values.  The order of the fields is not a part of the comparison: the query is an OrderedDictionary, and OrderedDictionary>>= compares the position of each key."
			(self hasQuery and: [ url hasQuery ]) ifFalse: [ ^ false ].
			self query size = url query size ifFalse: [ ^ false ].
			self query keysDo: [ :key |
				(url query at: key ifAbsent: [ ^ false ]) = (self query at: key) ifFalse: [ ^ false ] ] ].
	self fragment = url fragment ifFalse: [ ^ false ].
	^ true'.

add value: #ZnUrlTest value: #testEqualsWithQueryFieldsInAnotherOrder value: 'testing'
	value:
'testEqualsWithQueryFieldsInAnotherOrder
	"Two URLs with the same query fields are equal, in each order of the fields."

	| url |
	url := ''https://example.com/pets?start=3&limit=2'' asZnUrl.
	self assert: url equals: ''https://example.com/pets?limit=2&start=3'' asZnUrl.
	self assert: url hash equals: ''https://example.com/pets?limit=2&start=3'' asZnUrl hash.
	self assert: url equals: ''https://example.com/pets?start=3&limit=2'' asZnUrl.
	self deny: url equals: ''https://example.com/pets?start=3&limit=5'' asZnUrl.
	self deny: url equals: ''https://example.com/pets?start=3'' asZnUrl.
	self deny: url equals: ''https://example.com/pets?start=3&limit=2&sort=name'' asZnUrl.
	self deny: url equals: ''https://example.com/pets'' asZnUrl.
	self deny: ''https://example.com/pets'' asZnUrl equals: url.
	"A field that is there two times: its values keep their order."
	self assert: ''https://example.com/pets?tag=a&tag=b&limit=2'' asZnUrl equals: ''https://example.com/pets?limit=2&tag=a&tag=b'' asZnUrl.
	self deny: ''https://example.com/pets?tag=a&tag=b'' asZnUrl equals: ''https://example.com/pets?tag=b&tag=a'' asZnUrl'.
