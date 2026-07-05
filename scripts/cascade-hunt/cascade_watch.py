
import lldb
armed_done = [False]
hits = [0]

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
    print('[WATCH] cascade corpse=0x%x sb=0x%x sp=0x%x' % (corpse, sb, sp))
    err = lldb.SBError(); slots = []
    if corpse and sb and sp > sb:
        for i in range((sp - sb) // 8):
            v = process.ReadUnsignedFromMemory(sb + i*8, 8, err)
            if err.Success() and v == corpse:
                slots.append(sb + i*8)
    print('[WATCH] slots: %s' % [hex(h) for h in slots])
    for addr in slots[:2]:
        wp = target.WatchAddress(addr, 8, False, True, err)
        if err.Success():
            print('[WATCH] armed @0x%x id=%d' % (addr, wp.GetID()))
            dbg.HandleCommand('watchpoint command add -F cascade_watch.on_watch %d' % wp.GetID())
            armed_done[0] = True
    return False

def on_watch(frame, wp, internal_dict):
    thread = frame.GetThread(); process = thread.GetProcess()
    err = lldb.SBError()
    addr = wp.GetWatchAddress()
    v = process.ReadUnsignedFromMemory(addr, 8, err)
    if not err.Success() or (v & 7) != 0 or v < 0x100000000:
        return False
    hdr = process.ReadUnsignedFromMemory(v, 8, err)
    if not err.Success() or (hdr & 0x3FFFFF) != 0:
        return False
    # DEAD-POINTER DEPOSIT: log 6 frames loud.
    hits[0] += 1
    if hits[0] <= 6:
        print('[DEADWRITE #%d] slot=0x%x value=0x%x hdr=0x%x' % (hits[0], addr, v, hdr))
        for i in range(6):
            f = thread.GetFrameAtIndex(i)
            if not f.IsValid(): break
            print('  [%d] 0x%x %s' % (i, f.GetPC(), f.GetFunctionName() or 'JITCODE'))
        if (frame.GetFunctionName() or '') == '':
            dbg = process.GetTarget().GetDebugger()
            pc = frame.GetPC()
            print('[DEADWRITE-DISASM around 0x%x]' % pc)
            dbg.HandleCommand('disassemble -s 0x%x -c 14' % (pc - 24))
            # register snapshot for operand attribution
            regs = frame.GetRegisters().GetFirstValueByName('General Purpose Registers')
            for rn in ('x0','x1','x2','x3','x4','x5','x6','x7','x19','x20'):
                rv = regs.GetChildMemberWithName(rn)
                if rv.IsValid():
                    print('  %s=0x%x' % (rn, rv.GetValueAsUnsigned()))
    return False
