"Pharotales_Books: FairyTaleTest>>testInitialize compared the publication year
of a new FairyTale with the fixed number 2025.  FairyTale>>initialize sets
publicationYear to Date today year, so the test passed only in 2025 and fails
in every other year.  The repair compares with Date today year, the value that
the code under test uses.  The test still checks that a new FairyTale has the
current year and empty author, title and publisher."

fix value: #FairyTaleTest value: #testInitialize
	value: [ :source | source includesSubstring: '2025' ]
	value:
'testInitialize
	| tale |
	tale := FairyTale new.

	self assert: tale author equals: ''''.
	self assert: tale title equals: ''''.
	self assert: tale publisher equals: ''''.
	self assert: tale publicationYear equals: Date today year'.
