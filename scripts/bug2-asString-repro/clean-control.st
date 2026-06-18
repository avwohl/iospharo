"BUG-2 CLEAN CONTROL — the same nested send-bearing cross-method shape, but
isolated to x-prefixed selectors so startup stays leaf-only. Run via run.sh clean.
With PHARO_T1_X86_J2J_SEL='x*' PHARO_T1_X86_XMETHOD_SENDS=1 this computes CORRECTLY:
   EVAL-RESULT='xfib10=143'
This PROVES the nested send-bearing mechanism (xfib -> xinc -> xmax:, all three
inline-J2J'd and returning via the V2 prelude) is SOUND. So the bug is NOT the
mechanism — it is a specific real method (#asString) interacting with something
the clean loop never triggers (suspected: a GC between admit and resume).
Vary this (1-arg callee, class-literal inner receiver, inner-send-bails, deeper
nesting) — ALL stay correct; see the README 'Ruled out' list before re-trying."

Integer compile: 'xmax: a ^self > a ifTrue: [self] ifFalse: [a]'.
Integer compile: 'xinc ^(self + 1) xmax: 0'.
Integer compile: 'xfib ^self < 2 ifTrue: [self] ifFalse: [((self - 1) xfib + (self - 2) xfib) xinc]'.
'xfib10=', (10 xfib) printString
