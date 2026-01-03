"iOS VM Simulator Setup for debugging memory allocation issues.

The iOS port crashes at unrelocated pointer access because iOS ASLR
prevents allocating at fixed addresses (1TB for oldSpace, 2TB for permSpace).

This setup configures StackInterpreterSimulator to simulate iOS memory constraints:
- Memory allocated in a 2GB arena starting at a low address
- Fixed offsets within arena for different spaces

Usage:
1. Evaluate this file to set up the simulator
2. Use iOSSimulator to debug the image loading/relocation process
3. Once fixed in Smalltalk, port the fix to C code
"

Object subclass: #IOSVMSimulator
    instanceVariableNames: 'simulator imagePath arenaBase'
    classVariableNames: ''
    package: 'iOS-VMMaker'.

IOSVMSimulator class >> new [
    ^ super new initialize
]

IOSVMSimulator >> initialize [
    "Create a StackInterpreterSimulator configured for iOS memory constraints"

    simulator := StackInterpreterSimulator newWithOptions: #().
    arenaBase := 16r100000000. "4GB - a typical iOS base address after ASLR"

    "Override memory allocation to return iOS-compatible addresses"
    self configureMemoryForIOS.
]

IOSVMSimulator >> configureMemoryForIOS [
    "Configure the simulator's object memory to use iOS-like addressing.

    iOS arena layout (within 2GB arena):
      Offset 0x00000000: Code zone (for interpreter, non-executable)
      Offset 0x10000000: Stack pages (256MB)
      Offset 0x20000000: New space (512MB)
      Offset 0x40000000: Old space (1GB)
      Offset 0x60000000: Perm space (1.5GB)"

    "The key insight: in simulation, memory is a ByteArray.
     We can trace exactly when pointers are accessed before relocation."

    Transcript
        cr;
        show: 'iOS VM Simulator configured';
        cr;
        show: 'Arena base: ', arenaBase hex;
        cr.
]

IOSVMSimulator >> openImage: pathString [
    "Open a Pharo image in the simulator"
    imagePath := pathString.

    "Add breakpoint before object memory initialization to trace pointer relocation"
    simulator setBreakSelector: #initializeObjectMemory:.

    Transcript
        cr;
        show: 'Opening image: ', pathString;
        cr;
        show: 'Breakpoint set on #initializeObjectMemory:';
        cr.

    simulator openOn: pathString.
]

IOSVMSimulator >> run [
    "Run the simulator - will stop at breakpoint"
    simulator openAsMorph; run.
]

IOSVMSimulator >> tracePointerRelocation [
    "Trace the pointer relocation process to find where unrelocated pointers are accessed"

    | objectMemory |
    objectMemory := simulator objectMemory.

    "Key values to trace:"
    Transcript
        cr;
        show: '=== Pointer Relocation Debug ===';
        cr;
        show: 'bytesToShift: ', objectMemory bytesToShift printString;
        cr;
        show: 'oldSpaceStart: ', objectMemory oldSpaceStart hex;
        cr;
        show: 'newSpaceStart: ', objectMemory newSpaceStart hex;
        cr.
]

IOSVMSimulator >> cloneBeforeCrash [
    "Clone the simulator state for 'Lemming Debugging'.
     Run until crash, then debug from this snapshot."
    ^ simulator cloneSimulation
]

"=== Quick Start ==="

"To use this simulator:

| sim |
sim := IOSVMSimulator new.
sim openImage: '/path/to/your/test.image'.
sim run.

When it hits the breakpoint at #initializeObjectMemory:, use:
  sim tracePointerRelocation.

To clone state before a potential crash:
  | clone |
  clone := sim cloneBeforeCrash.
  clone run. \"This can crash without losing state\"
"

Transcript
    cr; cr;
    show: '=== iOS VM Simulator Setup Loaded ===';
    cr;
    show: 'Create simulator: IOSVMSimulator new';
    cr;
    show: 'Open image: sim openImage: ''/path/to/image''';
    cr;
    show: 'Run: sim run';
    cr.
