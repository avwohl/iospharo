"Prepare image for SimpleFormWorldRenderer"

"First, define the SimpleFormWorldRenderer class"
Object subclass: #SimpleFormWorldRenderer
	instanceVariableNames: 'world alreadyActivated display screenExtent'
	classVariableNames: ''
	package: 'Morphic-Core-Worlds'.

SimpleFormWorldRenderer class compile: 'isApplicableFor: aWorld
	"Apply when primitive-based display is available"
	| screenSize |
	screenSize := self primScreenSize.
	^ screenSize notNil and: [ screenSize x > 0 and: [ screenSize y > 0 ] ]'.

SimpleFormWorldRenderer class compile: 'primScreenSize
	"Primitive 106: Get screen size as Point"
	<primitive: 106>
	^ nil'.

SimpleFormWorldRenderer class compile: 'priority
	^ 5'.

SimpleFormWorldRenderer compile: 'initialize
	alreadyActivated := false'.

SimpleFormWorldRenderer compile: 'world
	^ world'.

SimpleFormWorldRenderer compile: 'world: aWorld
	world := aWorld'.

SimpleFormWorldRenderer compile: 'actualScreenSize
	^ screenExtent ifNil: [ self primScreenSize ifNil: [ 1024@768 ] ]'.

SimpleFormWorldRenderer compile: 'primScreenSize
	<primitive: 106>
	^ nil'.

SimpleFormWorldRenderer compile: 'primForceDisplayUpdate
	<primitive: 126>
	^ self'.

SimpleFormWorldRenderer compile: 'activate
	alreadyActivated ifTrue: [ ^ self ].
	self doActivate.
	alreadyActivated := true'.

SimpleFormWorldRenderer compile: 'doActivate
	screenExtent := self primScreenSize ifNil: [ 1024@768 ].
	display := Form extent: screenExtent depth: 32.
	display beDisplay.
	world extent: screenExtent.
	world worldState doFullRepaint.
	world displayWorld'.

SimpleFormWorldRenderer compile: 'deactivate
	display := nil'.

SimpleFormWorldRenderer compile: 'checkForNewScreenSize
	| newSize |
	newSize := self primScreenSize.
	(newSize notNil and: [ newSize ~= screenExtent ]) ifTrue: [
		screenExtent := newSize.
		self updateDisplayForm.
		world restoreMorphicDisplay ]'.

SimpleFormWorldRenderer compile: 'updateDisplayForm
	display := Form extent: screenExtent depth: 32.
	display beDisplay'.

SimpleFormWorldRenderer compile: 'drawDuring: aBlock
	display ifNil: [ ^ self ].
	aBlock value: display getCanvas'.

SimpleFormWorldRenderer compile: 'updateDamage: allDamage
	display ifNil: [ ^ self ].
	self primForceDisplayUpdate'.

SimpleFormWorldRenderer compile: 'convertWindowMouseEventPosition: aPosition
	^ aPosition'.

SimpleFormWorldRenderer compile: 'activateCursor: aCursor withMask: maskForm
	"Platform handles cursor"'.

SimpleFormWorldRenderer compile: 'clipboardText
	^ '''''.

SimpleFormWorldRenderer compile: 'clipboardText: aString
	"No clipboard support"'.

SimpleFormWorldRenderer compile: 'icon: aForm
	"Not supported"'.

SimpleFormWorldRenderer compile: 'windowTitle: aString
	"Not supported"'.

SimpleFormWorldRenderer compile: 'requestStopTextEditing
	"Not supported"'.

SimpleFormWorldRenderer compile: 'requestTextEditingAt: aRectangle
	"Not supported"'.

SimpleFormWorldRenderer compile: 'updateToNewResolution
	self checkForNewScreenSize'.

SimpleFormWorldRenderer compile: 'viewBox
	^ 0@0 corner: self actualScreenSize'.

SimpleFormWorldRenderer compile: 'usableArea
	^ self viewBox'.

SimpleFormWorldRenderer compile: 'restoreMorphicDisplay
	world
		extent: self actualScreenSize;
		viewBox: self viewBox;
		handsDo: [ :h | h visible: true; showTemporaryCursor: nil ];
		resizeBackgroundMorph;
		submorphsDo: [ :each | each displayExtentChanged ];
		fullRepaintNeeded.
	world defer: [ Cursor normal show ].
	world layoutChanged'.

SimpleFormWorldRenderer compile: 'displayWorldState: aWorldState ofWorld: aWorld
	| submorphs |
	submorphs := aWorld submorphs.
	submorphs do: [ :m | m fullBounds ].
	aWorldState checkIfUpdateNeeded ifFalse: [ ^ self ].
	self drawDuring: [ :canvas |
		| worldDamageRects handDamageRects allDamage handsToDraw |
		worldDamageRects := aWorldState drawWorld: aWorld submorphs: submorphs invalidAreasOn: canvas.
		handsToDraw := aWorldState selectHandsToDrawForDamage: worldDamageRects.
		handDamageRects := handsToDraw collect: [:h | h savePatchFrom: canvas].
		allDamage := worldDamageRects, handDamageRects.
		handsToDraw reverseDo: [:h | canvas fullDrawMorph: h].
		canvas finish.
		self updateDamage: allDamage.
		handsToDraw do: [:h | h restoreSavedPatchOn: canvas ] ]'.

SimpleFormWorldRenderer compile: 'invalidate
	"Nothing to do"'.

SimpleFormWorldRenderer compile: 'deferUpdates: aBoolean
	^ false'.

SimpleFormWorldRenderer compile: 'deferUpdatesDuring: aBlock
	^ aBlock value'.

SimpleFormWorldRenderer compile: 'fullscreenMode: aValue
	"Not supported"'.

"Make SimpleFormWorldRenderer a subclass of AbstractWorldRenderer properly"
AbstractWorldRenderer subclass: #SimpleFormWorldRenderer
	instanceVariableNames: 'display screenExtent'
	classVariableNames: ''
	package: 'Morphic-Core-Worlds'.

"Reset the main world renderer so it will be re-detected"
AbstractWorldRenderer class instVarNamed: 'MainWorldRenderer' put: nil.

"Confirm installation"
Transcript cr; show: 'SimpleFormWorldRenderer installed successfully!'.
Transcript cr; show: 'Priority: ', SimpleFormWorldRenderer priority asString.
Transcript cr; show: 'Saving image...'.

"Save the image"
Smalltalk saveSession.

Transcript cr; show: 'Image saved. Ready for custom VM.'.
