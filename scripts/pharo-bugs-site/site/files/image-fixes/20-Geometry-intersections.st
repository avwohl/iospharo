fix value: #GEllipse value: #intersectionsWithEllipse: value: [ :src | src includesSubstring: 'shouldBeImplemented' ] value: 'intersectionsWithEllipse: aGEllipse
	"Answer the points where the boundary of the receiver and the boundary of aGEllipse meet. Works for any two ellipses (also circles), whatever their orientation, and also when the boundaries only touch. The boundaries of two equal ellipses have infinitely many common points; an empty collection is answered for them.

	Method: the receiver is written as P(t) = c + cos(t) u + sin(t) v. Seen from aGEllipse (the map that makes aGEllipse the unit circle), g(t) = |Q(t)|^2 - 1 is zero for a common point. The real zeros of g are found by sampling, bisection for sign changes and a search for the dips where g touches zero without crossing."

	| n h tol cax cay uax uay vax vay cbx cby ubx uby vbx vby det g values roots pointAt bisect result scale |
	cax := center x asFloat. cay := center y asFloat.
	uax := vertex x - center x. uay := vertex y - center y.
	vax := coVertex x - center x. vay := coVertex y - center y.
	cbx := aGEllipse center x asFloat. cby := aGEllipse center y asFloat.
	ubx := aGEllipse vertex x - aGEllipse center x. uby := aGEllipse vertex y - aGEllipse center y.
	vbx := aGEllipse coVertex x - aGEllipse center x. vby := aGEllipse coVertex y - aGEllipse center y.
	det := (ubx * vby - (vbx * uby)) asFloat.
	det = 0 ifTrue: [ ^ {  } ].
	pointAt := [ :t | | c s |
		c := t cos. s := t sin.
		Array with: cax + (c * uax) + (s * vax) with: cay + (c * uay) + (s * vay) ].
	g := [ :t | | p dx dy qx qy |
		p := pointAt value: t.
		dx := (p at: 1) - cbx. dy := (p at: 2) - cby.
		qx := ((vby * dx) - (vbx * dy)) / det.
		qy := ((ubx * dy) - (uby * dx)) / det.
		(qx * qx) + (qy * qy) - 1 ].
	n := 720.
	h := Float twoPi / n.
	tol := 1.0e-9.
	values := (0 to: n - 1) collect: [ :i | g value: i * h ].
	roots := OrderedCollection new.
	bisect := [ :lo :hi | | a b ga m gm |
		a := lo. b := hi. ga := g value: a.
		1 to: 100 do: [ :k |
			m := (a + b) / 2. gm := g value: m.
			(gm = 0 or: [ (gm > 0) = (ga > 0) ]) ifTrue: [ gm = 0 ifTrue: [ a := m. b := m ] ifFalse: [ a := m. ga := gm ] ] ifFalse: [ b := m ] ].
		roots add: (a + b) / 2 ].
	values isEmpty ifFalse: [
		(values allSatisfy: [ :each | each abs < tol ]) ifTrue: [ ^ {  } ] ].
	1 to: n do: [ :i | | gi gn gp t0 sign lo hi mid1 mid2 f1 f2 tmin gmin |
		gi := values at: i.
		gn := values at: i \\ n + 1.
		gp := values at: i - 2 \\ n + 1.
		t0 := (i - 1) * h.
		gi = 0
			ifTrue: [ roots add: t0 ]
			ifFalse: [
				(gi > 0) ~= (gn > 0)
					ifTrue: [ gn = 0 ifFalse: [ bisect value: t0 value: t0 + h ] ]
					ifFalse: [
						(((gp > 0) = (gi > 0)) and: [ gp ~= 0 and: [ gn ~= 0 and: [ gi abs <= gp abs and: [ gi abs <= gn abs ] ] ] ]) ifTrue: [
							sign := gi > 0 ifTrue: [ 1 ] ifFalse: [ -1 ].
							lo := t0 - h. hi := t0 + h.
							1 to: 100 do: [ :k |
								mid1 := lo + ((hi - lo) / 3). mid2 := hi - ((hi - lo) / 3).
								f1 := sign * (g value: mid1). f2 := sign * (g value: mid2).
								f1 < f2 ifTrue: [ hi := mid2 ] ifFalse: [ lo := mid1 ] ].
							tmin := (lo + hi) / 2.
							gmin := sign * (g value: tmin).
							gmin <= 0
								ifTrue: [
									gmin = 0
										ifTrue: [ roots add: tmin ]
										ifFalse: [ bisect value: t0 - h value: tmin. bisect value: tmin value: t0 + h ] ]
								ifFalse: [ gmin < tol ifTrue: [ roots add: tmin ] ] ] ] ] ].
	scale := (uax abs + uay abs + vax abs + vay abs + ubx abs + uby abs + vbx abs + vby abs) max: 1.
	result := OrderedCollection new.
	roots do: [ :t | | p candidate |
		p := pointAt value: t.
		candidate := (p at: 1) , (p at: 2).
		(result anySatisfy: [ :each | (each x - candidate x) abs + (each y - candidate y) abs < (1.0e-7 * scale) ])
			ifFalse: [ result add: candidate ] ].
	^ result asArray'.

fix value: #GArcTest value: #testIntersectionsWithArc value: [ :src | src includesSubstring: '<expectedFailure>' ] value: 'testIntersectionsWithArc
	| arc2 |
	arc2 := GArc center: 1 , -1 origin: 5 , -1 direction: 5 , 0.
	arc := GArc center: 1 , 0 origin: 4 , 0 direction: -1 , 2.
	self assert: (arc intersectionsWith: arc2) isEmpty.


	arc2 := GArc center: 1 , -1 origin: 5 , -1 direction: 0 , 2.
	self assert: (arc intersectionsWith: arc2) hasCloseElementsTo: {(1 , 3)}.
	self assertCollection: (arc intersectionsWith: arc2) hasSameElements: (arc intersectionsWithArc: arc2)'.

fix value: #GArcTest value: #testIntersectionsWithEllipse value: [ :src | src includesSubstring: '<expectedFailure>' ] value: 'testIntersectionsWithEllipse
	| ellipse |
	ellipse := GEllipse center: -4 , 3 vertex: 1 , 3 coVertex: -4 , 7.
	arc := GArc center: 1 , 0 origin: 4 , 0 direction: 3 , 2.
	self assert: (arc intersectionsWith: ellipse) isEmpty.

	arc := GArc center: 1 , 0 origin: 4 , 0 direction: -1 , 2.
	self assert: (arc intersectionsWith: ellipse) hasCloseElementsTo: {(1 , 3)}.
	self assertCollection: (arc intersectionsWith: ellipse) hasSameElements: (arc intersectionsWithEllipse: ellipse)'.

fix value: #GEllipseTest value: #testIntersectionsWithArc value: [ :src | src includesSubstring: '<expectedFailure>' ] value: 'testIntersectionsWithArc
	| arc |
	ellipse := GEllipse center: -4 , 3 vertex: 1 , 3 coVertex: -4 , 7.
	arc := GArc center: 1 , 0 origin: 4 , 0 direction: 3 , 2.
	self assert: (ellipse intersectionsWith: arc) isEmpty.

	arc := GArc center: 1 , 0 origin: 4 , 0 direction: -1 , 2.
	self assert: (ellipse intersectionsWith: arc) hasCloseElementsTo: {(1 , 3)}.
	self assertCollection: (ellipse intersectionsWith: arc) hasSameElements: (ellipse intersectionsWithArc: arc)'.
