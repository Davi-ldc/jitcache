# SPEC-integrator.maintenance: `clean` and `compact`

Part of [SPEC-integrator.md](SPEC-integrator.md), which indexes it; the container sub-SPEC's formats and names apply here. It specifies the backend THREAD Maintenance gives `clean` and `compact`. [SPEC-integrator-history.md](SPEC-integrator-history.md) records the path to the set's non-obvious decisions.

## 1. Interface

`JITCacheMaintenance.h`, exported to Bun (SPEC-integrator.md section 3.5):

```cpp
namespace JSC::JITCache::Maintenance {

enum class Outcome : uint8_t { Done, NoArtifact, Busy, NeedsConfirmation, Declined, PlanChanged, Failed };

struct Diagnostic {
    ASCIILiteral code;     // section 6
    String detail;
};

struct Eviction {
    std::array<uint8_t, 40> key;
    uint64_t bytes { 0 };
    uint64_t version { 0 };       // the envelope's commit identifier; 0 for a body whose envelope failed
    double score { 0 };           // (L + P) / B; -infinity for a body evicted as damaged or foreign
};

struct Plan {
    std::array<uint8_t, 16> headerDigest;     // the artifact's (container sub-SPEC section 3.3)
    uint64_t postCleanBytes { 0 };
    uint64_t targetBytes { 0 };
    uint64_t evictedBytes { 0 };
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
    std::optional<Plan> confirmedPlan;        // set by the second call of section 4.4
};

JS_EXPORT_PRIVATE Report clean(const String& parentPath);
JS_EXPORT_PRIVATE Report compact(const String& parentPath, double ratio, const CompactOptions&);
JS_EXPORT_PRIVATE int runCommandLine(std::span<const CString> arguments, FILE* in, FILE* out, FILE* err);

}
```

## 2. Independence from VMs

The backend needs no VM and no `JSC::initialize`: it uses the filesystem, SHA-256 (SPEC-ucb.md section 3.8) and CRC32C (container sub-SPEC section 4.4), and runs synchronously on the calling thread. It does not compare the header with the running process, since the command line may run under another binary than the one that produced the artifact; it reads the header to check its CRC and structure (container sub-SPEC section 3.2) and to compute its digest. Bodies are judged by their envelopes alone (container sub-SPEC section 4.5, checks B1 to B5 in `Full` mode, with the file's own name as the key and the header's digest): maintenance decides what to evict rather than trusting what it reads, and has no strict switch. Planning therefore reads envelopes only and its memory grows with the number of bodies by one `Eviction`-sized record each, never with their sizes (THREAD Maintenance).

Each call first opens `<parent>` and looks for `cache/` without taking the lock: a parent that does not exist or holds no `cache/` gives `NoArtifact`, so a call on a path without an artifact creates no lock file, and any other failure to open the parent gives `Failed` with `io`. Otherwise it takes the producer lock with `ProducerLock::tryAcquire` on that parent's descriptor, without blocking, and bumps the epoch through it when sections 3 and 4.5 say so (container sub-SPEC section 2). A lock another producer or maintenance holds gives `Busy` and changes nothing, and a `cache/` that went away before the lock was taken gives `NoArtifact`. Each listing of a directory opens it anew, as the container's listings do (container sub-SPEC section 6.2), so a second listing in one call reads the whole directory again. The call holds the lock for all its work and releases it before it returns; a plan that waits for confirmation is therefore never applied under the lock it was planned under, which section 4.4 accounts for.

## 3. `clean`

1. No parent or no `cache/` gives `NoArtifact` without taking the lock (section 2).
2. Take the lock; `Busy` if held, and `NoArtifact` when `cache/` went away meanwhile.
3. Remove every temporary in `cache/`, where the writer creates them (container sub-SPEC section 1.1), adding each one's size (taken with `fstatat` before `unlinkat`) to `bytesReclaimed` and counting it in `temporariesRemoved`. A producer that crashed or was killed mid-commit leaves these (THREAD Failures).
4. A `cache/` without a header (container sub-SPEC section 1.2) is a remnant. When `bodies/` holds no body name and neither directory holds a name step 5 reports, remove `bodies/` and `cache/`. When unknown files remain, leave both directories: a remnant without a body is what the next Producer reuses (container sub-SPEC section 1.3). Otherwise leave every body and report `remnant-with-bodies`. Clean never removes a committed body (THREAD Maintenance).
5. Report every other name in the two directories as `unknown-file`, and leave it.
6. Bump the epoch when anything was removed; release the lock; `Done`.

A header that fails its checks does not stop `clean`, which needs only names; it is reported as `bad-header`.

## 4. `compact`

### 4.1 Scores

The writer stamped L and P, the terms THREAD Maintenance defines, in each envelope (container sub-SPEC section 4.1); P is the CB lane's `counterProgress`, floored, capped and zero for a body whose counter does not travel (SPEC-cb.md section 4.3). The score is `(double(L) + double(P)) / double(B)`, with B the body file's size.

### 4.2 Planning

Compact cleans, then plans whole-body eviction (THREAD Maintenance). The plan is computed on the artifact as the clean leaves it, with temporaries left out of every count, and the clean itself runs only when the plan is applied, so a plan that is never applied changes nothing.

1. No parent or no `cache/` gives `NoArtifact` without taking the lock (section 2). Then take the lock; `Busy` if held.
2. No `cache/` or no header under the lock gives `NoArtifact`; a header that fails its checks gives `Failed` with `bad-header`.
3. A ratio that is NaN or outside [0, 1] gives `Failed` with `bad-ratio`.
4. Read every body's envelope. A body whose envelope fails B1 to B5 is evicted first, with score negative infinity and a diagnostic: `damaged-body` for B1, B2 or B5, `foreign-body` for B4, and `misnamed-body` for B3.
5. `postCleanBytes` is the sum of the body files' sizes plus the header's size, which is what remains once the clean has removed the temporaries. `targetBytes` is `ceil(ratio * postCleanBytes)` (THREAD Maintenance).
6. Order the bodies by score, then by key bytes, both ascending, and take bodies in that order while `evictedBytes < targetBytes`.
7. `deletesArtifact` holds when the ratio is 1, or when `targetBytes > 0` and the plan took every body (THREAD Maintenance). The header's bytes count in `postCleanBytes` and no body removes them, so a target that only the header's bytes could reach takes every body and deletes the artifact; an artifact with no body is such a case for any ratio above 0. A ratio of 0 gives a target of 0, which no plan needs a body to reach, so it never deletes the artifact, even one with no body.

### 4.3 Applying without confirmation

A plan that does not delete the artifact is applied at once, under the lock it was planned under: the clean of `clean`'s steps 3 to 5, then the evictions (section 4.5). A ratio of 0 plans no eviction, so it only cleans.

### 4.4 Confirmation

A plan that deletes the artifact "needs confirmation" (THREAD Maintenance):

- `Answer::Yes`: applied at once, under the same lock.
- `Answer::No`: `Declined` with the plan; nothing is removed, temporaries included.
- `Answer::Ask`: `NeedsConfirmation` with the plan; nothing is removed and the lock is released. The caller asks and, on a yes, calls `compact` again with `answer = Yes` and `confirmedPlan` set to the returned plan. That call takes the lock, plans again, and applies only when the new plan equals `confirmedPlan` field for field, header digest included; otherwise it returns `PlanChanged` with the new plan and removes nothing. Only a plan that deletes the artifact waits for confirmation, and its evictions list every body with its key, size and version, so a body added, removed or rewritten meanwhile changes the plan, as THREAD Maintenance requires.

### 4.5 Applying

Evictions are `unlinkat` calls, one file each, so every surviving body stays whole and an interruption leaves only "a partial eviction" (THREAD Maintenance). Each successful unlink adds the body's size to `bytesReclaimed` and counts in `bodiesEvicted`; an unlink that fails is reported as `unlink-failed` and the body stays.

An applied plan first removes the temporaries, as `clean` does, then evicts. A whole-artifact deletion removes, in order: every temporary, every body, `header`, then `bodies/` and `cache/` with `unlinkat(..., AT_REMOVEDIR)`; the lock file stays (THREAD Storage). Until `header` goes the artifact is valid with fewer bodies, and afterwards `cache/` is a remnant with no body, which the next Producer or `clean` finishes (container sub-SPEC section 1.3). When a body's unlink fails, the deletion stops before `header`: a header-less `cache/` that still held a body would make `start` reject every role at `start.not-an-artifact` and `clean` keep it as `remnant-with-bodies`, while the artifact with its header stays valid with fewer bodies. A directory removal that fails after `header` is gone, such as `bodies/` or `cache/` holding an unknown file, leaves a remnant without a body, which the next Producer reuses. Consumers that pinned the deleted directory see their lookups miss (container sub-SPEC sections 5.2 and 6.3).

After applying, the epoch is bumped once, the lock released, and the outcome is `Done`, except for a whole-artifact deletion that stopped before `header`, which returns `Failed`.

## 5. Command line

`runCommandLine` is the one front end both hosts use:

```text
clean <path>
compact <path> <ratio> [--yes | --no]
```

- `clean` calls `clean(path)` and prints one summary line: the outcome, the temporaries removed and the bytes reclaimed.
- `compact` calls `compact(path, ratio, options)` with `Answer::Yes` for `--yes`, `Answer::No` for `--no`, and `Answer::Ask` otherwise. On `NeedsConfirmation`, when `in` is a terminal (`isatty`), it prints the plan's summary (bodies, bytes, and that the whole artifact goes) and the prompt `This operation will delete the entire cache. Ok? (y/n)`, reads one line, and on `y` or `Y` calls `compact` again with the returned plan as `confirmedPlan`; any other answer is `Declined`. When `in` is not a terminal, it prints the plan and stops, changing nothing.
- Each run prints its summary to `out` and its diagnostics, one per line, to `err`.

Exit codes: 0 for `Done` and `NoArtifact`, 1 for `Failed`, 2 for a usage error, 3 for `Busy`, 4 for `NeedsConfirmation` and `Declined`, 5 for `PlanChanged`.

The jsc shell runs it with `--jitcache-maintenance`, passing the script arguments (those after `--`) as the command line (SPEC-integrator.md section 11.1 says when).

Bun runs it as `bun jitcache <command>`. `src/runtime/cli/mod.rs` gains `Tag::JITCacheCommand`, matched by `RootCommandMatcher::case(b"jitcache")`, dispatched to `jitcache_command::JITCacheCommand::exec()` in the new `src/runtime/cli/jitcache_command.rs` (declared with `#[path = "jitcache_command.rs"] pub(crate) mod jitcache_command;`), and a help entry, `bun jitcache clean <path>` and `bun jitcache compact <path> <ratio> [--yes|--no]`. `exec` passes the arguments after `jitcache` to `Bun__JITCache__runMaintenance` (SPEC-integrator.md section 11.2), which calls `runCommandLine` with `stdin`, `stdout` and `stderr`, and exits with its code.

## 6. Diagnostics and failures

| code | when | outcome |
|---|---|---|
| `bad-header` | the header fails its checks | `clean` goes on; `compact` returns `Failed` and removes nothing |
| `bad-ratio` | the ratio is NaN or outside [0, 1] | `Failed` |
| `damaged-body`, `foreign-body`, `misnamed-body` | an envelope fails B1, B2 or B5; B4; B3 | the body is evicted first |
| `remnant-with-bodies` | a header-less `cache/` holds body names | nothing is removed but temporaries |
| `unknown-file` | a name that is neither a body, a temporary nor `header` | left as it is |
| `unlink-failed` | an `unlinkat` fails | the body stays; the outcome is still `Done`, except that a whole-artifact deletion keeps `header` and returns `Failed` (section 4.5) |
| `rmdir-failed` | removing `bodies/` or `cache/` fails after `header` is gone | the bodiless remnant stays for the next Producer or `clean`; the outcome is still `Done` |
| `io` | opening a directory (the parent's absence aside, section 2), opening the lock file, locking it with an error other than `busy`, or listing fails | `Failed` |

## 7. Tests

`tests/MaintenanceTests.cpp`, on temporary directories with bodies the writer produced:

- M1. `clean` removes exactly the temporaries and reports their bytes; it leaves bodies, the header, unknown files and a remnant's bodies; it finishes a header-less remnant with no body, and leaves one that holds an unknown file in place with `Done`; on a path that does not exist, and on a directory without `cache/`, `clean` and `compact` return `NoArtifact` and create no lock file; it is `Busy` while a `ProducerLock` of the test process holds the lock.
- M2. Ordering: bodies with known L, P and sizes are evicted in increasing (L + P) / B, ties by key; ratio 0 evicts nothing, and on an artifact with no body it only cleans and keeps the header; a ratio whose target falls inside a body's bytes evicts that body; damaged, foreign and misnamed bodies go first with their diagnostics.
- M3. Whole deletion: ratio 1, a ratio whose target needs every body, and any ratio above 0 on an artifact with no body give `NeedsConfirmation` with `Ask` and change nothing, temporaries included; `No` gives `Declined` and changes nothing; `Yes` deletes temporaries, bodies, header and directories and keeps the lock file. With one body's unlink made to fail (a test hook), `Yes` evicts the others, keeps `header` and that body, returns `Failed` with `unlink-failed`, and the store's `open` accepts what remains; with an unknown file in `bodies/`, it returns `Done` with `rmdir-failed`, and the next Producer reuses the remnant.
- M4. Confirmation: a plan confirmed after a commit, an eviction or a header change in between gives `PlanChanged` and evicts nothing; an unchanged one applies.
- M5. Interruption: an apply stopped after each unlink (a test hook) leaves an artifact that the store's `open` and its index accept (container sub-SPEC section 7.2), with only whole bodies; one stopped after the header's unlink leaves a remnant that `clean` finishes.
- M6. The command line: each exit code; the prompt with a terminal (a pseudo-terminal in the test), `y` and `n`; no prompt and no change without a terminal.

The JS tests run maintenance between runs of a sequence (harness sub-SPEC section 7.3), and the Bun tests run `bun jitcache` (SPEC-integrator.md section 15.3).
