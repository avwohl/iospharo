"CodeImporterCodeSnippetTest>>testCodeImporter skipped for three code snippets
(2026-11-17).  The skips are the same on stock Cog and on our VM.

C1. The test started with 'snippet source isAllSeparators ifTrue: [ ^ self
skip ]', with the remark that the code importer needs a plain expression.  An
empty chunk and a chunk of separators are legal input of a chunk file, and
CodeImporter has code for them: for the empty source it makes no declaration,
and for a method chunk with no source the compiler signals an error, which is
what the test expects for a snippet with a fault.  The skip goes, and the
checks of the test apply to the two snippets.

C2. The snippet ' ]' had the mark 'skip: #testCodeImporter' with the remark
'FIXME. code importer do not like rogue starting ]'.  The chunk format gives
']' no meaning, and the parser handles ')', ']' and '}' at the start of a
statement with the same code.  The snippets ' )' and ' }' pass the same test.
The mark goes."

fix value: #CodeImporterCodeSnippetTest value: #testCodeImporter
	value: [ :source | source includesSubstring: 'isAllSeparators ifTrue: [ ^ self skip ]' ]
	value: (((CodeImporterCodeSnippetTest >> #testCodeImporter) sourceCode
		copyReplaceAll: 'snippet source isAllSeparators ifTrue: [ ^ self skip ].' with: '')
		copyReplaceAll: '"Code importer meed a plain expression or use a custom format"'
		with: '"An empty source and a source of separators are legal chunk input. The checks below apply to them as to each other snippet."').

fix value: #'OCCodeSnippet class' value: #badExpressions
	value: [ :source | source includesSubstring: 'skip: #testCodeImporter' ]
	value: ((OCCodeSnippet class >> #badExpressions) sourceCode
		copyReplaceAll: 'skip: #testCodeImporter). "FIXME. code importer do not like rogue starting `]`"'
		with: 'yourself).').
