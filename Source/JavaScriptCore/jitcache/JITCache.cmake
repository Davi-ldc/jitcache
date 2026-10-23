# JITCache's build entry. Source/JavaScriptCore/CMakeLists.txt includes this file while it assembles the engine's
# source lists and private headers, so every path here starts at Source/JavaScriptCore.

# Each module directory lists its .cpp files in its own Sources.txt, in the format of the engine's Sources.txt; a new
# directory adds its list here. The fragments under ucb/codec/ and ucb/generation/ are the exception: engine files
# include them at their end, so no list names them. The twins-only files compile to nothing without
# ENABLE(JITCACHE_TWINS), so the lists name them unconditionally. The C++ tests under tests/ have lists of their own,
# which tests/testjitcache.cmake builds into the testjitcache executable.
list(APPEND JavaScriptCore_UNIFIED_SOURCE_LIST_FILES
    "jitcache/base/Sources.txt"

    "jitcache/cb/Sources.txt"

    "jitcache/glue/Sources.txt"

    "jitcache/harness/Sources.txt"

    "jitcache/ics/Sources.txt"

    "jitcache/image/Sources.txt"

    "jitcache/maintenance/Sources.txt"

    "jitcache/session/Sources.txt"

    "jitcache/shell/Sources.txt"

    "jitcache/store/Sources.txt"

    "jitcache/twins/Sources.txt"

    "jitcache/ucb/Sources.txt"
)

# The exported headers, which Bun includes as <JavaScriptCore/...> from flat copies among the engine's private headers.
# Bun sees only the copies, so these headers may include only <JavaScriptCore/...>, <wtf/...> and system headers.
list(APPEND JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS
    jitcache/harness/JITCacheTwinsHost.h

    jitcache/maintenance/JITCacheMaintenance.h

    jitcache/session/JITCacheAPI.h
    jitcache/session/JITCacheBun.h
)
