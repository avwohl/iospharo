| maker outputDir vmmaker |

Transcript show: 'Setting up VMMaker...'; cr.

outputDir := '/Users/wohl/src/pharo/iospharo/src/generated'.
outputDir asFileReference ensureCreateDirectory.

"Use VMMaker directly"
vmmaker := VMMaker new.
vmmaker sourceDirectoryName: outputDir.
vmmaker interpreterClass: StackInterpreter.
vmmaker options: #(SPURVM true STACKVM true).
vmmaker stopOnErrors: true.

Transcript show: 'Building code generator...'; cr.
vmmaker buildCodeGeneratorForInterpreter.

Transcript show: 'Generating interpreter file...'; cr.
vmmaker generateInterpreterFile.

Transcript show: 'Done. Check: ', outputDir; cr.

Smalltalk exit: 0.
