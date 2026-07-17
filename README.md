# GitCube

GitCube is a local C++20 server for maintaining and browsing a collection of public Git repositories. It replaces a shell-script workflow with a SQLite-backed persistent queue, configurable parallel workers, health checks, public GitHub metadata and a web interface.

## Implemented features

- Creates `./data` on first start, or uses `--data-dir PATH`.
- Stores state in `data/gitcube.sqlite3` using SQLite WAL mode.
- Stores mirrors without worktrees using `git clone --mirror`.
- Maps `https://github.com/openeggbert/cna` to `data/repositories/github.com/openeggbert/cna.git`.
- Bulk URL import from a textarea, persistent clone queue and duplicate detection.
- Configurable `--workers`, default 2.
- Bulk and per-repository fetch using `fetch --all --prune --prune-tags --force --tags`.
- Pause/resume. Paused repositories are skipped by queued and bulk work; a command already running is allowed to finish.
- Statuses including queued, cloning, fetching, checking, ready, missing, unhealthy and error.
- Remote availability plus `git fsck --full --no-dangling` health checks.
- Public GitHub repository metadata and up to 100 releases, without a token.
- Branches and tags stored in SQLite after clone/fetch.
- Web browsing of refs, trees, text/binary blobs, Markdown, commit history and commit patches.
- Safe shutdown and interrupted-job recovery.
- No SSH URLs, credentials, tokens or private-repository support.

## Debian 13 dependencies

```bash
sudo apt install build-essential cmake git curl libsqlite3-dev
```

No web framework is downloaded during the build. The server is implemented with Linux/POSIX sockets to keep the Debian 13 build dependency set small.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run

```bash
./build/gitcube
```

Open `http://127.0.0.1:9999`.

Options:

```text
--data-dir PATH   Data directory, default ./data
--port PORT       HTTP port, default 9999
--workers COUNT   Concurrent Git workers, default 2
--bind ADDRESS    IPv4 bind address, default 127.0.0.1
```

Example:

```bash
./build/gitcube --data-dir /mnt/archive/gitcube --port 9999 --workers 4
```

## Graceful shutdown

Press Ctrl+C or send SIGTERM. GitCube stops accepting new work. Running child processes may finish during a ten-second grace period; after that GitCube terminates them. An interrupted queue item is returned to the queue at the next start. Mirror clones are first written under `data/tmp`, so a partial clone never appears as a completed repository.

## Public GitHub API

GitHub metadata is fetched with unauthenticated REST requests. Rate limits are therefore intentionally low. Metadata failure does not invalidate a healthy local mirror; it is recorded in job history and can be retried with **Refresh GitHub**.

## Current limitations

- Localhost single-user application; no login or TLS.
- Linux/Debian 13 only in this version.
- No SSH, GitHub token or private repositories.
- Markdown support is a deliberately small safe subset rather than complete GitHub Flavored Markdown.
- No blame, full-text code search, pull requests, issues or GitHub Actions yet.
- No per-job cancel button. Pausing affects future/queued jobs, not a child process already running.
