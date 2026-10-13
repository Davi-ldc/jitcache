# JITCache macro benches

These benches measure a serverless cold start with real libraries. Each case is a small app written the way a framework's getting-started guide writes it, and a fresh Bun process calls the app's fetch handler with the same list of requests, pass after pass, as a serverless platform invokes a function. With JITCache off, a process interprets, compiles and profiles the app and its libraries during its first passes. A JITCache consumer installs the baseline code a producer committed, so its early passes should come closer to the producer's warmed ones. The design is in [docs/JitCache/drafts/macro-benches.md](../../docs/JitCache/drafts/macro-benches.md), and HARNESS.md's Benches section defines the numbers.

## Layout

`apps/<case>/` holds an app's own files and nothing else: `index.ts` or `index.tsx`, which exports the request handler the way serverless platforms take it, and the JSON file it reads its data from, when it has one. The Hono and Elysia apps export the app itself (`export default app`); the others export `{ fetch }`. `requests/<case>.json` lists the requests of one pass, in order. `requests/pdf.ts` builds its list from the files in `inputs/pdf`, since its bodies are binary. `run.ts` is the only bench code: it imports a case's app and requests, runs the passes and prints their times and checksums.

## Cases

| case | the app | one pass | libraries |
|---|---|---|---|
| `hono` | A blog's JSON API: a hello route at `/`, the posts in `posts.json` with an optional `?tag=` filter, one post by id, and creating a post, validated by a Zod schema through `@hono/zod-validator`, or deleting one. | 30 requests: 19 reads, two of which get 404; 3 posts created and 4 bodies rejected with 400; 4 deletes, the last of a post already gone (404). | Hono 4.13.13, @hono/zod-validator 0.9.1, Zod 4.6.7 |
| `elysia` | The todo API of Collo's Elysia example (list, create with a `t` schema, delete) behind a login: `POST /login` checks a fixed demo user and answers a token from `@elysiajs/jwt`, and the todo routes require it as a bearer token. | 24 requests: 3 logins, of which one succeeds, one has a wrong password (401) and one lacks the password (422); 5 todos created and deleted; 5 lists; 2 todos rejected with 422; 4 requests with no token or a bad one, which get 401. | Elysia 1.4.30, @elysiajs/jwt 1.4.2 |
| `react-ssr` | A shop rendered with React's `renderToString`: a layout, a header and either the product grid of `products.json` at `/` or one product at `/products/:id`. | 26 requests: the home page 5 times, each of the 20 product pages once, and a missing product (404). | React and React DOM 19.3.0 |
| `preact-ssr` | The same shop written for Preact and rendered with `preact-render-to-string`. | The same 26 requests as `react-ssr`. | Preact 11.0.1, preact-render-to-string 6.8.0 |
| `pdf` | A function that extracts the text of an uploaded PDF with `extractText(..., { mergePages: true })`, as unpdf's README shows, and answers the page count and the text as JSON. | 6 uploads, one for each PDF in `inputs/pdf`. | unpdf 1.8.1, which bundles pdf.js |
| `webhook` | A payments webhook that validates each incoming event with a Zod discriminated union of five event types and answers 200, or 400 with Zod's issues. | 24 requests: 17 valid events, 5 invalid ones, a truncated JSON body (400) and a `GET` (405). | Zod 4.6.7 |

The Hono and Elysia apps number the records they create from a counter, so the ids continue from pass to pass. Each pass deletes what it created, so every pass does the same work, but the checksum of pass k differs from that of pass k - 1. The other cases give the same checksum in every pass. In every case, pass k must give the same checksum in every process.

`@elysiajs/jwt` writes the current time into a token's `iat` claim whatever value it is given, unless `iat` is `false`. The Elysia app passes `iat: false` and, like the plugin's documented example, sets no `exp`, so the login answers the same token in every process. Elysia validates a request before the token check runs, so a request with an invalid body and no token gets 422; the requests that should get 401 carry valid bodies.

The Preact app selects Preact's JSX runtime with a `/** @jsxImportSource preact */` comment, because Bun did not apply a `tsconfig.json` placed in the app's folder. unpdf imports pdf.js on its first call, so the `pdf` case's first pass includes loading it. pdf.js prints its font warnings on standard error.

## Requests

Each request in a requests file has a `method`, a `path`, and optionally `headers` and a `body`. A body that is neither a string nor bytes is sent as JSON with `content-type: application/json`, unless the request names another content type. `status` is the status the app must answer; a different one stops the process with exit 1, so a broken app fails instead of being measured on its error pages. `save` keeps a response's JSON under a name, and a later request of the same pass can write one of its fields into its path or a header as `{name.field}`: the Elysia requests send `Bearer {login.token}` and delete `/todos/{milk.id}`, as a client sends back the token or id the API gave it.

`run.ts` builds a `Request` for `http://localhost` plus the path and awaits the handler's `Response` and its whole body. After the pass's clock stops, it hashes every response's status, headers and body with SHA-256. No app sets a header that holds a time or a random id, so every header counts.

## Running a case by hand

```sh
cd PerformanceTests/JITCache
bun install
NODE_ENV=production bun run.ts elysia --passes=5
```

`run.ts` takes the case name and `--passes=N`, 34 by default. It refuses to run without `NODE_ENV=production`, the setting of a deployment: React picks its production or development build from it when it loads, and Bun picks the production or development JSX transform when the process starts, so the script cannot set it for itself.

The process writes JSON lines to standard output:

```
{"event":"ready","startupNanoseconds":100854442}
{"event":"pass","pass":1,"milliseconds":17.24,"checksum":"cd838927..."}
...
{"event":"end","rssBytes":48095232}
```

`startupNanoseconds` is `Bun.nanoseconds()` once the app and its requests are imported. `milliseconds` is the wall time of one pass from `performance.now()`. `rssBytes` is `process.memoryUsage().rss` after the last pass. The process exits 0 after the end line, 1 when a request gets an unexpected status or the app throws, with the error on standard error, and 2 on a bad command line or without `NODE_ENV=production`.

JITCache's flags go before the script. A producer needs an artifact path that holds no artifact yet, and Bun commits the warmed bodies when the process exits:

```sh
NODE_ENV=production <bun> --jitcache=/tmp/macro-hono --jitcache-mode=p --jitcache-bench-report=/tmp/producer.jsonl run.ts hono
NODE_ENV=production <bun> --jitcache=/tmp/macro-hono --jitcache-bench-report=/tmp/consumer.jsonl run.ts hono
```

## The driver

`Tools/Scripts/jitcache-macro-bench.ts` runs the cases with a Bun that has JITCache, normally the release build from `bun build.ts bun-release`:

```sh
bun Tools/Scripts/jitcache-macro-bench.ts --bun=<Bun executable> [--cases=hono,elysia,react-ssr,preact-ssr,pdf,webhook] [--processes=M] [--passes=P] [--out=<directory>] [--timeout=<seconds>] [--check]
```

`--check` checks a build's correctness without measuring it, for instance a shipping build while another compiles: it takes no build lock, runs 1 process per mode and 3 passes unless told otherwise, and applies every check below; its times mean nothing, and its summary says so.

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

`package.json` pins each library's exact version, and `bun.lock` pins the rest, among them Elysia's peers `@sinclair/typebox` 0.34.52, `file-type` and `openapi-types`, and `jose` 6.2.12, which `@elysiajs/jwt` signs and verifies with.
