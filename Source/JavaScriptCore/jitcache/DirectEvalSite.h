#pragma once

#include "BytecodeIndex.h"

namespace JSC {

class UnlinkedCodeBlock;

} // namespace JSC

namespace JSC::JITCache {

// The call site JSC::eval passes down to DirectEvalExecutable::create (SPEC-ucb.md section 7.2.3). A direct eval is
// identified by its caller's key and the call site's bytecode index.
struct DirectEvalSite {
    UnlinkedCodeBlock* callerUnlinkedCodeBlock;
    BytecodeIndex bytecodeIndex;
};

} // namespace JSC::JITCache
