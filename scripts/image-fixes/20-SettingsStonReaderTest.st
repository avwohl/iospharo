fix value: #SettingsStonReader value: #load
	value: [ :source | (source includesSubstring: 'InstanceVariableNotFound') not ]
	value:
'load
	"It loads all instances of StoredSetting from a stream.
	 A stored setting that names an instance variable which no longer exists
	 (the setting class changed since the file was written) cannot be loaded:
	 nothing is stored and an empty collection is answered."
	stream ifNil: [ ^ #() ].
	stream atEnd ifTrue: [ ^ #() ].
	[ | reader |
		reader := STON reader on: stream; yourself.
		[ storedSettings := reader next ]
			on: InstanceVariableNotFound
			do: [ :ex | storedSettings := nil ]
	] ensure: [ stream close ].
	^ storedSettings ifNil: [ #() ]'.

fix value: #SettingsStonReaderTest value: #testNonExistingStoredSettingVariable
	value: [ :source | (source includesSubstring: 'expectedFailure') or: [ source includesSubstring: 'SettingsStonReader new storedSettings' ] ]
	value:
'testNonExistingStoredSettingVariable
	reader stream: MockSettings changedClassStoredSettingVariable readStream.
	self shouldnt: [ reader load ] raise: Error.
	self assert: reader storedSettings isNil'.
