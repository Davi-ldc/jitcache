#pragma once

#include "BytecodeIndex.h"

namespace JSC {

class UnlinkedCodeBlock;

} // namespace JSC

namespace JSC::JITCache {

// The call site JSC::eval passes down to DirectEvalExecutable::create. A direct eval is identified by its caller's key and
// the call site's bytecode index.
struct DirectEvalSite {
    UnlinkedCodeBlock* callerUnlinkedCodeBlock;
    BytecodeIndex bytecodeIndex;
};

} // namespace JSC::JITCache
