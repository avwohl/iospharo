/*
 * SocketPlugin_win_stub.cpp - milestone-1 Windows stub for SocketPlugin.
 *
 * The real SocketPlugin (src/vm/plugins/SocketPlugin.cpp, ~1338 lines of BSD
 * sockets) is excluded from the Windows build and gets a winsock2 port in
 * milestone 3.  InterpreterProxy.cpp's named-primitive table still references
 * these symbols, so this stub supplies them so the VM links.
 *
 * This is NOT a silent-success stub: socket access is honestly reported as
 * UNAVAILABLE (primitiveHasSocketAccess pushes false), and every socket
 * primitive calls primitiveFail().  The image handles an absent SocketPlugin
 * the same way it would on any VM without networking — sockets simply don't
 * work until milestone 3.  See docs/windows-port-plan.md.
 */

#include "SocketPlugin.h"   // pulls sqVirtualMachine.h (VirtualMachine, sqInt)

namespace { VirtualMachine* vm = nullptr; }

extern "C" {

sqInt SocketPlugin_setInterpreter(VirtualMachine* anInterpreter) {
    vm = anInterpreter;
    return 0;
}

// Honestly report NO socket access on Windows milestone 1.
sqInt sp_primitiveHasSocketAccess(void) {
    vm->popthenPush(1, vm->falseObject());
    return 0;
}

// Every socket primitive fails (sockets unavailable until the milestone-3
// winsock2 port).  primitiveFail() is the same path the real plugin takes on
// an error, so the image degrades gracefully rather than mis-behaving.
sqInt sp_primitiveSocketAbortConnection(void)            { return vm->primitiveFail(); }
sqInt sp_primitiveSocketAccept3Semaphores(void)          { return vm->primitiveFail(); }
sqInt sp_primitiveSocketBindToPort(void)                 { return vm->primitiveFail(); }
sqInt sp_primitiveSocketCloseConnection(void)            { return vm->primitiveFail(); }
sqInt sp_primitiveSocketConnectToPort(void)              { return vm->primitiveFail(); }
sqInt sp_primitiveSocketConnectionStatus(void)           { return vm->primitiveFail(); }
sqInt sp_primitiveSocketCreate3Semaphores(void)          { return vm->primitiveFail(); }
sqInt sp_primitiveSocketDestroy(void)                    { return vm->primitiveFail(); }
sqInt sp_primitiveSocketError(void)                      { return vm->primitiveFail(); }
sqInt sp_primitiveSocketGetOptions(void)                 { return vm->primitiveFail(); }
sqInt sp_primitiveSocketListenOnPortBacklog(void)        { return vm->primitiveFail(); }
sqInt sp_primitiveSocketListenOnPortBacklogInterface(void) { return vm->primitiveFail(); }
sqInt sp_primitiveSocketListenWithBacklog(void)          { return vm->primitiveFail(); }
sqInt sp_primitiveSocketLocalAddress(void)               { return vm->primitiveFail(); }
sqInt sp_primitiveSocketLocalPort(void)                  { return vm->primitiveFail(); }
sqInt sp_primitiveSocketReceiveDataAvailable(void)       { return vm->primitiveFail(); }
sqInt sp_primitiveSocketReceiveDataBufCount(void)        { return vm->primitiveFail(); }
sqInt sp_primitiveSocketReceiveUDPDataBufCount(void)     { return vm->primitiveFail(); }
sqInt sp_primitiveSocketRemoteAddress(void)              { return vm->primitiveFail(); }
sqInt sp_primitiveSocketRemotePort(void)                 { return vm->primitiveFail(); }
sqInt sp_primitiveSocketSendDataBufCount(void)           { return vm->primitiveFail(); }
sqInt sp_primitiveSocketSendDone(void)                   { return vm->primitiveFail(); }
sqInt sp_primitiveSocketSendUDPDataBufCount(void)        { return vm->primitiveFail(); }
sqInt sp_primitiveSocketSetOptions(void)                 { return vm->primitiveFail(); }

}  // extern "C"
