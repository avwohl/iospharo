"Koen De Hondt / hera-for-pharo.

H1. The upstream commit 6bf7551 ('Apply syntax highlighting in the script
generator') uses the classes HeraSyntaxLightTheme and HeraSyntaxDarkTheme in
HeraSettings>>syntaxTheme, HeraSettings>>syntaxLightThemeDuring: and
HeraScriptGeneratorTestCase>>initializeSubject, but the commit does not
contain the two classes (no file of the repository defines them, and the git
history has no such file).  Every HeraScriptGenerator new reads
'Hera settings syntaxTheme', so each test that makes a script, a report or a
rich script stops with UndeclaredVariableRead (86 errors, and 4 failures
that come from a run process that stopped for the same reason).  The repair
creates an abstract class HeraSyntaxTheme and the two subclasses with the
six colour messages that HeraScriptGenerator sends: tagColor, keywordColor,
stepKeywordColor, titleColor, tableColor, docStringColor.  The light colours
are the ones that the expected texts of the tests contain (116329 tags,
CF222E keywords and step keywords, 0A3069 titles, 8250DF tables, data trees
and doc strings).  The dark colours are the GitHub dark palette; no test
reads them.

H2. The tests that compare coloured text expect the light colours, but
HeraSettings>>syntaxTheme picks the theme from the brightness of the active
Pharo UI theme.  The image of the run has a DARK UI theme: 85 of the 86
errors name HeraSyntaxDarkTheme, which only the dark branch reads.  So this
entry is needed, not only a precaution.  Only HeraFeatureTestCase fixes the light theme.  A
test run on a dark UI theme would fail on colours.  HeraUnitTestCase>>performTest
now runs each test inside 'Hera settings syntaxLightThemeDuring:', the
method that the package has for this purpose."

"H1: the three classes of the syntax theme."
(Smalltalk globals includesKey: #HeraSyntaxTheme) ifFalse: [
	(Object << #HeraSyntaxTheme)
		package: 'Hera';
		tag: 'Core';
		install.
	Stdio stdout nextPutAll: 'ADDED class HeraSyntaxTheme'; lf; flush ].
(Smalltalk globals includesKey: #HeraSyntaxLightTheme) ifFalse: [
	((Smalltalk globals at: #HeraSyntaxTheme) << #HeraSyntaxLightTheme)
		package: 'Hera';
		tag: 'Core';
		install.
	Stdio stdout nextPutAll: 'ADDED class HeraSyntaxLightTheme'; lf; flush ].
(Smalltalk globals includesKey: #HeraSyntaxDarkTheme) ifFalse: [
	((Smalltalk globals at: #HeraSyntaxTheme) << #HeraSyntaxDarkTheme)
		package: 'Hera';
		tag: 'Core';
		install.
	Stdio stdout nextPutAll: 'ADDED class HeraSyntaxDarkTheme'; lf; flush ].

#( #tagColor #keywordColor #stepKeywordColor #titleColor #tableColor #docStringColor ) do: [ :each |
	add value: #HeraSyntaxTheme value: each value: 'accessing' value:
(each asString, '
	"The colour of this kind of text in a generated script."

	^ self subclassResponsibility') ].

add value: #HeraSyntaxLightTheme value: #tagColor value: 'accessing' value:
'tagColor

	^ Color fromHexString: ''116329'''.
add value: #HeraSyntaxLightTheme value: #keywordColor value: 'accessing' value:
'keywordColor

	^ Color fromHexString: ''CF222E'''.
add value: #HeraSyntaxLightTheme value: #stepKeywordColor value: 'accessing' value:
'stepKeywordColor

	^ Color fromHexString: ''CF222E'''.
add value: #HeraSyntaxLightTheme value: #titleColor value: 'accessing' value:
'titleColor

	^ Color fromHexString: ''0A3069'''.
add value: #HeraSyntaxLightTheme value: #tableColor value: 'accessing' value:
'tableColor

	^ Color fromHexString: ''8250DF'''.
add value: #HeraSyntaxLightTheme value: #docStringColor value: 'accessing' value:
'docStringColor

	^ Color fromHexString: ''8250DF'''.

add value: #HeraSyntaxDarkTheme value: #tagColor value: 'accessing' value:
'tagColor

	^ Color fromHexString: ''7EE787'''.
add value: #HeraSyntaxDarkTheme value: #keywordColor value: 'accessing' value:
'keywordColor

	^ Color fromHexString: ''FF7B72'''.
add value: #HeraSyntaxDarkTheme value: #stepKeywordColor value: 'accessing' value:
'stepKeywordColor

	^ Color fromHexString: ''FF7B72'''.
add value: #HeraSyntaxDarkTheme value: #titleColor value: 'accessing' value:
'titleColor

	^ Color fromHexString: ''A5D6FF'''.
add value: #HeraSyntaxDarkTheme value: #tableColor value: 'accessing' value:
'tableColor

	^ Color fromHexString: ''D2A8FF'''.
add value: #HeraSyntaxDarkTheme value: #docStringColor value: 'accessing' value:
'docStringColor

	^ Color fromHexString: ''D2A8FF'''.

"H2: each Hera unit test runs with the light syntax theme."
add value: #HeraUnitTestCase value: #performTest value: 'running' value:
'performTest
	"The expected texts of the tests have the colours of the light syntax theme."

	Hera settings syntaxLightThemeDuring: [ super performTest ]'.
