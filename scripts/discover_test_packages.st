"Discover all TestCase subclasses in the image, grouped by package.
 Writes results to /tmp/test_package_inventory.txt
 Usage: inject via 'scripts/discover_test_packages.st' asFileReference fileIn"!

| stream packages sorted |
stream := '/tmp/test_package_inventory.txt' asFileReference writeStream.
[
  stream nextPutAll: '=== Test Package Inventory ==='; cr.
  stream nextPutAll: 'Image: '; nextPutAll: SystemVersion current imageVersionString; cr.
  stream nextPutAll: 'Date: '; nextPutAll: DateAndTime now printString; cr; cr.

  "Group concrete TestCase subclasses by package"
  packages := Dictionary new.
  (TestCase allSubclasses reject: [:c | c isAbstract]) do: [:c |
    | pkgName |
    pkgName := [c package name] on: Error do: [:e | 'Unknown'].
    (packages at: pkgName ifAbsentPut: [OrderedCollection new]) add: c name].

  "Sort by test count descending"
  sorted := packages associations asSortedCollection: [:a :b | a value size > b value size].

  stream nextPutAll: 'Total packages: '; nextPutAll: sorted size printString; cr.
  stream nextPutAll: 'Total test classes: ';
    nextPutAll: (sorted inject: 0 into: [:sum :a | sum + a value size]) printString; cr; cr.

  sorted do: [:assoc |
    | count |
    count := 0.
    assoc value do: [:className |
      | cls |
      cls := Smalltalk globals at: className asSymbol ifAbsent: [nil].
      cls ifNotNil: [count := count + cls testSelectors size]].
    stream nextPutAll: assoc value size printString; nextPutAll: ' classes, ~';
      nextPutAll: count printString; nextPutAll: ' tests in ';
      nextPutAll: assoc key; cr.
    assoc value asSortedCollection do: [:name |
      stream nextPutAll: '  '; nextPutAll: name; cr]].

  stream nextPutAll: cr; nextPutAll: '=== END ==='; cr.
] ensure: [stream close].

Transcript show: 'Test inventory written to /tmp/test_package_inventory.txt'; cr!
