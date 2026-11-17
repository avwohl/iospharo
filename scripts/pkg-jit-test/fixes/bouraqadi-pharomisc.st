"PharoMisc (bouraqadi/PharoMisc, branch pharo12).  The code is for Pharo 12.
The tests that do not pass on Pharo 14 are the same on stock Cog and on our
VM.

M1. CodeMetrics (CmCodeAnalyzerTest, 5 errors: 'Attempt to read undeclared
variable' for the old organizer class).  CmCodeAnalyzer asked the old
organizer global for the category names and for the classes of a category.
Pharo 14 does not have that global.  'Smalltalk organization' answers a
PackageOrganizer, and a category is a package tag: PackageTag>>categoryName
is the package name, a hyphen and the tag name, or the package name for the
root tag.  The two methods now read the package tags and keep the match of
the category pattern.  The tests need a category with no class
(CodeMetrics-Tests-CategoryEmptyOnPurpose).  A Tonel repository cannot hold a
tag with no class, so setUp makes the tag and tearDown removes it when it is
still empty."

fix value: #CmCodeAnalyzer value: #initCategories
	value: [ :source | source includesSubstring: 'SystemOrganizer' ]
	value:
'initCategories
	"The categories are the names of the package tags (PackageTag>>categoryName).  The root tag of a package counts only when it holds a class."

	| all |
	all := OrderedCollection new.
	Smalltalk organization packages do: [ :package |
		package tags do: [ :tag |
			(tag isRoot not or: [ tag isEmpty not ])
				ifTrue: [ all add: tag categoryName asString ] ] ].
	^ categories := all select: [ :each | categoriesMatchString match: each ]'.

fix value: #CmCodeAnalyzer value: #addClassesInCategory:
	value: [ :source | source includesSubstring: 'SystemOrganizer' ]
	value:
'addClassesInCategory: aCategory

	Smalltalk organization packages do: [ :package |
		package tags do: [ :tag |
			tag categoryName asString = aCategory asString
				ifTrue: [ classes addAll: tag classes ] ] ]'.

fix value: #CmCodeAnalyzerTest value: #setUp
	value: [ :source | (source includesSubstring: 'CategoryEmptyOnPurpose') not ]
	value:
'setUp

	super setUp.
	Smalltalk organization ensureTag: #CategoryEmptyOnPurpose inPackage: #''CodeMetrics-Tests''.
	analyzer := CmCodeAnalyzer new'.

add value: #CmCodeAnalyzerTest value: #tearDown value: 'running'
	value:
'tearDown

	| package |
	package := Smalltalk organization packageNamed: #''CodeMetrics-Tests'' ifAbsent: [ nil ].
	package ifNotNil: [
		(package tagNamed: #CategoryEmptyOnPurpose ifAbsent: [ nil ]) ifNotNil: [ :tag |
			tag isEmpty ifTrue: [ package removeTag: tag ] ] ].
	super tearDown'.
