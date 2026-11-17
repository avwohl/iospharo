"pillar-markup-pillar: the integration tests (PRIntegrationTest) fail with
'Archetype <name> does not exist' for four tests, for two causes.

1. PRArchetype>>directory looks for the archetype in <image directory>/archetypes/<name>.
   The repository has the archetypes in archetypes/ at its root (smalltalkCI copies
   the folder next to the image; the package run does not).  The run directory of
   the package run has the clone of the repository in pharo-local/iceberg/pillar-markup/pillar,
   so the entry makes PRArchetype>>directory use the archetype of the clone when
   the image directory has none.

2. The tests name archetypes that the repository removed: 'basic-latex' became
   'basic-latex-report' (commit 733174e9), the Pillar-format archetype 'book' was
   removed (commit c4bfe878; the book archetype now is bod-book-latex2026, the newest
   of the three bod-book-latex archetypes), and 'pillar-presentation' was removed
   (commit 905a07d9, with no replacement for an HTML presentation; 'templated' is
   the nearest archetype that has HTML templates, a RUN decides whether it builds).
   The three entries put the names of the archetypes that exist.

The five PDF/EPub tests that skip are NOT changed: see the .md (no latexmk on this
machine; PREPubOutput is not in the baseline and PRXHTMLWriter has no definition).

The 20 errors of PRHtmlBookOutputTest (undeclared variable MicPathRelativizer) are
repaired by patches/pillar-markup-pillar.patch (Microdown from branch dev), not here."

fix value: #PRArchetype value: #directory
	value: [ :source | (source includesSubstring: 'pharo-local') not ]
	value:
'directory
	^ directory ifNil: [
		| inImage inClone |
		inImage := FileLocator imageDirectory / ''archetypes'' / name.
		inImage exists
			ifTrue: [ inImage ]
			ifFalse: [
				inClone := FileLocator imageDirectory / ''pharo-local'' / ''iceberg''
					/ ''pillar-markup'' / ''pillar'' / ''archetypes'' / name.
				inClone exists ifTrue: [ inClone ] ifFalse: [ inImage ] ] ]'.

fix value: #PRIntegrationTest value: #archetypeBasicLatex
	value: [ :source | source includesSubstring: '''basic-latex''' ]
	value:
'archetypeBasicLatex

	^ ''basic-latex-report'''.

fix value: #PRIntegrationTest value: #archetypeBook
	value: [ :source | source includesSubstring: '''book''' ]
	value:
'archetypeBook

	^ ''bod-book-latex2026'''.

fix value: #PRIntegrationTest value: #archetypePresentation
	value: [ :source | source includesSubstring: '''pillar-presentation''' ]
	value:
'archetypePresentation

	^ ''templated'''.
