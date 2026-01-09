# Claude Code Instructions

## Git Workflow
- Use large commits to stop less fequently
- Always run `git status` before and after commits to verify state

## Project Context
This is an iOS Pharo VM that moves oop encoding from high address bits to low bits for iOS/ASLR compatibility.

Key directories:
- `src/vm/` - Clean C++ VM implementation (ImageLoader, Interpreter, ObjectMemory, Primitives)
- `src/ios/` - iOS-specific VM code and generated interpreter
- `scripts/` - Build and transformation scripts
- `docs/` - Documentation and WIP notes

## Bytecode Reference
The Sista V1 bytecode spec (used by Pharo 10+) is notoriously hard to find online.
A local copy is at: `docs/SistaV1-Bytecode-Spec.md`

The authoritative source is the class comment in the Pharo source:
`src/Kernel-BytecodeEncoders/EncoderForSistaV1.class.st` (in the main pharo repo)

**Warning:** Online resources often document the older V3PlusClosures bytecode set, not Sista V1.
The bytecode ranges 0xE0-0xFF are completely different between the two sets.

## Debugging
- Always test on Mac first - it starts up much faster than the iOS simulator
- Use `./build/test_load_image <image-path>` for quick VM testing
- Build with `cmake --build build` from the project root
