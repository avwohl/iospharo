"pkg_classes.st -- write the names of the test classes of a loaded package to
 <prefix>_class_names.txt, one name for each line.  run_sunit.st then runs
 only these classes.

 Environment:
   SUNIT_PREFIX   prefix of the state files of the runner
   PKG_REPO       name of the repository of the package (the directory name of
                  the clone).  Each loaded package of that Iceberg repository
                  gives its test classes.
   PKG_PREFIXES   package name prefixes, with commas between them: a test
                  class is in the list too when the name of its package starts
                  with one of the prefixes
   PKG_FIXTURES   class names, with commas between them, of fixture classes.
                  A fixture class is not in the list.  A class in a package
                  with TestResources in its name is a fixture too.  The file
                  <prefix>_fixtures.txt gets the classes that were left out.

 A test class is a non-abstract TestCase subclass.  The file
 <prefix>_packages.txt gets the package names that the repository gave,
 <prefix>_repos.txt the names of all Iceberg repositories of the image, and
 <prefix>_error.txt the error of the repository lookup, if it had one.

 The names are in the environment and not in this file, so the method
 literals of the image do not name them."
| env prefix prefixes repoName repoPackages classes fixtureNames isFixture fixtures |
env := [ :name | (OSEnvironment current at: name ifAbsent: [ '' ]) trimBoth ].
prefix := env value: 'SUNIT_PREFIX'.
prefix isEmpty ifTrue: [ prefix := '/tmp/sunit' ].
prefixes := (((env value: 'PKG_PREFIXES') substrings: ',') collect: [ :s | s trimBoth ]) reject: [ :s | s isEmpty ].
repoName := env value: 'PKG_REPO'.
repoPackages := Set new.
repoName isEmpty ifFalse: [
	[ | registry |
	  registry := (Smalltalk globals at: #IceRepository) registry.
	  (prefix , '_repos.txt') asFileReference ensureDelete; writeStreamDo: [ :o |
		registry do: [ :r | o nextPutAll: r name asString; lf ] ].
	  registry do: [ :r |
		(r name sameAs: repoName) ifTrue: [
			r workingCopy packages do: [ :p |
				p isLoaded ifTrue: [ repoPackages add: p name asString ] ] ] ] ]
		on: Error do: [ :e |
			(prefix , '_error.txt') asFileReference ensureDelete; writeStreamDo: [ :o |
				o nextPutAll: e class name; nextPutAll: ': '; nextPutAll: (e messageText ifNil: [ '' ]) asString; lf ] ] ].
(prefix , '_packages.txt') asFileReference ensureDelete; writeStreamDo: [ :o |
	repoPackages asSortedCollection do: [ :n | o nextPutAll: n; lf ] ].
classes := (TestCase allSubclasses select: [ :c |
	([ c isAbstract ] on: Error do: [ :e | true ]) not and: [ | pn |
		pn := c package name asString.
		(repoPackages includes: pn) or: [ prefixes anySatisfy: [ :pre | pn beginsWith: pre ] ] ] ])
	asSortedCollection: [ :a :b | a name <= b name ].
"Fixture classes: a test of the package runs them, and some fail on purpose."
fixtureNames := (((env value: 'PKG_FIXTURES') substrings: ',') collect: [ :s | s trimBoth ]) asSet.
isFixture := [ :c | (fixtureNames includes: c name asString)
	or: [ #('TestResources' 'TestsResources' 'Test-Resources' 'Tests-Resources') anySatisfy: [ :part | c package name asString includesSubstring: part ] ] ].
fixtures := classes select: isFixture.
classes := classes reject: isFixture.
(prefix , '_fixtures.txt') asFileReference ensureDelete; writeStreamDo: [ :o |
	fixtures do: [ :c | o nextPutAll: c name; tab; nextPutAll: c package name asString; lf ] ].
(prefix , '_class_names.txt') asFileReference ensureDelete; writeStreamDo: [ :o |
	classes do: [ :c | o nextPutAll: c name; lf ] ].
classes size
