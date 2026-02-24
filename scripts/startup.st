!MenuItemMorph methodsFor: 'balloon'!
boundsForBalloon
	"Position balloon help outside the containing menu.
	Returns a 1px strip at the right edge of the menu so the
	balloon algorithm places the tooltip to the right of the menu."
	| menu menuBounds |
	menu := self owner.
	(menu isNil or: [(menu isKindOf: MenuMorph) not])
		ifTrue: [^ self boundsInWorld].
	menuBounds := menu boundsInWorld.
	^ (menuBounds right @ self boundsInWorld top)
		corner: (menuBounds right + 1 @ self boundsInWorld bottom)! !
