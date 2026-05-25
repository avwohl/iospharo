#ifndef PHARO_PROFILER_HPP
#define PHARO_PROFILER_HPP

namespace pharo {

class Interpreter;

namespace Profiler {

// Enable SIGPROF-based sampling of `interp->activeMethod()`.  Caller
// retains ownership of interp.  Re-entry is a no-op.
void enable(Interpreter* interp);

// Stop sampling and dump top-N method histogram to stderr.  Safe to
// call multiple times (subsequent calls are no-ops).
void dump();

}  // namespace Profiler

}  // namespace pharo

#endif
