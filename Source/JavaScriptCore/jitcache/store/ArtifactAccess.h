#pragma once

#include "ArtifactStore.h"
#include <expected>
#include <memory>
#include <stdint.h>
#include <wtf/RefPtr.h>
#include <wtf/text/WTFString.h>

// How a VM reaches its artifact when it starts: a Producer creates one, a Consumer opens a compatible one, and a
// ConsumerProducer opens a compatible one or replaces an incompatible one. start, in JITCacheAPI.cpp, is the only
// caller, and it turns a failure into its own result. Directories are created with mode 0755 and files with 0644. The
// opened artifact pins descriptors of its own, so the ones opened here close before openArtifact returns.

namespace JSC::JITCache {

enum class ArtifactAccessMode : uint8_t {
    Open, // a Consumer's: no producer lock; <parent>, cache/ and a compatible header must exist
    Create, // a Producer's: the producer lock; creates <parent>, one level deep, and an artifact where none exists
    OpenOrReplace, // a ConsumerProducer's: the producer lock; opens a compatible artifact or replaces an incompatible one
};

struct ArtifactAccess {
    std::unique_ptr<ProducerLock> producerLock; // Create and OpenOrReplace
    RefPtr<OpenedArtifact> artifact;
    String replaced; // OpenOrReplace: the incompatibility of the artifact it replaced; null otherwise
};

// Why the artifact could not be reached, with a detail that names the path and the cause.
struct ArtifactAccessFailure {
    enum class Kind : uint8_t {
        Missing, // <parent>, cache/ or cache/header does not exist
        Exists, // Create: cache/header exists
        NotAnArtifact, // Create: a header-less cache/ whose pack may hold bodies
        Incompatible, // the header passes its own checks but differs from the process's
        Busy, // another producer or maintenance holds the producer lock
        IO, // a system call failed
        CorruptHeader, // the header fails its own checks
        DamagedPack, // cache/pack fails its checks
    };
    Kind kind;
    String detail;
};

// Opens or creates the artifact at path in the given mode, takes the producer lock first when the mode needs it, and
// takes the opened artifact from the process's registry. A failure releases the lock it took. The calling thread; it
// takes the registry's lock and the opened artifact's index lock, both leaves.
std::expected<ArtifactAccess, ArtifactAccessFailure> openArtifact(const String& path, ArtifactAccessMode);

} // namespace JSC::JITCache
