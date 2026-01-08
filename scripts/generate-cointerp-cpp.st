"Generate cointerp.c using standard VMMaker (for later C++ transformation)"

| vmmaker outputPath |

Transcript show: '=== Generating cointerp.c for iOS ==='; cr.

"Initialize VMMaker configuration"
Transcript show: 'Initializing VMMaker...'; cr.

VMMakerConfiguration initializeForPharo.

"Create VMMaker using proper factory method"
outputPath := '/Users/wohl/src/pharo/iospharo/src/ios'.

vmmaker := VMMaker
    makerFor: StackInterpreter
    and: nil  "no cogit for stack VM"
    with: #(ObjectMemory Spur64BitMemoryManager
            bytecodeTableInitializer initializeBytecodeTableForSistaV1)
    to: outputPath
    platformDir: outputPath
    including: #()
    configuration: VMMakerConfiguration.

Transcript show: 'Generating interpreter...'; cr.

vmmaker
    stopOnErrors: false;
    generateInterpreterFile.

Transcript show: 'Generation complete'; cr.
Transcript show: 'Output directory: ', outputPath; cr.

Smalltalk exit: 0.
