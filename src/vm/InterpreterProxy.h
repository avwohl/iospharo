// InterpreterProxy.h - Adapter between VMMaker-generated plugins and our clean C++ VM
#ifndef INTERPRETER_PROXY_H
#define INTERPRETER_PROXY_H

// Forward declarations
namespace pharo { class Interpreter; }

// The sqVirtualMachine.h defines the VirtualMachine struct
extern "C" {
#include "plugins/sqVirtualMachine.h"
}

// Initialize the interpreter proxy for use by plugins.
// Must be called after the interpreter is fully initialized.
void initializeInterpreterProxy(pharo::Interpreter* interp);

// Get the proxy struct (for passing to plugin setInterpreter functions)
VirtualMachine* getInterpreterProxy();

// Initialize B2DPlugin and register its primitives
void initializeB2DPlugin(pharo::Interpreter* interp);

// Reset the proxy failure flag before calling external primitives
void resetProxyFailure();

// Check if the proxy indicates failure
bool proxyFailed();

#endif // INTERPRETER_PROXY_H
