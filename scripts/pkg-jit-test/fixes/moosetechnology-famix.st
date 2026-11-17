"moosetechnology/Famix: four skipped tests and one expected failure.  All
five are tests whose own source is out of date.  No VM is at fault.

1. FamixMetricsTest>>testLCOM2 and testLCOM3 start with 'self skip'.  The
   methods lcom2 and lcom3 exist (FamixTLCOMMetrics) and the expected values
   in the test agree with the formulas of calculateLCOM2 and calculateLCOM3
   for ClassA to ClassF1 (checked by hand against the classes of
   Moose-TestResources-LCOM).  The repair removes the skip.  The last line of
   testLCOM2 asked for 'classG lcom3' (a copy and paste error): the line now
   asks for 'classG lcom2'.  By the formula, lcom2 of ClassG is
   1 - 1/(1*3) = 2/3, not 0.

2. FamixMetricsTest>>testMartinCohesionFamixPackage starts with 'self skip'
   and a flag 'should be rethink with FamixNG'.  The repair removes the skip.
   The expected values may be out of date; only a run can decide.

3. FamixTest2ModelTest>>testRootUniqueness starts with 'self skip' for an
   issue of the Moose project.  MooseModel class>>root answers
   MooseModelRoot root, and FamixTest2Model class inherits the method, so
   the superclass test (FamixTest2Model root identical to MooseModel root)
   holds.  The repair removes the override, so the test of MooseModelTest
   runs for the subclass.

4. FamixPackageNavigationChefTest>>testStaticProviderPackages is marked
   expectedFailure.  The result of the query is the three packages of the
   test plus Moose-TestResources-KGB-PExtensions (the test run says
   'additions: a FamixStPackage PExtensions', 'missing: empty').  The test
   testProviderPackages of the same class, with the same query, lists
   pExtensions.  The comment of the test says that static and other
   associations are no longer distinguished, so the two tests are the same
   query.  The mark hides an expectation that is out of date.  The repair adds
   pExtensions to the expected packages and removes the mark."

fix value: #FamixMetricsTest value: #testLCOM2
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testLCOM2
	self assert: classA lcom2 equals: 0.
	self assert: classB lcom2 equals: 0.
	self assert: classB1 lcom2 equals: 1/6.
	self assert: classB2 lcom2 equals: 1/6.
	self assert: classC lcom2 equals: 1/3.
	self assert: classD lcom2 equals: 0.
	self assert: classE lcom2 equals: 0.
	self assert: classF lcom2 equals: 1.
	self assert: classF1 lcom2 equals: 1.
	self assert: classG lcom2 equals: 2/3'.

fix value: #FamixMetricsTest value: #testLCOM3
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testLCOM3
	self assert: classA lcom3 equals: 0.
	self assert: classB lcom3 equals: 0.
	self assert: classB1 lcom3 equals: 0.25.
	self assert: classB2 lcom3 equals: 0.25.
	self assert: classC lcom3 equals: 0.5.
	self assert: classD lcom3 equals: 0.
	self assert: classE lcom3 equals: 0.
	self assert: classF lcom3 equals: 1.5.
	self assert: classF1 lcom3 equals: 2.
	self assert: classG lcom3 equals: 0'.

fix value: #FamixMetricsTest value: #testMartinCohesionFamixPackage
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testMartinCohesionFamixPackage
	self assert: p1m1 martinCohesion equals: 1.0.	"i.e. (0+1)/1"
	self assert: p2m1 martinCohesion equals: 1.0.	"M1P2C3->M1P2C4 M1P2C3->M1P2C2"
	self assert: p1m2 martinCohesion equals: (10 / 9) asFloat.	"B1->A1 C1->A1 C1->D1 D1->E1 E1->C1 E1->D1 F1->E1 H1->G1 I1->G1"
	self assert: p2m2 martinCohesion equals: 0.5.
	self assert: p3m2 martinCohesion equals: 1.0.	"A3->C3 B3->C3"
	self assert: p4m2 martinCohesion equals: 0.5'.

"The override only skips: remove the method, the inherited test runs."
(Smalltalk globals at: #FamixTest2ModelTest ifAbsent: [ nil ]) ifNotNil: [ :cls |
	(cls includesSelector: #testRootUniqueness)
		ifFalse: [ Stdio stdout nextPutAll: 'SKIP FamixTest2ModelTest>>testRootUniqueness (already removed)'; lf; flush ]
		ifTrue: [
			((cls >> #testRootUniqueness) sourceCode includesSubstring: 'self skip')
				ifFalse: [ Stdio stdout nextPutAll: 'SKIP FamixTest2ModelTest>>testRootUniqueness (defect not present)'; lf; flush ]
				ifTrue: [
					cls removeSelector: #testRootUniqueness.
					Stdio stdout nextPutAll: 'FIXED FamixTest2ModelTest>>testRootUniqueness'; lf; flush ] ] ].

fix value: #FamixPackageNavigationChefTest value: #testStaticProviderPackages
	value: [ :source | source includesSubstring: 'expectedFailure' ]
	value:
'testStaticProviderPackages
	"The distinction between static and other associations is no longer made, so the packages are the packages of testProviderPackages."

	self assertCollection: (self p8FullReferencer queryAllOutgoing containersOfType: FamixTPackage) hasSameElements: {
			self p10InteractedReferee.
			self p11FullReferee.
			self p7ReferencerReferee.
			self pExtensions }'.
