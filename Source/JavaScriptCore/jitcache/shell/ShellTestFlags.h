#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

// The jsc shell's test flags that set process-wide hooks: the store's and the writer's faults, the kill point, the twin
// report's test entry and forced blinding. JITCacheShell.cpp parses the flags in jscmain, before JSC::initialize and
// before any other thread exists, and calls these with their values.

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

// Each parses its flag's value and returns false when it is malformed, which the shell reports as it does a bad JSC
// option; a later call replaces what an earlier one set, so the last occurrence of a flag wins.
bool setStoreFaultFlag(const char*); // <call>:<error>[@<n>]; sets the store's process-wide hook at once
bool setWriterFaultFlag(const char*); // <check>@<n>, kept for applyTestFlags
bool setKillFlag(const char*); // <point>@<n>, a kill point the writer names, kept for applyTestFlags
bool setTwinEntryFlag(const char*); // difference, skip or coincidence:<domain>, kept for applyTestFlags

// Forced blinding, which MacroAssembler::shouldConsiderBlinding reads through forcesBlindingForTesting
// (MacroAssembler.h) to blind every immediate the assembler considers for blinding. The shell sets it from
// --jitcache-test-force-blinding while it parses its command line, before any thread emits code, so the threads that
// emit read it without a lock. Only runs with JITCache off take it: in a producer it would blind literals that a
// consumer's image twin, which replays the producer's draws without the flag, emits plain.
void setForcesBlindingForTesting(bool);

// Right after start configures the shell's main VM: hands the kept writer fault and kill point to the VM's writer when
// the VM has one, and writes the test entry when the VM's twin report is open.
void applyTestFlags(VM&);

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
