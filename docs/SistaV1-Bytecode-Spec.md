# Sista V1 Bytecode Specification

Source: `EncoderForSistaV1.class.st` from the Pharo source code.

EncoderForSistaV1 encodes a bytecode set for Sista, the Speculative Inlining Smalltalk Architecture, a project by Clement Bera and Eliot Miranda.

Bytecodes are ordered by length to make decoding easier. Bytecodes marked with `*` are extensible via a prefix bytecode. Bytecodes marked with `**` are extensible via both Extend A and Extend B.

**Important:** Extension bytecodes can only come before extensible bytecodes. An extensible bytecode consumes (and zeros) its extension(s).

---

## 1 Byte Bytecodes (0x00-0xDB / 0-219)

| Code | Binary | Description |
|------|--------|-------------|
| 0-15 (0x00-0x0F) | `0000 iiii` | Push Receiver Variable #iiii |
| 16-31 (0x10-0x1F) | `0001 iiii` | Push Literal Variable #iiii |
| 32-63 (0x20-0x3F) | `001 iiiii` | Push Literal #iiiii |
| 64-71 (0x40-0x47) | `01000 iii` | Push Temp #iii |
| 72-75 (0x48-0x4B) | `010010 ii` | Push Temp #ii + 8 |
| 76 (0x4C) | `01001100` | Push Receiver |
| 77 (0x4D) | `01001101` | Push true |
| 78 (0x4E) | `01001110` | Push false |
| 79 (0x4F) | `01001111` | Push nil |
| 80 (0x50) | `01010000` | Push 0 |
| 81 (0x51) | `01010001` | Push 1 |
| 82* (0x52) | `01010010` | Push thisContext (Extend B = 1 => push thisProcess) |
| 83 (0x53) | `01010011` | Duplicate Stack Top |
| 84-87 (0x54-0x57) | `010101 ii` | UNASSIGNED |
| 88-91 (0x58-0x5B) | `010110 ii` | Return Receiver/true/false/nil |
| 92 (0x5C) | `01011100` | Return top |
| 93 (0x5D) | `01011101` | BlockReturn nil |
| 94* (0x5E) | `01011110` | BlockReturn Top (return from enclosing block N, N = Extend A, jump by Ext B) |
| 95* (0x5F) | `01011111` | Nop |
| 96-111 (0x60-0x6F) | `0110 iiii` | Send Arithmetic Message #iiii |
| 112-127 (0x70-0x7F) | `0111 iiii` | Send Special Message #iiii |
| 128-143 (0x80-0x8F) | `1000 iiii` | Send Literal Selector #iiii With 0 Arguments |
| 144-159 (0x90-0x9F) | `1001 iiii` | Send Literal Selector #iiii With 1 Argument |
| 160-175 (0xA0-0xAF) | `1010 iiii` | Send Literal Selector #iiii With 2 Arguments |
| 176-183 (0xB0-0xB7) | `10110 iii` | Jump iii + 1 (1 through 8) |
| 184-191 (0xB8-0xBF) | `10111 iii` | Pop and Jump On True iii + 1 (1 through 8) |
| 192-199 (0xC0-0xC7) | `11000 iii` | Pop and Jump On False iii + 1 (1 through 8) |
| 200-207 (0xC8-0xCF) | `11001 iii` | Pop and Store Receiver Variable #iii |
| 208-215 (0xD0-0xD7) | `11010 iii` | Pop and Store Temporary Variable #iii |
| 216 (0xD8) | `11011000` | Pop Stack Top |
| 217 (0xD9) | `11011001` | Unconditional trap [Sista specific] |
| 218-219 (0xDA-0xDB) | `1101101 i` | UNASSIGNED |
| 220-223 (0xDC-0xDF) | `110111 ii` | UNASSIGNED |

### Arithmetic Selectors (for bytecode 0x60-0x6F)
```
#(#+ #- #< #> #<= #>= #= #~= #* #/ #\\ #@ #bitShift: #// #bitAnd: #bitOr:)
```

### Special Selectors (for bytecode 0x70-0x7F)
```
#(#at: #at:put: #size #next #nextPut: #atEnd #== class #~~ #value #value: #do: #new #new: #x #y)
```

---

## 2 Byte Bytecodes (0xE0-0xF7 / 224-247)

| Code | Binary | Byte 2 | Description |
|------|--------|--------|-------------|
| 224* (0xE0) | `11100000` | `aaaaaaaa` | Extend A (unsigned: Ext A = Ext A prev * 256 + a) |
| 225* (0xE1) | `11100001` | `bbbbbbbb` | Extend B (signed: Ext B = Ext B prev * 256 + b) |
| 226* (0xE2) | `11100010` | `iiiiiiii` | Push Receiver Variable #i (+ Extend A * 256) |
| 227* (0xE3) | `11100011` | `iiiiiiii` | Push Literal Variable #i (+ Extend A * 256) |
| 228* (0xE4) | `11100100` | `iiiiiiii` | Push Literal #i (+ Extend A * 256) |
| 229 (0xE5) | `11100101` | `iiiiiiii` | Push Temporary Variable #i |
| 230 (0xE6) | `11100110` | `iiiiiiii` | UNASSIGNED (was pushNClosureTemps) |
| 231 (0xE7) | `11100111` | `jkkkkkkk` | j=0: Push (Array new: k); j=1: Pop k into (Array new: k) |
| 232* (0xE8) | `11101000` | `iiiiiiii` | Push Integer #i (+ Extend B * 256, signed) |
| 233* (0xE9) | `11101001` | `iiiiiiii` | Push Character #i (+ Extend B * 256) |
| 234** (0xEA) | `11101010` | `iiiiijjj` | Send Literal Selector #iiiii (+ ExtA*32) with jjj (+ ExtB*8) args |
| 235** (0xEB) | `11101011` | `iiiiijjj` | Send To Superclass (see note 1) |
| 236 (0xEC) | `11101100` | `iiiiiiii` | Call Mapped Inlined Primitive #i [Sista specific] |
| 237* (0xED) | `11101101` | `iiiiiiii` | Jump #i (+ Extend B * 256, signed) |
| 238** (0xEE) | `11101110` | `iiiiiiii` | Pop and Jump On True #i (+ Extend B * 256) |
| 239** (0xEF) | `11101111` | `iiiiiiii` | Pop and Jump On False #i (+ Extend B * 256) |
| 240* (0xF0) | `11110000` | `iiiiiiii` | Pop and Store Receiver Variable #i (+ Extend A * 256) |
| 241* (0xF1) | `11110001` | `iiiiiiii` | Pop and Store Literal Variable #i (+ Extend A * 256) |
| 242 (0xF2) | `11110010` | `iiiiiiii` | Pop and Store Temporary Variable #i |
| 243* (0xF3) | `11110011` | `iiiiiiii` | Store Receiver Variable #i (+ Extend A * 256) |
| 244* (0xF4) | `11110100` | `iiiiiiii` | Store Literal Variable #i (+ Extend A * 256) |
| 245 (0xF5) | `11110101` | `iiiiiiii` | Store Temporary Variable #i |
| 246-247 (0xF6-0xF7) | `1111011 i` | `xxxxxxxx` | UNASSIGNED |

---

## 3 Byte Bytecodes (0xF8-0xFF / 248-255)

| Code | Binary | Byte 2 | Byte 3 | Description |
|------|--------|--------|--------|-------------|
| 248 (0xF8) | `11111000` | `iiiiiiii` | `mssjjjjj` | Call Primitive (see note 2) |
| 249 (0xF9) | `11111001` | `xxxxxxxx` | `siyyyyyy` | Push FullBlockClosure (literal index x + ExtA*256, numCopied y, s=receiverOnStack, i=ignoreOuterContext) |
| 250** (0xFA) | `11111010` | `eeiiikkk` | `jjjjjjjj` | Push Closure (numCopied iii + ExtA//16*8, numArgs kkk + ExtA\\16*8, blockSize j + ExtB*256, ee=num extensions) |
| 251 (0xFB) | `11111011` | `kkkkkkkk` | `jjjjjjjj` | Push Temp At k In Temp Vector At j |
| 252* (0xFC) | `11111100` | `kkkkkkkk` | `jjjjjjjj` | Store Temp At k In Temp Vector At j |
| 253* (0xFD) | `11111101` | `kkkkkkkk` | `jjjjjjjj` | Pop and Store Temp At k In Temp Vector At j |
| 254 (0xFE) | `11111110` | `kkkkkkkk` | `jjjjjjjj` | UNASSIGNED |
| 255 (0xFF) | `11111111` | `xxxxxxxx` | `jjjjjjjj` | UNASSIGNED |

---

## Notes

### (1) Bytecode 235 - Super Send
Bytecode 235 is a super send bytecode with two forms:
- **Normal form (ExtB < 64):** Lookup starts in superclass of the method's methodClassAssociation (last literal)
- **Directed form (ExtB >= 64):** Lookup starts in class on top of stack

### (2) Bytecode 248 - Call Primitive
Format: `iiiiiiii mssjjjjj`
- Primitive index = iiiiiiii + (jjjjj * 256)
- m=0: Normal primitive from table, returns from method on success
- m=1: Inlined primitive, does not return, yields result on stack
- ss: Operation set (0=sista unsafe, 1=lowcode)
- This bytecode is only valid as the first bytecode of a method

### [Sista specific]
Bytecodes 217 (trap), 236 (mapped inlined primitive), and 248 with m=1 (inlined primitives) are not used in the default Pharo runtime but only in specific circumstances (Sista runtime, etc.)

---

## Inlined Primitives (for bytecode 0xEC / 236)

Primitives are sorted by arity:
- 0-999: Nullary
- 1000-1999: Unary
- 2000-2999: Binary
- 3000-5999: Higher arity
- 6000+: Jumps

### Common Unsafe Operations (ss=00)

| Index | Name | Description |
|-------|------|-------------|
| 1000 | rawClass | Class of non-forwarder |
| 1001 | numSlots | Pointer object slot count |
| 1002 | numBytes | Byte object byte count |
| 2000 | smiAdd: | SmallInteger addition (no overflow) |
| 2001 | smiSub: | SmallInteger subtraction (no overflow) |
| 2002 | smiMul: | SmallInteger multiplication (no overflow) |
| 2016 | smiBitAnd: | SmallInteger bitwise AND |
| 2017 | smiBitOr: | SmallInteger bitwise OR |
| 2032 | smiGreater: | SmallInteger > comparison |
| 2033 | smiLess: | SmallInteger < comparison |
| 2064 | pointerAt: | Fetch slot from pointer object |
| 3000 | pointerAt:put: | Store slot in pointer object |

---

## References

- [Sista Project Blog - Clement Bera](https://clementbera.wordpress.com/2016/06/09/an-update-on-the-sista-project/)
- [FullBlockClosure Design](https://clementbera.wordpress.com/2016/06/27/fullblockclosure-design/)
- [A bytecode set for adaptive optimizations (IWST'14)](https://inria.hal.science/hal-01088801/document)
- [OpenSmalltalk VM GitHub](https://github.com/OpenSmalltalk/opensmalltalk-vm)
