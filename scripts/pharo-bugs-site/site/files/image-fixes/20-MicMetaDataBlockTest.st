fix value: #MicMetaDataBlockTest value: #testCreatingAnInstanceInitializeTheMetadatDictionary
	value: [ :s | s includesSubstring: 'self skip' ]
	value: 'testCreatingAnInstanceInitializeTheMetadatDictionary

	"MicStartStopMarkupBlock>>bodyFromLine: collects the lines of the block in body, starting from nil (the initialization to a SmallDictionary is commented out in MicMetaDataBlock>>initialize).  closeMe turns the collected text into the metadata dictionary.  So the dictionary exists after closeMe, not after new."

	| metadata |
	metadata := MicMetaDataBlock new.
	metadata bodyFromLine: ''"authors" : "S. Ducasse"''.
	metadata closeMe.
	self assert: metadata body isNotNil.
	self assert: (metadata atKey: ''authors'') equals: ''S. Ducasse''.
	self deny: metadata bogusParsing'.
