# GitCube 0.1.0 implementation status

## Complete in this prototype

- Debian 13 C++20/CMake build.
- First-run data-directory creation and configurable `--data-dir`.
- SQLite schema, WAL mode, schema migration table and persistent jobs.
- Mirror-only Git storage (`git clone --mirror`), no checked-out worktree.
- Atomic clone staging under `data/tmp`.
- Public HTTP/HTTPS repository URL validation; SSH and embedded credentials are rejected.
- Bulk import, clone/fetch/health/metadata jobs and duplicate-job suppression.
- Configurable worker count and live status polling in the web UI.
- Per-repository pause/resume and bulk fetch/health actions.
- Safe SIGINT/SIGTERM behavior and restart recovery for interrupted jobs.
- Missing/unhealthy/error status reporting and retained command output.
- GitHub public REST metadata and release import through `curl`, without tokens.
- SQLite snapshots of branches and tags after clone/fetch.
- Branch/tag/tree/blob/Markdown/commit/diff browsing without a worktree.
- CSRF protection, shell-free process execution, path/ref validation and conservative response headers.
- Example systemd user service.

## Verified workflows

- Debug and Release builds with GCC 14.
- Unit tests for URL parsing, SSH rejection, HTML/Markdown escaping, JSON parsing and queue semantics.
- AddressSanitizer and UndefinedBehaviorSanitizer test run.
- End-to-end clone from a local public HTTP Git server into a bare mirror.
- Branch and tag discovery, README rendering, raw blob serving and commit browsing.
- Upstream commit followed by queued fetch and updated file content.
- Remote/local health check.
- SIGTERM clean shutdown.
- Simulated interrupted job recovery, pause retention, resume and successful retry.

## Deliberately deferred

- User accounts, TLS and safe non-localhost deployment.
- Private repositories, SSH and access tokens.
- Per-running-job cancellation.
- Full GitHub Flavored Markdown, syntax highlighting, blame and full-text search.
- Issues, pull requests and Actions synchronization.
- Windows and macOS support.
