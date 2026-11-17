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

Not repaired here: XMLConformanceSunTest>>testinvalidutf16bxml and
testinvalidutf16lxml.  The tests hold the two files as strings, and the defect
of the files is in their bytes."

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
