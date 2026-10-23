#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "JSCJSValue.h"
#include <optional>
#include <stdint.h>
#include <wtf/text/ASCIILiteral.h>

// What the host functions both hosts register (HostFunctions.cpp) share with the shell functions only the jsc shell
// registers (shell/ShellFunctions.cpp): how they write a count and how they read a function's CB from their arguments.

namespace JSC {

class CallFrame;
class CodeBlock;
class JSGlobalObject;

} // namespace JSC

namespace JSC::JITCache {

// A count as the host functions write it, a JavaScript number.
JSValue countValue(uint64_t);
// The CB of the kind its second argument names of the JSFunction with JS code its first argument holds, null when the
// function has no CB of that kind; a TypeError, and nothing, for any other function or kind.
std::optional<CodeBlock*> functionCodeBlockArgument(JSGlobalObject*, CallFrame*, ASCIILiteral caller);

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
