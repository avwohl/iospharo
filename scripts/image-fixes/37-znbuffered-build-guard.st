"37-znbuffered-build-guard.st -- two Zinc stream tests skip in each Pharo 15 build
 before build 690 (2026-11-20).

 ZnBufferedReadWriteStreamTest>>testReadThenWrite and >>testWriteThenRead start with
     ((SystemVersion current major < 7) or: [ SystemVersion current build < 690 ])
         ifTrue: [ ^ self skip ].
 The guard means 'older than Pharo 7.0 build 690'.  The build number starts again at
 1 for each Pharo version (Pharo 15.0 build 141 on 2026-11-20), so the second test of
 the guard skips the two tests on Pharo 15 although the code under test is there.
 Pharo 14 build 791 runs the tests and passes them.  The repair compares the build
 only for Pharo 7: the tests skip for a major version below 7, and for Pharo 7 before
 build 690.  The assertions do not change.
 Measured on Pharo 15 build 141: the two tests skip on stock Cog v12.0.6 and on our VM."

[ :oldGuard :newGuard |
	#( #testReadThenWrite #testWriteThenRead ) do: [ :sel |
		fix value: #ZnBufferedReadWriteStreamTest value: sel
			value: [ :src | src includesSubstring: oldGuard ]
			value: (((Smalltalk at: #ZnBufferedReadWriteStreamTest) >> sel) sourceCode
				copyReplaceAll: oldGuard with: newGuard) ] ]
	value: '((SystemVersion current major < 7) or: [ SystemVersion current build < 690 ])'
	value: '((SystemVersion current major < 7) or: [ SystemVersion current major = 7 and: [ SystemVersion current build < 690 ] ])'.
