"38-mc-load-keeps-method-stamp.st -- Monticello loads a method with no time stamp when
 the definition came from the image (2026-11-20).

 MCMethodDefinition>>loadWithLoader: gives the method installer
     changeStamp: timeStamp;
 the instance variable.  A definition made from a method of the image (a snapshot,
 CompiledMethod>>asMCMethodDefinition) has a source pointer and computes its stamp
 lazily in #timeStamp; the instance variable stays nil until a sender asks.  So each
 method that such a definition installs gets the stamp nil, and its timeStamp answers
 the string 'nil'.

 Seen in the tests: MCPackageTest unloads the mock package, and its tearDown installs
 MCSnapshotResource current snapshot again.  After that MCMockClassA>>one and
 MCMockClassA class>>one have the stamp 'nil', and MCStWriterTest>>testMethodDefinition
 and >>testClassMethodDefinition fail when they run after MCPackageTest in the same
 image.  Measured on Pharo 14 build 791 and Pharo 15 build 141, stock Cog v12.0.6
 and our VM: the same.  The sweeps show it on Pharo 15 only because the two classes
 fall into one batch (one image) there.

 A second defect is behind the first one: the load runs inside
 SourcesManager current deferFlushDuring: (MCPackageLoader>>basicLoad), and
 FilesSourcesManager>>timeStampAt:for: answers '' while the flush is deferred
 ('flushChanges ifFalse: [ ^ '''' ]').  So a stamp that is computed during the load is
 empty: with only the first repair the methods get the stamp ''.  Measured 2026-11-20:
 the lazy stamp of MCMockClassA>>one reads '2026-11-20T15:23:20...' before and after
 MCSnapshot>>install, and '' during it.

 The repairs: #loadWithLoader: sends #timeStamp, the accessor that computes the stamp;
 #basicLoad reads the stamps of the method definitions before it defers the flush, so
 the accessor answers the stamp that the definition cached."

fix value: #MCMethodDefinition value: #loadWithLoader:
	value: [ :src | src includesSubstring: 'changeStamp: timeStamp;' ]
	value: (((Smalltalk at: #MCMethodDefinition) >> #loadWithLoader:) sourceCode
		copyReplaceAll: 'changeStamp: timeStamp;' with: 'changeStamp: self timeStamp;').

fix value: #MCPackageLoader value: #basicLoad
	value: [ :src | (src includesSubstring: 'deferFlushDuring: [ self basicLoadDefinitions ]') and: [ (src includesSubstring: 'each timeStamp') not ] ]
	value: 'basicLoad

	"Read the time stamps of the method definitions first: while the flush of the changes file is deferred, FilesSourcesManager>>timeStampAt:for: answers the empty string, and a definition made from a method of the image computes its stamp lazily."
	additions do: [ :each | each isMethodDefinition ifTrue: [ each timeStamp ] ].
	SourcesManager current deferFlushDuring: [ self basicLoadDefinitions ]'.
