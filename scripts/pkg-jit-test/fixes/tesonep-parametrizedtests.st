"ParametrizedTests (tesonep/ParametrizedTests, master 9c43791): Pharo 14 contains
this package in SUnit-Core (ParametrizedTestCase, ParametrizedTestMatrix,
ParametrizedTestOption, ParametrizedTestExpandedParameter) and SUnit-Tests
(PaSuiteTest and the Pa*ExampleTest classes).  The load of the old package
replaces the Pharo 14 method ParametrizedTestMatrix>>addCase: with the old
version.  The old version sends #asDictionary to its argument and expects
associations.  Pharo 14 passes arrays of ParametrizedTestExpandedParameter
(ParametrizedTestMatrix>>*), and the old code sends #key to an expanded
parameter: ShouldBeImplemented.  Three tests of PaSuiteTest that exist only in
Pharo 14 (testMultiply...MultiplyCases) fail on stock Cog and on our VM.

The repair puts back the Pharo 14 source of the method."

fix value: #ParametrizedTestMatrix value: #addCase:
	value: [ :source | source includesSubstring: 'asDictionary' ]
	value:
'addCase: aCollection
	"Declare a specific configuration of parameters. See forSelector:addOptions: for matrix combinations."

	cases add: (aCollection
		collect: [ :assoc | assoc asTestParameter ]
		as: Array).

	^ self'.
