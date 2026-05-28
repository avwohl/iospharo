| pkgMap |
pkgMap := OrderedCollection new.
pkgMap
  add: #('OpalCompiler-Tests' 'opal');
  add: #('AST-Core-Tests' 'ast');
  add: #('Reflectivity-Tests' 'reflectivity');
  add: #('Ring-Core-Tests' 'ring');
  add: #('Microdown-Tests' 'microdown');
  add: #('Fuel-Core-Tests' 'fuel');
  add: #('Regex-Core-Tests' 'regex');
  add: #('STON-Tests' 'ston');
  add: #('NumberParser-Tests' 'numberparser');
  add: #('System-Time-Tests' 'systime');
  add: #('Collections-Strings-Tests' 'strings');
  add: #('Collections-Sequenceable-Tests' 'seq');
  add: #('AI-Algorithms-Graph-Tests' 'aigraph');
  add: #('Zinc-Character-Encoding-Tests' 'zincenc').
pkgMap do: [:pair | | pkgName tag classes |
  pkgName := pair first. tag := pair second.
  classes := (TestCase allSubclasses select: [:c |
      c isAbstract not and: [
        ([c package name = pkgName] on: Error do: [:e | false]) and: [
          (c selectors anySatisfy: [:s | s beginsWith: 'test'])]]])
    asSortedCollection: [:a :b | a name < b name].
  ('/tmp/pkg_', tag, '.txt') asFileReference writeStreamDo: [:s |
    classes do: [:c | s nextPutAll: c name; lf]].
  Transcript show: tag; show: ' '; show: classes size printString; show: ' classes'; cr].
