"olekscode/MicroUML: MicroUMLCodeGeneratorTest>>setUp named the dummy package
'__MicroUML__Dummy-Package__'.  Pharo 14 PackageOrganizer>>addPackage: refuses a
package name whose first character is not an uppercase letter ('Package names
should be uppercase'), and '_' is not uppercase.  The four tests that make the
package (testGenerateClassInPackageNamed, testGenerateClassSideVariables,
testGenerateClassesInPackageNamed, testGenerateInstanceVariables) failed with
that error.  The repair starts the name with an uppercase letter and keeps the
rest, so the name stays unlikely to exist already.

testIsReadyToGenerateClass failed with 'Denial failed' as a consequence: the
failed tests had already made classes such as Object__ and Animal__ in
Smalltalk globals, and tearDown only removes the package, which did not exist.
The classes stayed, so isReadyToGenerateClass: answered true for Animal__ where
the test denies it.  With a valid package name, tearDown removes the package
together with the classes, and the test starts from a clean system.  No entry
is needed for that test."

fix value: #MicroUMLCodeGeneratorTest value: #setUp
	value: [ :source | source includesSubstring: '''__MicroUML__Dummy-Package__''' ]
	value:
'setUp

	super setUp.
	
	uml := self exampleUML.
	generator := MicroUMLCodeGenerator forDiagram: uml diagram.
	
	"We add __ to decrease the chance that a package with this names already exists.  Pharo 14 requires that a package name starts with an uppercase letter."
	packageName := ''Z_MicroUML__Dummy-Package__'''.
