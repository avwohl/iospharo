"BUG-2 minimal corruption repro. Run via run.sh corrupt (which sets the env).
With PHARO_T1_X86_J2J_SEL=asString PHARO_T1_X86_XMETHOD_SENDS=1 PHARO_DET_SCHED=1
this deterministically produces:
   PragmaMenuAndShortcutRegistrationItem(Object)>>doesNotUnderstand: #asForm
   ... and ... #capitalized rcvr=... cls=ByteString class
i.e. a send-bearing cross-method inline-J2J resume swaps a string-producing value
to a CLASS (ByteString class). No EVAL-RESULT is printed (result lost).
A bare '3 + 4' eval is enough — the bug is in startup methods (asString variants),
not in any user code here; this expression just gives the runner something to do."

3 + 4
