import lldb, collections
def run(debugger, command, result, internal_dict):
    target = debugger.GetSelectedTarget()
    thread = target.GetProcess().GetSelectedThread()
    lines = collections.Counter(); seq = []
    n = int(command) if command else 3000
    for i in range(n):
        f = thread.GetFrameAtIndex(0)
        sym = f.GetFunctionName() or "?"
        le = f.GetLineEntry()
        key = "%s:%d %s" % (le.GetFileSpec().GetFilename(), le.GetLine(), sym.split('(')[0][-40:])
        lines[key] += 1
        if not seq or seq[-1][0] != key: seq.append([key, 1])
        else: seq[-1][1] += 1
        thread.StepInstruction(False)
    out = open("/tmp/steplines.txt", "w")
    for k, v in lines.most_common(80): out.write("%6d %s\n" % (v, k))
    out.write("\n--- sequence ---\n")
    for s, c in seq[:700]: out.write("%5d %s\n" % (c, s))
    out.close()
def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand('command script add -f steplines.run steplines')
