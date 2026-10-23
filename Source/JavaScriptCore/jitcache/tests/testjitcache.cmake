# The testjitcache executable, JITCache's C++ unit tests. shell/CMakeLists.txt includes this file in builds with
# ENABLE_JITCACHE_TWINS, then declares and attaches the target. Each test directory lists its files in its own
# Sources.txt, one path from Source/JavaScriptCore per line, where a line that starts with // is a comment; a new
# directory adds its name here.
set(testjitcache_SOURCES ${CMAKE_CURRENT_LIST_DIR}/testjitcache.cpp)
foreach (_directory IN ITEMS base cb glue harness ics image maintenance session store twins ucb)
    set(_list ${CMAKE_CURRENT_LIST_DIR}/${_directory}/Sources.txt)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_list})
    file(STRINGS ${_list} _sources REGEX "^[^/]")
    list(TRANSFORM _sources PREPEND ${JAVASCRIPTCORE_DIR}/)
    list(APPEND testjitcache_SOURCES ${_sources})
endforeach ()

set(testjitcache_DEFINITIONS ${jsc_PRIVATE_DEFINITIONS})
set(testjitcache_PRIVATE_INCLUDE_DIRECTORIES ${jsc_PRIVATE_INCLUDE_DIRECTORIES})
set(testjitcache_FRAMEWORKS ${jsc_FRAMEWORKS})
set(testjitcache_LIBRARIES ${CMAKE_DL_LIBS})
if (USE_EXTERNAL_MIMALLOC)
    list(APPEND testjitcache_LIBRARIES $<TARGET_OBJECTS:mimalloc-obj>)
endif ()
