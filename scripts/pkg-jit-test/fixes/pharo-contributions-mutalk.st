"MuTalk (pharo-contributions/mutalk): two tests run a mutant that is a
recursion with no end (MTAnalysisTest>>testExecutingInfiniteRecursionAnalysis
and MTMethodMutationTest>>testMutationInfiniteRecursion).  The mutant runs in
the fixture test MTFakeInfiniteLoopsTest>>testRecursiveFactorial, and only the
SUnit time limit of that fixture test stops it (10 s).

On stock Cog a recursion has no depth limit: the frames become heap Contexts
at about 1 GB each second on this machine.  With no old-space limit, stock
Cog used 13 GB before the time limit (the memory guard of vmrun.sh stopped
the VM).  With the 6144 MB limit of the package run, stock Cog stops with
'Could not allocate new object in the old space' and prints the stack of the
process, some million lines.

Our VM passes the two tests in 25 ms for another cause: it signals an error at
a depth of 56000 frames (docs/vm-compat-bugs.md, open item of 2026-11-17).

The repair in the package: the fixture test gets a time limit of 1 second.
The mutant still ends through the time limit, which is the thing that the two
tests check, and the recursion uses about 1 GB."

add value: #MTFakeInfiniteLoopsTest value: #defaultTimeLimit value: 'accessing'
	value:
'defaultTimeLimit
	"A mutant of the two methods under test is a loop or a recursion with no end.  Only this limit stops the mutant, and a recursion fills about 1 GB of memory each second."

	^ 1 second'.

"MuTalk-CI: MTCITonelJsonExporterTest>>testWorking starts with 'self skip'
since 2024-06-27 (commit f0cc02b of mutalk, 'Skip test that has a lots of
strong dependencies').  The test needs the Iceberg repository of mutalk and
the files of its clone.  The package run has the two, so the skip goes and
the body stays.

The code under test names RPackageOrganizer, which Pharo 14 does not have
(PackageOrganizer took its place)."

fix value: #MTCITonelJsonExporter value: #getPathOfClass:
	value: [ :source | source includesSubstring: 'RPackageOrganizer' ]
	value: ((MTCITonelJsonExporter >> #getPathOfClass:) sourceCode
		copyReplaceAll: 'RPackageOrganizer' with: 'PackageOrganizer').

fix value: #MTCITonelJsonExporterTest value: #testWorking
	value: [ :source | source includesSubstring: 'self skip.' ]
	value: ((MTCITonelJsonExporterTest >> #testWorking) sourceCode
		copyReplaceAll: 'self skip.' with: '').
