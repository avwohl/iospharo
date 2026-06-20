"tier-1 JIT bug repro — deep-recursion operand corruption.
 Surfaced by ArrayTest>>testPrintingRecursive.

 printString of a SELF-REFERENTIAL array recurses ~15000 levels (printOn: ->
 element printOn: -> ... ) to fill the ~50000-char print limit.  Under the
 tier-1 JIT an operand is corrupted deep in the recursion: a boolean slot for a
 conditional jump gets a non-boolean, raising NonBooleanReceiver (or, in other
 configs, MessageNotUnderstood: True/False) — the signature SHIFTS with JIT
 config, the hallmark of operand corruption.

 JIT-CONFIRMED: passes under PHARO_NO_JIT=1, fails under the JIT.
 NOT isolatable by knobs: NO_SISTA / NO_INLINE_J2J / XMETHOD_MAX_IC=0 /
 NO_CAN_SKIP_J2J_SAVE / SP_IN_X25=0 / FSR_LAZY=0 / NO_NATIVE_BACKJUMP all still
 fail (they only SHIFT the error).  Only NO_JIT fixes it => a fundamental
 deep-recursion corruption in tier-1 base codegen, not one feature.  The
 signaler-context walk shows 0x300000000 (corrupted frames), same family as the
 STON deep-recursion issue.  Needs lldb (break at the mustBeBoolean / non-boolean
 conditional-jump site, walk the JIT frames) to pin the corrupting instruction.

 Run:  build-rel/test_load_image IMAGE eval <this file>
 Want: RECPRINT OK size=<n>.  Bug: RECPRINT EXC NonBooleanReceiver..."

| log a b r |
log := Stdio stdout.
a := Array new: 3. b := Array new: 3.
a at: 1 put: a; at: 2 put: 2; at: 3 put: 3.
r := [ 'OK size=', a printString size printString ]
  on: Exception do: [:e | 'EXC ', e class name ].
log nextPutAll: 'RECPRINT direct ', r; lf.
a at: 1 put: b. b at: 1 put: a.   "indirect (mutual) recursion"
r := [ 'OK size=', a printString size printString ]
  on: Exception do: [:e | 'EXC ', e class name ].
log nextPutAll: 'RECPRINT indirect ', r; lf; flush.
