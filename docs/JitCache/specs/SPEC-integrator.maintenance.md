# SPEC-integrator.maintenance: `compact`

Part of [SPEC-integrator.md](SPEC-integrator.md), which indexes it; the container sub-SPEC's formats and names apply here. It specifies the backend and the command line of THREAD Maintenance's `compact`, the one maintenance operation. [SPEC-integrator-history.md](SPEC-integrator-history.md) records the path to the set's non-obvious decisions.

## 1. Interface

`maintenance/JITCacheMaintenance.h` (section 7), exported to Bun as `<JavaScriptCore/JITCacheMaintenance.h>` (SPEC-integrator.md section 3.5). It includes only WTF headers and `JSExportMacros.h`, and declares nothing that exists only in twins builds.

```cpp
namespace JSC::JITCache::Maintenance {

enum class Outcome : uint8_t { Done, NoArtifact, Busy, NeedsConfirmation, Declined, PlanChanged, Failed };

struct Diagnostic {
    ASCIILiteral code;     // such as "damaged-body"; the command line prints it
    String detail;
};

struct Eviction {
    std::array<uint8_t, 40> key;
    uint64_t bytes { 0 };         // S, the body's size
    uint64_t version { 0 };       // the body's commit identifier
    double score { 0 };           // (L + P) / S
    friend bool operator==(const Eviction&, const Eviction&) = default;   // for comparing a confirmed plan
};

struct Plan {
    std::array<uint8_t, 16> headerDigest;     // the artifact's header digest
    uint64_t postCleanBytes { 0 };
    uint64_t targetBytes { 0 };
    uint64_t evictedBytes { 0 };
    uint64_t damagedPackBytes { 0 };          // a damaged pack's size when the plan evicts it, else 0
    Vector<Eviction> evictions;               // in eviction order
    bool deletesArtifact { false };
    friend bool operator==(const Plan&, const Plan&) = default;
};

struct Report {
    Outcome outcome { Outcome::Failed };
    uint64_t temporariesRemoved { 0 };
    uint64_t bodiesEvicted { 0 };
    uint64_t bytesReclaimed { 0 };
    std::optional<Plan> plan;
    Vector<Diagnostic> diagnostics;
};

enum class Answer : uint8_t { Ask, Yes, No };
struct CompactOptions {
    Answer answer { Answer::Ask };            // for a plan that deletes the whole artifact
    std::optional<Plan> confirmedPlan;        // the plan a first call returned, on the call that confirms it
};

JS_EXPORT_PRIVATE Report compact(const String& parentPath, double ratio, const CompactOptions&);

enum class RatioArgument : uint8_t { Required, Optional };   // whether the command line may leave the ratio out
JS_EXPORT_PRIVATE int runCommandLine(std::span<const UTF8CString> arguments, FILE* in, FILE* out, FILE* err,
    RatioArgument = RatioArgument::Required);

}
```

## 2. Independence from VMs

The backend needs no VM and no `JSC::initialize`. It uses the filesystem, SHA-256 for the header digest and the container's CRC-32C (container sub-SPEC section 4.4) for the pack's and the bodies' checks, and runs synchronously on the calling thread. It never compares the header with the running process, since the command line may run under another binary than the one that produced the artifact, and it has no config, so the app a header names changes nothing it does: a compaction keeps the header, app record included, and a whole deletion removes it with the rest. It reads `cache/header` with `readHeaderFile` (`store/JITCacheFiles.h`), which opens it with `O_NONBLOCK` and reads a file that is not a regular file, such as a FIFO, as empty, so it fails the checks instead of blocking the call, and reads at most one byte more than the largest header a reader accepts. `readArtifactHeader` then runs the header's own checks (container sub-SPEC section 3.2), and `artifactHeaderDigest` gives the digest the pack is read against (container sub-SPEC section 3.3). Planning reads the pack's prefix, root and index, whose entries repeat each body's key, size, commit identifier, L and P, and reads no body. Its memory therefore grows with the number of bodies, by one index entry and one `Eviction` each, never with their sizes (THREAD Maintenance). Only the apply of section 4.3 reads bodies, one at a time through the mapping, to check every body the pack keeps.

Off Linux, `compact` returns `Failed` with `platform` and touches nothing (SPEC-integrator.md section 11). On Linux, each call first opens `<parent>` and its `cache/` without the lock. When either open fails with `ENOENT` or `ENOTDIR`, the call returns `NoArtifact`, so a path without an artifact gets no lock file; any other failure gives `Failed` with `io`. The call then takes the producer lock with `ProducerLock::tryAcquire` on the parent's descriptor, without blocking (container sub-SPEC section 2). A lock that a producer or another maintenance run holds gives `Busy` and changes nothing, and any other failure to take it gives `Failed` with `io`. Under the lock the call opens `cache/` again, and a `cache/` that went away meanwhile gives `NoArtifact`. The call holds the lock for all its work and releases it when it returns, so a plan that waits for confirmation is never applied under the lock it was planned under (section 4.4).

## 3. Cleanup

The cleanup removes what interrupted writes left (THREAD Maintenance) and never a committed body. It runs under the lock after planning: when the call goes on to apply its plan (sections 4.3 and 4.5), and before the call returns for a `cache/` that gets no plan (section 4.2, steps 3 and 4). Its steps, in order:

1. Every temporary in `cache/` goes: `.header.<32 hex>.tmp` and `.pack.<32 hex>.tmp` (container sub-SPEC section 1.1), which an interrupted creation or a compaction that died before its rename leaves. Each one's size, taken with `fstatat` before its `unlinkat`, adds to `bytesReclaimed`, and each counts in `temporariesRemoved`.
2. A `.cache.replaced` that a ConsumerProducer's replacement left goes through `removeReplacedArtifact`, as the next replacement would remove it (container sub-SPEC section 1.4): its temporaries, its pack and its header, then the directory. Their sizes add to `bytesReclaimed`. A name in it that is none of these stays, with the directory, and that failure, like any other of this step, is reported as `unlink-failed`.
3. A `cache/` without a header is a remnant (container sub-SPEC section 1.2). When its pack may hold a body (`packHoldsBodies`: its root is not the empty pack's, or it cannot be read), the remnant stays and is reported as `remnant-with-bodies`. When it holds a name that step 4 reports, it stays as it is, and the next Producer reuses it (container sub-SPEC section 1.3). Otherwise its pack is marked retired (`retirePack`, container sub-SPEC section 1.2) and goes, its size adding to `bytesReclaimed`, and then `cache/` with `unlinkat(..., AT_REMOVEDIR)`. A failed mark or unlink is reported as `unlink-failed` and leaves the pack and `cache/`.
4. Every other name in `cache/`, one that is neither `header`, `pack` nor a temporary, a directory included, is reported as `unknown-file` and stays.
5. When the plan keeps bodies and the pack passed its checks, the pack's own cleanup runs through a descriptor opened `O_RDWR`, provided the path still names the file planning mapped. A retired mark that the file carries is cleared to 0; only a compaction that died or failed between its mark and its rename leaves one. The file is then truncated to its valid end, which removes the bytes that commits which never stored their root left, and those bytes add to `bytesReclaimed`. A failure to open, clear or truncate is reported as `io`, and the call goes on.

After steps 1 to 4, a call without a plan returns: `NoArtifact` for a remnant, `Failed` with `bad-header` for a header that fails its checks, and `Failed` with `incompatible-pack` for a pack of another layout. Of an artifact whose header fails its checks or whose pack is of another layout, the cleanup removes only what names identify, and the pack stays untouched.

## 4. `compact`

### 4.1 Scores

The writer stamps L, P and the commit identifier, the terms THREAD Maintenance defines, in each body's envelope, and the body's entry in the pack's index repeats them (container sub-SPEC section 6.1). P is the CB lane's `counterProgress`, floored, capped and zero for a body whose counter does not travel (SPEC-cb.md section 4.3). A body of tier 0 has L the LLInt progress its import carries, `carriedLLIntPoints`, and P zero (SPEC-ucb.md section 8.3), so compact needs no case of its own for it. The score is `(double(L) + double(P)) / double(S)`, with S the body's size from its index entry.

### 4.2 Planning

Compact cleans up (section 3), then plans whole-body eviction (THREAD Maintenance). The plan is computed on the artifact as the cleanup would leave it, and the cleanup runs only when the plan is applied, so a plan that is never applied changes nothing.

1. Section 2 gives `NoArtifact`, `Busy` or `Failed` with `io` before any plan.
2. A ratio that is NaN or outside [0, 1] gives `Failed` with `bad-ratio`, before anything is read or removed.
3. The header is read (section 2). A `cache/` without a header, or with one that fails its checks or cannot be opened or read, gets no plan: the cleanup runs and the call returns (section 3).
4. With a valid header, the pack is read. No `cache/pack` means no body. An error from `fstatat` on the pack or from mapping it gives `Failed` with `io` before anything is removed. A prefix that carries the pack's tag and a layout version other than this build's (`isOtherPackLayout`) marks a pack of another layout, which another binary can still read, so no ratio may delete it: it gets no plan, the cleanup runs and the call returns. A pack that fails its checks otherwise, as a name that holds no regular file (container sub-SPEC section 5.2), at the prefix, at a root past the file's size, or at an index block that `readPackIndex` rejects on its walk from the root back to the full index, is damaged. It is reported as `damaged-pack` with the rejection, holds no body the plan can keep, and counts with its file's size. Otherwise every live entry of the index is a body.
5. `postCleanBytes` is H, the header file's size, plus a damaged pack's size, plus the size S of every body. Dead bytes, the bytes past the valid end, the prefix, the record headers and the index blocks count nowhere. `targetBytes` is `ceil(ratio * postCleanBytes)`, at most `postCleanBytes` (THREAD Maintenance).
6. When `targetBytes` is above 0, a damaged pack is evicted first: `damagedPackBytes` and `evictedBytes` start at its size.
7. Bodies are ordered by score, then by key bytes, both ascending, and taken in that order while `evictedBytes < targetBytes`. `evictions` lists those taken, each with its key, S, commit identifier and score.
8. `deletesArtifact` holds when the ratio is 1, or when `targetBytes > 0` and the plan took every body (THREAD Maintenance). H counts in `postCleanBytes` and no body removes it, so a target that only H could reach takes every body and deletes the artifact. An artifact with no body, a damaged pack included, is such a case for any ratio above 0. A ratio of 0 gives a target of 0, which evicts nothing and never deletes the artifact, even one with no body or a damaged pack.

The report carries the plan whenever one was made, whatever the outcome.

### 4.3 Applying a plan that keeps bodies

A plan that does not delete the artifact is applied at once, under the lock it was planned under: the cleanup of section 3, then the rewrite when the next paragraph calls for one. Without a pack, or with a damaged pack at ratio 0, there is nothing to rewrite, so the call only cleans up.

The pack is rewritten when the plan evicts a body, when the pack holds a dead byte, that is, when its valid end passes the dense size (`packPrefixBytes`, plus `packRecordExtent(S)` for every live body, plus `packIndexBlockBytes` of their count), or when a live body fails its checks. A dense pack with no eviction is checked before the call keeps its file: `checkPackedBody` (container sub-SPEC section 8.4) runs on each live body in key order, and the first that fails makes the call rewrite the pack. Ratio 0 on a dense pack therefore only cleans up when every body passes. ([history](SPEC-integrator-history.md#a-damaged-body-is-dropped-by-the-rewrite))

The rewrite is `ArtifactWriter::compactPack` (container sub-SPEC section 8.4), the compaction a `delta` runs too, which leaves every body whole in the new pack or gone with the old one, so an interruption leaves no incomplete survivor (THREAD Maintenance). Maintenance passes it the pack's mapping, valid end and live entries in key order, a descriptor opened `O_RDWR` while the path still names the mapped file, the plan's evicted keys to exclude, no budget, no timing and `dropsDamagedBodies`, so a body that fails `checkPackedBody` is left out and reported as `damaged-body` with the check it failed.

`bodiesEvicted` counts the plan's evictions and the bodies left out, and `bytesReclaimed` gains the cleaned file's size minus the new pack's valid end. The outcome is `Done`. A rewrite that fails returns `Failed` with `io`, its detail naming the compaction check that failed (`compaction.create`, `compaction.write`, `compaction.reread` or `compaction.publish`). The pack's cleanup ran first, so a rewrite that fails, as on a full disk, still leaves the pack cleaned and every body in it. A failure before the retired mark leaves no temporary, and a failed rename leaves the old pack marked retired, which the next compact clears (section 3, step 5).

### 4.4 Confirmation

A plan that deletes the artifact needs confirmation (THREAD Maintenance):

- `Answer::Yes`: applied at once, under the same lock (section 4.5).
- `Answer::No`: `Declined` with the plan; nothing is removed, temporaries included.
- `Answer::Ask`: `NeedsConfirmation` with the plan; nothing is removed and the lock is released. The caller asks and, on a yes, calls `compact` again with `answer = Yes` and `confirmedPlan` set to the returned plan.

A call whose options carry a `confirmedPlan` plans again under its own lock and compares the new plan with it field for field, header digest and `damagedPackBytes` included, before it reads the answer. A plan that differs gives `PlanChanged` with the new plan and removes nothing, whatever the answer; an equal plan follows the answer as a first call would. A plan that deletes the artifact lists every body with its key, size and commit identifier, so a body added, removed or recaptured meanwhile changes it, as THREAD Maintenance requires. A producer's compaction meanwhile keeps every key, size and commit identifier, so it leaves the plan equal. A call that makes no plan (section 4.2, steps 3 and 4) ignores the answer and `confirmedPlan`.

### 4.5 Deleting the whole artifact

An applied plan that deletes the artifact runs the cleanup's steps 1 to 4 (section 3), then three steps of its own. The pack's own cleanup does not run, since the pack goes.

1. The pack is emptied. When it passed its checks, it is first marked retired, through a descriptor opened `O_RDWR` while the path still names the mapped file; a pack that cannot be opened is reported as `io` and replaced unmarked. `createEmptyPack` then renames an empty pack, a prefix under the header's digest followed by an empty full index, over `cache/pack`. Every reader that maps the old pack meets the mark, reopens the path and finds no body (container sub-SPEC section 6.3). `bodiesEvicted` gains the plan's evictions.
2. `header` is unlinked.
3. The empty pack is marked retired and unlinked, and then `cache/` is removed with `unlinkat(..., AT_REMOVEDIR)`. A reader that met the old pack's mark after step 1 maps the empty pack, and the mark makes it look at the path again. The lock file stays (THREAD Storage).

`bytesReclaimed` counts the old pack's file size once, the part above the empty pack's size at step 1 and the rest at step 3, and H at step 2. A finished deletion therefore reclaims the old pack's file size, H and the bytes of the cleanup's first two steps, while `plan.evictedBytes` counts only the bodies' sizes and a damaged pack's size.

Until `header` goes the artifact is valid, without bodies once step 1 is done. After step 2, `cache/` is a remnant whose pack holds no body, which the next Producer reuses and the next compact finishes (section 3, step 3).

A failure at step 1, of the retired mark or of the empty pack, is reported as `unlink-failed` and stops the deletion before `header`: a header-less `cache/` whose pack still held bodies would stop every Producer's creation (container sub-SPEC section 1.3) and stay as `remnant-with-bodies`, while the artifact with its header stays valid. A failed unlink of `header` is reported as `unlink-failed` and stops the deletion too. Both return `Failed`. A failed mark or unlink of the empty pack is reported as `unlink-failed` and leaves `cache/` in place, and a failed removal of `cache/`, such as one that still holds an unknown file, is reported as `rmdir-failed`. Both leave a remnant without a body and return `Done`. An unlink that fails with `ENOENT` counts as done. Consumers that pinned the deleted directory see every lookup miss.

## 5. Command line

`runCommandLine` is the one front end both hosts use. Its `RatioArgument` says whether the ratio may be left out:

```text
compact <ratio> [<path>] [--yes | --no]      RatioArgument::Required, the jsc shell's
compact [<ratio>] [<path>] [--yes | --no]    RatioArgument::Optional, Bun's
```

- The first argument names the command. `compact` is the only one, and any other word, `clean` included, is a usage error.
- The ratio is the second argument: a decimal number that `WTF::parseDouble` reads whole, without the locale. The backend rejects a number outside [0, 1] as `bad-ratio`. Under `Required`, a missing ratio or one that is not a number is a usage error. Under `Optional`, a second argument that is not a number leaves the ratio at 0 and is read as the path or an option, so a path that reads as a number is written `./1`.
- Every later argument that starts with `--` is an option, wherever it stands: `--yes` or `--no`, at most one of them, once. Any other option is a usage error, so a mistyped answer is never taken for a path; a path that starts with `--` is written `./--name`.
- At most one path, which must be UTF-8, since the backend takes a `String`. An empty argument is the empty path, which gives `NoArtifact`. Without a path, the command works on `defaultArtifactPath`, `./.jitcache` (SPEC-integrator.md section 3.1).
- A usage error prints `jitcache maintenance: <reason>` and the usage line of the rule in force, `usage: compact <ratio> [<path>] [--yes | --no]` or `usage: compact [<ratio>] [<path>] [--yes | --no]`, to `err`, prints nothing to `out`, and exits with 2.

A valid command line calls `compact(path, ratio, options)` with `Answer::Yes` for `--yes`, `Answer::No` for `--no` and `Answer::Ask` otherwise. It prints each diagnostic to `err` on a line of its own, as `<code>: <detail>`, or the code alone when the detail is empty. When the report carries a plan, it prints `plan: evicts <n> bodies (<evictedBytes> bytes; target <targetBytes> of <postCleanBytes> bytes)` to `out`, followed by ` and deletes the whole artifact` when the plan does.

On `NeedsConfirmation`, when `in` is a terminal (`isatty` on its descriptor), the command line prints the prompt `This operation will delete the entire cache. Ok? (y/n)` and a space to `out`, flushes it and reads one line. The line is a yes when it holds `y` or `Y` alone, around ASCII whitespace; the rest of a line longer than 63 bytes is read and dropped, so it is not left for whoever reads `in` next. On a yes it calls `compact` again with `Answer::Yes` and the returned plan as `confirmedPlan`, prints that call's diagnostics, and prints its plan when it returns `PlanChanged`. Any other answer, or the end of `in`, makes the outcome `Declined`. When `in` is not a terminal, the command line prints the plan and stops, changing nothing.

Each run ends with one summary line on `out`, `compact: <outcome>, <t> temporaries removed, <b> bodies evicted, <r> bytes reclaimed`, where the outcome is `done`, `no-artifact`, `busy`, `needs-confirmation`, `declined`, `plan-changed` or `failed`, and flushes `out` and `err` before it returns. Exit codes: 0 for `Done` and `NoArtifact`, 1 for `Failed`, 2 for a usage error, 3 for `Busy`, 4 for `NeedsConfirmation` and `Declined`, 5 for `PlanChanged`.

The jsc shell runs it with `--jitcache-maintenance`, a flag that takes no value, passing the script arguments (those after `--`) with `RatioArgument::Required`; `jscmain` runs it right after the command line is parsed, before `JSC::initialize`, and exits with its code (SPEC-integrator.md section 11.1). The shell keeps argv's bytes as Latin-1 strings, so `latin1()` gives the original bytes back.

Bun runs it as `bun jitcache compact [<ratio>] [<path>] [--yes|--no]`, a subcommand like any other. `Tag` (`src/options_types/command_tag.rs`, which `src/runtime/cli/mod.rs` re-exports) has `JITCacheCommand`, with the crash-report character `b'J'` in `Tag::char` and an entry in `Tag::ALL`; the flag tables keep their defaults for it. In `src/runtime/cli/mod.rs`, `which()` returns it for `RootCommandMatcher::case(b"jitcache")`, and `Command::start` dispatches it to `jitcache_command::JITCacheCommand::exec()` in `src/runtime/cli/jitcache_command.rs` (declared with `#[path = "jitcache_command.rs"] pub(crate) mod jitcache_command;`), as it dispatches `DiscordCommand`: without a context, since the command needs no configuration and no VM. The root help lists `jitcache` with its usage, and `tag_print_help` gives the usage line above with a description of `compact`, its default path, its ratio of 0 by default, and its `--yes` and `--no`. `exec` takes its arguments from `bun_core::argv()`: every entry after the first one past `argv[0]` equal to `jitcache`, which is the word `which()` matched, since `which()` reads `argv[0]` only to recognize `bunx` and `node`, and every later entry it skips begins with `-`. It passes them to `Bun__JITCache__runMaintenance` (`src/jsc/bindings/JITCacheHost.cpp`, SPEC-integrator.md section 11.2), which calls `runCommandLine` with `stdin`, `stdout`, `stderr` and `RatioArgument::Optional`, and Bun exits with its code. ([history](SPEC-integrator-history.md#bun-jitcache-is-a-subcommand))

## 6. Diagnostics and failures

| code | when | outcome |
|---|---|---|
| `bad-header` | `cache/header` fails its checks, or exists but cannot be opened or read (the error is in the detail) | the cleanup's steps 1 to 4, then `Failed` with no plan |
| `bad-ratio` | the ratio is NaN or outside [0, 1] | `Failed`, with nothing read or removed |
| `incompatible-pack` | the pack of a valid header is of another layout | the cleanup's steps 1 to 4, then `Failed` with no plan |
| `damaged-pack` | a pack of this layout fails its checks | evicted whole by any ratio above 0, which deletes the artifact (section 4.2); kept at ratio 0, with `Done` |
| `damaged-body` | a body fails its checks when the apply checks a dense pack or the rewrite moves it | left out of the new pack and counted in `bodiesEvicted` |
| `remnant-with-bodies` | a header-less `cache/` has a pack that may hold a body | the remnant stays; `NoArtifact` |
| `unknown-file` | a name in `cache/` that is neither `header`, `pack` nor a temporary | left as it is |
| `unlink-failed` | removing a temporary, the `.cache.replaced` leftover or a remnant's pack fails, marking a remnant's pack retired fails, or, in a whole deletion, emptying the pack, unlinking `header`, or marking or unlinking the empty pack fails | what failed stays; the outcome is unchanged, except `Failed` when a whole deletion keeps `header` (section 4.5) |
| `rmdir-failed` | removing `cache/` fails after its last file went | the remnant without a body stays for the next Producer or compact; the outcome is unchanged |
| `io` | opening `<parent>` or `cache/` for a reason other than absence, opening or locking the lock file with an error other than busy, listing `cache/`, examining or mapping the pack, opening it for writing, clearing its retired mark, truncating it, or the rewrite fails | `Failed`, except in the pack's cleanup (section 3, step 5) and when a whole deletion cannot open the pack to mark it retired (section 4.5, step 1): both report it and go on |
| `platform` | the process does not run on Linux (section 2) | `Failed`, with nothing touched |

The cases the sections above leave open read as follows, with no outcome or code beyond the table's:

- A temporary whose `fstatat` or `unlinkat` fails with anything but `ENOENT` is reported as `unlink-failed` and stays, without changing the outcome; one already gone is skipped without a diagnostic.
- Diagnostics of the planning, such as `damaged-pack`, stay in the report whatever the outcome, `NeedsConfirmation`, `Declined` and `PlanChanged` included.

## 7. Files and tests

Maintenance lives in `Source/JavaScriptCore/jitcache/maintenance/`:

| file | holds |
|---|---|
| `JITCacheMaintenance.h` | the interface of section 1, exported to Bun |
| `JITCacheMaintenance.cpp` | the backend and the command line, with their helpers in `Maintenance::MaintenanceInternal` |
| `JITCacheMaintenanceTesting.h` | the test hooks below, a private header declared only in twins builds |

It builds on the store (`jitcache/store/`): `ArtifactStore.h` for the artifact's names, `ProducerLock`, `MappedPack`, `readPackIndex`, `listDirectory`, `temporaryKindOfFileName`, `removeReplacedArtifact`, `createEmptyPack`, `retirePack` and `packHoldsBodies`; `ArtifactWriter.h` for `ArtifactWriter::compactPack` and `ArtifactWriter::checkPackedBody`; `JITCacheContainer.h` for the header's checks and digest and the pack's layout; and `JITCacheFiles.h` for `openAt`, `readHeaderFile` and `errorText`. `defaultArtifactPath` comes from `jitcache/session/JITCacheAPI.h`.

The tests are `Source/JavaScriptCore/jitcache/tests/maintenance/MaintenanceTests.cpp`, run by `testjitcache` in twins builds, on temporary directories whose bodies the writer committed; a test places temporaries, damage and unknown names by hand.

- M1. Cleanup. At ratio 0 over a dense pack, compact removes exactly the temporaries of both kinds and the bytes past the valid end and reports their bytes, and it leaves the bodies, the header and unknown names, reporting each name. It clears an abandoned retired mark, in place on a dense pack and through the rewrite otherwise, and keeps every body. With the rewrite made to fail, the bytes past the valid end are still gone, the mark is cleared and every body stays. It removes a `.cache.replaced` leftover with its files and counts their bytes, and leaves one that holds an unknown name with `unlink-failed`. It keeps a remnant whose pack holds a body, with `remnant-with-bodies` and `NoArtifact` at any ratio; finishes one without bodies; and leaves one that holds an unknown file, with `NoArtifact`. On a path that does not exist, a directory without `cache/` and one whose `cache` is a file, it returns `NoArtifact` before it reads the ratio and creates no lock file. It is `Busy` while a `ProducerLock` of the test process holds the lock. A bad header and a pack of another layout each stop it after the cleanup's steps 1 to 4, with their codes, no plan and the pack untouched.
- M2. Planning and the rewrite. Bodies with known L, P and sizes are planned in increasing (L + P) / S, ties by key bytes, and `postCleanBytes` counts H. Ratio 0 evicts nothing and only cleans up, on an artifact with bodies and on one without, whose header stays. An artifact whose header names an app plans and rewrites as one that names none, and keeps its header byte for byte. A ratio whose target falls inside a body's bytes evicts that body and every body before it, and nothing after. The plan lists the newest version of each key only, and the rewrite leaves the evicted bodies and the superseded versions behind, reclaiming exactly the artifact's decrease. A damaged pack is evicted whole by any ratio above 0, with `damaged-pack`, and stays at ratio 0. A body damaged inside the pack is left out with `damaged-body` while its neighbors move whole, at ratio 0.5 and at ratio 0 alike: a dense pack holding one body with a flipped section byte loses that body to `compact 0` and keeps the others, while a dense pack with no damage keeps its file. A FIFO at `cache/pack`, which `replaceWithFIFO` (`tests/support/TestFiles.h`) puts in place of the pack, is a `damaged-pack` that ratio 0 keeps and ratio 0.5 with `--yes` deletes with the artifact, and a FIFO at `cache/header` is `bad-header` with no plan, each without blocking (`maintenanceCompactTreatsAFIFOAsDamage`). A bad ratio fails and changes nothing. At ratio 0 a dense pack's live bodies are checked, and one that fails is dropped by a rewrite (`maintenanceCompactChecksTheBodiesOfADensePack`); at ratio 0.5 a damaged body the plan keeps is dropped the same way (`maintenanceCompactDropsADamagedBodyItsPlanKeeps`).
- M3. Whole deletion. Ratio 1, a ratio whose target needs every body, and any ratio above 0 on an artifact without bodies give `NeedsConfirmation` with `Ask` and change nothing, temporaries included; `No` gives `Declined` and changes nothing. `Yes` deletes the temporaries, the pack, the header and `cache/`, and keeps the lock file. With the pack's emptying made to fail, `Yes` keeps every body and the header and returns `Failed` with `unlink-failed`, and the store's `open` accepts what remains. With an unknown file in `cache/`, `Yes` returns `Done` with `rmdir-failed`, and the next Producer reuses the remnant.
- M4. Confirmation. A plan confirmed after a commit, a recapture of a key, an eviction or a header change gives `PlanChanged` with the new plan and evicts nothing; an unchanged plan applies.
- M5. Interruption. An apply stopped after the pack's emptying leaves a valid artifact without bodies, and one stopped after the header's unlink leaves a remnant whose pack holds no body, which the next compact finishes. An object that read the empty pack of an apply stopped after the header's unlink finds that pack marked retired once the next compact has finished the remnant (`maintenanceFinishedRemnantRetiresItsPack`). When a Producer of another process, stood for by registry sharing off, reuses that remnant and commits instead, the registry's object for `cache/` lists the Producer's bodies as soon as `take` returns it (`maintenanceReusedRemnantMovesAnObjectToTheNewPack`).
- M6. The command line. Exit codes 0 to 4, with the summary on `out` and the diagnostics on `err`. Usage errors under `RatioArgument::Required`: no arguments, an unknown command, `clean`, `compact` without a ratio, a path before the ratio, a ratio that is not a number, an empty ratio, two paths, an unknown option, a mistyped answer without a path, and both answers. Without a terminal there is no prompt and no change, even with an answer waiting on `in`. Without a path, compact works on `./.jitcache` in the working directory, with and without an answer. On a pseudo-terminal it prints the plan and the prompt and applies on `y` or `Y`, and any other answer is `Declined` with no change. A commit while the prompt waits makes the confirmed call return `PlanChanged`, exit code 5, with nothing evicted.

M1, M3 and M5 set three process-wide hooks that `JITCacheMaintenanceTesting.h` declares; `JITCacheMaintenance.h` cannot hold them, since it is exported and declares nothing twins-only (SPEC-integrator.md section 3.5).

```cpp
#if ENABLE(JITCACHE_TWINS)
namespace JSC::JITCache::Maintenance::Testing {
void setFailingBodyUnlink(std::optional<uint64_t> n);   // each apply's n-th body unlink fails with EIO
void setStopAfterUnlink(std::optional<uint64_t> n);     // each apply stops right after its n-th unlink of a body or of header
void setFailingCompaction(bool);                         // each rewrite fails at its first write with EIO
}
#endif
```

Tests set the hooks while no maintenance call runs, and `std::nullopt` or `false` turns one off. The counts start at 1 within one call. In a whole deletion (section 4.5), the pack's emptying is the one body unlink and the first unlink, and the header's unlink is the second, so `setFailingBodyUnlink(1)` fails step 1, and `setStopAfterUnlink(1)` and `setStopAfterUnlink(2)` stop after steps 1 and 2. A stopped apply removes nothing more and returns `Failed`; returning releases the lock, so the artifact is left as a process that died at that point would leave it. `setFailingCompaction(true)` sets the rewrite's `faultForTesting` to `compaction.write` (section 4.3).

The JS tests run `Maintenance` runs between the runs of a sequence (harness sub-SPEC section 7.3); `JSTests/jitcache/integrator/producer-kill.js` runs `Maintenance compact 0 $ARTIFACT` after each producer kill. The Bun tests run `bun jitcache compact`, the only tests of `RatioArgument::Optional` (SPEC-integrator.md section 15.3).
