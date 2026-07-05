
import lldb
armed_done = [False]
hits = [0]
state = {'watch_addr': 0, 'wp_id': 0, 'sb': 0}

def _arm(target, dbg, addr):
    err = lldb.SBError()
    if state['wp_id']:
        dbg.HandleCommand('watchpoint delete %d' % state['wp_id'])
        state['wp_id'] = 0
    wp = target.WatchAddress(addr, 8, False, True, err)
    if err.Success():
        state['wp_id'] = wp.GetID(); state['watch_addr'] = addr
        dbg.HandleCommand('watchpoint command add -F cascade_watch.on_watch %d' % wp.GetID())
        print('[CHASE] watching @0x%x id=%d' % (addr, wp.GetID()))
        return True
    print('[CHASE] arm-failed @0x%x: %s' % (addr, err.GetCString()))
    return False

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
    state['sb'] = sb
    err = lldb.SBError(); lowest = 0
    if corpse and sb and sp > sb:
        for i in range((sp - sb) // 8):
            v = process.ReadUnsignedFromMemory(sb + i*8, 8, err)
            if err.Success() and v == corpse:
                lowest = sb + i*8
                break   # LOWEST cell = closest to the origin
    if lowest and _arm(target, dbg, lowest):
        armed_done[0] = True
    return False

def on_watch(frame, wp, internal_dict):
    thread = frame.GetThread(); process = thread.GetProcess()
    target = process.GetTarget(); dbg = target.GetDebugger()
    err = lldb.SBError()
    addr = state['watch_addr']
    v = process.ReadUnsignedFromMemory(addr, 8, err)
    if not err.Success() or (v & 7) != 0 or v < 0x100000000:
        return False
    hdr = process.ReadUnsignedFromMemory(v, 8, err)
    if not err.Success() or (hdr & 0x3FFFFF) != 0:
        return False
    hits[0] += 1
    name0 = frame.GetFunctionName() or 'JITCODE'
    print('[DEADWRITE #%d] slot=0x%x value=0x%x writer=%s' % (hits[0], addr, v, name0))
    if hits[0] > 24:
        return False
    # Find an OLDER (lower) cell with the same dead value: migrate there.
    sb = state['sb']; lower = 0
    for i in range((addr - sb) // 8):
        cv = process.ReadUnsignedFromMemory(sb + i*8, 8, err)
        if err.Success() and cv == v:
            lower = sb + i*8
            break
    if lower and lower < addr:
        print('[CHASE] migrating to lower cell 0x%x (offset %d)' % (lower, (lower-sb)//8))
        _arm(target, dbg, lower)
        return False
    # ORIGIN: no older cell holds it — this writer produced the value.
    print('[ORIGIN] writer=%s pc=0x%x' % (name0, frame.GetPC()))
    for i in range(8):
        f = thread.GetFrameAtIndex(i)
        if not f.IsValid(): break
        print('  [%d] 0x%x %s' % (i, f.GetPC(), f.GetFunctionName() or 'JITCODE'))
    dbg.HandleCommand('disassemble -s 0x%x -c 16' % (frame.GetPC() - 32))
    regs = frame.GetRegisters().GetFirstValueByName('General Purpose Registers')
    for rn in ('x0','x1','x2','x3','x4','x5','x6','x7','x19','x25'):
        rv = regs.GetChildMemberWithName(rn)
        if rv.IsValid():
            print('  %s=0x%x' % (rn, rv.GetValueAsUnsigned()))
    return True  # stop for good measure; batch has no more commands
