"XML-XMLParser (pharo-contributions/XML-XMLParser, master): 110 tests skip on
stock Cog and on our VM.  XMLSkippableTest has a class-side flag isSkipping
that is true when nobody sets it, and each test of the class-side list
skippableTests then calls skip.

X1. XMLFileHandleTest and its 5 encoding subclasses (90 tests) make and delete
files in the working directory.  They need no network.  The flag is a
class-side instance variable, so the image save keeps the value.

X2. XMLExternalEntityTest (16 tests): 7 tests use files in the working
directory, and 9 tests get documents and DTDs with HTTP.  The class comment
says how to turn the tests on (stopSkipping).

X3. XMLConformanceExpatTest>>testnotwfsa140xml and testnotwfsa141xml expect a
parser error for a name with U+309A and for a name with U+0E5C.  The two files
are not well-formed in XML 1.0 edition 4.  The parser has the name characters
of edition 5 (Character>>isXMLNameStartChar: the ranges #x37F-#x1FFF and
#x3001-#xD7FF), and the two documents are well-formed there.  The tests now
expect no parser error.

X5. XMLConformanceSunTest>>testinvalidutf16bxml and testinvalidutf16lxml
were in the list of the tests to skip, because they fail.  The two files of
the test suite (sun/invalid/utf16b.xml and utf16l.xml) are documents with no
DTD: <?xml version='1.0' encoding='UTF-16'?> and <root/>.  The suite
lists them as invalid, which means that a parser that validates must refuse
them.  The parser of the conformance tests validates, and does not require a
DTD (SAXHandler>>requiresSchema: is off when nobody sets it), so it raised no
error.  The two tests now set requiresSchema: true, and the parser then
refuses a document with no DTD."

XMLFileHandleTest stopSkippingAll.
XMLExternalEntityTest stopSkipping.
Stdio stdout nextPutAll: 'XML tests that skip: ';
	print: ((XMLFileHandleTest withAllSubclasses , { XMLExternalEntityTest }) select: [ :c | c isSkipping ]);
	lf; flush.

fix value: #XMLConformanceExpatTest value: #testnotwfsa140xml
	value: [ :source | source includesSubstring: 'should:' ]
	value:
'testnotwfsa140xml
	"Source file: xmltest/not-wf/sa/140.xml.  Not well-formed in XML 1.0 edition 4.  Well-formed in edition 5: U+309A is a name start character (#x3001-#xD7FF)."

	self
		shouldnt: [
			self
				parse: self class sourcenotwfsa140xml
				documentURI: ''not-wf/sa/140.xml'']
		raise: self parserExceptions'.

fix value: #XMLConformanceExpatTest value: #testnotwfsa141xml
	value: [ :source | source includesSubstring: 'should:' ]
	value:
'testnotwfsa141xml
	"Source file: xmltest/not-wf/sa/141.xml.  Not well-formed in XML 1.0 edition 4.  Well-formed in edition 5: U+0E5C is a name start character (#x37F-#x1FFF)."

	self
		shouldnt: [
			self
				parse: self class sourcenotwfsa141xml
				documentURI: ''not-wf/sa/141.xml'']
		raise: self parserExceptions'.

"X4. Two test documents name a DTD address that answers 404 Not Found
(checked 2026-11-17).  The tests that read them failed on stock Cog and on
our VM: testParsingDocBookArticle (assertion), testParsingExternalEntities
and testParsingExternalEntitiesLimit (XMLHTTPException).
- http://docbook.org/xml/4.5/docbookx.dtd: the system identifier of DocBook
  XML V4.5 is http://www.oasis-open.org/docbook/xml/4.5/docbookx.dtd, and
  that address answers.
- http://www.musicxml.org/dtds/partwise.dtd: the W3C Music Notation group
  has the DTD files of MusicXML in its repository."
fix value: #XMLExternalEntityTest value: #docBookArticleSource
	value: [ :source | source includesSubstring: 'http://docbook.org/xml/4.5/docbookx.dtd' ]
	value: ((XMLExternalEntityTest >> #docBookArticleSource) sourceCode
		copyReplaceAll: 'http://docbook.org/xml/4.5/docbookx.dtd'
		with: 'http://www.oasis-open.org/docbook/xml/4.5/docbookx.dtd').

fix value: #XMLExternalEntityTest value: #musicXMLDocument
	value: [ :source | source includesSubstring: 'http://www.musicxml.org/dtds/partwise.dtd' ]
	value: ((XMLExternalEntityTest >> #musicXMLDocument) sourceCode
		copyReplaceAll: 'http://www.musicxml.org/dtds/partwise.dtd'
		with: 'https://raw.githubusercontent.com/w3c/musicxml/gh-pages/schema/partwise.dtd').

#( #( #testinvalidutf16bxml 'utf16b' ) #( #testinvalidutf16lxml 'utf16l' ) ) do: [ :pair |
	fix value: #XMLConformanceSunTest value: pair first
		value: [ :source | source includesSubstring: 'ifSkippable:' ]
		value: pair first , '
	"Source file: sun/invalid/' , pair second , '.xml.  The document has no DTD.  The test suite lists it as invalid: a parser that validates and requires a DTD must refuse it."

	self
		should: [
			(self
				parserClass: self domParserClass
				on: self class sourceinvalid' , pair second , 'xml
				documentURI: ''invalid/' , pair second , '.xml'')
					requiresSchema: true;
					parseDocument]
		raise: self parserExceptions' ].
