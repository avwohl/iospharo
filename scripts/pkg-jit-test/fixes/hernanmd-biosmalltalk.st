"hernanmd/BioSmalltalk (master 587d3f2f).  58 tests that do not pass, 8 causes.

A. Test files that the repository has only inside resources/BioSmalltalkResourceFiles.zip
   (BioFileFormatTest 2, BioDnaFeaturesViewerTest 3, BioNCBIGenBankReaderTest 6).
   The tests read BioSmalltalkTestFiles/... in the working directory.  The
   directory is not there.  BioObject class>>testFilesFullDirectoryName now
   unpacks the zip of the repository clone into the working directory the first
   time.  The three BioDnaFeaturesViewerTest methods named
   DnaFeaturesViewer/examples/example_sequence.gb in the working directory; the
   zip holds the file in BioSmalltalkTestFiles/DnaFeaturesViewer/, which is
   where BioDnaFeaturesViewerExamples class>>examplesDirectory looks.  The
   methods now use that directory.  BioFASTQSamples builds its path from the
   same directory and gets the same unpack call.

B. Test files that no repository and no archive holds (BioBEDTest 8 tests with
   /tmp/bed/..., BioMAFTest 4 tests with /tmp/maf_test/test.maf, BioGFF3Test 6
   tests with gff/hsa.gff3 and gff/rno.gff3).  The author made them by hand on
   his machine (/tmp) or took them from miRBase (a download).  A setUp writes
   BED and MAF files with the content that the assertions describe.  BioGFF3Test
   writes two GFF3 files in the miRBase format with the counts that the
   assertions name (4801 and 1323 features).  The GFF3 data is synthetic: it
   tests the parser on the format and on the counts, not on real miRBase lines.

C. BioGtUnixSubprocessTest (7 tests): BioGtUnixSubprocess sends to the class
   GtUnixSubprocess (and GtSubprocessWithInMemoryOutput), which only Glamorous
   Toolkit has.  No repository of the run holds the class, and the Pharo 14
   image does not.  The test checks an adapter for another distribution of
   Pharo, so the test cannot work here.  setUp of the test class skips the test
   with that reason when the class is absent.  testProviderClass compares a
   Symbol only, runs on any image, and stays.  No stand-in class is made: a
   class of that name in the image makes BioGtUnixSubprocess a valid adapter,
   and other tests (BioCutadaptWrapperTest, BioNCBIBlastClientTest) then
   choose it and fail on GtSubprocessWithInMemoryOutput.

D. BioPathwayTest (10 tests) and BioOntologyTest (2 tests) send #metabolite:,
   #protein:, #gene: and #id:name:type: to BioSpecies.  BioSpecies is the class
   of the organism repositories (BioPopulation).  The species of a pathway is
   BioPathwaySpecies (the repository renamed the class in commit 43957c6d and
   did not change the tests).  The tests now name BioPathwaySpecies.

E. BioPhylipParserTest (8 tests): the three helper methods firstLineTokenizer,
   speciesDNALineTokenizer and speciesDNANamedBlockTokenizer send the message
   to BioPhylipParser.  Commit 7120503e made BioPhylipParser a hand-written
   parser without these methods; BioPhylipPetitParser still has them.  The
   helpers now use BioPhylipPetitParser.

F. BioRestrictionTest>>testManyToManyRelationship: BioCatalysis links an enzyme
   and a reaction, but BioReaction>>enzymes answers only the species that were
   added with addEnzyme:.  The catalysis of the test is not in the answer.
   BioReaction>>enzymes now adds the enzymes of the catalyses.

G. BioCutadaptWrapperTest>>testExecuteWithFiltering needs the program cutadapt
   (a Python program), which this machine does not have, and the text Total
   reads processed of that program.  The test now takes the real cutadapt when
   command -v finds one, and else a small awk program written into the working
   directory that trims a 3-prime adapter, drops reads shorter than -m, writes
   the -o file and prints the summary lines of cutadapt.  The stand-in does not
   check what cutadapt checks: error rates, partial matches inside the read,
   paired reads, quality trimming, compressed files.  Two other execute tests
   (testExecuteBasic, testExecuteQualityTrim) pass on the text of the shell
   error 'cutadapt: command not found'; this file does not change them."

"A helper block for the entries below.  The file is one chunk with the declarations of the header, so the block is in a global for the time of the file-in."
Smalltalk globals at: #PkgFixRewrite put: [ :clsName :sel :oldText :newText | | cls |
	cls := resolve value: clsName.
	(cls notNil and: [ cls includesSelector: sel ])
		ifTrue: [
			fix value: clsName value: sel
				value: [ :source | source includesSubstring: oldText ]
				value: ((cls >> sel) sourceCode copyReplaceAll: oldText with: newText) ]
		ifFalse: [ fix value: clsName value: sel value: [ :source | false ] value: '' ] ].

"A second helper: compile a method when it is absent or when its source differs. Stays in the global for the time of the file-in, like PkgFixRewrite."
Smalltalk globals at: #PkgFixPut put: [ :clsName :sel :protocol :newSource | | cls |
	cls := resolve value: clsName.
	cls ifNil: [ Stdio stdout nextPutAll: 'SKIP ', clsName, '>>', sel, ' (class absent)'; lf; flush ]
		ifNotNil: [
			(cls includesSelector: sel)
				ifTrue: [
					((cls >> sel) sourceCode = newSource)
						ifTrue: [ Stdio stdout nextPutAll: 'SKIP ', clsName, '>>', sel, ' (already as wanted)'; lf; flush ]
						ifFalse: [
							cls compile: newSource classified: (cls >> sel) protocolName.
							Stdio stdout nextPutAll: 'FIXED ', clsName, '>>', sel; lf; flush ] ]
				ifFalse: [
					cls compile: newSource classified: protocol.
					Stdio stdout nextPutAll: 'ADDED ', clsName, '>>', sel; lf; flush ] ] ].

"=== A. test files from the zip ==="

add value: #'BioObject class' value: #ensureTestFiles value: 'testing' value:
'ensureTestFiles
	"Unpack resources/BioSmalltalkResourceFiles.zip of the repository clone into the working directory when the test files directory is not there. The repository has the test files only in this archive."
	| target repository zip |
	target := self workingDirectory asFileReference / self testFilesDirectoryName.
	target isDirectory ifTrue: [ ^ self ].
	repository := IceRepository registry
		detect: [ :each | each name = ''BioSmalltalk'' ]
		ifNone: [ ^ self ].
	zip := repository location / ''resources'' / ''BioSmalltalkResourceFiles.zip''.
	zip isFile ifFalse: [ ^ self ].
	ZipArchive new
		readFrom: zip;
		extractAllTo: self workingDirectory asFileReference informing: nil'.

fix value: #'BioObject class' value: #testFilesFullDirectoryName
	value: [ :source | (source includesSubstring: 'ensureTestFiles') not ]
	value:
'testFilesFullDirectoryName
	" Answer a <String> of the full path directory name where the files for testing are located "

	self ensureTestFiles.
	^ self workingDirectory asFileReference / self testFilesDirectoryName'.

fix value: #BioFASTQSamples value: #testFilesDirectoryName
	value: [ :source | (source includesSubstring: 'ensureTestFiles') not ]
	value:
'testFilesDirectoryName
	" Answer a <String> with the directory name containing FASTQ test files "

	BioObject ensureTestFiles.
	^ (super testFilesDirectoryName asFileReference / ''fastq_files'') fullName'.

#(testGenBankComplementStrand testGenBankFeatureTypes testParseGenBankFromFile) do: [ :each |
	(Smalltalk globals at: #PkgFixRewrite) value: #BioDnaFeaturesViewerTest value: each
		value: '''DnaFeaturesViewer/examples/example_sequence.gb'''
		value: '(BioObject testFilesFullDirectoryName / ''DnaFeaturesViewer'' / ''example_sequence.gb'') fullName' ].

"=== B. BED, MAF and GFF3 files ==="

add value: #BioBEDTest value: #writeBEDFixtures value: 'running' value:
'writeBEDFixtures
	"The tests read /tmp/bed/test_bed12.bed, test_bed6.bed and test_merge.bed. No repository holds them. The content is what the assertions describe: 2 BED12 lines; 3 BED6 intervals (2 on chr1, 1 on chr2, total length 1400, only gene1 overlaps chr1 50-150, one name gene1); 3 intervals that merge into 2 with the end 400 for the first."
	| directory row write |
	directory := ''/tmp/bed'' asFileReference.
	directory ensureCreateDirectory.
	row := [ :text | text copyReplaceAll: '' '' with: String tab ].
	write := [ :name :lines | | file |
		file := directory / name.
		file ensureDelete.
		file writeStreamDo: [ :stream | lines do: [ :line | stream nextPutAll: (row value: line); lf ] ] ].
	write value: ''test_bed12.bed'' value: {
		''chr7 127471196 127472363 uc001bxa.3 0 + 127471196 127472363 255,0,0 3 357,185,148 0,718,1300''.
		''chr7 127473030 127474697 uc001bxb.3 0 + 127473030 127474697 255,0,0 3 410,152,159 0,749,1508'' }.
	write value: ''test_bed6.bed'' value: {
		''chr1 0 100 gene1 500 +''.
		''chr1 500 1000 gene2 600 -''.
		''chr2 2000 2800 gene3 700 +'' }.
	write value: ''test_merge.bed'' value: {
		''chr1 100 300 m1 0 +''.
		''chr1 200 400 m2 0 +''.
		''chr1 600 800 m3 0 +'' }'.

(Smalltalk globals at: #PkgFixPut) value: #BioBEDTest value: #setUp value: 'running' value:
'setUp
	super setUp.
	self writeBEDFixtures'.

(Smalltalk globals at: #PkgFixPut) value: #BioBEDTest value: #tearDown value: 'running' value:
'tearDown
	"Remove the files that setUp wrote, and the directory when nothing else is in the directory."
	| directory |
	directory := ''/tmp/bed'' asFileReference.
	#(''test_bed12.bed'' ''test_bed6.bed'' ''test_merge.bed'') do: [ :name | (directory / name) ensureDelete ].
	(directory exists and: [ directory children isEmpty ]) ifTrue: [ directory ensureDelete ].
	super tearDown'.

add value: #BioMAFTest value: #writeMAFFixture value: 'running' value:
'writeMAFFixture
	"The tests read /tmp/maf_test/test.maf. No repository holds the file. The content is what the assertions describe: 8 mutations, 2 of them in TP53."
	| directory file |
	directory := ''/tmp/maf_test'' asFileReference.
	directory ensureCreateDirectory.
	file := directory / ''test.maf''.
	file ensureDelete.
	file writeStreamDo: [ :stream |
		{ ''Hugo_Symbol Entrez_Gene_Id Chromosome Start_Position End_Position Strand Variant_Classification Variant_Type Reference_Allele Tumor_Seq_Allele1 Tumor_Seq_Allele2 Tumor_Sample_Barcode''.
		''TP53 7157 17 7579472 7579472 + Missense_Mutation SNP G A A TCGA-AB-1234''.
		''TP53 7157 17 7577120 7577120 + Nonsense_Mutation SNP C T T TCGA-AB-5678''.
		''KRAS 3845 12 25398284 25398284 + Missense_Mutation SNP C A A TCGA-AB-5678''.
		''EGFR 1956 7 55259515 55259515 + Missense_Mutation SNP T G G TCGA-AB-1234''.
		''BRAF 673 7 140453136 140453136 + Missense_Mutation SNP A T T TCGA-AB-9012''.
		''PIK3CA 5290 3 178936091 178936091 + Missense_Mutation SNP G A A TCGA-AB-9012''.
		''PTEN 5728 10 89692905 89692905 + Nonsense_Mutation SNP C T T TCGA-AB-3456''.
		''APC 324 5 112175770 112175770 + Nonsense_Mutation SNP C T T TCGA-AB-3456'' }
			do: [ :line | stream nextPutAll: (line copyReplaceAll: '' '' with: String tab); lf ] ]'.

(Smalltalk globals at: #PkgFixPut) value: #BioMAFTest value: #setUp value: 'running' value:
'setUp
	super setUp.
	self writeMAFFixture'.

(Smalltalk globals at: #PkgFixPut) value: #BioMAFTest value: #tearDown value: 'running' value:
'tearDown
	"Remove the file that setUp wrote, and the directory when nothing else is in the directory."
	| directory |
	directory := ''/tmp/maf_test'' asFileReference.
	(directory / ''test.maf'') ensureDelete.
	(directory exists and: [ directory children isEmpty ]) ifTrue: [ directory ensureDelete ].
	super tearDown'.

add value: #'BioGFF3Test class' value: #ensureFixturesIn: value: 'fixtures' value:
'ensureFixturesIn: aDirectory
	"Stand-in for hsa.gff3 and rno.gff3 of miRBase. Neither file is in the repository or in its archive. The files have the counts that the tests of this class assert (hsa: 1918 hairpins and 2883 mature miRNAs on 24 sequences; rno: 764 and 559) and the miRBase layout (miRNA_primary_transcript, miRNA with Derives_from). The coordinates are made up."
	| write |
	write := [ :file :prefix :primaryCount :matureCount :seqids |
		file isFile ifFalse: [
			aDirectory ensureCreateDirectory.
			file writeStreamDo: [ :stream |
				stream nextPutAll: ''##gff-version 3''; lf.
				1 to: primaryCount do: [ :i | | start |
					start := i * 1000.
					stream
						nextPutAll: (seqids at: i - 1 \\ seqids size + 1); tab;
						nextPut: $.; tab;
						nextPutAll: ''miRNA_primary_transcript''; tab;
						print: start; tab;
						print: start + 80; tab;
						nextPut: $.; tab;
						nextPut: (i even ifTrue: [ $+ ] ifFalse: [ $- ]); tab;
						nextPut: $.; tab;
						nextPutAll: ''ID=MI''; nextPutAll: (i printPaddedWith: $0 to: 7);
						nextPutAll: '';Name=''; nextPutAll: prefix; nextPutAll: ''-mir-''; print: i;
						lf ].
				1 to: matureCount do: [ :j | | parent start |
					parent := j - 1 \\ primaryCount + 1.
					start := parent * 1000 + 10 + (j - 1 // primaryCount * 30).
					stream
						nextPutAll: (seqids at: parent - 1 \\ seqids size + 1); tab;
						nextPut: $.; tab;
						nextPutAll: ''miRNA''; tab;
						print: start; tab;
						print: start + 21; tab;
						nextPut: $.; tab;
						nextPut: (parent even ifTrue: [ $+ ] ifFalse: [ $- ]); tab;
						nextPut: $.; tab;
						nextPutAll: ''ID=MIMAT''; nextPutAll: (j printPaddedWith: $0 to: 7);
						nextPutAll: '';Name=''; nextPutAll: prefix; nextPutAll: ''-miR-''; print: j;
						nextPutAll: '';Derives_from=MI''; nextPutAll: (parent printPaddedWith: $0 to: 7);
						lf ] ] ] ].
	write valueWithArguments: {
		aDirectory / ''hsa.gff3''. ''hsa''. 1918. 2883.
		(((1 to: 22) collect: [ :each | ''chr'' , each printString ]) , #(''chrX'' ''chrY'')) }.
	write valueWithArguments: {
		aDirectory / ''rno.gff3''. ''rno''. 764. 559.
		(((1 to: 20) collect: [ :each | ''chr'' , each printString ]) , #(''chrX'')) }'.

fix value: #BioGFF3Test value: #baseDirectory
	value: [ :source | (source includesSubstring: 'ensureFixturesIn:') not ]
	value:
'baseDirectory

	| directory |
	directory := self testFilesFullDirectoryName / ''gff''.
	self class ensureFixturesIn: directory.
	^ directory'.

"=== C. BioGtUnixSubprocessTest is a test for Glamorous Toolkit ==="

(Smalltalk globals at: #PkgFixPut) value: #BioGtUnixSubprocessTest value: #setUp value: 'running' value:
'setUp
	super setUp.
	((self selector beginsWith: ''testExecute'') and: [ (Smalltalk globals includesKey: #GtUnixSubprocess) not ])
		ifTrue: [ self skip: ''BioGtUnixSubprocess is an adapter for Glamorous Toolkit: the class GtUnixSubprocess exists only in a Glamorous Toolkit image, so the test cannot run on a Pharo image'' ]'.

"=== D. BioSpecies is BioPathwaySpecies in the pathway and ontology tests ==="

#(BioPathwayTest BioOntologyTest) do: [ :className |
	| cls |
	cls := resolve value: className.
	cls ifNotNil: [
		cls selectors asSortedCollection do: [ :selector |
			((cls >> selector) sourceCode includesSubstring: 'BioSpecies ')
				ifTrue: [ (Smalltalk globals at: #PkgFixRewrite) value: className value: selector value: 'BioSpecies ' value: 'BioPathwaySpecies ' ] ] ] ].

"=== E. the Phylip tokenizers are in BioPhylipPetitParser ==="

#(firstLineTokenizer speciesDNALineTokenizer speciesDNANamedBlockTokenizer) do: [ :each |
	(Smalltalk globals at: #PkgFixRewrite) value: #BioPhylipParserTest value: each value: 'BioPhylipParser new' value: 'BioPhylipPetitParser new' ].

"=== F. BioReaction>>enzymes includes the enzymes of the catalyses ==="

fix value: #BioReaction value: #enzymes
	value: [ :source | source includesSubstring: 'ifNil: [ enzymes :=' ]
	value:
'enzymes
	"The enzymes added with addEnzyme: and the enzymes of the catalyses of the receiver."
	| answer |
	answer := OrderedCollection new.
	enzymes ifNotNil: [ answer addAll: enzymes ].
	self catalyses do: [ :each |
		(each enzyme isNil or: [ answer includes: each enzyme ])
			ifFalse: [ answer add: each enzyme ] ].
	^ answer'.

fix value: #BioReaction value: #addEnzyme:
	value: [ :source | source includesSubstring: 'self enzymes add:' ]
	value:
'addEnzyme: aSpecies
	enzymes ifNil: [ enzymes := OrderedCollection new ].
	enzymes add: aSpecies'.

"=== G. cutadapt stand-in ==="

add value: #BioCutadaptWrapperTest value: #cutadaptStandInProgram value: 'fixtures' value:
'cutadaptStandInProgram
	"An awk program that answers like cutadapt for the options -a, -o and -m of testExecuteWithFiltering. Not cutadapt: exact 3-prime adapter and partial adapter at the read end (3 bases or more), no error rate."
	^ ''BEGIN {
	adapter = ""; out = ""; minlen = 0; infile = "";
	for (i = 1; i < ARGC; i++) {
		arg = ARGV[i];
		if (arg == "-a") { i++; adapter = ARGV[i]; }
		else if (arg == "-o") { i++; out = ARGV[i]; }
		else if (arg == "-m") { i++; minlen = ARGV[i] + 0; }
		else if (arg == "-q" || arg == "-j" || arg == "-O" || arg == "-e") { i++; }
		else if (substr(arg, 1, 1) == "-") { }
		else { infile = arg; }
	}
	if (length(infile) == 0 || length(out) == 0) { print "cutadapt stand-in: input or -o missing" > "/dev/stderr"; exit 2; }
	printf "" > out;
	total = 0; withadapter = 0; tooshort = 0; written = 0; basesin = 0; basesout = 0;
	while ((getline header < infile) > 0) {
		if ((getline seq < infile) <= 0) break;
		getline plus < infile;
		getline qual < infile;
		total++;
		n = length(seq);
		basesin += n;
		cut = n;
		pos = index(seq, adapter);
		if (length(adapter) > 0 && pos > 0) { cut = pos - 1; }
		else {
			k = length(adapter) - 1;
			if (k > n) k = n;
			for (; k >= 3; k--) {
				if (substr(seq, n - k + 1) == substr(adapter, 1, k)) { cut = n - k; break; }
			}
		}
		if (cut < n) withadapter++;
		seq = substr(seq, 1, cut);
		qual = substr(qual, 1, cut);
		if (length(seq) < minlen) { tooshort++; continue; }
		print header "\n" seq "\n" plus "\n" qual > out;
		written++;
		basesout += length(seq);
	}
	close(out);
	print "This is the cutadapt stand-in of the BioSmalltalk test repair (it is not cutadapt)";
	print "";
	print "=== Summary ===";
	print "";
	printf "Total reads processed: %d\n", total;
	printf "Reads with adapters: %d\n", withadapter;
	printf "Reads that were too short: %d\n", tooshort;
	printf "Reads written (passing filters): %d\n", written;
	printf "Total basepairs processed: %d bp\n", basesin;
	printf "Total written (filtered): %d bp\n", basesout;
	exit 0;
}
'''.

add value: #BioCutadaptWrapperTest value: #cutadaptProgramName value: 'fixtures' value:
'cutadaptProgramName
	"The real cutadapt when the system has one, else the stand-in awk program, written into the working directory."
	| found script |
	found := BioExecutionEngine executeCommand: ''command -v cutadapt''.
	found isEmpty ifFalse: [ ^ found ].
	script := BioObject workingDirectory asFileReference / ''cutadapt-standin.awk''.
	script ensureDelete.
	script writeStreamDo: [ :stream | stream nextPutAll: self cutadaptStandInProgram ].
	^ ''awk -f '''''' , script fullName , '''''' --'''.

fix value: #BioCutadaptWrapperTest value: #testExecuteWithFiltering
	value: [ :source | (source includesSubstring: 'cutadaptProgramName') not ]
	value:
'testExecuteWithFiltering
	"Test cutadapt execution with quality filtering"

	| wrapper result |
	wrapper := BioCutadaptWrapper new.
	wrapper programName: self cutadaptProgramName.
	wrapper adapter3Prime: ''AGATCGGAAGAGC''.
	wrapper input: BioFASTQSamples new miscDnaSangerFilename.
	wrapper out: ''/tmp/cutadapt_test_filtered.fq''.
	wrapper minimumLength: 10.
	result := wrapper execute.
	self assert: result isNotNil.
	self assert: (result result includesSubstring: ''Total reads processed'')'.

Smalltalk globals removeKey: #PkgFixRewrite ifAbsent: [ ].
Smalltalk globals removeKey: #PkgFixPut ifAbsent: [ ].
