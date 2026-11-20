fix value: #StObjectTranscriptPresenterTest value: #testTrueShowInspectorShowsInspector value: [:s | s includesSubstring: 'self skip'] value: 'testTrueShowInspectorShowsInspector
	"The window extent is remembered per class when a window is resized (SpPresenter class>>preferredExtent:). Other tests open windows and can leave a remembered extent behind, so this test starts from the default extent and puts the remembered extent back afterwards."
	| st win current remembered |
	current := StObjectTranscriptPresenter showInspector.
	remembered := StObjectTranscriptPresenter instVarNamed: #customExtent.
	[
	StObjectTranscriptPresenter resetRememberedResizeExtent.
	StObjectTranscriptPresenter showInspector: true.
	st := StObjectTranscriptPresenter new.
	st logger: StObjectTranscriptLogger instance.
	win := st asWindow.
	win resizable: false.
	win open.
	self assert: win initialExtent equals: 550 @ 850 ] ensure: [
		win ifNotNil: [ win close ].
		StObjectTranscriptPresenter showInspector: current.
		StObjectTranscriptPresenter instVarNamed: #customExtent put: remembered ]'.
