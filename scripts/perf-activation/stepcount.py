import lldb, collections
def run(debugger, command, result, internal_dict):
    target = debugger.GetSelectedTarget()
    proc = target.GetProcess()
    thread = proc.GetSelectedThread()
    counts = collections.Counter()
    seq = []
    n = int(command) if command else 4000
    for i in range(n):
        frame = thread.GetFrameAtIndex(0)
        sym = frame.GetSymbol().GetName() or frame.GetFunctionName() or "?"
        counts[sym] += 1
        if not seq or seq[-1][0] != sym:
            seq.append([sym, 1])
        else:
            seq[-1][1] += 1
        thread.StepInstruction(False)
    out = open("/tmp/stepcount.txt", "w")
    for k, v in counts.most_common(60):
        out.write("%6d %s\n" % (v, k))
    out.write("\n--- sequence ---\n")
    for s, c in seq[:400]:
        out.write("%5d %s\n" % (c, s))
    out.close()
def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand('command script add -f stepcount.run stepcount')
