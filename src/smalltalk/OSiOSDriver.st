"OSiOSDriver - OSWindow driver for iOS VM"

OSWindowDriver subclass: #OSiOSDriver
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'OSWindow-iOS'!

!OSiOSDriver class methodsFor: 'testing'!

isSuitable
    ^ self isSupported!

isSupported
    | result |
    [result := self primScreenSize]
        on: Error
        do: [:e | ^ false].
    ^ result isPoint and: [result x > 0 and: [result y > 0]]!

driverName
    ^ 'iOS'!

priority
    ^ 5! !

!OSiOSDriver class methodsFor: 'primitives'!

primScreenSize
    <primitive: 106>
    self primitiveFailed! !

!OSiOSDriver methodsFor: 'window creation'!

createWindowWithAttributes: attributes osWindow: osWindow
    ^ OSiOSBackendWindow new
        osWindow: osWindow;
        attributes: attributes;
        yourself! !

!OSiOSDriver methodsFor: 'system startup'!

startUp: resuming
    resuming ifTrue: [
        OSWindowDriver current == self ifTrue: [
            Smalltalk at: #Display ifPresent: [:disp | disp beDisplay]]]!

shutDown: quitting
    ! !

!OSiOSDriver methodsFor: 'testing'!

isNullDriver
    ^ false! !

OSBackendWindow subclass: #OSiOSBackendWindow
    instanceVariableNames: 'attributes extent title'
    classVariableNames: ''
    package: 'OSWindow-iOS'!

!OSiOSBackendWindow class methodsFor: 'primitives'!

primScreenSize
    <primitive: 106>
    ^ 1024 @ 768! !

!OSiOSBackendWindow methodsFor: 'initialization'!

initialize
    super initialize.
    extent := self class primScreenSize.
    title := 'Pharo'!

attributes: anObject
    attributes := anObject! !

!OSiOSBackendWindow methodsFor: 'accessing'!

bounds
    ^ 0@0 extent: extent!

bounds: aRectangle
    extent := aRectangle extent!

extent
    ^ extent!

extent: aPoint
    extent := aPoint.
    renderer ifNotNil: [renderer newExtent: aPoint]!

position
    ^ 0@0!

position: aPoint
    !

title
    ^ title!

title: aString
    title := aString!

isValid
    ^ true!

platformSpecificHandle
    ^ nil! !

!OSiOSBackendWindow methodsFor: 'visibility'!

show
    !

hide
    ! !

!OSiOSBackendWindow methodsFor: 'renderer'!

newFormRenderer: aForm
    renderer := OSiOSFormRenderer new.
    renderer backendWindow: self; form: aForm.
    ^ renderer!

newGenericRenderer
    ^ self newFormRenderer: (Form extent: extent depth: 32)! !

!OSiOSBackendWindow methodsFor: 'text input'!

isTextInputActive
    ^ false!

startTextInput
    !

startTextInputAtRectangle: aRectangle
    !

stopTextInput
    ! !

!OSiOSBackendWindow methodsFor: 'clipboard'!

clipboardText
    ^ ''!

clipboardText: aString
    ! !

OSWindowFormRenderer subclass: #OSiOSFormRenderer
    instanceVariableNames: ''
    classVariableNames: ''
    package: 'OSWindow-iOS'!

!OSiOSFormRenderer methodsFor: 'accessing'!

outputExtent
    ^ backendWindow extent!

pixelExtent
    ^ form extent! !

!OSiOSFormRenderer methodsFor: 'rendering'!

updateAreas: aCollectionOfRectangles immediate: forceFlag
    form beDisplay.
    self primForceDisplayUpdate!

primForceDisplayUpdate
    <primitive: 103>
    ^ self!

newExtent: aPoint
    | newForm |
    (form isNil or: [form extent ~= aPoint]) ifTrue: [
        newForm := Form extent: aPoint depth: 32.
        form ifNotNil: [newForm getCanvas drawImage: form at: 0@0].
        form := newForm.
        form beDisplay]! !

!OSiOSFormRenderer methodsFor: 'drawing'!

drawDuring: aBlock
    aBlock value: form getCanvas.
    self updateAreas: {0@0 extent: form extent} immediate: true! !

!OSiOSDriver class methodsFor: 'installation'!

install
    self isSupported ifTrue: [
        OSWindowDriver current: self new.
        Transcript show: 'OSiOSDriver installed'; cr]
    ifFalse: [
        Transcript show: 'OSiOSDriver not supported'; cr]! !
