"moosetechnology/Famix: causes and repairs.

1. FamixMetricsTest>>testLCOM2 and testLCOM3 (error: FamixStClass does not
   understand #lcom2 and #lcom3).  The class FamixStClass of the Smalltalk
   metamodel does not use the trait FamixTLCOMMetrics, which has lcom2 and
   lcom3.  The Java metamodel puts the trait in its class with the line
   'class --|> #TLCOMMetrics' in FamixJavaGenerator, but
   FamixPharoSmalltalkGenerator does not have the line.  The first entry adds
   the line to defineHierarchy of the generator.  The second entry adds the
   trait to the trait composition of the generated class FamixStClass (a
   change of the composition of the instance side also changes the class
   side).  Both tests start with 'self skip'; the entries remove the skip.
   The last line of testLCOM2 asked for 'classG lcom3' (a copy and paste
   error).  By the formula, lcom2 of ClassG is 1 - 1/(1*3) = 2/3, so the
   line asks for 'classG lcom2'.

2. FamixMetricsTest>>testMartinCohesionFamixPackage (failure: got 5.0
   instead of 1.0).  Martin's relational cohesion of a package is
   H = (R + 1) / N, with R the number of relations between classes of the
   package and N the number of classes of the package.  The expected values
   of the test are right: for the package P1 of the blueprint resources
   there are nine relations among nine classes, so H = 10/9, and so on.
   The method FamixTCohesionCouplingMetrics>>martinCohesion is wrong.  It
   keeps only the target classes that are inside the same namespace as the
   source class ('withinContainerUsing: FamixTNamespace').  In the
   Smalltalk metamodel every class is in the namespace Smalltalk, so every
   used class counts (also Object and the classes of other packages) and
   the value is too big.  FamixJavaPackage>>martinCohesion has the same code
   with 'FamixTPackage', which is the definition.  The entry changes the
   trait method to use FamixTPackage.  The test starts with 'self skip';
   the entry removes the skip.

3. FamixTest2ModelTest>>testRootUniqueness starts with 'self skip' for an
   old issue of the Moose project.  MooseModel class>>root answers
   MooseModelRoot root, and FamixTest2Model class inherits the method, so
   the test of the superclass runs for the subclass.  The entry removes the
   override.

4. FamixPackageNavigationChefTest>>testStaticProviderPackages is marked
   expectedFailure.  The query answers the three packages of the test plus
   Moose-TestResources-KGB-PExtensions, the same as testProviderPackages of
   the same class.  The expected set is out of date.  The entry adds
   pExtensions and removes the mark."


"The generator of the Smalltalk metamodel lacks the LCOM trait for the class."
(Smalltalk globals at: #FamixPharoSmalltalkGenerator ifAbsent: [ nil ])
	ifNil: [ Stdio stdout nextPutAll: 'SKIP FamixPharoSmalltalkGenerator>>defineHierarchy (absent)'; lf; flush ]
	ifNotNil: [ :gen |
		(gen includesSelector: #defineHierarchy)
			ifFalse: [ Stdio stdout nextPutAll: 'SKIP FamixPharoSmalltalkGenerator>>defineHierarchy (absent)'; lf; flush ]
			ifTrue: [ | src |
				src := (gen >> #defineHierarchy) sourceCode.
				(src includesSubstring: 'TLCOMMetrics')
					ifTrue: [ Stdio stdout nextPutAll: 'SKIP FamixPharoSmalltalkGenerator>>defineHierarchy (defect not present)'; lf; flush ]
					ifFalse: [
						gen compile: (src copyReplaceAll: '	class --|> #TClass.
' with: '	class --|> #TClass.
	class --|> #TLCOMMetrics.
') classified: (gen >> #defineHierarchy) protocolName.
						Stdio stdout nextPutAll: 'FIXED FamixPharoSmalltalkGenerator>>defineHierarchy'; lf; flush ] ] ].

"The generated class FamixStClass: add the trait."
(Smalltalk globals at: #FamixStClass ifAbsent: [ nil ]) ifNotNil: [ :cls |
	(cls traitComposition includesTrait: FamixTLCOMMetrics)
		ifTrue: [ Stdio stdout nextPutAll: 'SKIP FamixStClass (FamixTLCOMMetrics already in the composition)'; lf; flush ]
		ifFalse: [
			cls setTraitComposition: cls traitComposition + FamixTLCOMMetrics.
			Stdio stdout nextPutAll: 'FIXED FamixStClass (added FamixTLCOMMetrics)'; lf; flush ] ].

fix value: #FamixTCohesionCouplingMetrics value: #martinCohesion
	value: [ :source | source includesSubstring: 'withinContainerUsing: FamixTNamespace' ]
	value: (((Smalltalk globals at: #FamixTCohesionCouplingMetrics) >> #martinCohesion) sourceCode
		copyReplaceAll: 'withinContainerUsing: FamixTNamespace' with: 'withinContainerUsing: FamixTPackage').

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
