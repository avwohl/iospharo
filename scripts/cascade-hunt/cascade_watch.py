
import lldb
armed_done = [False]
hits = [0]
state = {'addrs': [], 'sb': 0}

def on_cascade(frame, bp_loc, internal_dict):
    if armed_done[0]:
        return False
    thread = frame.GetThread(); process = thread.GetProcess()
    target = process.GetTarget(); dbg = target.GetDebugger()
    regs = frame.GetRegisters().GetFirstValueByName('General Purpose Registers')
    def reg(n):
        v = regs.GetChildMemberWithName(n)
        return v.GetValueAsUnsigned() if v.IsValid() else 0
    corpse, sb, sp = reg('x0'), reg('x1'), reg('x2')
    oldStart, oldFree = reg('x3'), reg('x4')
    print('[WATCH] corpse=0x%x old=[0x%x..0x%x) span=%dKB' % (corpse, oldStart, oldFree, (oldFree-oldStart)//1024))
    if not corpse:
        return False
    err = lldb.SBError()
    # HEAP scan: find old-space cells holding the corpse value.
    heapHolders = []
    CHUNK = 1 << 20
    a = oldStart
    while a < oldFree and len(heapHolders) < 8:
        n = min(CHUNK, oldFree - a)
        data = process.ReadMemory(a, n, err)
        if err.Success():
            off = 0
            needle = corpse.to_bytes(8, 'little')
            while True:
                i = data.find(needle, off)
                if i < 0: break
                if (a + i) % 8 == 0:
                    heapHolders.append(a + i)
                    if len(heapHolders) >= 8: break
                off = i + 1
        a += n
    print('[WATCH] heap holders: %s' % [hex(h) for h in heapHolders])
    armed = 0
    for addr in heapHolders:
        if armed >= 3: break
        wp = target.WatchAddress(addr, 8, False, True, err)
        if err.Success():
            armed += 1
            print('[WATCH] armed heap wp @0x%x id=%d' % (addr, wp.GetID()))
            dbg.HandleCommand('watchpoint command add -F cascade_watch.on_watch %d' % wp.GetID())
    if armed:
        armed_done[0] = True
    return False

def on_watch(frame, wp, internal_dict):
    thread = frame.GetThread(); process = thread.GetProcess()
    hits[0] += 1
    name0 = frame.GetFunctionName() or 'JITCODE'
    if hits[0] <= 10:
        print('[HEAPWRITE #%d] wp=%d writer=%s pc=0x%x' % (hits[0], wp.GetID(), name0, frame.GetPC()))
        for i in range(6):
            f = thread.GetFrameAtIndex(i)
            if not f.IsValid(): break
            print('  [%d] 0x%x %s' % (i, f.GetPC(), f.GetFunctionName() or 'JITCODE'))
    return False
