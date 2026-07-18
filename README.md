# GitCube

GitCube is a local C++20 server for maintaining and browsing a collection of public Git
repositories. It replaces a shell-script clone/fetch workflow with a SQLite-backed
persistent job queue, a configurable worker pool, health checks, public GitHub metadata,
and a small server-rendered web UI — no Node.js, no external web framework, no worktrees
on disk.

## Contents

- [Features](#features)
- [Quick start](#quick-start)
- [User guide](#user-guide)
  - [Adding repositories](#adding-repositories)
  - [The repository list](#the-repository-list)
  - [Repository detail page](#repository-detail-page)
  - [Browsing files, commits and Markdown](#browsing-files-commits-and-markdown)
  - [Exporting a repository as a .zip](#exporting-a-repository-as-a-zip)
  - [Jobs](#jobs)
  - [GitHub metadata and rate limits](#github-metadata-and-rate-limits)
  - [Safe shutdown](#safe-shutdown)
- [Command-line reference](#command-line-reference)
- [For developers](#for-developers)
  - [Source layout](#source-layout)
  - [Request/data flow](#requestdata-flow)
  - [Data directory layout](#data-directory-layout)
  - [Database schema and migrations](#database-schema-and-migrations)
  - [Security model](#security-model)
  - [Building and testing](#building-and-testing)
- [Current limitations](#current-limitations)
- [License](#license)

## Features

- Mirrors repositories with `git clone --mirror` — bare `.git` directories only, no
  worktree ever touches disk.
- Persistent SQLite-backed job queue (clone, fetch, health check, GitHub metadata,
  GitHub-account bulk import) processed by a configurable worker pool.
- Bulk import from a textarea: one URL per line, or a bare `https://github.com/<account>`
  URL to queue every public repository under that account.
- An AJAX "does GitCube already have this?" check on the Add repositories page — no page
  reload.
- Search, filter (status, GitHub account, git tag, importance) and paginated browsing of
  the repository list.
- A 0–3 "importance" rating per repository (★/★★/★★★), filterable and editable from the
  repository detail page.
- Pause/resume per repository; paused repositories are skipped by queued and bulk work.
- Automatic backoff when the GitHub API rate limit is hit — the affected job (and every
  other pending GitHub job) is rescheduled instead of being marked permanently failed.
- Remote-availability and `git fsck --full --no-dangling` health checks.
- Public GitHub repository metadata and up to 100 releases per repository, without a
  token (unauthenticated REST API).
- Web browsing of branches, tags, commit history, commit diffs, file trees and blobs —
  all read directly from the bare mirror, no checkout needed.
- Automatic rendering of the root `README.md` on a repository's file listing, and of any
  `.md`/`.markdown` file when viewed directly, through a small safe Markdown subset.
- Export a branch/tag as a files-only `.zip` (`git archive`), or the whole bare mirror as
  a `.zip` (re-clonable as-is, no worktree).
- Paginated, filterable job history.
- Safe shutdown on Ctrl+C/SIGTERM with interrupted-job recovery on the next start.
- No SSH URLs, credentials, tokens or private-repository support — public HTTP(S) only.

## Quick start

Debian 13 dependencies:

```bash
sudo apt install build-essential cmake git curl zip libsqlite3-dev
```

No web framework is downloaded during the build: the HTTP server is hand-written on
Linux/POSIX sockets, which keeps the dependency set to `git`, `curl`, `zip` and
`libsqlite3-dev`.

Build and test:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Run it:

```bash
./build/gitcube
```

Unless `--data-dir` is supplied, persistent data is stored under
`$XDG_DATA_HOME/gitcube`, or `~/.local/share/gitcube` when `XDG_DATA_HOME` is unset.
GitCube holds an exclusive lock on that directory; a second instance exits before it
can recover jobs or touch clone staging directories.

Open `http://127.0.0.1:9999`.

## User guide

### Adding repositories

Go to **Add repository** in the header. The form takes one URL per line:

- `https://github.com/openeggbert/cna` (or any other public `http://`/`https://` Git
  host) queues that one repository for cloning.
- A bare account/org URL with no repository path, e.g. `https://github.com/openeggbert`,
  queues an **account import** job: GitCube lists every public repository under that
  account through the GitHub API (up to 1000, 100 per page) and queues a clone for each
  one that isn't already known.
- Lines for repositories GitCube already has are silently left alone — no retry, no
  side effect.
- Up to 500 non-empty lines are processed per submission.

Before pasting a URL into the bulk box, you can type it into the **Check a repository**
field above the form and press **Check** (or Enter) to find out, without leaving the
page, whether GitCube already has it — and jump straight to it if so.

SSH URLs, embedded credentials and any non-`http(s)` scheme are rejected at parse time,
both here and everywhere else a URL is accepted.

### The repository list

The home page (**Repositories**) lists every known repository, 25 per page, with:

- **Search** — substring match against owner, name, host or description.
- **Status** — exact match (`queued`, `cloning`, `fetching`, `checking`, `metadata`,
  `ready`, `missing`, `unhealthy`, `error`).
- **Account** — GitHub owner/org (substring; only matches GitHub repositories).
- **Tag** — matches a git tag name actually present on the repository (from the `refs`
  table, refreshed on every clone/fetch).
- **Importance** — see below.

Every open dashboard tab polls `/api/status` every two seconds to keep status badges and
the active/queued job counters live, without a full page reload.

**Importance** is a simple 0–3 priority you can set yourself (GitCube never sets it):

| Value | Label     | Shown as        |
|------:|-----------|------------------|
| 0     | Undefined | *(no stars)*     |
| 1     | Low       | ★ (blue)         |
| 2     | Medium    | ★★ (orange)      |
| 3     | High      | ★★★ (red)        |

Set it from the repository detail page (see below); it's just a label GitCube stores and
lets you filter/sort by — it has no effect on cloning, fetching or scheduling.

### Repository detail page

`/repo/<id>` shows one repository's status, GitHub metadata (stars, forks, license,
description) if applicable, its releases, and:

- **Pause** / **Resume** — a paused repository is skipped by bulk *Fetch all*/*Check
  all* and by newly queued work; a command already running when you pause is allowed to
  finish.
- **Fetch** — `git fetch --all --prune --prune-tags --force --tags`.
- **Health** — checks the remote is still reachable, then runs `git fsck --full
  --no-dangling` on the local mirror.
- **Refresh GitHub** (GitHub repositories only) — re-fetches metadata and releases.
- **Clone again** — only shown if the local mirror is missing (e.g. it was deleted on
  disk, or a previous clone failed).
- The **Importance** selector described above.
- The **Export** controls described below.

### Browsing files, commits and Markdown

From a repository page, **Files** and **Commits** browse a chosen ref (branch or tag,
selectable in the URL/tree view):

- The file tree lists names, type, size and a short object id, with `..` navigation.
- If the tree at the repository **root** contains a `README.md` (case-insensitive), it's
  rendered as Markdown directly below the file listing.
- Opening a `.md`/`.markdown` file renders it the same way; any other text file is shown
  with escaping only (no syntax highlighting); binary files offer a **Raw** link instead.
- **Raw** serves the blob with a content type derived from its extension — deliberately
  conservative: `.html`/`.htm`/`.xhtml`/`.svg`/`.svgz`/`.xml` are all served as
  `text/plain` rather than their "native" type, so a file from a mirrored repository can
  never execute as a script in GitCube's own origin.
- **Commits** lists up to 200 commits on the chosen ref; opening one shows
  `git show --format=fuller --stat --patch`.

The Markdown renderer is a small, deliberately limited safe subset (headings, bold,
italic, inline code, links, images, code fences, blockquotes, lists, horizontal rules) —
not full GitHub Flavored Markdown.

### Exporting a repository as a `.zip`

The repository detail page's **Export** row offers two downloads:

- **Download files (.zip)** — pick a branch or tag; GitCube runs `git archive
  --format=zip` and sends the result. This is the tree contents only, no `.git`
  history.
- **Download whole repository (.git, .zip)** — zips the actual bare mirror directory as
  it sits on disk (refs, objects, packs — everything needed to re-clone it, no
  worktree). Extract it and `git clone path/to/extracted.git` works.

Both are fully buffered in memory before being sent (this server has no chunked/
streaming response support yet) and capped at 500 MiB; a repository larger than that
will fail the export with a 413 rather than exhaust memory.

### Jobs

**Jobs** shows job history, 50 per page, filterable by status (`queued`, `running`,
`success`, `failed`, `interrupted`). Each entry shows its type, timestamps, attempt
count, the message GitCube recorded, and — on failure — the captured command output.
Account-import jobs (no single repository) show the GitHub account name instead of a
repository link.

**Fetch all** / **Check all** on the home page bulk-queue a fetch or health job for
every eligible repository (skipping paused ones, and — for fetch — repositories that
have never successfully cloned).

### GitHub metadata and rate limits

GitHub metadata (description, stars, forks, license, releases, and the repository list
for account imports) is fetched through the **unauthenticated** GitHub REST API, capped
at 60 requests/hour per IP address by GitHub. A bulk account import queues one metadata
fetch per repository, so a large account can exhaust that budget in one batch.

When that happens, GitCube detects the rate-limit response specifically and reschedules
the affected job — and every other pending GitHub-API job — about 15 minutes out, rather
than recording a permanent failure. No action is needed; the queue drains on its own
once the limit resets. A repository's own local mirror is never affected by a metadata
failure — cloning, fetching and browsing work regardless.

### Safe shutdown

Press **Ctrl+C** in the terminal running GitCube, or send it `SIGTERM` (e.g. `kill
<pid>`, or a single click of Stop in an IDE run configuration). GitCube then:

1. Stops accepting new HTTP requests and new job claims.
2. Gives a Git/curl child process still running up to ~10 seconds to finish on its own;
   after that it sends `SIGTERM`, then `SIGKILL` five seconds later if still alive.
3. Returns the interrupted job to the queue (not a permanent failure) so it retries
   automatically after restart.
4. Cleans up any orphaned partial-clone directory left in `data/tmp` on the next start.

Avoid `kill -9` and closing the terminal without Ctrl+C first (that sends `SIGHUP`,
which isn't handled) — neither is catastrophic (the next start recovers), but neither
gets the graceful shutdown either.

## Command-line reference

```text
--data-dir PATH   Data directory, default $XDG_DATA_HOME/gitcube
--port PORT       HTTP port, default 9999
--workers COUNT   Concurrent Git workers, default 2
--bind ADDRESS    IPv4 bind address, default 127.0.0.1
--allowed-host H  Additional accepted HTTP Host name, repeatable
--allow-remote-unauthenticated
                  Permit a non-loopback bind without authentication
--help            Show usage
--version         Show version
```

Example:

```bash
./build/gitcube --data-dir /mnt/archive/gitcube --port 9999 --workers 4
```

## For developers

### Source layout

Everything is under `src/`; each `.cpp`/`.hpp` pair is one responsibility, and none of
them depend on a web framework or ORM:

| File | Responsibility |
|---|---|
| `main.cpp` | CLI argument parsing, signal handlers, process entry point. |
| `application.hpp/.cpp` | HTTP route dispatch and every page/handler — the only place that renders HTML or reads `HttpRequest`. Owns the worker threads and the CSRF token. |
| `git_service.hpp/.cpp` | Every `git`/`curl` invocation: clone, fetch, health check, GitHub metadata, GitHub account listing, tree/blob/commit/archive reads. Takes a `Job` or `Repository`, never an `HttpRequest`. |
| `database.hpp/.cpp` | The only file that touches SQLite. Schema, migrations, and every query, wrapped around a tiny RAII `Connection`/`Statement` pair (not a public API — see below). |
| `http_server.hpp/.cpp` | The HTTP/1.1 server itself: POSIX sockets, bounded client queue and fixed worker pool, request parsing, Host/Origin checks, response deadlines and security headers. Knows nothing about GitCube's routes. |
| `process.hpp/.cpp` | `posix_spawnp`/`waitpid` wrapper used for every child process, with separate stdout/stderr capture, a timeout, and graceful-shutdown-aware process-group termination. |
| `json.hpp/.cpp` | A small recursive-descent JSON parser (with a nesting-depth cap) used to read GitHub API responses. Not a general-purpose library. |
| `util.hpp/.cpp` | URL/repository-URL parsing and validation, HTML/JSON/URL escaping, the Markdown renderer, ref/path validation. |
| `favicon_assets.hpp/.cpp` | The favicon (SVG text + ICO/PNG bytes) embedded as C++ constants; see `docs/favicon/`. |
| `tests/test_main.cpp` | Unit tests — no test framework dependency, just assertions and a `main()`. |

### Request/data flow

```text
HttpServer (sockets, one thread per connection, capped)
        │  HttpRequest
        ▼
Application::handle_request  →  route dispatch  →  page/API handlers
        │                                                │
        │ reads/writes                                   │ reads (git/curl)
        ▼                                                 ▼
    Database (SQLite)                               GitService
        ▲                                                 │
        │ claim_next_job() / finish_job()                 │ runs
        │                                                  ▼
  worker thread pool  ─────────────────────────────  ProcessRunner (fork/exec)
```

Everything that mutates repository/job state — clone, fetch, health check, GitHub
metadata, account import — is a **job**: inserted into the `jobs` table by an HTTP
handler, then picked up and executed by one of the worker threads started in
`Application::start_workers()`. HTTP handlers never run `git`/`curl` themselves for
anything that could take a while; browsing routes (tree/blob/commits/archive) are the
exception — those run synchronously on the request-handling thread since they're
expected to be fast local reads, not network operations.

### Data directory layout

```text
data/
├── gitcube.sqlite3          # WAL mode; repositories, jobs, refs, releases, schema_migrations
├── repositories/
│   └── github.com/openeggbert/cna.git/   # bare mirror, no worktree
├── tmp/                      # clone staging: clone-<repo id>-<job id>.git
├── cache/
└── logs/
```

A clone is written to `data/tmp/clone-<repo>-<job>.git` and only `rename()`d into its
final path after `git clone --mirror` exits successfully — an interrupted clone can
never appear as a completed repository, and any leftover staging directory found in
`data/tmp` at startup (from a hard crash) is removed automatically.

The data directory and newly created contents are owner-only by default. A process-wide
`gitcube.lock` prevents two instances from recovering or executing the same queue.

### Database schema and migrations

`Database::initialize()` contains a **frozen v1 snapshot** of the schema — do not add
columns there. Every column added since v1 lives exclusively in the `kMigrations[]`
array at the top of `database.cpp`:

```cpp
const Migration kMigrations[] = {
    {2, R"SQL( ... )SQL"},   // rebuilds jobs: adds payload column, widens type CHECK
    {3, "ALTER TABLE jobs ADD COLUMN scheduled_at TEXT NOT NULL DEFAULT '';"},
    {4, "ALTER TABLE repositories ADD COLUMN importance INTEGER NOT NULL DEFAULT 0;"},
};
```

`run_pending_migrations()` applies every migration newer than the database's current
`schema_migrations` row, each in its own transaction — for **every** database, fresh or
years old, so a brand-new database reaches the current schema by replaying this exact
list, not by the CREATE TABLE statements already containing the latest columns. That
symmetry matters: mixing "some columns inline in CREATE TABLE, some via migration"
already caused a real bug (a migration's unconditional `ALTER TABLE ADD COLUMN` failing
with "duplicate column name" on a fresh database that already had the column inline).

**To add a column or table**, append a new `{version, sql}` entry — never edit an
already-released one, and never touch the CREATE TABLE block. After migrations run,
`verify_schema()` checks that a fixed set of load-bearing columns actually exist (via
`pragma_table_info`) and throws a specific, immediate startup error if not, rather than
letting a mismatch surface later as a cryptic "no such column" from whichever query
happens to run first.

### Security model

- GitCube binds to `127.0.0.1` by default and has no authentication — treat it as a
  single-user local tool, same trust boundary as a CLI. A non-loopback bind is refused
  unless `--allow-remote-unauthenticated` and at least one `--allowed-host` are both
  supplied; that explicit escape hatch still needs authentication and TLS in front of it.
- HTTP/1.1 requests must use an allowed `Host`; cross-site browser POSTs and mismatched
  `Origin` headers are rejected to limit DNS-rebinding and cross-origin attacks.
- Every POST requires a per-process CSRF token embedded in the page (checked in
  `Application::valid_csrf`).
- `git`/`curl`/`zip` are invoked with `posix_spawnp` and an argument array — **never** through
  a shell, so there's no shell-metacharacter injection surface regardless of what a
  repository URL, ref name or file path contains.
- Child processes receive a controlled environment: global/system Git configuration,
  credential helpers, Git prompting, `.curlrc`, and `ZIPOPT` cannot silently change the
  public-HTTP-only behavior or corrupt binary output.
- Repository URLs are restricted to public `http://`/`https://`, with credentials and
  any other scheme rejected at parse time (`parse_repository_url` in `util.cpp`).
- Ref names and repository-relative paths are validated (`valid_git_ref`,
  `valid_repo_path`) before being passed to `git`, blocking traversal (`..`), leading
  `-` (option injection), and control characters.
- Raw blob responses map file extension to content type conservatively —
  `.html`/`.svg`/`.xml`/etc. are all served as `text/plain` — so a script embedded in a
  mirrored repository's file can't execute in GitCube's own origin when viewed raw.
- The HTTP server uses 16 joinable client workers with a 64-connection bound and
  wall-clock deadlines for both receiving a request and sending a response. Shutdown
  closes every queued/active socket and joins the pool before application state is
  destroyed.
- The JSON parser used for GitHub API responses caps nesting depth to guard against a
  stack-overflow from a malformed/hostile response.

### Building and testing

```bash
# Debug build + unit tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure

# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j
./build-asan/gitcube_tests
```

The library target (`gitcube_core`) builds with `-Wall -Wextra -Wpedantic -Wconversion
-Wshadow`; keep new code warning-clean under those flags.

## Current limitations

- Localhost single-user application; no login or TLS.
- Linux/Debian 13 only in this version.
- No SSH, GitHub token or private repositories.
- Markdown support is a deliberately small safe subset, not complete GitHub Flavored
  Markdown; no syntax highlighting or blame.
- No full-text code search, pull requests, issues or GitHub Actions synchronization.
- No per-running-job cancel button — pausing affects future/queued jobs, not a child
  process already running.
- Zip export is fully buffered in memory (no streaming response support), capped at
  500 MiB.

## License

MIT — see [LICENSE](LICENSE).
