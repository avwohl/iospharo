"Fame (moosetechnology/Fame, development): the two skipped tests of
FMLazyRelationTest.  Both carry 'self flag: #currentLimitation. self skip.'
Cause 1 (testManyToOneWithlazyOne): the test class RPGLazyHero>>talismans:
assigns the slot directly (talismans := anObject).  FMMany>>updateOld:new:in:
then writes the raw Array into the slot and never updates the opposite side.
RPGLazyHero>>kills: uses 'kills value: anObject' and works.
Cause 2 (testOneToOneRelationReverse): FMLazyOne>>materializeFor:slot: loads a
value whose inverse to-one slot already holds another object, and takes the
stale lazy value over the explicit write.  A lazy value that conflicts with an
explicit write on the other side must lose."

fix value: #RPGLazyHero value: #talismans:
	value: [ :source | (source includesSubstring: 'talismans := anObject') ]
	value:
'talismans: anObject
	"value when multimulti relation"
	talismans value: anObject'.

fix value: #FMLazyOne value: #materializeFor:slot:
	value: [ :source | (source includesSubstring: 'isKindOf: FMLazyValue') not ]
	value:
'materializeFor: anObject slot: aSlot

	| holder value inverse current |
	holder := FMLazyOneHolder new.

	loader value: holder value: anObject.

	value := holder value.

	"An explicit write on the other side of a to-one relation, made before this lazy value was read, wins: the loaded value already points to another object. The raw instance variable is read, so no lazy value of the other side is materialized."
	(value notNil and: [ aSlot hasInverse ]) ifTrue: [
		inverse := aSlot realInverseSlotFor: value.
		inverse isToOne ifTrue: [
			current := value instVarAt: inverse index.
			(current notNil
				and: [ (current isKindOf: FMLazyValue) not
				and: [ current ~~ anObject ] ]) ifTrue: [ value := nil ] ] ].

	aSlot writeMaterialized: value to: anObject.

	value ifNotNil: [ aSlot addAssociationFrom: anObject to: value ].

	^ value'.

fix value: #FMLazyRelationTest value: #testManyToOneWithlazyOne
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testManyToOneWithlazyOne

	| entity hero hero2 |
	entity := RPGLazyTalisman new.
	hero := RPGLazyHero new.
	hero2 := RPGLazyHero new.

	FMLazyValue
		makeRelation: #owner
		of: entity
		lazyUsing: [ :rel :entit | rel value: hero ].
	hero2 talismans: { entity }.
	self assert: entity owner identicalTo: hero2.
	self assert: hero talismans size equals: 0.
	self assert: hero2 talismans anyOne identicalTo: entity'.

fix value: #FMLazyRelationTest value: #testOneToOneRelationReverse
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testOneToOneRelationReverse

	| weapon weapon2 hero |
	weapon := RPGLazyUniqueWeapon new.
	hero := RPGLazyHero new.
	weapon2 := RPGLazyUniqueWeapon new.

	FMLazyValue
		makeRelation: #owner
		of: weapon
		lazyUsing: [ :rel :entit | rel value: hero ].

	hero uniqueWeapon: weapon2.

	self assert: hero uniqueWeapon identicalTo: weapon2.
	self assert: weapon owner isNil'.
