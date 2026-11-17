"Ficus (tinchodias/Ficus, mini): two tests that do not pass on both VMs.
See docs/upstream/package-repairs.md.

1. FiSetSimpleChangesTest>>testUpdate is marked <expectedFailure>.  The
   conversion FiSet>>editFromSimpleChange: answers a FiSubEdit for an update
   with children, while FiSetUpdate>>edit (pinned by the passing test
   FiSetChangesTest>>testUpdate) answers FiInclude value: newValue.  The
   converter now answers the same FiInclude, and the mark goes.

2. FiBuilderTest>>testErrorWhenSettingAnExcludedVariable is skipped by the
   author because the validation was removed.  The validation is back in
   FiFixedObject>>initializeFromStepsAndValues: (one membership test for each
   step), and the skip goes."

fix value: #FiSet value: #editFromSimpleChange:
	value: [ :source | source includesSubstring: 'ifUpdate: [ ].' ]
	value:
'editFromSimpleChange: aSimpleChange
	"Convert as edit, assuming the FiSimpleChange applies to myself."
	
	aSimpleChange
		ifAdd: [ ^ self editInclude: aSimpleChange newValue ] 
		ifRemove: [ ^ self editExclude: aSimpleChange oldValue ] 
		ifUpdate: [ ].
	
	"An update of an element is the edit that FiSetUpdate>>edit answers: include the new value."
	^ self editInclude: aSimpleChange newValue'.

fix value: #FiSetSimpleChangesTest value: #testUpdate
	value: [ :source | source includesSubstring: '<expectedFailure>' ]
	value:
'testUpdate
	| shot otherShot edit source intermediate change simpleChanges |
	source := FiSet new.
	shot := FiChef nickname: #A.
	otherShot := shot resultOf: (shot editAt: #shortBio equal: ''42'').

	edit := source editInclude: shot.
	intermediate := source resultOf: edit.

	edit := intermediate editInclude: otherShot.
	change := intermediate changeOf: edit.

	simpleChanges := change asSimpleChanges.

	self 
		assert: (intermediate editFromSimpleChanges: simpleChanges) 
		equals: change edit.
	
	self assert: simpleChanges equals: 
		(FiSet withValues: {
			(FiUpdate builder
				path: FiPath root / #A;
				children: (FiSet withValues: {
					(FiUpdate builder
						path: FiPath root / #A / #shortBio;
						children: FiSet new;
						newValue: ''42'';
						oldValue: '''';
						new).
					});
				newValue: (FiChef nickname: #A shortBio: ''42'');
				oldValue: (FiChef nickname: #A);
				new).
			})'.

fix value: #FiFixedObject value: #initializeFromStepsAndValues:
	value: [ :source | (source includesSubstring: 'excludedInstanceVariableNamesForSteps') not ]
	value:
'initializeFromStepsAndValues: aDictionary
	| selector excluded |
	excluded := self class excludedInstanceVariableNamesForSteps.
	self initialize.
	aDictionary
		keysAndValuesDo: [ :slotName :value | 
			(excluded includes: slotName asSymbol)
				ifTrue: [ self error: slotName asString , '' is not a step of '' , self class name ].
			selector := (slotName , '':'') asSymbol.
			
			(self respondsTo: selector)
				ifTrue: [ self perform: selector with: value ]
				ifFalse: [ [ self instVarNamed: slotName put: value ]
						on: Error
						do: [ :e | self error: slotName asString , '' not found in '' , self asString ] ] ]'.

fix value: #FiBuilderTest value: #testErrorWhenSettingAnExcludedVariable
	value: [ :source | source includesSubstring: 'self skip:' ]
	value:
'testErrorWhenSettingAnExcludedVariable

	self 
		assert: (FiRecipe excludedInstanceVariableNamesForSteps includes: #cachedHash)
		description: ''This is an assumption of this test''.
	
	self 
		should: [ 
			FiRecipe builder
				cachedHash: 123; "<- this is invalid"
				new ]
		raise: Error'.
