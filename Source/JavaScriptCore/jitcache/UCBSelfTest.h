#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include <stdint.h>
#include <wtf/Forward.h>

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

// The UCB lane's self-test (SPEC-ucb.md section 13.1), which $vm.jitCacheUCBSelfTest() runs (M4).

// `start` fixes a VM's role and strictness for good, and invalid material turns its cache activity off for good, so one
// call covers one configuration. Configured runs U1 to U7, U9 and the parts of U8 the VM's configuration has.
// InvalidMaterial runs only U8's two cases of a supplied digest that differs from its text, the decoded program and then
// the import that the digest makes invalid material, which turns cache activity off, so nothing runs after them.
enum class UCBSelfTestScope : uint8_t { Configured, InvalidMaterial };

// Runs the scope's parts in a VM that `start` configured, from inside a VM entry, whose global object the parts use. The
// first check that fails writes its part and what broke to `failure`, and the call returns false.
bool runUCBSelfTest(VM&, UCBSelfTestScope, String& failure);

// The parts defined in the file whose private state they read, which runUCBSelfTest calls. Each writes its first failure to
// `failure` and returns false.
bool runSHA256SelfTest(String& failure); // U1, JITCacheSHA256.cpp: every SHA-256 path the CPU offers
bool runCoreCodecSelfTest(VM&, String& failure); // U7's cases that alter a core's records, runtime/CachedTypes.cpp

// U4, UCBRegistry.cpp: whether the calling thread is inside a critical section of a registry's lock, which the stubs whose
// destructors the registry's destructor callbacks run read (section 6.3).
bool isUCBRegistryLockHeldByCurrentThread();

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
