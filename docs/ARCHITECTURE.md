# GitCube architecture

GitCube is a single C++20 process with four principal layers:

1. A small localhost HTTP/1.1 server using POSIX sockets.
2. A SQLite database in WAL mode for repositories, refs, releases and the persistent job queue.
3. A configurable worker pool. Every Git operation is a child process in its own process group.
4. A Git read layer based on `git --git-dir`, so no worktree is required for browsing.

## Repository layout

```text
data/
├── gitcube.sqlite3
├── repositories/
│   └── github.com/openeggbert/cna.git/
├── tmp/
├── cache/
└── logs/
```

Clones use `git clone --mirror` into `tmp/clone-<repository>-<job>.git`. The completed directory is renamed into the final path only after Git exits successfully. This makes interrupted clones easy to retry.

## Shutdown

SIGINT and SIGTERM stop accepting work. Running Git/curl child processes receive a grace period. After ten seconds they receive SIGTERM; after another five seconds they receive SIGKILL. Jobs left in `running` state are recovered to `queued` on the next start.

## Security boundary

GitCube binds to `127.0.0.1` by default and has no user authentication. POST actions require a process-local CSRF token. Repository URLs are limited to public `http://` and `https://` URLs. Git and curl are invoked with argument arrays, never through a shell. Do not bind to a non-loopback address without adding authentication and TLS in front of the service.
