# JITCache macro benches

These benches measure a serverless cold start with real libraries. Each case is shaped like a function a company deploys: a fresh Bun process sets it up and then runs passes, each of which runs every unit of the case once, in order. With JITCache off, a process interprets, compiles and profiles its code during its first passes. A JITCache consumer installs the baseline code a producer committed, so its early passes should come closer to the producer's warmed ones. The design is in [docs/JitCache/drafts/macro-benches.md](../../docs/JitCache/drafts/macro-benches.md), and HARNESS.md's Benches section defines the numbers.

## Cases

| case | one pass | libraries |
|---|---|---|
| `api` | 100 requests with `fetch` to a server in the same process, one at a time: 40 `GET /users/:id`, 30 `POST /orders` whose every fifth body fails validation and gets a 400 with Zod's issues, 20 `GET /search?q=&sort=` that filter and sort 1,000 users, and 10 `GET /health`, in one seeded shuffled order | Hono 4.13 with Zod 4.6, served by `Bun.serve` on 127.0.0.1 with port 0 |
| `elysia` | 100 requests with `fetch` to a SaaS control plane's API in the same process, one at a time, in one seeded order. Four users' dashboard sessions read `/v1/me`, list a team's projects with search, filters, sorting and pages, open projects and their deployments, list team members, and create, deploy and delete six projects; creating one of them a second time gets 409. Among them are 6 CORS preflights, 2 health checks, 7 requests that fail validation and get Elysia's 422, 5 that get 403 or 404, and 12 whose bearer token is missing, altered, expired or signed with another key, which get 401 | Elysia 1.4.30 with @elysiajs/cors 1.4.2 and jose 6.2.12, served by Elysia's `listen` (`Bun.serve`) on 127.0.0.1 with port 0 |
| `ingest` | `POST /documents` with each of the six PDFs in `inputs/pdf`: the function extracts the text page by page, normalizes whitespace, splits each page into chunks of about 1,000 characters that overlap by about 100, hashes each chunk with SHA-256 and answers the chunks with the document's metadata. Between the documents, `POST /events` with three webhook batches of 300, 250 and 350 orders, refunds and customer updates: the function validates each record with strict Zod schemas, reports the 4 invalid records of each batch, counts its 3 redeliveries once and answers totals by currency and event type and a summary per customer | unpdf 1.8.1, which bundles pdf.js 6.1.200, and Zod 4.6, served by `Bun.serve` routes on 127.0.0.1 with port 0 |
| `ssr` | renders the page of each of 10 shop categories with `renderToString`: a layout, a header, the navigation, breadcrumbs and a grid of 60 product cards with badges, rating stars, sale prices, color swatches and stock notes | React and React DOM 19.3 |

Each pass returns everything it produced: each response's status and body for `api`, the status, every header but `Date` and the body for `elysia` and `ingest`, and the HTML for `ssr`. `run.ts` hashes that output with SHA-256 after the pass's clock stops. `api` leaves its headers out, since Hono's timing middleware puts each request's duration in `Server-Timing`. The `api` and `elysia` servers create records whose ids continue from pass to pass, so their checksum changes from one pass to the next; `ingest` and `ssr` produce the same output in every pass. In every case, pass k must give the same checksum in every process.

Setup builds every dataset with a seeded generator and reads every input file, so no pass touches the file system. `elysia` signs its test tokens at setup with a fixed HS256 key and fixed claims that expire in 2099, so every process sends the same tokens. Its server takes the ids and times of the records it creates from a seeded generator and a clock that advances one second per write, where a deployment would read the system clock. A case checks its own output as it goes. `api` requires the status each request is due and the `Server-Timing` header on every response, and `elysia` requires the status each request is due and the dashboard's CORS headers on every request a browser sends. `ingest` requires at least one chunk per page of each document, and requires each batch to reject exactly the records it was built with and to count its redeliveries. `ssr` requires each page to end its document.

Elysia validates a request before the session check runs, so a request with an invalid body and no token gets 422; the 401 requests carry valid input. unpdf runs pdf.js's worker code on the calling thread, so `ingest` starts no `Worker`. pdf.js detaches the buffer it parses, so the function hashes the document before handing it over, and it opens documents with verbosity 0 because pdf.js prints its font warnings with `console.log`, onto the standard output that carries `run.ts`'s events.

## Running a case by hand

```sh
cd PerformanceTests/JITCache
bun install
NODE_ENV=production bun run.ts elysia --passes=5
```

`run.ts` takes the case name and `--passes=N`, 34 by default. It refuses to run without `NODE_ENV=production`, the setting of a deployment: React picks its production or development build from it when it loads, and Bun picks the production or development JSX transform when the process starts, so the script cannot set it for itself.

The process writes JSON lines to standard output:

```
{"event":"ready","startupNanoseconds":118283896}
{"event":"pass","pass":1,"milliseconds":143.15,"checksum":"0ff02300..."}
...
{"event":"end","rssBytes":63188992}
```

`startupNanoseconds` is `Bun.nanoseconds()` once setup is done: imports loaded (for `ingest`, pdf.js too, which unpdf would otherwise load on the first request), inputs read, data built and, for the servers, the server listening. `milliseconds` is the wall time of one pass from `performance.now()`. `rssBytes` is `process.memoryUsage().rss` after the last pass. The process exits 0 after the end line, 1 when a case's check or a library fails, with the error on standard error, and 2 on a bad command line or without `NODE_ENV=production`.

JITCache's flags go before the script. A producer needs an artifact path that holds no artifact yet, and Bun commits the warmed bodies when the process exits:

```sh
NODE_ENV=production <bun> --jitcache=/tmp/macro-api --jitcache-mode=p --jitcache-bench-report=/tmp/producer.jsonl run.ts api
NODE_ENV=production <bun> --jitcache=/tmp/macro-api --jitcache-bench-report=/tmp/consumer.jsonl run.ts api
```

## The driver

`Tools/Scripts/jitcache-macro-bench.ts` runs the cases with a Bun that has JITCache, normally the release build from `bun build.ts bun-release`:

```sh
bun Tools/Scripts/jitcache-macro-bench.ts --bun=<Bun executable> [--cases=api,elysia,ingest,ssr] [--processes=M] [--passes=P] [--out=<directory>] [--timeout=<seconds>]
```

M is 5 and P is 34 by default. For each case the driver first runs one producer with `--jitcache=<fresh artifact> --jitcache-mode=p`, then M processes with JITCache off alternating with M consumers of the producer's artifact, so drift on the machine spreads over both. It runs one process at a time, in this directory, with `NODE_ENV=production`, `BUN_DEBUG_QUIET_LOGS=1` (a debug Bun's scoped logs would otherwise mix with the events) and no `JSC_` or `BUN_JSC_` variable. Every JITCache process writes a bench report. The driver holds the build lock, `${XDG_CACHE_HOME:-$HOME/.cache}/jitcache/build.lock`, while it measures, and says so when it has to wait for a build. A process that runs longer than `--timeout`, 1800 seconds by default, is killed.

A case fails when a process exits with anything but 0, lacks one of its events, or gives a pass a checksum that differs from another process's. A JITCache process also fails when its report has no `start` event that started its role, when Bun warns on standard error that JITCache turned itself off or stopped writing, when the producer committed no body, or when a consumer installed none. A failed case keeps its directory under the output directory, with every process's output, the bench reports and the artifact; a passed case's directory is deleted.

The output directory, by default `jitcache-macro/<time>` next to the Bun executable, receives two files. `results.jsonl` holds one record for the run, one per process (command, exit, wall time, startup, every pass's time and checksum, peak memory and the bench report's counts) and one per case. `summary.md`, which the driver also prints, gives for each case:

- startup and the first pass of the off processes (N1) and the consumers (C1), and C1/N1;
- W, the median of the producer's last five passes, and the curves Ck/W and Nk/W for k from 2 to P, with the first pass within 1.1 of W, a candidate for HARNESS.md's `catchUpMargin`;
- each mode's process wall time and peak resident set;
- the bench report's counts: installs, requests by action, lookups by result (an absent key is a miss), native baseline compiles, captures by trigger and outcome, committed bodies and bytes, and JITCache's CPU time in each.

The producer's own startup and first pass sit beside the others, since it records while it runs. Values over several processes are the minimum, with the median beside it. The driver exits 0 when every case passed, 1 otherwise, and 2 on a bad command line.

## Inputs

`inputs/pdf` holds six documents of the US federal government, 3.7 MB in all, each text-based and of a different kind. A work that an officer or employee of the US government prepares as part of their official duties has no copyright in the United States (17 U.S.C. § 105). They were downloaded on 2026-10-13, and five are copied unchanged.

| file | document | pages | source | why it is public domain |
|---|---|---|---|---|
| `bea-personal-income-2025-08.pdf` | "Personal Income and Outlays, August 2025", news release BEA 25-41 of the Bureau of Economic Analysis, September 26, 2025: text and tables | 13 | https://www.bea.gov/sites/default/files/2025-09/pi0825.pdf | a release written by BEA staff, 17 U.S.C. § 105 |
| `census-acsbr-025.pdf` | "Household Income in States and Metropolitan Areas: 2024", American Community Survey Brief ACSBR-025 of the US Census Bureau, by Gloria G. Guzman, September 2025: a statistical report with tables and maps | 10 | https://www2.census.gov/library/publications/2025/demo/acsbr-025.pdf | a report written by a Census Bureau employee, 17 U.S.C. § 105 |
| `federal-register-2025-23783.pdf` | Federal Register document 2025-23783, Vol. 90, No. 244, December 23, 2025, page 59945 on: the Department of Homeland Security's final rule "Privacy Act of 1974" (6 CFR Part 5), in the Register's three columns; its last page also starts a Federal Housing Finance Agency rule | 4 | https://www.govinfo.gov/content/pkg/FR-2025-12-23/pdf/2025-23783.pdf | a rule written by DHS and published by the Office of the Federal Register, 17 U.S.C. § 105 |
| `irs-form-w-9.pdf` | Form W-9 (Rev. March 2024), "Request for Taxpayer Identification Number and Certification", of the Internal Revenue Service: a fillable form with its instructions | 6 | https://www.irs.gov/pub/irs-pdf/fw9.pdf | a form written by the IRS, 17 U.S.C. § 105 |
| `nist-sp-800-218-pages-1-16.pdf` | pages 1 to 16 of NIST Special Publication 800-218, "Secure Software Development Framework (SSDF) Version 1.1", February 2022: the front matter, the introduction and the first three pages of the practices table | 16 of 36 | https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-218.pdf | its page 3 says the publication "is not subject to copyright in the United States" |
| `supreme-court-22-1218.pdf` | the slip opinion of the Supreme Court of the United States in Smith v. Spizzirri, No. 22-1218, decided May 16, 2024 | 8 | https://www.supremecourt.gov/opinions/23pdf/22-1218_5357.pdf | judicial opinions have no copyright under the government edicts doctrine, and the Court's opinion is a work of the federal government, 17 U.S.C. § 105 |

The IRS URL serves the form's current revision, and the file here is the March 2024 one. The NIST excerpt came from qpdf 11.9.0, which copies the pages and the document's metadata without changing their content; with `--deterministic-id` the command below gives the same bytes again:

```sh
qpdf --deterministic-id NIST.SP.800-218.pdf --pages . 1-16 -- nist-sp-800-218-pages-1-16.pdf
```

```
3308842a2c5eb813444b05d83820aed19e8e673cf301e838327ff7fffe4106e3  bea-personal-income-2025-08.pdf
5de1b0d5ec469344896bdf8485d3e93206351c0f1f9e51029b8d57d2df9054b9  census-acsbr-025.pdf
85a83cfaa8b688a5a16b1ecb28ff9109304ee7f9ad77b36186991806dfc976b0  federal-register-2025-23783.pdf
2d420cbb4123dcf1fb82595b2359cfbb5d81f00b9df9d359fcc7af361d093f53  irs-form-w-9.pdf
298697e0a140e0fa4a3f026b282dc873a03f18575935cc8a25747044b177e4ad  nist-sp-800-218-pages-1-16.pdf
f21d3b8e0f0759fb3314a7ac847a7cc0e7dad69c2e83ec7caf36961634e8bb01  supreme-court-22-1218.pdf
```

Those are the files' SHA-256 sums; the full NIST SP 800-218 from the URL above has `617746e553a9e2da49bfbd4eef0dfc3094758a39b869314e4173ac36605cde22`.

`package.json` pins each library's exact version, and `bun.lock` pins the rest, among them Elysia's peers `@sinclair/typebox` 0.34.52, `file-type` and `openapi-types`.
