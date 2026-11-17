"OpenSmock/Iconography: 26 of 32 tests error with 'KeyNotFound: key
#iconCheckCircleFilled32x32 not found in MethodDictionary'.  The icon methods
are not missing from a load: the tests generate them on purpose, in the
class SmockTestIconExport, in the two tests that serialize an icon as a form.
Every test of AntDesignIconsLibraryTest and MaterialDesignIcons4LibraryTest
(and of SmockAbstractIconsLibraryTest, whose setUp and tearDown call the
setUp and tearDown of AntDesignIconsLibraryTest) first removes the generated
methods in setUp and in tearDown, by sending removeSelector: for three
selectors.  In most tests the three methods do not exist, because no test
generated them.  Pharo 14 Behavior>>removeSelector: fails when the selector is
absent (its comment says: Fail if it is not the case); the tests were written
for a Pharo where the removal of an absent selector did nothing.  So setUp
fails in 26 tests before the test body runs.  The repair is in the package
test: removeGeneratedMethods in the two test classes removes only the
selectors that exist.  No assertion is changed."

#(#AntDesignIconsLibraryTest #MaterialDesignIcons4LibraryTest) do: [ :className |
	fix value: className value: #removeGeneratedMethods
		value: [ :source | (source includesSubstring: 'includesSelector:') not ]
		value:
'removeGeneratedMethods
	"remove generated methods; remove only the selectors that exist (Pharo 14 removeSelector: fails on an absent selector)"
	#(#iconCheckCircleFilled32x32 #iconCheckCircleFilled32x16 #iconCheckCircleFilled16x16) do: [ :each |
		(SmockTestIconExport class includesSelector: each)
			ifTrue: [ SmockTestIconExport class removeSelector: each ] ]' ].
