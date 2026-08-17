"Directory enumeration must not report '.' and '..'.

 Filtering those two is the plugin's job, not the image's — the reference
 FileAttributesPlugin loops on readdir until it has an entry that is neither
 (faSupport.c, faReadDirectory). Our primitiveReaddir returned every entry,
 including both.

 That is not a cosmetic difference. The image turns each entry into a
 FileReference, so '..' becomes the parent directory and '.' becomes the
 directory itself under its own name. Anything that walks a tree then walks
 upward too: FileReference>>deleteAll on a directory recursed into '..', climbed
 toward the filesystem root, and never terminated.

 It was expensive to find because of where it surfaced. The Fuel serialisation
 test classes were skip-listed as hangs for months; they hang in resource
 teardown, which is a deleteAll, not in any test — every individual test passed.
 Several other classes were merely slowed: GlobalIdentifierFuelPersistenceTest
 took 100 seconds walking the filesystem and takes 28 milliseconds now.

 Written 2026-08-16."!

TestCase subclass: #DirectoryEnumerationTest
  instanceVariableNames: 'root'
  classVariableNames: ''
  package: 'Tests-FileSystem'!

!DirectoryEnumerationTest methodsFor: 'running'!
setUp
  super setUp.
  root := (FileLocator temp / ('dirEnumTest' , 999999 atRandom printString)) asFileReference.
  root ensureCreateDirectory! !

!DirectoryEnumerationTest methodsFor: 'running'!
tearDown
  "Deliberately uses deleteAll, which is the operation the bug broke."
  root exists ifTrue: [ root deleteAll ].
  super tearDown! !

!DirectoryEnumerationTest methodsFor: 'helpers'!
populate
  (root / 'a.txt') writeStreamDo: [ :s | s nextPutAll: 'alpha' ].
  (root / 'b.txt') writeStreamDo: [ :s | s nextPutAll: 'beta' ].
  (root / 'nested') ensureCreateDirectory.
  ((root / 'nested') / 'c.txt') writeStreamDo: [ :s | s nextPutAll: 'gamma' ]! !

!DirectoryEnumerationTest methodsFor: 'tests'!
testChildrenExcludesDotAndDotDot
  "The direct statement of the bug. With '.' and '..' reported this answered
   five entries for a directory holding three, one of them the parent."
  | names |
  self populate.
  names := (root children collect: [ :each | each basename ]) asSortedCollection asArray.
  self
    assert: names equals: #('a.txt' 'b.txt' 'nested').
  self
    deny: (names includes: '..')
    description: 'directory enumeration reported the parent entry'.
  self
    deny: (names includes: root basename)
    description: 'directory enumeration reported itself — the "." entry'! !

!DirectoryEnumerationTest methodsFor: 'tests'!
testEmptyDirectoryHasNoChildren
  "An empty directory still contains '.' and '..' on disk, so this is where an
   unfiltered readdir shows up most starkly: two children instead of none."
  self assert: root children isEmpty
    description: 'an empty directory answered ', root children size printString, ' children'! !

!DirectoryEnumerationTest methodsFor: 'tests'!
testDeleteAllRemovesTreeAndTerminates
  "The consequence, and the reason this mattered. Following '..' made deleteAll
   climb out of its own directory, so it neither terminated nor stayed inside
   the tree it was given. A plain assertion is enough: on the broken VM this
   test does not fail, it never returns."
  | outside |
  self populate.
  outside := root parent / ('sibling' , 999999 atRandom printString , '.txt').
  outside writeStreamDo: [ :s | s nextPutAll: 'must survive' ].
  [ root deleteAll.
    self deny: root exists description: 'deleteAll left the directory behind'.
    self
      assert: outside exists
      description: 'deleteAll escaped its directory and deleted a sibling' ]
    ensure: [ outside exists ifTrue: [ outside delete ] ]! !

!DirectoryEnumerationTest methodsFor: 'tests'!
testDirectoryTreeWalkTerminates
  "allChildren walks the whole tree. Under the bug it walked upward as well.
   Note allChildren includes the receiver, so the root's own name is expected
   here — that is Pharo's behaviour, not the '.' entry leaking through. The
   difference is that it appears exactly once and at the top, whereas the bug
   put it in every directory's children."
  | all expected |
  self populate.
  all := (root allChildren collect: [ :each | each basename ]) asSortedCollection asArray.
  expected := (Array with: 'a.txt' with: 'b.txt' with: 'c.txt' with: 'nested'),
              (Array with: root basename).
  self assert: all equals: expected asSortedCollection asArray! !
