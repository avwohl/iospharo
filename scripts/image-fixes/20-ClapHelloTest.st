"ClapHelloTest>>testHelloBothShoutingAndWhispering: the author left self skip: should probably fail or obey the latest flag.  Repair: obey the latest flag.  The Clap match keeps the flags in
 command line order (ClapCompositeMatch children is an OrderedCollection, ClapCompositeMatch>>flags
 selects from it in order), so ClapCommandLineExamples can see which of --shout and --whisper came
 last.  The test keeps its intent: it now asserts the whispering form for --shout --whisper.
 A second test covers the opposite order."
add value: #ClapCommandLineExamples value: #latestVoiceFlag value: 'accessing'
	value: 'latestVoiceFlag
	"The voice flag given last on the command line wins: --shout --whisper whispers, --whisper --shout shouts."
	^ (self context lastSubcommand flags collect: [ :each | each specification identifier ])
		reverse
		detect: [ :id | id = #shout or: [ id = #whisper ] ]
		ifNone: [ nil ]'.
fix value: #ClapCommandLineExamples value: #shouting
	value: [ :src | src includesSubstring: 'hasFlag: #shout' ]
	value: 'shouting
	^ self latestVoiceFlag == #shout'.
fix value: #ClapCommandLineExamples value: #whispering
	value: [ :src | src includesSubstring: 'hasFlag: #whisper' ]
	value: 'whispering
	^ self latestVoiceFlag == #whisper'.
fix value: #ClapHelloTest value: #testHelloBothShoutingAndWhispering
	value: [ :src | src includesSubstring: 'self skip:' ]
	value: 'testHelloBothShoutingAndWhispering
	context := ClapCommandLineExamples hello activateWith: #(''hello'' ''--shout'' ''--whisper'').
	self assertSuccess.
	self
		assert: self outputString
		equals: ''(hello, world)'' , self lineEnding'.
add value: #ClapHelloTest value: #testHelloBothWhisperingAndShouting value: 'tests'
	value: 'testHelloBothWhisperingAndShouting
	context := ClapCommandLineExamples hello activateWith: #(''hello'' ''--whisper'' ''--shout'').
	self assertSuccess.
	self
		assert: self outputString
		equals: ''HELLO, WORLD'' , 33 asCharacter asString , self lineEnding'.
