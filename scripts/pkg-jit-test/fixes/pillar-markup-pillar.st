"pillar-markup-pillar: repairs for the tests that do not pass (PRIntegrationTest).
The record of the repair: docs/upstream/package-repairs.md

The 20 errors of PRHtmlBookOutputTest (undeclared variable MicPathRelativizer)
and the 4 errors 'PRHtmlDocument>>#basicWriter not found' of PRIntegrationTest
have one cause, and this file does not repair that cause.  Pillar dev uses
classes that only the dev branch of Microdown has (MicPathRelativizer,
MicHeaderIdentifier, MicChapterSplitter, MicPageReferenceResolver, and
MicBookHTMLVisitor, the writer named michtml).  The baseline of Pillar asked
for Microdown v2.11.2, which has none of them (patches/pillar-markup-pillar.patch
asks for the dev branch).  Also, the Pharo 14 image already has a class
BaselineOfMicrodown, and Metacello does not load a baseline whose class is in the
image: it used the old baseline of the image and its package list (no
Microdown-Toc) whatever the repository said.  The load expression of the
manifest row has the option ignoreImage, which makes Metacello load the
baseline from the repository.

Cause of each entry, in plain words:

A. PRIntegrationTest, 4 tests with 'Archetype <name> does not exist'
   (testWelcomeHtml, testBasicLatexHtml, testBookHtml, testPresentationHtml).
   Two causes.
   1. PRArchetype>>directory looks for the archetype only in the folder
      'archetypes' beside the image.  The repository keeps the archetypes in
      its own folder 'archetypes'; the build server of the package (smalltalkCI)
      copies that folder next to the image, and the package run does not.  The
      entry uses the folder of the repository (the Iceberg repository named
      pillar, a clone of the package) when the folder beside the image has no
      archetype of that name.  The folder beside the image still has priority.
   2. Three tests name archetypes that the repository removed: 'basic-latex'
      became 'basic-latex-report', the archetype 'book' (Pillar syntax) was
      removed and the book archetype now is 'bod-book-latex2026' (the newest
      of three), and 'pillar-presentation' was removed with no HTML
      presentation to replace it.  The test names are wrong, not the code.  The
      three entries give the name of an archetype that exists.  For the
      presentation the nearest archetype is 'templated' (it has HTML
      templates of its own); the owner of the package should decide whether
      testPresentationHtml and testPresentationPdf stay.

B. PRIntegrationTest>>testBookEpub (a skip: 'self skip', 'bug in P8 with
   ZipArchive').  The test also sends #outputEPub, which the class never had,
   and the package Pillar-ExporterEPub is not in the baseline: its classes
   write the document model of Pillar syntax (superclasses PRHTMLWriter and
   PRDocumentWriter and the writer PRXHTMLWriter have no definition any
   more), so the code cannot run.  The feature is missing, so the entries write
   a small exporter of the same names, in the style of PRHtmlOutput and
   PRHtmlDocument: PREPubOutput (the target epub) and PREPubDocument, which
   writes the book with the HTML writer of pillar.conf, puts the HTML in the
   epub template of the project, and zips the files of an EPUB 3 file
   (mimetype first and not compressed, META-INF/container.xml, contents.opf,
   navigation.xhtml, contents.xhtml, and stylesheet.css when the project
   has one).  The skip is removed from the test and the test gets #outputEPub.
   The assertions of the test are not changed.  Limit: the contents are the
   HTML of the HTML writer, which is not always well-formed XML, and the files of
   the figures are not in the zip.

The four PDF tests (testBasicLatexPdf, testBookPdf, testPresentationPdf,
testWelcomePdf) stay skips: PRPDFDocument runs latexmk with lualatex, and the
machine has neither (no latexmk, lualatex, pdflatex or xelatex on the PATH, no
/Library/TeX).  The test cannot apply there."

fix value: #PRArchetype value: #directory
	value: [ :source | (source includesSubstring: 'IceRepository') not ]
	value:
'directory
	^ directory ifNil: [
		| inImage inRepository |
		inImage := FileLocator imageDirectory / ''archetypes'' / name.
		inImage exists
			ifTrue: [ inImage ]
			ifFalse: [
				inRepository := (IceRepository repositoryNamed: ''pillar'')
					ifNotNil: [ :repository | repository location / ''archetypes'' / name ].
				(inRepository notNil and: [ inRepository exists ])
					ifTrue: [ inRepository ]
					ifFalse: [ inImage ] ] ]'.

fix value: #PRIntegrationTest value: #archetypeBasicLatex
	value: [ :source | source includesSubstring: '''basic-latex''' ]
	value:
'archetypeBasicLatex

	^ ''basic-latex-report'''.

fix value: #PRIntegrationTest value: #archetypeBook
	value: [ :source | source includesSubstring: '''book''' ]
	value:
'archetypeBook

	^ ''bod-book-latex2026'''.

fix value: #PRIntegrationTest value: #archetypePresentation
	value: [ :source | source includesSubstring: '''pillar-presentation''' ]
	value:
'archetypePresentation

	^ ''templated'''.

(Smalltalk globals includesKey: #PREPubOutput) ifFalse: [
	(Smalltalk globals at: #PRHtmlOutput ifAbsent: [ nil ])
		ifNil: [ Stdio stdout nextPutAll: 'SKIP PREPubOutput (PRHtmlOutput absent)'; lf; flush ]
		ifNotNil: [ :superclass |
			(superclass << #PREPubOutput) package: 'Pillar-ExporterEPub'; install.
			Stdio stdout nextPutAll: 'ADDED class PREPubOutput'; lf; flush ] ].

(Smalltalk globals includesKey: #PREPubDocument) ifFalse: [
	(Smalltalk globals at: #PRHtmlDocument ifAbsent: [ nil ])
		ifNil: [ Stdio stdout nextPutAll: 'SKIP PREPubDocument (PRHtmlDocument absent)'; lf; flush ]
		ifNotNil: [ :superclass |
			(superclass << #PREPubDocument) package: 'Pillar-ExporterEPub'; install.
			Stdio stdout nextPutAll: 'ADDED class PREPubDocument'; lf; flush ] ].

add value: #'PREPubOutput class' value: #builderName value: 'accessing'
	value:
'builderName

	^ #epub'.

add value: #PREPubOutput value: #documentFor: value: 'building'
	value:
'documentFor: aFile

	^ PREPubDocument new
		project: aFile project;
		file: aFile;
		target: self;
		outputDirectory: aFile project outputDirectory / self extension;
		yourself'.

add value: #PREPubOutput value: #extension value: 'accessing'
	value:
'extension

	^ ''epub'''.

add value: #PREPubOutput value: #outputDirectoryName value: 'accessing'
	value:
'outputDirectoryName

	^ ''epub'''.

(Smalltalk globals at: #PREPubDocument ifAbsent: [ nil ]) ifNotNil: [ :class |
	class comment:
'I write a book as an EPUB 3 file (index.epub): the zip archive of the mimetype (first, not compressed), META-INF/container.xml, contents.opf, navigation.xhtml, contents.xhtml, and stylesheet.css when the project has _support/epub/stylesheet.css. The contents are what the HTML writer of pillar.conf (htmlWriter) writes, in the template _support/templates/epub/xhtml.mustache; the package file is the template _support/templates/epub/contents.opf.mustache. The archetypes of Pillar have these templates. Limits: the HTML of the writer is not always well-formed XML, and the files of the figures are not in the archive.' ].

add value: #PREPubDocument value: #extension value: 'accessing'
	value:
'extension

	^ ''epub'''.

add value: #PREPubDocument value: #epubTemplateNamed: value: 'templating'
	value:
'epubTemplateNamed: aString
	"The file of a template of the EPUB files, in the support folder of the project."

	^ project baseDirectory / ''_support'' / ''templates'' / ''epub'' / aString'.

add value: #PREPubDocument value: #stylesheetFile value: 'writing'
	value:
'stylesheetFile
	"The style sheet of the project for the EPUB file, or nil."

	| stylesheet |
	stylesheet := project baseDirectory / ''_support'' / ''epub'' / ''stylesheet.css''.
	^ stylesheet exists
		  ifTrue: [ stylesheet ]
		  ifFalse: [ nil ]'.

add value: #PREPubDocument value: #escapedXml: value: 'writing'
	value:
'escapedXml: aString
	"aString as text of an XML element or of an attribute."

	^ ((aString asString copyReplaceAll: ''&'' with: ''&amp;'')
		   copyReplaceAll: ''<'' with: ''&lt;'')
		  copyReplaceAll: ''>'' with: ''&gt;'''.

add value: #PREPubDocument value: #containerFile value: 'writing'
	value:
'containerFile
	"META-INF/container.xml: it names the package file."

	^ ''<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="contents.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>'''.

add value: #PREPubDocument value: #packageFileFor: value: 'writing'
	value:
'packageFileFor: aTemplateEnvironment
	"contents.opf: the metadata of the book, the files of the archive and their order."

	| manifest values |
	manifest := ''<item id="navigation" properties="nav" href="navigation.xhtml" media-type="application/xhtml+xml"/>
    <item id="contents" href="contents.xhtml" media-type="application/xhtml+xml"/>''.
	self stylesheetFile ifNotNil: [
		manifest := manifest , ''
    <item id="style" href="stylesheet.css" media-type="text/css"/>'' ].
	values := Dictionary new
		          at: ''epub-id'' put: ''urn:uuid:'' , UUID new asString;
		          at: ''title''
		          put: (self escapedXml: (aTemplateEnvironment propertyAt: #title ifAbsent: [ '''' ]));
		          at: ''language''
		          put: (self escapedXml: (aTemplateEnvironment propertyAt: #language ifAbsent: [ ''en'' ]));
		          at: ''attribution''
		          put: (self escapedXml: (aTemplateEnvironment propertyAt: #attribution ifAbsent: [ '''' ]));
		          at: ''rights''
		          put: (self escapedXml: (aTemplateEnvironment propertyAt: #rights ifAbsent: [ '''' ]));
		          at: ''manifestItems'' put: manifest;
		          at: ''spineItems''
		          put: ''<itemref idref="navigation" linear="yes"/>
    <itemref idref="contents" linear="yes"/>'';
		          yourself.
	^ (MustacheTemplate on: (self epubTemplateNamed: ''contents.opf.mustache'') contents) value: values'.

add value: #PREPubDocument value: #navigationFileFor: value: 'writing'
	value:
'navigationFileFor: aTemplateEnvironment
	"navigation.xhtml: the table of contents that EPUB 3 requires, with one entry, the book."

	| values |
	values := Dictionary new
		          at: ''title'' put: (aTemplateEnvironment propertyAt: #title ifAbsent: [ '''' ]) asString;
		          yourself.
	^ (MustacheTemplate on: ''<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
  <head><title>{{title}}</title></head>
  <body>
    <nav epub:type="toc">
      <ol><li><a href="contents.xhtml">{{title}}</a></li></ol>
    </nav>
  </body>
</html>'') value: values'.

add value: #PREPubDocument value: #contentsFileFor: value: 'writing'
	value:
'contentsFileFor: aTemplateEnvironment
	"contents.xhtml: the book, in the xhtml template of the project."

	^ (MustacheTemplate on: (self epubTemplateNamed: ''xhtml.mustache'') contents) value: aTemplateEnvironment'.

add value: #PREPubDocument value: #writeDocument: value: 'building'
	value:
'writeDocument: aDocument
	"Write the book as an EPUB 3 file and answer the file.  The mimetype is the first member of the archive and is not compressed, as the EPUB specification asks."

	| values archive outputFile |
	values := self getMustacheTemplateValuesFromDocument: aDocument.
	archive := ZipArchive new.
	(archive addString: ''application/epub+zip'' asByteArray as: ''mimetype'')
		desiredCompressionMethod: ZipArchive compressionStored.
	archive addString: self containerFile utf8Encoded as: ''META-INF/container.xml''.
	archive addString: (self packageFileFor: values) utf8Encoded as: ''contents.opf''.
	archive addString: (self navigationFileFor: values) utf8Encoded as: ''navigation.xhtml''.
	archive addString: (self contentsFileFor: values) utf8Encoded as: ''contents.xhtml''.
	self stylesheetFile ifNotNil: [ :stylesheet |
		archive addString: stylesheet contents utf8Encoded as: ''stylesheet.css'' ].
	outputFile := (self outputDirectory resolve: (file file asAbsolute relativeTo: project baseDirectory asAbsolute)) withoutExtension , self extension.
	outputFile ensureDelete.
	outputFile parent ensureCreateDirectory.
	outputFile binaryWriteStreamDo: [ :stream | archive writeTo: stream ].
	^ outputFile'.

add value: #PRIntegrationTest value: #outputEPub value: 'parameters'
	value:
'outputEPub

	^ PREPubOutput new'.

fix value: #PRIntegrationTest value: #testBookEpub
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testBookEpub
	| archive |
	self testArchetype: self archetypeBook output: self outputEPub.
	self assert: (project outputDirectory / ''epub'' / ''index.epub'') exists.

	archive := ZipArchive new.
	archive readFrom: (project outputDirectory / ''epub'' / ''index.epub'') fullName.

	self assert: archive members first fileName equals: ''mimetype''.
	self assert: (archive memberNamed: ''contents.opf'') isNotNil.
	self assert: (archive memberNamed: ''contents.xhtml'') isNotNil'.
