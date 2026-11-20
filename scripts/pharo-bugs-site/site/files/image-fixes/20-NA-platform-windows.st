"Win32EnvironmentTest: the three tests check the platform independent OSEnvironment contract (at:put:, at: with a value longer than 1000 characters, NotFound for an absent name).  Their only Windows part was the skip; the code under test (OSEnvironment current) works on macOS and Linux, so the tests run everywhere."
fix value: #Win32EnvironmentTest value: #testExistingEnvironmentVariableLongerThanBufferShouldReturnValue value: [ :s | s includesSubstring: 'isWindows' ] value: 'testExistingEnvironmentVariableLongerThanBufferShouldReturnValue

	| value aExpectedValue |

	aExpectedValue := String loremIpsum: 1500.

	OSEnvironment current at: ''ExistingNonEmpty'' put: aExpectedValue.

	value := OSEnvironment current at: ''ExistingNonEmpty''.

	self assert: value equals: aExpectedValue'.
fix value: #Win32EnvironmentTest value: #testExistingEnvironmentVariableShouldReturnValue value: [ :s | s includesSubstring: 'isWindows' ] value: 'testExistingEnvironmentVariableShouldReturnValue

	| value |

	OSEnvironment current at: ''ExistingNonEmpty'' put: ''IamAValue''.

	value := OSEnvironment current at: ''ExistingNonEmpty''.

	self assert: value equals: ''IamAValue'''.
fix value: #Win32EnvironmentTest value: #testNonExistingEnvironmentVariableShouldGenerateException value: [ :s | s includesSubstring: 'isWindows' ] value: 'testNonExistingEnvironmentVariableShouldGenerateException

	self should: [ OSEnvironment current at: ''NonExistingVariable'' ] raise: NotFound'.
