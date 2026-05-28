| byPkg lines |
byPkg := Dictionary new.
TestCase allSubclasses do: [:c |
  c isAbstract ifFalse: [
    | pkg n |
    pkg := [c package name] on: Error do: [:e | 'UNKNOWN'].
    n := (c selectors select: [:s | s beginsWith: 'test']) size.
    n > 0 ifTrue: [
      byPkg at: pkg put: (byPkg at: pkg ifAbsent: [0]) + n]]].
lines := WriteStream on: String new.
(byPkg associations asSortedCollection: [:a :b | a value > b value]) do: [:assoc |
  lines nextPutAll: (assoc value printString); tab; nextPutAll: assoc key; lf].
lines nextPutAll: 'TOTAL_PKGS '; nextPutAll: byPkg size printString; nextPutAll: ' TOTAL_TESTS '; nextPutAll: (byPkg values inject: 0 into: [:a :b | a+b]) printString; lf.
'/tmp/pkg_enum.txt' asFileReference writeStreamDo: [:s | s nextPutAll: lines contents].
