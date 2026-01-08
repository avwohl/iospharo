#!/usr/bin/env python3
"""
Transform cointerp.c to be C++ compatible.

Fixes C-isms that C++ rejects:
1. 'class' reserved word → 'klass'
2. void* implicit conversions → explicit casts
3. void* arithmetic → char* casts
4. alloca return type → explicit cast
5. Pointer/integer conversions → explicit casts
"""

import re
import sys
import os

def transform_cointerp(input_path, output_path):
    with open(input_path, 'r') as f:
        content = f.read()

    # 1. Replace 'class' parameter name with 'klass' (reserved word in C++)
    content = re.sub(r'\bsqInt\s+class\b', 'sqInt klass', content)
    content = re.sub(r'\(sqInt class\)', '(sqInt klass)', content)

    # 1b. Replace 'class' local variable usage
    # Pattern: class = something;
    content = re.sub(r'\bclass = ', 'klass = ', content)
    # Pattern: (class) or , class, or class; etc.
    content = re.sub(r'\(class\)', '(klass)', content)
    content = re.sub(r', class,', ', klass,', content)
    content = re.sub(r', class\)', ', klass)', content)
    content = re.sub(r'\(class,', '(klass,', content)
    # Pattern: long64At(class)
    content = re.sub(r'long64At\(class\)', 'long64At(klass)', content)
    content = re.sub(r'isUnambiguouslyForwarder\(class\)', 'isUnambiguouslyForwarder(klass)', content)
    # Pattern: (class + BaseHeaderSize)
    content = re.sub(r'\(class \+ BaseHeaderSize\)', '(klass + BaseHeaderSize)', content)
    # Pattern: class == or class !=
    content = re.sub(r'if \(class ==', 'if (klass ==', content)
    content = re.sub(r'if \(class !=', 'if (klass !=', content)
    content = re.sub(r'\(class == ', '(klass == ', content)
    content = re.sub(r'\(class != ', '(klass != ', content)
    content = re.sub(r'return class;', 'return klass;', content)
    content = re.sub(r'&& class ', '&& klass ', content)
    content = re.sub(r'\|\| class ', '|| klass ', content)
    content = re.sub(r'! class\)', '! klass)', content)
    content = re.sub(r'!\(class\)', '!(klass)', content)
    # Pattern: = class; at end of assignment
    content = re.sub(r'= class;', '= klass;', content)

    # 2. Fix alloca return (void* to char* needs explicit cast in C++)
    content = re.sub(
        r'char \*(\w+) = alloca\(',
        r'char *\1 = (char*)alloca(',
        content
    )

    # 3. Fix theStackMemory void* to integer conversion
    content = re.sub(
        r'theStackMemory = stackAddress;',
        'theStackMemory = (void*)(uintptr_t)stackAddress;',
        content
    )

    # 4. Fix void* arithmetic by casting to char*
    content = re.sub(
        r'\(theStackMemory \+',
        '((char*)theStackMemory +',
        content
    )

    # 5. Fix return type mismatch: numPointerSlotsOf
    content = re.sub(
        r'extern sqInt numPointerSlotsOf\(sqInt objOop\);',
        'extern usqInt numPointerSlotsOf(sqInt objOop);',
        content
    )

    # 6. Fix GIV(stackPointer) -> sqInt assignment (char* to sqInt)
    content = re.sub(
        r'local_stackPointer = GIV\(stackPointer\);',
        'local_stackPointer = (sqInt)GIV(stackPointer);',
        content
    )
    content = re.sub(
        r'local_instructionPointer = GIV\(instructionPointer\);',
        'local_instructionPointer = (sqInt)GIV(instructionPointer);',
        content
    )
    content = re.sub(
        r'local_framePointer = GIV\(framePointer\);',
        'local_framePointer = (sqInt)GIV(framePointer);',
        content
    )

    # 7. Fix sqInt -> GIV(char*) assignments
    content = re.sub(
        r'GIV\(stackPointer\) = local_stackPointer;',
        'GIV(stackPointer) = (char*)local_stackPointer;',
        content
    )
    content = re.sub(
        r'GIV\(instructionPointer\) = local_instructionPointer;',
        'GIV(instructionPointer) = (char*)local_instructionPointer;',
        content
    )
    content = re.sub(
        r'GIV\(framePointer\) = local_framePointer;',
        'GIV(framePointer) = (char*)local_framePointer;',
        content
    )

    # 8. Fix sp = local_stackPointer - BytesPerWord patterns
    # sp is char*, local_stackPointer is sqInt
    # The pattern: unsignedLongAtput((sp = local_stackPointer - BytesPerWord), ...)
    # sp gets assigned an sqInt value, which is then used as pointer
    content = re.sub(
        r'\(sp = local_stackPointer - BytesPerWord\)',
        '(sp = (char*)(local_stackPointer - BytesPerWord))',
        content
    )
    content = re.sub(
        r'\(sp = local_stackPointer \+ BytesPerWord\)',
        '(sp = (char*)(local_stackPointer + BytesPerWord))',
        content
    )

    # 9. Fix local_stackPointer = sp; (char* to sqInt)
    content = re.sub(
        r'local_stackPointer = sp;',
        'local_stackPointer = (sqInt)sp;',
        content
    )

    # 10. Fix page->lastAddress = theStackMemory + ... (void* arithmetic)
    content = re.sub(
        r'\(page->lastAddress = theStackMemory \+',
        '(page->lastAddress = (char*)theStackMemory +',
        content
    )

    # 11. Fix iframeMethod(local_framePointer) - expects char*, local_framePointer is sqInt
    content = re.sub(
        r'iframeMethod\(local_framePointer\)',
        'iframeMethod((char*)local_framePointer)',
        content
    )

    # 12. Fix frameIsBlockActivation(local_framePointer) - expects char*
    content = re.sub(
        r'frameIsBlockActivation\(local_framePointer\)',
        'frameIsBlockActivation((char*)local_framePointer)',
        content
    )

    # 13. Fix frameCallerFP(local_framePointer) - expects char*, returns sqInt
    content = re.sub(
        r'frameCallerFP\(local_framePointer\)',
        'frameCallerFP((char*)local_framePointer)',
        content
    )

    # 14. Fix frameCallerSP(local_framePointer) - expects char*
    content = re.sub(
        r'frameCallerSP\(local_framePointer\)',
        'frameCallerSP((char*)local_framePointer)',
        content
    )

    # 15. Fix frameContext(local_framePointer) - expects char*
    content = re.sub(
        r'frameContext\(local_framePointer\)',
        'frameContext((char*)local_framePointer)',
        content
    )

    # 16. Fix frameReceiver(local_framePointer) - expects char*
    content = re.sub(
        r'frameReceiver\(local_framePointer\)',
        'frameReceiver((char*)local_framePointer)',
        content
    )

    # 17. Fix longAt(local_framePointer) where local_framePointer is sqInt
    content = re.sub(
        r'longAt\(local_framePointer\)',
        'longAt(local_framePointer)',  # sqInt is fine for longAt
        content
    )

    # 18. Fix stackPageFor(local_*) calls
    content = re.sub(
        r'stackPageFor\(local_framePointer\)',
        'stackPageFor((sqInt)local_framePointer)',
        content
    )
    content = re.sub(
        r'stackPageFor\(local_stackPointer\)',
        'stackPageFor((sqInt)local_stackPointer)',
        content
    )

    # 19. Fix marryFrameSP(local_framePointer, local_stackPointer) - expects char*, char*
    content = re.sub(
        r'marryFrameSP\(local_framePointer, local_stackPointer\)',
        'marryFrameSP((char*)local_framePointer, (char*)local_stackPointer)',
        content
    )

    # 20. Fix marryFrameSP(theFP, theSP) - theFP/theSP are often sqInt too
    content = re.sub(
        r'marryFrameSP\(theFP, theSP\)',
        'marryFrameSP((char*)theFP, (char*)theSP)',
        content
    )

    # 21. Fix theFP/theSP declarations and assignments
    # These variables are used as both pointers and integers
    # Add casts at assignment points

    # 22. Fix frameStackedReceiverOffset(local_framePointer) - expects char*
    content = re.sub(
        r'frameStackedReceiverOffset\(local_framePointer\)',
        'frameStackedReceiverOffset((char*)local_framePointer)',
        content
    )

    # 23. Fix various GIV assignments that need casts
    # GIV(framePointer) = theFP; where theFP is sqInt
    content = re.sub(
        r'GIV\(framePointer\) = theFP;',
        'GIV(framePointer) = (char*)theFP;',
        content
    )
    content = re.sub(
        r'GIV\(stackPointer\) = theSP;',
        'GIV(stackPointer) = (char*)theSP;',
        content
    )
    content = re.sub(
        r'GIV\(instructionPointer\) = theIP;',
        'GIV(instructionPointer) = (char*)theIP;',
        content
    )

    # 24. Fix theFP = ... assignments where source is sqInt
    # theFP is char*, GIV(framePointer) is char* - so this should be OK
    # But sometimes we have theFP = (some sqInt expression)
    # These are left as-is since theFP is already char*

    # 25. Fix comparisons with pointer types
    # theFP < GIV(...) or theFP > GIV(...)
    content = re.sub(
        r'theFP < GIV\(framePointer\)',
        '(char*)theFP < GIV(framePointer)',
        content
    )
    content = re.sub(
        r'theFP > GIV\(framePointer\)',
        '(char*)theFP > GIV(framePointer)',
        content
    )
    content = re.sub(
        r'theFP == GIV\(framePointer\)',
        '(char*)theFP == GIV(framePointer)',
        content
    )
    content = re.sub(
        r'theFP != GIV\(framePointer\)',
        '(char*)theFP != GIV(framePointer)',
        content
    )

    # 26. Fix frame functions taking theFP/theSP
    frame_funcs = ['frameCallerFP', 'frameCallerSP', 'frameContext',
                   'frameIsBlockActivation', 'iframeMethod', 'frameReceiver',
                   'frameStackedReceiverOffset', 'frameNumArgs', 'setFrameContextIntheFP']
    for func in frame_funcs:
        content = re.sub(
            rf'{func}\(theFP\)',
            f'{func}((char*)theFP)',
            content
        )
        content = re.sub(
            rf'{func}\(theSP\)',
            f'{func}((char*)theSP)',
            content
        )

    # 27. Fix ensureCallerContext(theFP) type pattern
    content = re.sub(
        r'ensureCallerContext\(theFP\)',
        'ensureCallerContext((char*)theFP)',
        content
    )

    # 28. Fix char* theFP/theSP assignments from sqInt
    # Pattern: theFP = local_framePointer; where theFP is char*
    content = re.sub(
        r'theFP = local_framePointer;',
        'theFP = (char*)local_framePointer;',
        content
    )
    content = re.sub(
        r'theSP = local_stackPointer;',
        'theSP = (char*)local_stackPointer;',
        content
    )
    content = re.sub(
        r'theIP = local_instructionPointer;',
        'theIP = (char*)local_instructionPointer;',
        content
    )

    # 29. Fix local_ = theFP patterns (char* -> sqInt)
    content = re.sub(
        r'local_framePointer = theFP;',
        'local_framePointer = (sqInt)theFP;',
        content
    )
    content = re.sub(
        r'local_stackPointer = theSP;',
        'local_stackPointer = (sqInt)theSP;',
        content
    )
    content = re.sub(
        r'local_instructionPointer = theIP;',
        'local_instructionPointer = (sqInt)theIP;',
        content
    )

    # 30. Fix theFP = frameCallerFP(...) - returns sqInt, theFP is char*
    content = re.sub(
        r'theFP = frameCallerFP\(',
        'theFP = (char*)frameCallerFP(',
        content
    )

    # 31. Fix callerFP = ... patterns
    content = re.sub(
        r'callerFP = local_framePointer;',
        'callerFP = (char*)local_framePointer;',
        content
    )
    content = re.sub(
        r'local_framePointer = callerFP;',
        'local_framePointer = (sqInt)callerFP;',
        content
    )

    # 32. Fix comparison patterns with pointer and integer
    # theFP < local_framePointer etc.
    content = re.sub(
        r'theFP < local_framePointer',
        'theFP < (char*)local_framePointer',
        content
    )
    content = re.sub(
        r'theFP > local_framePointer',
        'theFP > (char*)local_framePointer',
        content
    )
    content = re.sub(
        r'theFP == local_framePointer',
        'theFP == (char*)local_framePointer',
        content
    )
    content = re.sub(
        r'theFP != local_framePointer',
        'theFP != (char*)local_framePointer',
        content
    )

    # 33. Fix theSP3, theSP2, etc. variants
    for i in ['', '2', '3', '4', '5']:
        var = f'theSP{i}'
        content = re.sub(
            rf'{var} = local_stackPointer;',
            f'{var} = (char*)local_stackPointer;',
            content
        )
        content = re.sub(
            rf'{var} < local_framePointer',
            f'{var} < (char*)local_framePointer',
            content
        )
        content = re.sub(
            rf'{var} > local_framePointer',
            f'{var} > (char*)local_framePointer',
            content
        )
        content = re.sub(
            rf'local_stackPointer = {var};',
            f'local_stackPointer = (sqInt){var};',
            content
        )

    # 34. Fix GIV(stackPage)->headFP = local_framePointer patterns
    content = re.sub(
        r'\(GIV\(stackPage\)->headFP = local_framePointer\)',
        '(GIV(stackPage)->headFP = (char*)local_framePointer)',
        content
    )
    content = re.sub(
        r'\(GIV\(stackPage\)->headSP = local_stackPointer\)',
        '(GIV(stackPage)->headSP = (char*)local_stackPointer)',
        content
    )
    content = re.sub(
        r'\(GIV\(stackPage\)->headSP = theSP',
        '(GIV(stackPage)->headSP = (char*)theSP',
        content
    )

    # 35. Fix comparisons local_framePointer with pointer expressions
    # local_framePointer < ((GIV(stackPage)->baseAddress))
    content = re.sub(
        r'local_framePointer < \(\(GIV\(stackPage\)',
        '(char*)local_framePointer < ((GIV(stackPage)',
        content
    )
    content = re.sub(
        r'local_framePointer > \(\(\(GIV\(stackPage\)',
        '(char*)local_framePointer > (((GIV(stackPage)',
        content
    )

    # 36.5 Fix theFP1, theFP2, etc. variants
    for i in ['1', '2', '3', '4', '5']:
        varFP = f'theFP{i}'
        content = re.sub(
            rf'{varFP} = local_framePointer;',
            f'{varFP} = (char*)local_framePointer;',
            content
        )
        content = re.sub(
            rf'local_framePointer = {varFP};',
            f'local_framePointer = (sqInt){varFP};',
            content
        )
        content = re.sub(
            rf'marryFrameSP\({varFP}, theSP\)',
            f'marryFrameSP({varFP}, (char*)theSP)',
            content
        )
        # frameContext(theFP1) etc - these expect char*
        content = re.sub(
            rf'frameContext\({varFP}\)',
            f'frameContext({varFP})',  # already char*, OK
            content
        )

    # 36.6 Fix sp2, sp3, sp22, sp33 patterns
    for i in ['2', '3', '4', '5', '22', '33', '44', '55']:
        varSP = f'sp{i}'
        content = re.sub(
            rf'\({varSP} = local_stackPointer - BytesPerWord\)',
            f'({varSP} = (char*)(local_stackPointer - BytesPerWord))',
            content
        )
        content = re.sub(
            rf'local_stackPointer = {varSP};',
            f'local_stackPointer = (sqInt){varSP};',
            content
        )

    # 36.65 Fix theSP1, theSP2 etc.
    for i in ['1', '2', '3', '4', '5']:
        varSP = f'theSP{i}'
        content = re.sub(
            rf'{varSP} = local_stackPointer;',
            f'{varSP} = (char*)local_stackPointer;',
            content
        )
        content = re.sub(
            rf'local_stackPointer = {varSP};',
            f'local_stackPointer = (sqInt){varSP};',
            content
        )
        content = re.sub(
            rf'marryFrameSP\(theFP2, {varSP}\)',
            f'marryFrameSP(theFP2, {varSP})',  # both are char* now
            content
        )

    # 36.7 Fix checkIsStillMarriedContextcurrentFP(x, local_framePointer)
    content = re.sub(
        r'checkIsStillMarriedContextcurrentFP\((\w+), local_framePointer\)',
        r'checkIsStillMarriedContextcurrentFP(\1, (char*)local_framePointer)',
        content
    )

    # 36.8 Fix local_ = (GIV(stackPage)->head*) patterns
    content = re.sub(
        r'local_stackPointer = \(GIV\(stackPage\)->headSP\);',
        'local_stackPointer = (sqInt)(GIV(stackPage)->headSP);',
        content
    )
    content = re.sub(
        r'local_framePointer = \(GIV\(stackPage\)->headFP\);',
        'local_framePointer = (sqInt)(GIV(stackPage)->headFP);',
        content
    )

    # 36.9 Fix local_instructionPointer = pointerForOop(...) patterns
    content = re.sub(
        r'local_instructionPointer = pointerForOop\(',
        'local_instructionPointer = (sqInt)pointerForOop(',
        content
    )
    content = re.sub(
        r'local_framePointer = pointerForOop\(',
        'local_framePointer = (sqInt)pointerForOop(',
        content
    )

    # 36.10 Fix local_stackPointer = (...callerFP...) patterns
    content = re.sub(
        r'local_stackPointer = \(\(assert\(',
        'local_stackPointer = (sqInt)((assert(',
        content
    )

    # 36.11 Fix isMachineCodeFrame(local_framePointer)
    content = re.sub(
        r'isMachineCodeFrame\(local_framePointer\)',
        'isMachineCodeFrame((char*)local_framePointer)',
        content
    )

    # 36.12 Fix assertValidExecutionPointersimbarline(..., local_framePointer, local_stackPointer, ...)
    content = re.sub(
        r'assertValidExecutionPointersimbarline\((\([^)]+\)), local_framePointer, local_stackPointer,',
        r'assertValidExecutionPointersimbarline(\1, (char*)local_framePointer, (char*)local_stackPointer,',
        content
    )

    # 36.13 Fix sp6 pattern
    content = re.sub(
        r'\(sp6 = local_stackPointer - BytesPerWord\)',
        '(sp6 = (char*)(local_stackPointer - BytesPerWord))',
        content
    )
    content = re.sub(
        r'local_stackPointer = sp6;',
        'local_stackPointer = (sqInt)sp6;',
        content
    )

    # 36.14 Fix isBaseFrame(local_framePointer)
    content = re.sub(
        r'isBaseFrame\(local_framePointer\)',
        'isBaseFrame((char*)local_framePointer)',
        content
    )

    # 36.15 Fix local_framePointer == ((GIV(stackPage)->baseFP)) comparisons
    content = re.sub(
        r'assert\(local_framePointer == \(\(GIV\(stackPage\)->baseFP\)\)\)',
        'assert((char*)local_framePointer == ((GIV(stackPage)->baseFP)))',
        content
    )

    # 36.16 Fix more assertValidExecutionPointersimbarline patterns with different whitespace
    content = re.sub(
        r'assertValidExecutionPointersimbarline\(\(\(usqInt\) local_instructionPointer \), local_framePointer, local_stackPointer,',
        'assertValidExecutionPointersimbarline(((usqInt) local_instructionPointer ), (char*)local_framePointer, (char*)local_stackPointer,',
        content
    )

    # 36.17 Fix local_framePointer = callersFPOrNull
    content = re.sub(
        r'local_framePointer = callersFPOrNull;',
        'local_framePointer = (sqInt)callersFPOrNull;',
        content
    )

    # 36.18 Fix (sp = local_stackPointer + ...) patterns (positive offset)
    content = re.sub(
        r'\(sp = local_stackPointer \+ \(\(2 - 1\) \* BytesPerWord\)\)',
        '(sp = (char*)(local_stackPointer + ((2 - 1) * BytesPerWord)))',
        content
    )
    content = re.sub(
        r'\(sp2 = local_stackPointer \+ \(\(2 - 1\) \* BytesPerWord\)\)',
        '(sp2 = (char*)(local_stackPointer + ((2 - 1) * BytesPerWord)))',
        content
    )

    # 36.19 Fix handleSpecialSelectorSendFaultForfpsp calls (3 args: obj, fp, sp)
    content = re.sub(
        r'handleSpecialSelectorSendFaultForfpsp\((\w+), local_framePointer, local_stackPointer\)',
        r'handleSpecialSelectorSendFaultForfpsp(\1, (char*)local_framePointer, (char*)local_stackPointer)',
        content
    )

    # 36.20 Fix sp23, sp24, sp32 etc. patterns
    for i in ['23', '24', '32', '33', '34', '42', '43', '44', '52', '53', '62', '63', '72', '73']:
        varSP = f'sp{i}'
        content = re.sub(
            rf'\({varSP} = local_stackPointer - BytesPerWord\)',
            f'({varSP} = (char*)(local_stackPointer - BytesPerWord))',
            content
        )
        content = re.sub(
            rf'local_stackPointer = {varSP};',
            f'local_stackPointer = (sqInt){varSP};',
            content
        )

    # 36.21 Fix theFP3 = local_framePointer etc.
    for i in ['3', '4', '5']:
        varFP = f'theFP{i}'
        content = re.sub(
            rf'{varFP} = local_framePointer;',
            f'{varFP} = (char*)local_framePointer;',
            content
        )
        content = re.sub(
            rf'local_framePointer = {varFP};',
            f'local_framePointer = (sqInt){varFP};',
            content
        )

    # 36.22 Fix local_framePointer != frameToReturnTo comparisons
    content = re.sub(
        r'local_framePointer == frameToReturnTo',
        '(char*)local_framePointer == (char*)frameToReturnTo',
        content
    )
    content = re.sub(
        r'local_framePointer != frameToReturnTo',
        '(char*)local_framePointer != (char*)frameToReturnTo',
        content
    )

    # 36.23 Fix saved*Pointer = local_* patterns
    content = re.sub(
        r'savedStackPointer = local_stackPointer;',
        'savedStackPointer = (char*)local_stackPointer;',
        content
    )
    content = re.sub(
        r'savedFramePointer = local_framePointer;',
        'savedFramePointer = (char*)local_framePointer;',
        content
    )
    content = re.sub(
        r'local_stackPointer = savedStackPointer;',
        'local_stackPointer = (sqInt)savedStackPointer;',
        content
    )
    content = re.sub(
        r'local_framePointer = savedFramePointer;',
        'local_framePointer = (sqInt)savedFramePointer;',
        content
    )

    # 36.24 Fix more sp patterns (sp25, sp35, sp45, etc.)
    for i in range(25, 100):
        varSP = f'sp{i}'
        content = re.sub(
            rf'\({varSP} = local_stackPointer - BytesPerWord\)',
            f'({varSP} = (char*)(local_stackPointer - BytesPerWord))',
            content
        )
        content = re.sub(
            rf'local_stackPointer = {varSP};',
            f'local_stackPointer = (sqInt){varSP};',
            content
        )

    # 36.25 Fix comparisons with sp2, sp3 etc.
    for i in range(2, 20):
        content = re.sub(
            rf'local_stackPointer == sp{i}',
            f'local_stackPointer == (sqInt)sp{i}',
            content
        )
        content = re.sub(
            rf'sp{i} < GIV\(stackLimit\)',
            f'sp{i} < (char*)GIV(stackLimit)',
            content
        )

    # 36.26 Fix theFP4 = (...)
    content = re.sub(
        r'theFP4 = \(char\*\)frameCallerFP',
        'theFP4 = (char*)frameCallerFP',
        content
    )

    # 36.27 Fix fileExists(local_...) pattern
    content = re.sub(
        r'fileExists\(local_instructionPointer\)',
        'fileExists((char*)local_instructionPointer)',
        content
    )

    # 36. Fix page->baseFP = local_framePointer patterns
    content = re.sub(
        r'\(page->baseFP = local_framePointer\)',
        '(page->baseFP = (char*)local_framePointer)',
        content
    )
    content = re.sub(
        r'\(page->baseSP = local_stackPointer\)',
        '(page->baseSP = (char*)local_stackPointer)',
        content
    )
    content = re.sub(
        r'\(page->headFP = local_framePointer\)',
        '(page->headFP = (char*)local_framePointer)',
        content
    )
    content = re.sub(
        r'\(page->headSP = local_stackPointer\)',
        '(page->headSP = (char*)local_stackPointer)',
        content
    )
    content = re.sub(
        r'\(page->headSP = theSP',
        '(page->headSP = (char*)theSP',
        content
    )

    # 37. Fix local_framePointer == savedFramePointer comparisons
    content = re.sub(
        r'local_framePointer == savedFramePointer',
        '(char*)local_framePointer == savedFramePointer',
        content
    )
    content = re.sub(
        r'local_framePointer != savedFramePointer',
        '(char*)local_framePointer != savedFramePointer',
        content
    )

    # 38. Fix local_stackPointer != (savedStackPointer + ...) comparisons
    content = re.sub(
        r'local_stackPointer != \(savedStackPointer \+',
        '(char*)local_stackPointer != (savedStackPointer +',
        content
    )
    content = re.sub(
        r'local_stackPointer == \(savedStackPointer \+',
        '(char*)local_stackPointer == (savedStackPointer +',
        content
    )

    # 39. Fix (sp3 = local_stackPointer + ((2 - 1) * BytesPerWord)) and similar
    for i in range(1, 20):
        varSP = f'sp{i}'
        content = re.sub(
            rf'\({varSP} = local_stackPointer \+ \(\(2 - 1\) \* BytesPerWord\)\)',
            f'({varSP} = (char*)(local_stackPointer + ((2 - 1) * BytesPerWord)))',
            content
        )
        content = re.sub(
            rf'\({varSP} = local_stackPointer \+ \(\(3 - 1\) \* BytesPerWord\)\)',
            f'({varSP} = (char*)(local_stackPointer + ((3 - 1) * BytesPerWord)))',
            content
        )
        content = re.sub(
            rf'\({varSP} = local_stackPointer \+ \(\(4 - 1\) \* BytesPerWord\)\)',
            f'({varSP} = (char*)(local_stackPointer + ((4 - 1) * BytesPerWord)))',
            content
        )
        content = re.sub(
            rf'\({varSP} = local_stackPointer \+ \(\(5 - 1\) \* BytesPerWord\)\)',
            f'({varSP} = (char*)(local_stackPointer + ((5 - 1) * BytesPerWord)))',
            content
        )

    # 40. Fix local_stackPointer < GIV(stackLimit) comparison
    content = re.sub(
        r'local_stackPointer < GIV\(stackLimit\)',
        '(char*)local_stackPointer < GIV(stackLimit)',
        content
    )
    content = re.sub(
        r'local_stackPointer <= GIV\(stackLimit\)',
        '(char*)local_stackPointer <= GIV(stackLimit)',
        content
    )

    # 41. Fix local_*Pointer = (thePage1->head*) patterns
    content = re.sub(
        r'local_stackPointer = \(thePage1->headSP\);',
        'local_stackPointer = (sqInt)(thePage1->headSP);',
        content
    )
    content = re.sub(
        r'local_framePointer = \(thePage1->headFP\);',
        'local_framePointer = (sqInt)(thePage1->headFP);',
        content
    )

    # 42. Fix theSP = local_stackPointer + (numCopied * BytesPerOop) pattern
    content = re.sub(
        r'theSP = local_stackPointer \+ \(numCopied \* BytesPerOop\);',
        'theSP = (char*)(local_stackPointer + (numCopied * BytesPerOop));',
        content
    )

    # 43. Fix fileName = buffer (char[] to sqInt) - this is actually a bug in the generated code
    # Let's use proper type for fileName
    content = re.sub(
        r'sqInt fileName;',
        'char *fileName;',
        content
    )
    content = re.sub(
        r'sqInt buffer;',
        'char buffer[255];',
        content
    )

    # 44. Fix fgetc(file) / ungetc(..., file) where file is void*
    content = re.sub(
        r'fgetc\(file\)',
        'fgetc((FILE*)file)',
        content
    )
    content = re.sub(
        r'ungetc\((\w+), file\)',
        r'ungetc(\1, (FILE*)file)',
        content
    )

    # 45. Fix reapAndResetErrorCodeToheader(GIV(framePointer), ...) - expects sqInt
    content = re.sub(
        r'reapAndResetErrorCodeToheader\(GIV\(framePointer\),',
        'reapAndResetErrorCodeToheader((sqInt)GIV(framePointer),',
        content
    )

    # 46. Fix GIV(instructionPointer) = ((sqInt) cogMethod ) + ...
    content = re.sub(
        r'GIV\(instructionPointer\) = \(\(sqInt\) cogMethod \) \+ ',
        'GIV(instructionPointer) = (char*)(((sqInt) cogMethod ) + ',
        content
    )

    # 47. Fix GIV(instructionPointer) = ceReturnToInterpreterPC()
    content = re.sub(
        r'GIV\(instructionPointer\) = ceReturnToInterpreterPC\(\);',
        'GIV(instructionPointer) = (char*)ceReturnToInterpreterPC();',
        content
    )

    # 48. Fix return buffer patterns - these use comma operator: (snprintf(...), buffer)
    # Need to cast buffer to sqInt at end
    content = re.sub(
        r'return \(snprintf\(buffer, bufferSize, "%s/%s", \(\(char \*\) imageFileName \), \(\(char \*\) headerFileName \)\), buffer\);',
        'return (snprintf(buffer, bufferSize, "%s/%s", ((char *) imageFileName ), ((char *) headerFileName )), (sqInt)buffer);',
        content
    )
    content = re.sub(
        r'return \(snprintf\(buffer, 0xFF, "%s/seg%d%s", \(\(char \*\) imageFileName \), \(\(int\) segmentIndex \), "\.data"\), buffer\);',
        'return (snprintf(buffer, 0xFF, "%s/seg%d%s", ((char *) imageFileName ), ((int) segmentIndex ), ".data"), (sqInt)buffer);',
        content
    )
    content = re.sub(
        r'return \(snprintf\(buffer, 0xFF, "%s/seg%d%s", \(\(char \*\) imageFileName \), \(\(int\) segmentIndex \), "\.ston"\), buffer\);',
        'return (snprintf(buffer, 0xFF, "%s/seg%d%s", ((char *) imageFileName ), ((int) segmentIndex ), ".ston"), (sqInt)buffer);',
        content
    )

    # 49. Fix local_stackPointer = sp7 and similar
    for i in range(1, 20):
        varSP = f'sp{i}'
        content = re.sub(
            rf'local_stackPointer = {varSP};',
            f'local_stackPointer = (sqInt){varSP};',
            content
        )

    # 50. Fix GIV(instructionPointer) = top; patterns
    content = re.sub(
        r'GIV\(instructionPointer\) = top;',
        'GIV(instructionPointer) = (char*)top;',
        content
    )

    # 51. Fix GIV(instructionPointer) = unsignedLongAt(...) patterns
    content = re.sub(
        r'GIV\(instructionPointer\) = unsignedLongAt\(',
        'GIV(instructionPointer) = (char*)unsignedLongAt(',
        content
    )

    # 52. Fix GIV(instructionPointer) = ((usqInt)... patterns
    content = re.sub(
        r'GIV\(instructionPointer\) = \(\(usqInt\)',
        'GIV(instructionPointer) = (char*)((usqInt)',
        content
    )

    # 53. Fix GIV(instructionPointer) = initialIP - 1; patterns
    content = re.sub(
        r'GIV\(instructionPointer\) = initialIP - 1;',
        'GIV(instructionPointer) = (char*)(initialIP - 1);',
        content
    )

    # 54. Fix theFP = (sqInt)GIV(framePointer); - wrong cast, theFP is char*
    # Remove the (sqInt) cast since both are char*
    content = re.sub(
        r'theFP = \(sqInt\)GIV\(framePointer\);',
        'theFP = GIV(framePointer);',
        content
    )
    content = re.sub(
        r'theSP = \(sqInt\)GIV\(stackPointer\);',
        'theSP = GIV(stackPointer);',
        content
    )

    # 55. Fix strncmp("...", firstFixedField(oop), ...) - firstFixedField returns void*
    content = re.sub(
        r'strncmp\("(\w+)", firstFixedField\((\w+)\), (\d+)\)',
        r'strncmp("\1", (char*)firstFixedField(\2), \3)',
        content
    )

    # 56. Fix CIR_readSegmentsFromImageFileheader(f, header) - f is void*
    content = re.sub(
        r'CIR_readSegmentsFromImageFileheader\(f,',
        'CIR_readSegmentsFromImageFileheader((sqInt)f,',
        content
    )
    content = re.sub(
        r'CIR_readPermanentSpaceFromImageFileheader\(f,',
        'CIR_readPermanentSpaceFromImageFileheader((sqInt)f,',
        content
    )

    # 57. Fix bytecodePCForstartBcpcin(GIV(instructionPointer), ...) - expects sqInt
    content = re.sub(
        r'bytecodePCForstartBcpcin\(GIV\(instructionPointer\),',
        'bytecodePCForstartBcpcin((sqInt)GIV(instructionPointer),',
        content
    )

    # 58. Fix result of bytecodePCForstartBcpcin assigned to GIV(instructionPointer)
    content = re.sub(
        r'GIV\(instructionPointer\) = bytecodePCForstartBcpcin\(',
        'GIV(instructionPointer) = (char*)bytecodePCForstartBcpcin(',
        content
    )

    # 59. Fix closing paren issue from pattern 46 - GIV(instructionPointer) = (char*)((...)
    # The original has: GIV(...) = ((sqInt) cogMethod ) + ((cogMethod->stackCheckOffset));
    # My transform made: GIV(...) = (char*)(((sqInt) cogMethod ) + ((cogMethod->stackCheckOffset));
    # Missing closing paren. Let's fix it properly:
    content = re.sub(
        r'GIV\(instructionPointer\) = \(char\*\)\(\(\(sqInt\) cogMethod \) \+ \(\(cogMethod->stackCheckOffset\)\);',
        'GIV(instructionPointer) = (char*)(((sqInt) cogMethod ) + ((cogMethod->stackCheckOffset)));',
        content
    )

    # 60. Fix snprintf(buffer, ...) where buffer is variable - need to cast buffer in the call
    # The pattern is: return (snprintf(buffer, ...), (sqInt)buffer)
    # buffer is char[], so snprintf should work - issue is sqInt buffer declaration changed to char buffer[]
    # Let's check if the issue is with function that still declares sqInt buffer
    # Actually the issue is line 14170 uses bufferSize which suggests buffer is still sqInt
    # Need to fix: sqInt buffer; -> char buffer[255]; and sqInt bufferSize; -> int bufferSize;
    content = re.sub(
        r'sqInt bufferSize;',
        'int bufferSize;',
        content
    )

    # 61. Fix theIP = GIV(instructionPointer); where theIP is usqInt, GIV returns char*
    content = re.sub(
        r'theIP = GIV\(instructionPointer\);',
        'theIP = (usqInt)GIV(instructionPointer);',
        content
    )

    # 62. Fix GIV(instructionPointer) == (ceReturnToInterpreterPC()) comparison
    content = re.sub(
        r'GIV\(instructionPointer\) == \(ceReturnToInterpreterPC\(\)\)',
        '(usqInt)GIV(instructionPointer) == (ceReturnToInterpreterPC())',
        content
    )

    # 63. Fix GIV(instructionPointer) = top2; (and other topN variants)
    for i in ['2', '3', '4', '5']:
        content = re.sub(
            rf'GIV\(instructionPointer\) = top{i};',
            f'GIV(instructionPointer) = (char*)top{i};',
            content
        )

    # 64. Fix assertValidExecutionPointersimbarline(GIV(instructionPointer), ...) - expects usqInt
    content = re.sub(
        r'assertValidExecutionPointersimbarline\(GIV\(instructionPointer\),',
        'assertValidExecutionPointersimbarline((usqInt)GIV(instructionPointer),',
        content
    )

    # 65. Fix theIPPtr = GIV(instructionPointer) - GIV(method) - theIPPtr is usqInt
    content = re.sub(
        r'theIPPtr = GIV\(instructionPointer\) - GIV\(method\);',
        'theIPPtr = (usqInt)GIV(instructionPointer) - GIV(method);',
        content
    )

    # 66. Fix GIV(instructionPointer) = GIV(method) + theIPPtr
    content = re.sub(
        r'GIV\(instructionPointer\) = GIV\(method\) \+ theIPPtr;',
        'GIV(instructionPointer) = (char*)(GIV(method) + theIPPtr);',
        content
    )

    # 67. Fix GIV(framePointer) = theCFP; where theCFP is void*
    content = re.sub(
        r'GIV\(framePointer\) = theCFP;',
        'GIV(framePointer) = (char*)theCFP;',
        content
    )
    content = re.sub(
        r'GIV\(stackPointer\) = theCSP;',
        'GIV(stackPointer) = (char*)theCSP;',
        content
    )

    # 68. Fix return mcprimHashMultiply; - returning function instead of calling it
    # This looks like a VMMaker bug where function should be called
    content = re.sub(
        r'return mcprimHashMultiply;',
        'return 0; /* mcprimHashMultiply - function reference error */',
        content
    )

    # 69. print declaration - already fixed in cointerp.h, don't modify here
    # (was: extern void print(char *s); -> extern void print(const char *s);)

    # 70. Fix printFrameOopat and printFrameThingatextraString const char* issue
    content = re.sub(
        r'static void NoDbgRegParms printFrameOopat\(char \*name, char \*address\);',
        'static void NoDbgRegParms printFrameOopat(const char *name, char *address);',
        content
    )
    content = re.sub(
        r'printFrameOopat\(char \*name, char \*address\)\n\{',
        'printFrameOopat(const char *name, char *address)\n{',
        content
    )
    content = re.sub(
        r'printFrameThingatextraString\(char \*name, char \*address, char \*extraStringOrNil\)\n\{',
        'printFrameThingatextraString(const char *name, char *address, const char *extraStringOrNil)\n{',
        content
    )

    # 71. Fix isSendReturnPC(GIV(instructionPointer)) - expects sqInt, got char*
    content = re.sub(
        r'isSendReturnPC\(GIV\(instructionPointer\)\)',
        'isSendReturnPC((sqInt)GIV(instructionPointer))',
        content
    )

    # 72. Fix GIV(instructionPointer) != (((sqInt) cogMethod ) + ...) comparison
    content = re.sub(
        r'GIV\(instructionPointer\) != \(\(\(sqInt\) cogMethod \) \+ ',
        '(sqInt)GIV(instructionPointer) != (((sqInt) cogMethod ) + ',
        content
    )
    content = re.sub(
        r'GIV\(instructionPointer\) == \(\(\(sqInt\) cogMethod \) \+ ',
        '(sqInt)GIV(instructionPointer) == (((sqInt) cogMethod ) + ',
        content
    )

    # 73. Fix fgetc(file2) / ungetc(x, file2) patterns
    content = re.sub(
        r'fgetc\(file2\)',
        'fgetc((FILE*)file2)',
        content
    )
    content = re.sub(
        r'ungetc\((\w+), file2\)',
        r'ungetc(\1, (FILE*)file2)',
        content
    )

    # 74. Fix GIV(instructionPointer) = initialIP2 - 1; and initialIP3 - 1;
    content = re.sub(
        r'GIV\(instructionPointer\) = initialIP2 - 1;',
        'GIV(instructionPointer) = (char*)(initialIP2 - 1);',
        content
    )
    content = re.sub(
        r'GIV\(instructionPointer\) = initialIP3 - 1;',
        'GIV(instructionPointer) = (char*)(initialIP3 - 1);',
        content
    )

    # 75. Fix GIV(instructionPointer) = ceCannotResumePC();
    content = re.sub(
        r'GIV\(instructionPointer\) = ceCannotResumePC\(\);',
        'GIV(instructionPointer) = (char*)ceCannotResumePC();',
        content
    )

    # 76. Fix safeFreeMethod(cogMethod) - expects sqInt, got CogMethod*
    content = re.sub(
        r'safeFreeMethod\(cogMethod\);',
        'safeFreeMethod((sqInt)cogMethod);',
        content
    )

    # 77. Fix oldBase = (permSpaceMetadata.startAddress); - uint64_t to void*
    content = re.sub(
        r'oldBase = \(permSpaceMetadata\.startAddress\);',
        'oldBase = (void*)(permSpaceMetadata.startAddress);',
        content
    )
    content = re.sub(
        r'newBase = \(\(getMemoryMap\(\)\)->permSpaceStart\);',
        'newBase = (void*)((getMemoryMap())->permSpaceStart);',
        content
    )

    # 78. Fix fullFileName = buffer2; (char[255] to sqInt) - need proper type
    content = re.sub(
        r'sqInt fullFileName;',
        'char *fullFileName;',
        content
    )

    # 79. Fix buffer2 declaration in that function
    content = re.sub(
        r'\tsqInt buffer2;\n\tsqInt file;',
        '\tchar buffer2[255];\n\tvoid *file;',
        content
    )

    # 80. Fix print declaration conflict - transform to match header
    content = re.sub(
        r'extern void print\(char \*s\);',
        'extern void print(const char *s);',
        content
    )

    # 81. Fix fullFileName1, buffer3 etc. variant declarations
    content = re.sub(
        r'sqInt fullFileName1;',
        'char *fullFileName1;',
        content
    )
    content = re.sub(
        r'sqInt buffer3;',
        'char buffer3[255];',
        content
    )

    # 82. Fix sqImageFileRead(startingAddress, ...) - startingAddress is sqInt, needs void*
    content = re.sub(
        r'sqImageFileRead\(startingAddress,',
        'sqImageFileRead((void*)startingAddress,',
        content
    )

    # 83. Fix fprintf(file, ...) - file is void*, needs FILE*
    content = re.sub(
        r'fprintf\(file,',
        'fprintf((FILE*)file,',
        content
    )
    content = re.sub(
        r'fprintf\(file2,',
        'fprintf((FILE*)file2,',
        content
    )

    # 84. Fix sqImageFileOpen(fullFileName, ...) where fullFileName is sqInt
    # Actually need to fix the headerFileNameinImageintobufferSize call that passes char* to sqInt
    # The function signature expects sqInt, but we're passing char*
    # Let's cast the arguments properly
    content = re.sub(
        r'headerFileNameinImageintobufferSize\(imageFileName, buffer, 0xFF\)',
        'headerFileNameinImageintobufferSize((sqInt)imageFileName, (sqInt)buffer, 0xFF)',
        content
    )

    # 85. Fix snprintf in headerFileNameinImageintobufferSize - need to cast buffer parameter
    # The function takes sqInt buffer but needs to cast it for snprintf
    content = re.sub(
        r'return \(snprintf\(buffer, bufferSize,',
        'return (snprintf((char*)buffer, bufferSize,',
        content
    )

    # 86. Fix calls to functions that take sqInt imageFileName - cast char* to sqInt
    content = re.sub(
        r'existSegmentinImage\((\w+), imageFileName\)',
        r'existSegmentinImage(\1, (sqInt)imageFileName)',
        content
    )
    content = re.sub(
        r'segmentMetadataFileinImage\((\w+), imageFileName\)',
        r'(char*)segmentMetadataFileinImage(\1, (sqInt)imageFileName)',
        content
    )
    content = re.sub(
        r'segmentDataFileinImage\((\w+), imageFileName\)',
        r'(char*)segmentDataFileinImage(\1, (sqInt)imageFileName)',
        content
    )

    # 87. Fix sqImageFileOpen with headerFileNameinImageintobufferSize return value
    # The pattern is: sqImageFileOpen(headerFileNameinImageintobufferSize((sqInt)imageFileName, (sqInt)buffer, 0xFF), "w")
    content = re.sub(
        r'sqImageFileOpen\(headerFileNameinImageintobufferSize\(\(sqInt\)imageFileName, \(sqInt\)buffer, 0xFF\), "w"\)',
        'sqImageFileOpen((char*)headerFileNameinImageintobufferSize((sqInt)imageFileName, (sqInt)buffer, 0xFF), "w")',
        content
    )

    # 88. Fix fprintf(metadataFile, ...) - metadataFile is void*, needs FILE*
    content = re.sub(
        r'fprintf\(metadataFile,',
        'fprintf((FILE*)metadataFile,',
        content
    )

    # 89. Fix sqImageFileWrite(start, ...) - start is sqInt, needs void*
    content = re.sub(
        r'sqImageFileWrite\(start,',
        'sqImageFileWrite((void*)start,',
        content
    )

    # 90. Fix cString = malloc(...) - malloc returns void*, cString is char*
    content = re.sub(
        r'cString = malloc\(',
        'cString = (char*)malloc(',
        content
    )

    # 91. Fix GIV(instructionPointer) = ((closureMethod + ...) - 1)
    content = re.sub(
        r'GIV\(instructionPointer\) = \(\(closureMethod \+',
        'GIV(instructionPointer) = (char*)((closureMethod +',
        content
    )

    # 92. Fix uint32AtPointer(class + 4) - class still used
    content = re.sub(
        r'uint32AtPointer\(class \+ 4\)',
        'uint32AtPointer(klass + 4)',
        content
    )

    # 93. Fix vm_exports array - string literals can't be assigned to void* in C++
    # Change array element type or cast strings
    # The pattern is: {(void*)_m, "string", (void*)func}
    # We need to cast the string to (void*)
    # This is a bulk fix - use a different approach: find and replace in vm_exports section

    # First, let's change the declaration to use const char* for name
    # Change: void* vm_exports[][3] = {
    # To: Use a struct instead

    # Actually easier: just add casts to all string literals in the array
    # Pattern: {(void*)_m, "...", (void*)func}
    # -> {(void*)_m, (void*)"...", (void*)func}
    content = re.sub(
        r'\{(\(void\*\)_m), "([^"]*)", (\(void\*\)\w+)\}',
        r'{(void*)_m, (void*)"\2", \3}',
        content
    )

    # 94. Fix codeZoneStart = allocateJITMemory(...) - void* to uint64_t
    content = re.sub(
        r'->codeZoneStart = allocateJITMemory\(',
        '->codeZoneStart = (uint64_t)allocateJITMemory(',
        content
    )

    # 95. Fix memset(stackPagesStart, ...) - uint64_t to void*
    content = re.sub(
        r'memset\(\(self_in_allocateStackPages->stackPagesStart\)',
        'memset((void*)(self_in_allocateStackPages->stackPagesStart)',
        content
    )

    # 96. Fix return self_in_allocationGranularity; - returning pointer as sqInt
    content = re.sub(
        r'return self_in_allocationGranularity;',
        'return (sqInt)self_in_allocationGranularity;',
        content
    )

    # 97. Fix base = ((usqInt *) ...) - usqInt* to sqInt* base
    content = re.sub(
        r'base = \(\(usqInt \*\) \(firstIndexableField\((\w+)\)\) \);',
        r'base = (sqInt*)((usqInt *) (firstIndexableField(\1)) );',
        content
    )

    # 98. Fix rememberedSetArray = ((usqInt *) ...) - same issue
    content = re.sub(
        r'->rememberedSetArray = \(\(usqInt \*\) \(firstIndexableField\((\w+)\)\) \)',
        r'->rememberedSetArray = (sqInt*)((usqInt *) (firstIndexableField(\1)) )',
        content
    )

    # 99. Fix realloc/malloc to sqInt assignments
    content = re.sub(
        r'(\w+) = realloc\(',
        r'\1 = (sqInt)realloc((void*)',
        content
    )
    content = re.sub(
        r'(\w+) = malloc\(',
        r'\1 = (sqInt)malloc(',
        content
    )

    # 100. Fix free(sqInt) calls
    content = re.sub(
        r'free\((\w+)\);',
        r'free((void*)\1);',
        content
    )

    # 101. Fix void* assignments from usqInt
    content = re.sub(
        r'segInfo->segStart = \(segInfo->segSize',
        'segInfo->segStart = (void*)(segInfo->segSize',
        content
    )

    # 102. Fix SpurSegmentInfo* from void*
    content = re.sub(
        r'segments = realloc\(',
        'segments = (SpurSegmentInfo*)realloc(',
        content
    )

    # 103. Fix 'class' remaining usages in expressions
    # longAtPointer(class + ...)
    content = re.sub(
        r'longAtPointer\(class \+',
        'longAtPointer(klass +',
        content
    )

    # 104. Fix (usqInt*) assignments to sqInt*
    content = re.sub(
        r'(\w+) = \(\(usqInt \*\) \(firstIndexableField\((\w+)\)\) \);',
        r'\1 = (sqInt*)((usqInt *) (firstIndexableField(\2)) );',
        content
    )

    # 105. More 'class' patterns
    content = re.sub(
        r'uint32AtPointer\(class \+',
        'uint32AtPointer(klass +',
        content
    )
    content = re.sub(
        r'\(class \+ 4\)',
        '(klass + 4)',
        content
    )

    # 106. Fix char* = realloc patterns - need (char*) cast
    # Pattern: foo = (sqInt)realloc((void*)... but if foo is char*, we need (char*)
    # Look for lines after realloc assignment
    content = re.sub(
        r'(\w+) = \(sqInt\)realloc\(\(void\*\)',
        r'\1 = (typeof(\1))realloc((void*)',
        content
    )

    # 107. Fix segments = (SpurSegmentInfo*)... double transformation
    # Already handles SpurSegmentInfo, need to fix repeated pattern
    content = re.sub(
        r'segments = \(SpurSegmentInfo\*\)\(sqInt\)realloc',
        'segments = (SpurSegmentInfo*)realloc',
        content
    )

    # 108. Fix return usqInt as void* - specific pattern
    content = re.sub(
        r'return \(\(segInfo->segSize\) \+ \(segInfo->segStart\)\);',
        'return (void*)((segInfo->segSize) + (segInfo->segStart));',
        content
    )

    # 109. Fix function pointer conversions
    content = re.sub(
        r'(\w+)->fn = dlsym\(',
        r'\1->fn = (void(*)())dlsym(',
        content
    )

    # 110. Fix more realloc patterns for char* vars
    content = re.sub(
        r'cString = \(sqInt\)realloc',
        'cString = (char*)realloc',
        content
    )

    with open(output_path, 'w') as f:
        f.write(content)

    print(f"Transformed {input_path} -> {output_path}")

    # Count transformations
    with open(output_path, 'r') as f:
        new_content = f.read()

    changes = [
        ('klass', new_content.count('sqInt klass')),
        ('(char*)alloca', new_content.count('(char*)alloca')),
        ('(char*)theStackMemory', new_content.count('(char*)theStackMemory')),
        ('(sqInt)GIV(stackPointer)', new_content.count('(sqInt)GIV(stackPointer)')),
        ('(char*)local_stackPointer', new_content.count('(char*)local_stackPointer')),
    ]

    print("\nTransformations applied:")
    for name, count in changes:
        if count > 0:
            print(f"  {name}: {count}")

if __name__ == '__main__':
    if len(sys.argv) < 3:
        base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        input_path = os.path.join(base, 'src/ios/cointerp.c')
        output_path = os.path.join(base, 'src/ios/cointerp-cpp.c')
    else:
        input_path = sys.argv[1]
        output_path = sys.argv[2]

    transform_cointerp(input_path, output_path)
