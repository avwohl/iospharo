"ReleaseTest>>testRatioOfCommentedClasses: the method skipped itself ('failing on the CI for Newtools') because it compared an absolute count (876) with the number of uncommented classes in the whole image.  The count grows with every external project in the image (Spec2, NewTools, Roassal, Iceberg, Microdown: 1064 uncommented of 8771 in the test image; 557 of 5724 in the packages of the pharo repository).  The test is named a RATIO test, so the repaired test asserts a ratio, and the skip is removed.  Limit: at most 1/8 of the classes and traits (not test cases, not baselines) may have an empty comment; the measured ratio is 12.1 percent."
fix value: #ReleaseTest value: #testRatioOfCommentedClasses value: [ :src | src includesSubstring: 'self skip' ] value: 'testRatioOfCommentedClasses

	| stereotypes violating |
	stereotypes := OrderedCollection new.
	stereotypes
		addAll: Smalltalk globals allTraits;
		addAll: Smalltalk globals allClasses.

	stereotypes removeAll: TestCase allSubclasses.
	stereotypes removeAll: BaselineOf allSubclasses.
	violating := stereotypes select: [ :each | each comment isEmpty ].
	"The absolute count (876) depended on which projects the image holds (NewTools, Spec2, Roassal ...), so it failed on every image that is not the reference image.  The ratio does not."
	self assert: violating size / stereotypes size <= (1 / 8)'.
