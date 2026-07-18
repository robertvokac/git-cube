# GitCube implementation status

This is a running log of what has shipped and how it was verified, kept for anyone
tracking the project's progress or picking up development. It intentionally does not
explain how to use a feature or what GitCube's user-facing limitations are — that's
[`README.md`](../README.md). Current database schema version: **7** (see
`kMigrations[]` in `src/database.cpp`).

## Shipped, by area

**Core mirror/queue/worker system** — first-run `data/` creation, `git clone --mirror`
with atomic staging under `data/tmp`, a SQLite-backed persistent job queue
(clone/fetch/health/metadata/account) claimed by a configurable worker pool, per-repo
mutual exclusion (no two workers touch the same repo's directory at once), pause/resume,
SIGINT/SIGTERM handling with interrupted-job recovery.

**Web UI** — server-rendered dashboard, repository detail page, branch/tag/file-tree/
blob/commit/diff browsing without a worktree, root-`README.md` auto-render, safe Markdown
subset, CSRF-protected POST actions, live status polling via `/api/status`.

**Repository list UX** — search, filter by status/GitHub-account/git-tag/importance,
pagination (25/page); 0–3 importance rating, editable per repository.

**Job history UX** — pagination (50/page), filter by status.

**GitHub integration** — unauthenticated public metadata plus fully paginated releases;
bulk account import follows GitHub pagination links and persists one 100-item page at a
time; rate-limit detection honors `Retry-After`/`X-RateLimit-Reset` with jitter and
delays the triggering job plus every pending GitHub job. Transient network/5xx failures
retry with bounded exponential backoff. API URLs stay pinned to
`https://api.github.com/`, requests are serialized, individual responses are capped at
8 MiB, and release accumulation has a 32 MiB budget.

**Export** — branch/tag as a files-only zip (`git archive`), or the whole bare mirror as
a zip (re-clonable, no worktree), generated into an owner-only temporary file and
streamed with a 500 MiB cap, a global one-export limit, CSRF-protected POST initiation,
and per-repository locking against concurrent fetch.

**Schema migrations** — an ordered, additive `kMigrations[]` list applied to every
database (fresh or old) after a frozen v1 `CREATE TABLE` snapshot, plus a
`verify_schema()` startup check that fails loudly and specifically if a load-bearing
column is missing.

**Independent repository states** — local availability, the currently running
operation, health-check outcome, and GitHub-metadata outcome have separate persistent
fields and error messages. Job completion/requeue and startup recovery atomically clear
stale operation markers, while an unexpected worker exception requeues with backoff
and eventually fails after three attempts instead of leaving a job permanently running.

**Canonical repository identity** — strict authority/path parsing normalizes default
ports, DNS spelling and percent encoding; GitHub URL aliases/case map to one indexed
canonical key. New mirrors live under `repositories/by-id/<id>.git`, eliminating the
lossy filename-sanitization collisions while preserving every legacy storage path.

**SQLite connection reuse** — database calls use a thread-safe process-local connection
pool instead of reopening SQLite and reapplying WAL configuration for every operation.
The pool retains at most 32 idle handles and has a concurrent read/write regression test.

**Bounded live UI queries** — two-second dashboard polling requests only the repository
IDs visible on the current page (maximum 100) and gets job counters plus lightweight
states in one pooled DB operation. Repository detail responses cap rendered refs and
releases while retaining their complete database records.

**Bounded job history** — schema v7 adds indexes for active-job dedup/claiming,
terminal-history pruning and release ordering. GitCube retains the latest 5,000 finished
jobs and full command output for the latest 200; active/queued work is never pruned.
Message, payload, output and error fields are capped at write time.

**Continuous integration** — GitHub Actions blocks on GCC Debug/Release, Clang Debug,
warnings-as-errors, cppcheck, ASan+UBSan, and a gcovr line-coverage floor. CMake exposes
dedicated warnings/sanitizer/coverage options, all CI builds are capped at two parallel
tasks, and CTest enforces a per-test timeout.

**Security hardening pass** (found via review, fixed and verified in the same session):
SVG/HTML/XML raw-blob content-type XSS, a job-queue race allowing duplicate concurrent
jobs, per-repo job mutual exclusion, interrupted/paused jobs being recorded as permanent
failures instead of requeued, a `%2F`-smuggling path-validation bypass, unbounded HTTP
client threads / no per-request deadline (slowloris), `Transfer-Encoding` silently
dropping request bodies, unbounded JSON parser recursion, `git ls-tree -l` size column
always reading as 0 (padding broke the naive split).

**Favicon** — hand-authored SVG (cube + planet-and-ring), rendered to a multi-resolution
ICO and an apple-touch-icon PNG, checked at actual 16/32px size before finalizing;
embedded as C++ constants, no runtime file I/O.

**Add-repositories UX** — AJAX "does GitCube already have this?" check with no page
reload, using a previously-written-but-unused `get_repository_by_url`.

## Verified

- Debug and Release builds, GCC 14, clean under `-Wall -Wextra -Wpedantic -Wconversion
  -Wshadow`.
- Unit test suite and AddressSanitizer + UndefinedBehaviorSanitizer clean across every
  live-tested scenario below, not just the unit tests.
- End-to-end clone, browse and fetch against real public GitHub repositories (not just a
  local test Git server).
- GitHub account bulk import against a real account (48 repositories), including
  concurrent clone/metadata job scheduling.
- The GitHub API rate-limit backoff was exercised against the **real, actually
  exhausted** rate limit (not simulated) and confirmed to reschedule rather than fail.
- Migration convergence tested three ways: a brand-new database, a hand-built
  pre-migration (v1) database, and a database already at v3 — all reach the then-current
  v4 cleanly with no data loss. Automated downgrade fixtures now exercise convergence
  through schema v6, including state migration and canonical-URL backfill.
- `verify_schema()` confirmed to actually fire: a hand-built database with a
  `schema_migrations` row falsely claiming v4 while missing the v4 column fails loudly
  at startup with a specific error, instead of a cryptic later query failure.
- SIGTERM sent mid-clone: process exits within its grace period, the job comes back as
  `queued` (not `failed`), and the orphaned temp clone directory is gone after the next
  start.
- A repository exported as a whole-mirror zip was extracted and `git clone`d from
  successfully — a real, working bare repository, not just a well-formed archive.
- CSRF rejection, double-click job-dedup, and malicious-ref rejection (`--upload-pack=…`
  style) all behave as intended under live testing.

## Known gaps (development-facing)

- Dynamic HTML/JSON responses are still buffered, but large raw blobs and ZIP exports
  now use bounded temporary files streamed by `http_server.cpp`.
- Repo-browsing routes (tree/blob/commits/archive) run `git` synchronously on the
  HTTP request thread rather than going through the job queue — acceptable because
  they're local reads, but they do block that connection's thread for their duration.
- A handful of reuse/simplification findings from the original review were not applied
  (e.g. positional-field parsing of `git log`/`ls-tree` output by index rather than by
  name, some duplicated curl-argument construction) — tracked as low-priority polish,
  not correctness issues.
- The compact test suite still uses a dependency-free assertion harness rather than a
  richer parameterized/fuzzing framework; security-sensitive parsers have targeted
  regression cases, but broader fuzzing remains useful future work.
- Windows/macOS untested and unsupported; the process/signal/socket layer is
  POSIX-specific by design for now.
