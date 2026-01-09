"
SimpleFormWorldRenderer - A primitive-based world renderer for custom VMs

This renderer uses the classic Pharo/Squeak display primitives:
- Primitive 102: beDisplay - register a Form as the screen
- Primitive 126: forceDisplayUpdate - copy Form bits to platform surface

Use this renderer when your VM implements these primitives but doesn't
have the full OSWindow/SDL2 plugin.

To install in your image:
1. File in this class
2. Execute: AbstractWorldRenderer mainWorldRenderer: nil
3. The renderer will be auto-selected on next startup (priority 5 > NullWorldRenderer's 0)
"
Class {
	#name : 'SimpleFormWorldRenderer',
	#superclass : 'AbstractWorldRenderer',
	#instVars : [
		'display',
		'screenExtent'
	],
	#category : 'Morphic-Core-Worlds',
	#package : 'Morphic-Core',
	#tag : 'Worlds'
}

{ #category : 'accessing' }
SimpleFormWorldRenderer class >> isApplicableFor: aWorld [
	"Apply when we detect primitive-based display is available.
	 Check if primitiveScreenSize (106) works."

	| screenSize |
	screenSize := self primScreenSize.
	^ screenSize notNil and: [ screenSize x > 0 and: [ screenSize y > 0 ] ]
]

{ #category : 'accessing' }
SimpleFormWorldRenderer class >> primScreenSize [
	"Primitive 106: Get screen size as Point"
	<primitive: 106>
	^ nil  "Primitive failed - not available"
]

{ #category : 'accessing' }
SimpleFormWorldRenderer class >> priority [
	"Higher than NullWorldRenderer (0) so we get selected when applicable"
	^ 5
]

{ #category : 'accessing' }
SimpleFormWorldRenderer >> activateCursor: aCursor withMask: maskForm [
	"For now, cursor is handled by the platform"
]

{ #category : 'accessing' }
SimpleFormWorldRenderer >> actualScreenSize [
	^ screenExtent ifNil: [ self primScreenSize ifNil: [ 1024@768 ] ]
]

{ #category : 'display box access' }
SimpleFormWorldRenderer >> checkForNewScreenSize [
	| newSize |
	newSize := self primScreenSize.
	(newSize notNil and: [ newSize ~= screenExtent ]) ifTrue: [
		screenExtent := newSize.
		self updateDisplayForm.
		world restoreMorphicDisplay ]
]

{ #category : 'accessing' }
SimpleFormWorldRenderer >> clipboardText [
	^ ''  "No clipboard support yet"
]

{ #category : 'accessing' }
SimpleFormWorldRenderer >> clipboardText: aString [
	"No clipboard support yet"
]

{ #category : 'utilities' }
SimpleFormWorldRenderer >> convertWindowMouseEventPosition: aPosition [
	^ aPosition
]

{ #category : 'activation' }
SimpleFormWorldRenderer >> deactivate [
	display := nil
]

{ #category : 'activation' }
SimpleFormWorldRenderer >> doActivate [
	"Initialize the display Form and register it with the VM"

	screenExtent := self primScreenSize ifNil: [ 1024@768 ].
	display := Form extent: screenExtent depth: 32.

	"Register this Form as the screen display (primitive 102)"
	display beDisplay.

	world extent: screenExtent.
	world worldState doFullRepaint.
	world displayWorld
]

{ #category : 'rendering' }
SimpleFormWorldRenderer >> drawDuring: aBlock [
	"Execute aBlock with a canvas for drawing"

	display ifNil: [ ^ self ].
	aBlock value: display getCanvas
]

{ #category : 'operations' }
SimpleFormWorldRenderer >> forceDamageToScreen: allDamage [
	"Force damaged areas to the screen using primitive 126"

	display ifNil: [ ^ self ].

	"Copy the entire display to the platform surface"
	self primForceDisplayUpdate
]

{ #category : 'accessing' }
SimpleFormWorldRenderer >> icon: aForm [
	"Platform-specific, not supported yet"
]

{ #category : 'private' }
SimpleFormWorldRenderer >> primForceDisplayUpdate [
	"Primitive 126: Copy display Form bits to platform surface"
	<primitive: 126>
	^ self  "Primitive failed silently"
]

{ #category : 'private' }
SimpleFormWorldRenderer >> primScreenSize [
	"Primitive 106: Get screen size as Point"
	<primitive: 106>
	^ nil  "Primitive not available"
]

{ #category : 'events' }
SimpleFormWorldRenderer >> requestStopTextEditing [
	"Platform-specific text input not supported yet"
]

{ #category : 'events' }
SimpleFormWorldRenderer >> requestTextEditingAt: aRectangle [
	"Platform-specific text input not supported yet"
]

{ #category : 'rendering' }
SimpleFormWorldRenderer >> updateDamage: allDamage [
	"Push damaged rectangles to the screen"

	display ifNil: [ ^ self ].

	"For now, just update the entire display.
	 A more efficient implementation would only update damaged areas."
	self primForceDisplayUpdate
]

{ #category : 'private' }
SimpleFormWorldRenderer >> updateDisplayForm [
	"Resize the display Form when screen size changes"

	display := Form extent: screenExtent depth: 32.
	display beDisplay
]

{ #category : 'events' }
SimpleFormWorldRenderer >> updateToNewResolution [
	self checkForNewScreenSize
]

{ #category : 'accessing' }
SimpleFormWorldRenderer >> windowTitle: aString [
	"Platform-specific window title not supported yet"
]
