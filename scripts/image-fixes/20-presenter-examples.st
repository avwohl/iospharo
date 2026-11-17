"presenter-examples: eight presenter test classes skip testExample because the
 presenter class has no class-side #example.  Each presenter gets one."
add value: #'SpDemo class' value: #example value: 'examples' value: 'example

	"Open the demo window."
	^ self open'.
add value: #'SpComponentListSelectionExample class' value: #example value: 'examples' value: 'example

	"Open the component list selection example."
	^ self open'.
add value: #'SpCodeEditorPresenter class' value: #example value: 'examples' value: 'example

	"Show a code editor with a short text."
	^ self new
		text: ''42 factorial'';
		open'.
add value: #'MicTextPresenter class' value: #example value: 'examples' value: 'example

	"Show the Microdown sample document as rich text."
	^ self exampleSample'.
add value: #'MicrodownPresenter class' value: #example value: 'examples' value: 'example

	"Show the Microdown cheat sheet in an editable Microdown presenter."
	^ self new
		document: MicTextPresenter cheetSheet;
		open'.
add value: #'SpToolbarToggleButtonPresenter class' value: #example value: 'examples' value: 'example

	"Show a toolbar with two toggle buttons."
	| toolbar |
	toolbar := SpToolbarPresenter new.
	toolbar
		beBoth;
		add: (self new
				 icon: (self iconNamed: #smallOk);
				 label: ''Toggle'';
				 yourself);
		add: (self new
				 icon: (self iconNamed: #smallOk);
				 label: ''Selected'';
				 beSelected;
				 yourself).
	^ toolbar open'.
add value: #'SpMenuItemPresenter class' value: #example value: 'examples' value: 'example

	"Show a button that opens a menu with items, one of them with a sub menu.
	 The menu opens inside a window; a popup menu opened at a position runs a
	 modal loop and does not return until the user dismisses the menu."
	| presenter menuButton |
	(presenter := SpPresenter new)
		layout: (SpBoxLayout newTopToBottom
			add: (SpBoxLayout newLeftToRight
					addLast: (menuButton := presenter newMenuButton) expand: false;
					yourself)
				expand: false;
			yourself).
	menuButton menu: [
		SpMenuPresenter new
			addItem: [ :item | item name: ''Test 1''; action: [ "action" ] ];
			addItem: [ :item |
				item
					name: ''Test 2'';
					subMenu: (SpMenuPresenter new
						addItem: [ :subMenuItem | subMenuItem name: ''SubMenu 1''; action: [ ] ];
						addItem: [ :subMenuItem | subMenuItem name: ''SubMenu 2''; action: [ ] ];
						yourself) ];
			addItem: [ :item | item name: ''Test 3''; action: [ "action" ] ];
			yourself ].
	^ presenter open'.
add value: #'SpMenuPresenter class' value: #example value: 'examples' value: 'example

	"Show a button that opens a menu with two groups of items.  The menu opens
	 inside a window; a popup menu opened at a position runs a modal loop and
	 does not return until the user dismisses the menu."
	| presenter menuButton |
	(presenter := SpPresenter new)
		layout: (SpBoxLayout newTopToBottom
			add: (SpBoxLayout newLeftToRight
					addLast: (menuButton := presenter newMenuButton) expand: false;
					yourself)
				expand: false;
			yourself).
	menuButton menu: [
		SpMenuPresenter new
			addGroup: [ :group | group
				addItem: [ :item | item name: ''Group 1. Test 1''; action: [ "action" ] ];
				addItem: [ :item | item name: ''Group 1. Test 2''; action: [ "action" ] ] ];
			addGroup: [ :group | group
				addItem: [ :item | item name: ''Group 2. Test 1''; action: [ "action" ] ];
				addItem: [ :item | item name: ''Group 2. Test 2''; action: [ "action" ] ] ];
			yourself ].
	^ presenter open'.
