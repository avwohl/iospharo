"SpMorphicMorphAdapter ignored SpMorphPresenter>>help, so the inherited
 SpAbstractWidgetAdapterTest>>testChangingHelpAffectTheWidget was skipped in
 SpMorphPresenterTest with 'It does not has sense here'.  Every other adapter
 (Label, Image, Athens, Button) puts the presenter help on its morph as balloon
 text and follows whenHelpChangedDo:.  The Morph adapter does the same now, and
 the test override that skipped is removed so the inherited test runs."
add value: #SpMorphicMorphAdapter value: #applyHelpTo: value: 'private' value: 'applyHelpTo: aMorph
	"The presenter help becomes the balloon text of the wrapped morph.  A presenter without
	 help leaves the balloon text that the morph has itself."

	self presenter help ifNotNil: [ :aString |
		aMorph setBalloonText: (aString localizedForPresenter: self presenter) ]'.
fix value: #SpMorphicMorphAdapter value: #buildWidget
	value: [ :src | (src includesSubstring: 'applyHelpTo:') not ]
	value: 'buildWidget

	| newWidget |

	newWidget := self presenter morph 
		ifNil: [ Morph new color: Color transparent ].
	newWidget presenter: self presenter.
	self applyHelpTo: newWidget.

	self presenter whenMorphChangedDo: [ :aMorph | 
		self updateMorph: aMorph ].
	self presenter whenHelpChangedDo: [ 
		self widget ifNotNil: [ :w | self applyHelpTo: w ] ].
	
	^ newWidget'.
fix value: #SpMorphicMorphAdapter value: #updateMorph:
	value: [ :src | (src includesSubstring: 'applyHelpTo:') not ]
	value: 'updateMorph: aMorph

	self applyStyle: aMorph.
	aMorph presenter: self presenter.
	self applyHelpTo: aMorph.
	widget owner ifNotNil: [ :ownerMorph | 
		ownerMorph 
			replaceSubmorph: widget
			by: aMorph ].
	widget := aMorph'.
(SpMorphPresenterTest includesSelector: #testChangingHelpAffectTheWidget)
	ifTrue: [
		((SpMorphPresenterTest >> #testChangingHelpAffectTheWidget) sourceCode includesSubstring: 'self skip')
			ifTrue: [
				SpMorphPresenterTest removeSelector: #testChangingHelpAffectTheWidget.
				Stdio stdout nextPutAll: 'FIXED SpMorphPresenterTest>>testChangingHelpAffectTheWidget (skip override removed)'; lf; flush ]
			ifFalse: [ Stdio stdout nextPutAll: 'SKIP SpMorphPresenterTest>>testChangingHelpAffectTheWidget (not skipping)'; lf; flush ] ]
	ifFalse: [ Stdio stdout nextPutAll: 'SKIP SpMorphPresenterTest>>testChangingHelpAffectTheWidget (absent)'; lf; flush ].
