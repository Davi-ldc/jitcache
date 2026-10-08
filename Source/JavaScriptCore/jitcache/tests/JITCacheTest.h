/*
 * Copyright (C) 2026 The JITCache Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

namespace JSC {
class VM;
} // namespace JSC

// The C++ test framework of testjitcache (harness sub-SPEC section 8.1). A test is a function registered by name, with a
// flag saying whether it needs a VM and the JSC options its process needs, and it fails through a macro that records a
// message. The options string is a space-separated list of --name=value JSC options; tests with the same string share a
// process. tests/testjitcache.cpp defines the members below and the registry the registrations fill.

namespace JSC::JITCache::Tests {

enum class NeedsVM : bool { No, Yes };

class TestContext {
    WTF_MAKE_NONCOPYABLE(TestContext);
public:
    explicit TestContext(VM*); // testjitcache's main: the test's fresh VM, or null
    VM* vm() const; // NeedsVM::Yes: a fresh VM whose API lock the test holds; null otherwise
    void fail(const char* file, int line, String message);
    bool failed() const;
    const Vector<String>& messages() const; // what fail recorded, which main prints after FAIL <name>

private:
    VM* const m_vm;
    Vector<String> m_messages;
};

using TestFunction = void (*)(TestContext&);

struct TestRegistration {
    TestRegistration(ASCIILiteral name, NeedsVM, ASCIILiteral options, TestFunction);
};

} // namespace JSC::JITCache::Tests

#define JITCACHE_TEST_WITH_OPTIONS(name, needsVM, options) \
    static void name(JSC::JITCache::Tests::TestContext&); \
    static JSC::JITCache::Tests::TestRegistration name##Registration { #name ""_s, JSC::JITCache::Tests::NeedsVM::needsVM, options ""_s, name }; \
    static void name(JSC::JITCache::Tests::TestContext& context)
#define JITCACHE_TEST(name, needsVM) JITCACHE_TEST_WITH_OPTIONS(name, needsVM, "")
#define JITCACHE_CHECK(condition) \
    do { \
        if (!(condition)) \
            context.fail(__FILE__, __LINE__, "check failed: " #condition ""_s); \
    } while (false)
#define JITCACHE_FAIL(message) context.fail(__FILE__, __LINE__, message)

#endif // ENABLE(JITCACHE_TWINS)
