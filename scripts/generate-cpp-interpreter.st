| outputPath generator |

Transcript show: 'Generating interpreter for iOS'; cr.

generator := CCodeGenerator new.

generator
    vmClass: StackInterpreter;
    options: #(SPURVM true STACKVM true COGVM false).

generator
    addClass: SpurMemoryManager;
    addClass: StackInterpreter;
    addClass: SpurSegmentManager.

outputPath := '/Users/wohl/src/pharo/iospharo/src/ios/cointerp-generated.c'.
outputPath asFileReference writeStreamDo: [ :stream |
    generator emitCCodeOn: stream
].

Transcript show: 'Generated ', outputPath; cr.

Smalltalk exit: 0.
