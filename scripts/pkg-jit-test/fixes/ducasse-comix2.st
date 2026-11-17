"Comix2 (Ducasse/Comix2, master): three tests that do not pass on stock Cog
and on our VM.  The causes are in the package, not in a VM.  See
docs/upstream/package-repairs.md for the evidence.

1. CXXMLItemSaverLoaderTest>>testVisitorXML has the date of the day on which
   the author wrote the test ('24 January 2026') in the expected string.
   CXVisitor>>visitBase: writes Date today.  The expected string now uses
   Date today asString, as the code under test does.

2. CXXMLImporter>>import: makes a collection, never adds an item to the
   collection, and answers the empty collection.  The test
   CXXMLItemSaverLoaderTest>>testXMLImporter expects 2 items.  The repair
   is in the class (the defect), not in the test.

3. CBXMLImporterTest>>testImportRealCollection needs the file BD.362.xml
   (the private collection of the author, 3352 albums) in the image directory.
   The file is not in the repository and never was.  The test now writes a
   generated file in the historical format to a memory file system and imports
   the file by importFile:, so the test runs on each machine."

fix value: #CXXMLItemSaverLoaderTest value: #testVisitorXML
	value: [ :source | (source includesSubstring: 'String cr') not ]
	value:
'testVisitorXML

	| vis base today cr |
	vis := CXVisitor new. 
	base := CXBase new.
	base addSerie: CXItem nic serie. 
	today := Date today asString.
	cr := String cr.
	base accept: vis. 
	self 
		assert: vis contents
		equals: ''<?xml version="1.0" encoding="UTF-8"?>'', cr,
			''<Base date="'', today, ''" entriesNumber="2" note="nil">'', cr,
			''<bd authors="Larcenet" editor="Dargaud" entryCreationDate="25 March 2006" entryNumber="1" number="1" original="true" serie="Serie  Nic oumouk" serieComplete="false" sortedAs="1" title="Total souk pour nic oumouk" year="2005"/>'', cr,
			''<bd authors="Larcenet" editor="Dargaud" entryCreationDate="25 March 2007" entryNumber="1809" number="2" original="true" serie="Serie  Nic oumouk" serieComplete="false" sortedAs="2" title="La france a peur" year="2007"/></Base>''
'.

fix value: #CXXMLImporter value: #import:
	value: [ :source | (source includesSubstring: 'col add:') not ]
	value:
'import: aString
	"self new importFileName: ''BD.322.xml''"

	| p col |
	col := OrderedCollection new.
	p := XMLDOMParser on: aString.
	p parseDocument elements first elements do: [ :each |
			col add: (self importFromXMLEntry: each) ].
	^ col
'.

fix value: #CBXMLImporterTest value: #testImportRealCollection
	value: [ :source | source includesSubstring: 'BD.362.xml' ]
	value:
'testImportRealCollection
	"The collection of the author (3352 albums) is not in the repository.
	The test writes a file in the historical BD.xxx.xml format with generated data
	to a memory file system, and imports the file with importFile:."

	| file xml seriesCount albumsPerSeries |
	seriesCount := 50.
	albumsPerSeries := 20.
	xml := String streamContents: [ :s |
		s nextPutAll: ''<?xml version="1.0" encoding="UTF-8"?>''; cr.
		s nextPutAll: ''<Base date="31 October 2010" entriesNumber="''; print: seriesCount * albumsPerSeries; nextPutAll: ''" note="nil">''; cr.
		1 to: seriesCount do: [ :series |
			1 to: albumsPerSeries do: [ :number |
				s
					nextPutAll: ''<bd serieComplete="false" editor="Publisher '';
					print: series \\ 7;
					nextPutAll: ''" entryCreationDate="5 May 2006" entryNumber="'';
					print: (series - 1) * albumsPerSeries + number;
					nextPutAll: ''" authors="Author'';
					print: series;
					nextPutAll: '' Writer'';
					print: series;
					nextPutAll: ''" title="Title '';
					print: number;
					nextPutAll: ''" serie="Series '';
					print: series;
					nextPutAll: ''" original="true" number="'';
					print: number;
					nextPutAll: ''" year="'';
					print: 1950 + number;
					nextPutAll: ''" />''; cr ] ].
		s nextPutAll: ''</Base>'' ].
	file := FileSystem memory root / ''BD.generated.xml''.
	file writeStreamEncoded: ''utf-8'' do: [ :stream | stream nextPutAll: xml ].
	importer importFile: file.
	self assert: library owned size equals: seriesCount * albumsPerSeries.
	self assert: library allSeries size equals: seriesCount.
	self assert: (library owned albums allSatisfy: [ :each | each series albums includes: each ])
'.
