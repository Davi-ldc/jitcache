#pragma once

#include "ArtifactStore.h"
#include <expected>
#include <memory>
#include <optional>
#include <stdint.h>
#include <wtf/RefPtr.h>
#include <wtf/text/WTFString.h>

// How a VM reaches its artifact when it starts: a Producer creates one, whose header names the config's app when the
// config names one, a Consumer opens a compatible one not made for another app, and a ConsumerProducer opens a compatible
// one and replaces an incompatible one or one made for another app. start, in JITCacheAPI.cpp, is the only caller, and it
// turns a failure into its own result. Directories are created with mode 0755 and files with 0644. The opened artifact
// pins descriptors of its own, so the ones opened here close before openArtifact returns.

namespace JSC::JITCache {

enum class ArtifactAccessMode : uint8_t {
    Open, // a Consumer's: no producer lock; <parent>, cache/ and a compatible header not made for another app must exist
    Create, // a Producer's: the producer lock; creates <parent>, one level deep, and an artifact where none exists
    // A ConsumerProducer's: the producer lock; opens a compatible artifact and replaces an incompatible one or one made for
    // another app.
    OpenOrReplace,
};

struct ArtifactAccess {
    std::unique_ptr<ProducerLock> producerLock; // Create and OpenOrReplace
    RefPtr<OpenedArtifact> artifact;
    String replaced; // OpenOrReplace: why the old artifact went, its differing field or both apps; null otherwise
};

// Why the artifact could not be reached, with a detail that names the path and the cause.
struct ArtifactAccessFailure {
    enum class Kind : uint8_t {
        Missing, // <parent>, cache/ or cache/header does not exist
        Exists, // Create: cache/header exists
        NotAnArtifact, // Create: a header-less cache/ whose pack may hold bodies
        Incompatible, // the header passes its own checks but differs from the process's
        AnotherApp, // Open: the header is compatible but made for another app than the config's
        Busy, // another producer or maintenance holds the producer lock
        IO, // a system call failed
        CorruptHeader, // the header fails its own checks
        DamagedPack, // cache/pack fails its checks
    };
    Kind kind;
    String detail;
};

// Opens or creates the artifact at path in the given mode, for the config's app, takes the producer lock first when the
// mode needs it, and takes the opened artifact from the process's registry. A failure releases the lock it took. The
// calling thread; it takes the registry's lock and the opened artifact's index lock, both leaves.
std::expected<ArtifactAccess, ArtifactAccessFailure> openArtifact(const String& path, ArtifactAccessMode, std::optional<uint64_t> appDigest);

} // namespace JSC::JITCache
