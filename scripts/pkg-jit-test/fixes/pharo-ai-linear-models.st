"pharo-ai/linear-models (with pharo-ai/lapack): three defects.

1. AILinearRegressionLeastSquaresTest>>testExactFitSingleVariable errors with
'Cannot locate any of #(liblapack.dylib)'.  LapackLibrary>>macLibraryName asks
FFIMacLibraryFinder>>findAnyLibrary:, and that method tests each candidate path
with FileReference>>exists.  Since macOS 11 the system libraries are in the
dyld shared cache and have no file on disk (macOS 27 here), so /usr/lib/liblapack.dylib
is not found, but dlopen of that path works (Accelerate: its LAPACK exports
dgelsd_ and dgesvd_, the two symbols that LapackDgelsd and LapackDgesvd call).
The same error comes from stock Cog, because the lookup is in the image.
The repair: keep the search of the finder first (Homebrew, user paths), and
when the finder finds no file, answer the system path.  If the library is truly
absent, the first call then fails in dlopen with the error of the loader.

2. and 3. AILinearRegressionLeastSquaresVanillaTest>>testExactFitSingleVariable and
testExactFitDoubleVariable start with 'self skip' (upstream commit 8bab733:
'least squares vanilla is still broken so its tests are skipped').  The
skip hides a defect of the package or of PolyMath; the tests get their real
bodies back.  The single-variable body is the one of the superclass test.  The
double-variable body is the commented-out test of the same method.  If the two
tests then fail, see the diagnostics in pharo-ai-linear-models.md."

fix value: #LapackLibrary value: #macLibraryName
	value: [ :source | (source includesSubstring: '/usr/lib/liblapack.dylib') not ]
	value:
'macLibraryName
	"The system LAPACK of macOS (Accelerate) is in the dyld shared cache: there is no file on disk, so the finder cannot see it, but dlopen of the path works."

	^ [ FFIMacLibraryFinder new
		userPaths: { ''/usr/local/opt/lapack/lib/''. ''/opt/homebrew/opt/lapack/lib/'' };
		findAnyLibrary: #(''liblapack.dylib'') ]
			on: Error
			do: [ :error | ''/usr/lib/liblapack.dylib'' ]'.

fix value: #AILinearRegressionLeastSquaresVanillaTest value: #testExactFitSingleVariable
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testExactFitSingleVariable

	| newInput expectedOutput actualOutput |
	newInput := #( #( 4 ) #( 1 ) #( 7 ) #( 0 ) ).
	expectedOutput := #( 11 5 17 3 ).

	model fitX: self fixtureInput y: self fixtureOutput.
	actualOutput := model predict: newInput.

	actualOutput with: expectedOutput do: [ :actual :expected | self assert: actual closeTo: expected precision: 0.001 ]'.

fix value: #AILinearRegressionLeastSquaresVanillaTest value: #testExactFitDoubleVariable
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testExactFitDoubleVariable

	| newInput expectedOutput actualOutput initialInput initialOutput |
	initialInput := #( #( 2 4 ) #( 3 2 ) #( 1 2 ) #( 5 1 ) #( 2 6 ) #( 6 3 ) ).
	initialOutput := initialInput collect: [ :x | 2 + (x first * 3) + x second ].
	newInput := #( #( 4 3 ) #( 1 2 ) #( 7 3 ) #( 0 4 ) ).
	expectedOutput := #( 17 7 26 6 ).

	model fitX: (PMMatrix rows: initialInput) y: initialOutput.
	actualOutput := model predict: newInput.

	actualOutput with: expectedOutput do: [ :actual :expected | self assert: actual closeTo: expected precision: 0.001 ]'.
