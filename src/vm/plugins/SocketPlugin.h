/*
 * SocketPlugin.h - TCP socket primitives for Pharo VM
 *
 * Implements the SocketPlugin named primitives that the Pharo image calls
 * via <primitive: 'name' module: 'SocketPlugin'>.
 *
 * SQSocket struct layout must match what the Pharo image expects:
 * a ByteArray of sizeof(SQSocket) bytes (16 bytes on 64-bit).
 */

#ifndef SOCKET_PLUGIN_H
#define SOCKET_PLUGIN_H

#include "sqVirtualMachine.h"

// SQSocket — the handle stored in a Pharo ByteArray
// Layout must match the standard VM exactly.
typedef struct {
    int   sessionID;
    int   socketType;       // 0=TCP, 1=UDP
    void* privateSocketPtr;
} SQSocket;

// Socket connection status (returned by primitiveSocketConnectionStatus)
#define SOCK_INVALID              -1
#define SOCK_UNCONNECTED           0
#define SOCK_WAITING_FOR_CONNECTION 1
#define SOCK_CONNECTED             2
#define SOCK_OTHER_END_CLOSED      3
#define SOCK_THIS_END_CLOSED       4

// Socket types
#define TCP_SOCKET_TYPE  0
#define UDP_SOCKET_TYPE  1

// Initialize/shutdown the socket subsystem
void socketPluginInit(void);
void socketPluginShutdown(void);

#endif // SOCKET_PLUGIN_H
