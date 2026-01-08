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
