#include "database.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace gitcube {
namespace {

// Ordered, additive schema migrations. Each entry's `sql` is applied once, in a single
// transaction, when the database's current version is below `version`; the version is
// then recorded in schema_migrations. run_pending_migrations() runs unconditionally after
// the CREATE TABLE statements below, on every database, fresh or old — so a brand-new
// database ends up at the latest schema by executing this same list, not by the CREATE
// TABLE statements already containing it.
//
// This means the CREATE TABLE statements in Database::initialize() are a frozen v1
// snapshot and must NEVER be hand-edited to add a column a migration is responsible for.
// Doing that once already caused a real bug: a fresh database and an upgraded database
// would apply the migrations starting from different starting schemas, and at least one
// migration (an unconditional ALTER TABLE ADD COLUMN) then failed on a fresh database
// with "duplicate column name" because the column was already there. To add a column,
// append a new {version, sql} entry below — never touch the CREATE TABLE block.
struct Migration {
    int version;
    const char* sql;
};

const Migration kMigrations[] = {
    {2, R"SQL(
CREATE TABLE jobs_migration (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_id INTEGER REFERENCES repositories(id) ON DELETE CASCADE,
    type TEXT NOT NULL CHECK(type IN ('clone','fetch','health','metadata','account')),
    status TEXT NOT NULL DEFAULT 'queued' CHECK(status IN ('queued','running','success','failed','interrupted')),
    queued_at TEXT NOT NULL,
    started_at TEXT NOT NULL DEFAULT '',
    finished_at TEXT NOT NULL DEFAULT '',
    message TEXT NOT NULL DEFAULT '',
    payload TEXT NOT NULL DEFAULT '',
    output TEXT NOT NULL DEFAULT '',
    error TEXT NOT NULL DEFAULT '',
    attempts INTEGER NOT NULL DEFAULT 0
);
INSERT INTO jobs_migration (id, repo_id, type, status, queued_at, started_at, finished_at, message, output, error, attempts)
  SELECT id, repo_id, type, status, queued_at, started_at, finished_at, message, output, error, attempts FROM jobs;
DROP TABLE jobs;
ALTER TABLE jobs_migration RENAME TO jobs;
CREATE INDEX IF NOT EXISTS idx_jobs_status_id ON jobs(status, id);
CREATE INDEX IF NOT EXISTS idx_jobs_repo ON jobs(repo_id, id DESC);
)SQL"},
    {3, "ALTER TABLE jobs ADD COLUMN scheduled_at TEXT NOT NULL DEFAULT '';"},
    {4, "ALTER TABLE repositories ADD COLUMN importance INTEGER NOT NULL DEFAULT 0;"},
};


class Connection {
public:
    explicit Connection(const std::filesystem::path& path) {
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
            const std::string message = db_ ? sqlite3_errmsg(db_) : "cannot allocate SQLite handle";
            if (db_) sqlite3_close(db_);
            throw std::runtime_error("Cannot open SQLite database: " + message);
        }
        sqlite3_busy_timeout(db_, 10000);
        exec("PRAGMA foreign_keys=ON;");
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=NORMAL;");
    }

    ~Connection() { if (db_) sqlite3_close(db_); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    sqlite3* get() const { return db_; }

    void exec(const std::string& sql) const {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error("SQLite error: " + message + " SQL: " + sql);
        }
    }

private:
    sqlite3* db_ = nullptr;
};

class Statement {
public:
    Statement(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error("SQLite prepare failed: " + std::string(sqlite3_errmsg(db_)) + " SQL: " + sql);
        }
    }
    ~Statement() { if (stmt_) sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, std::int64_t value) {
        if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) fail();
    }
    void bind(int index, int value) {
        if (sqlite3_bind_int(stmt_, index, value) != SQLITE_OK) fail();
    }
    void bind(int index, const std::string& value) {
        if (sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) fail();
    }
    void bind_null(int index) {
        if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) fail();
    }
    bool step_row() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        fail();
        return false;
    }
    void step_done() {
        const int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_DONE) fail();
    }
    void reset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
    std::int64_t integer(int column) const { return sqlite3_column_int64(stmt_, column); }
    int int_value(int column) const { return sqlite3_column_int(stmt_, column); }
    std::string text(int column) const {
        const auto* value = sqlite3_column_text(stmt_, column);
        return value ? reinterpret_cast<const char*>(value) : std::string{};
    }
    bool is_null(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

private:
    [[noreturn]] void fail() const {
        throw std::runtime_error("SQLite statement failed: " + std::string(sqlite3_errmsg(db_)));
    }
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

void run_pending_migrations(Connection& db) {
    std::int64_t schema_version = 0;
    {
        // Finalized before any migration DDL runs on the same connection — an
        // unfinalized statement holds a schema-level lock that would make a migration's
        // CREATE/DROP/ALTER TABLE sequence fail with "database table is locked".
        Statement version_stmt(db.get(), "SELECT COALESCE(max(version), 0) FROM schema_migrations");
        schema_version = version_stmt.step_row() ? version_stmt.integer(0) : 0;
    }
    for (const auto& migration : kMigrations) {
        if (migration.version <= schema_version) continue;
        db.exec("BEGIN IMMEDIATE;");
        try {
            db.exec(migration.sql);
            Statement record(db.get(), "INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(?, datetime('now'))");
            record.bind(1, static_cast<std::int64_t>(migration.version));
            record.step_done();
            db.exec("COMMIT;");
        } catch (...) {
            db.exec("ROLLBACK;");
            throw;
        }
    }
}

// Regression guard for exactly the bug class that motivated the comment above kMigrations:
// a CREATE TABLE/migration mismatch that leaves some database (fresh or upgraded) missing
// a column the rest of this file assumes exists. Rather than surfacing as a "no such
// column" from whichever query happens to run first — at some arbitrary point after
// startup, on some arbitrary request — fail loudly here, immediately, with the specific
// table and column named.
void verify_schema(Connection& db) {
    auto has_column = [&](const char* table, const char* column) {
        Statement stmt(db.get(), std::string("SELECT 1 FROM pragma_table_info(?) WHERE name = ?"));
        stmt.bind(1, std::string(table));
        stmt.bind(2, std::string(column));
        return stmt.step_row();
    };
    auto require_columns = [&](const char* table, std::initializer_list<const char*> columns) {
        for (const char* column : columns) {
            if (!has_column(table, column)) {
                throw std::runtime_error(std::string("Database schema check failed: table '") + table +
                                         "' is missing column '" + column + "'. gitcube.sqlite3 may predate a "
                                         "schema migration that failed to apply; check the startup log above "
                                         "this error for the actual migration failure.");
            }
        }
    };
    require_columns("repositories", {"id", "storage_relpath", "importance", "branch_count", "tag_count", "object_count"});
    require_columns("jobs", {"id", "repo_id", "type", "status", "payload", "scheduled_at", "attempts"});
}

Repository read_repository(Statement& stmt) {
    Repository r;
    int c = 0;
    r.id = stmt.integer(c++);
    r.original_url = stmt.text(c++);
    r.normalized_url = stmt.text(c++);
    r.host = stmt.text(c++);
    r.owner = stmt.text(c++);
    r.name = stmt.text(c++);
    r.storage_relpath = stmt.text(c++);
    r.status = stmt.text(c++);
    r.paused = stmt.int_value(c++) != 0;
    r.github = stmt.int_value(c++) != 0;
    r.default_branch = stmt.text(c++);
    r.description = stmt.text(c++);
    r.homepage = stmt.text(c++);
    r.html_url = stmt.text(c++);
    r.license = stmt.text(c++);
    r.archived = stmt.int_value(c++) != 0;
    r.fork = stmt.int_value(c++) != 0;
    r.stars = stmt.integer(c++);
    r.forks = stmt.integer(c++);
    r.open_issues = stmt.integer(c++);
    r.github_repo_id = stmt.integer(c++);
    r.created_at = stmt.text(c++);
    r.updated_at = stmt.text(c++);
    r.pushed_at = stmt.text(c++);
    r.metadata_fetched_at = stmt.text(c++);
    r.last_fetch_at = stmt.text(c++);
    r.last_success_at = stmt.text(c++);
    r.last_health_at = stmt.text(c++);
    r.last_error = stmt.text(c++);
    r.head_oid = stmt.text(c++);
    r.branch_count = stmt.integer(c++);
    r.tag_count = stmt.integer(c++);
    r.object_count = stmt.integer(c++);
    r.importance = stmt.int_value(c++);
    return r;
}

const char* repository_columns = R"SQL(
 id, original_url, normalized_url, host, owner, name, storage_relpath, status,
 paused, is_github, default_branch, description, homepage, html_url, license,
 archived, is_fork, stars, forks, open_issues, github_repo_id, created_at,
 updated_at, pushed_at, metadata_fetched_at, last_fetch_at, last_success_at,
 last_health_at, last_error, head_oid, branch_count, tag_count, object_count, importance
)SQL";

Job read_job(Statement& stmt) {
    Job job;
    job.id = stmt.integer(0);
    if (!stmt.is_null(1)) job.repo_id = stmt.integer(1);
    job.type = stmt.text(2);
    job.status = stmt.text(3);
    job.queued_at = stmt.text(4);
    job.started_at = stmt.text(5);
    job.finished_at = stmt.text(6);
    job.message = stmt.text(7);
    job.payload = stmt.text(8);
    job.output = stmt.text(9);
    job.error = stmt.text(10);
    job.attempts = stmt.int_value(11);
    return job;
}

bool enqueue_job_locked(sqlite3* db, std::optional<std::int64_t> repo_id, const std::string& type,
                        const std::string& message, const std::string& payload) {
    if (repo_id) {
        Statement check(db, "SELECT 1 FROM jobs WHERE repo_id=? AND type=? AND status IN ('queued','running') LIMIT 1");
        check.bind(1, *repo_id);
        check.bind(2, type);
        if (check.step_row()) return false;
    } else if (!payload.empty()) {
        // Account-import jobs have no repo_id; dedup them on (type, payload) instead so
        // importing the same GitHub account twice in a row doesn't queue it twice.
        Statement check(db, "SELECT 1 FROM jobs WHERE repo_id IS NULL AND type=? AND payload=? AND status IN ('queued','running') LIMIT 1");
        check.bind(1, type);
        check.bind(2, payload);
        if (check.step_row()) return false;
    }
    Statement stmt(db, "INSERT INTO jobs(repo_id,type,status,queued_at,message,payload) VALUES(?,?,'queued',?,?,?)");
    if (repo_id) stmt.bind(1, *repo_id); else stmt.bind_null(1);
    stmt.bind(2, type);
    stmt.bind(3, now_utc());
    stmt.bind(4, message);
    stmt.bind(5, payload);
    stmt.step_done();
    if (repo_id && type == "clone") {
        Statement status(db, "UPDATE repositories SET status='queued', modified_at=? WHERE id=?");
        status.bind(1, now_utc());
        status.bind(2, *repo_id);
        status.step_done();
    }
    return true;
}

} // namespace

Database::Database(std::filesystem::path path) : path_(std::move(path)) {}

void Database::initialize() {
    std::filesystem::create_directories(path_.parent_path());
    Connection db(path_);
    // Frozen v1 schema — do not add columns here. See the kMigrations comment above:
    // every column added since v1 belongs exclusively in that list.
    db.exec(R"SQL(
CREATE TABLE IF NOT EXISTS repositories (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    original_url TEXT NOT NULL,
    normalized_url TEXT NOT NULL UNIQUE,
    host TEXT NOT NULL,
    owner TEXT NOT NULL,
    name TEXT NOT NULL,
    storage_relpath TEXT NOT NULL UNIQUE,
    status TEXT NOT NULL DEFAULT 'queued',
    paused INTEGER NOT NULL DEFAULT 0,
    is_github INTEGER NOT NULL DEFAULT 0,
    default_branch TEXT NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '',
    homepage TEXT NOT NULL DEFAULT '',
    html_url TEXT NOT NULL DEFAULT '',
    license TEXT NOT NULL DEFAULT '',
    archived INTEGER NOT NULL DEFAULT 0,
    is_fork INTEGER NOT NULL DEFAULT 0,
    stars INTEGER NOT NULL DEFAULT 0,
    forks INTEGER NOT NULL DEFAULT 0,
    open_issues INTEGER NOT NULL DEFAULT 0,
    github_repo_id INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT '',
    updated_at TEXT NOT NULL DEFAULT '',
    pushed_at TEXT NOT NULL DEFAULT '',
    metadata_fetched_at TEXT NOT NULL DEFAULT '',
    last_fetch_at TEXT NOT NULL DEFAULT '',
    last_success_at TEXT NOT NULL DEFAULT '',
    last_health_at TEXT NOT NULL DEFAULT '',
    last_error TEXT NOT NULL DEFAULT '',
    head_oid TEXT NOT NULL DEFAULT '',
    branch_count INTEGER NOT NULL DEFAULT 0,
    tag_count INTEGER NOT NULL DEFAULT 0,
    object_count INTEGER NOT NULL DEFAULT 0,
    github_metadata_json TEXT NOT NULL DEFAULT '',
    inserted_at TEXT NOT NULL,
    modified_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_repositories_status ON repositories(status);
CREATE INDEX IF NOT EXISTS idx_repositories_host_owner_name ON repositories(host, owner, name);

CREATE TABLE IF NOT EXISTS jobs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    repo_id INTEGER REFERENCES repositories(id) ON DELETE CASCADE,
    type TEXT NOT NULL CHECK(type IN ('clone','fetch','health','metadata')),
    status TEXT NOT NULL DEFAULT 'queued' CHECK(status IN ('queued','running','success','failed','interrupted')),
    queued_at TEXT NOT NULL,
    started_at TEXT NOT NULL DEFAULT '',
    finished_at TEXT NOT NULL DEFAULT '',
    message TEXT NOT NULL DEFAULT '',
    output TEXT NOT NULL DEFAULT '',
    error TEXT NOT NULL DEFAULT '',
    attempts INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_jobs_status_id ON jobs(status, id);
CREATE INDEX IF NOT EXISTS idx_jobs_repo ON jobs(repo_id, id DESC);

CREATE TABLE IF NOT EXISTS refs (
    repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,
    type TEXT NOT NULL CHECK(type IN ('branch','tag','other')),
    name TEXT NOT NULL,
    target_oid TEXT NOT NULL DEFAULT '',
    updated_at TEXT NOT NULL,
    PRIMARY KEY(repo_id, type, name)
);

CREATE TABLE IF NOT EXISTS releases (
    repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,
    github_id INTEGER NOT NULL,
    tag_name TEXT NOT NULL DEFAULT '',
    name TEXT NOT NULL DEFAULT '',
    html_url TEXT NOT NULL DEFAULT '',
    published_at TEXT NOT NULL DEFAULT '',
    prerelease INTEGER NOT NULL DEFAULT 0,
    draft INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(repo_id, github_id)
);

CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at TEXT NOT NULL
);
INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(1, datetime('now'));
)SQL");

    // The block above only creates tables that don't exist yet, so on an existing
    // database file it's a no-op even when the schema it describes has moved on since
    // that file was created. Bring such a database up to date explicitly.
    run_pending_migrations(db);
    verify_schema(db);
}

void Database::recover_interrupted_jobs() {
    Connection db(path_);
    db.exec("BEGIN IMMEDIATE;");
    try {
        db.exec("UPDATE jobs SET status='queued', started_at='', finished_at='', error='Recovered after GitCube shutdown', message='Recovered interrupted job' WHERE status='running';");
        db.exec("UPDATE repositories SET status='queued', modified_at=datetime('now') WHERE status IN ('cloning','fetching','checking','metadata');");
        db.exec("COMMIT;");
    } catch (...) {
        db.exec("ROLLBACK;");
        throw;
    }
}

AddRepositoryResult Database::add_repository(const ParsedRepositoryUrl& parsed) {
    Connection db(path_);
    Statement insert(db.get(), R"SQL(
INSERT OR IGNORE INTO repositories(
 original_url, normalized_url, host, owner, name, storage_relpath, status,
 paused, is_github, inserted_at, modified_at
) VALUES(?,?,?,?,?,?,'queued',0,?,?,?)
)SQL");
    insert.bind(1, parsed.original);
    insert.bind(2, parsed.normalized);
    insert.bind(3, parsed.host);
    insert.bind(4, parsed.owner);
    insert.bind(5, parsed.name);
    insert.bind(6, parsed.relative_storage_path.generic_string());
    insert.bind(7, parsed.github ? 1 : 0);
    const auto now = now_utc();
    insert.bind(8, now);
    insert.bind(9, now);
    insert.step_done();
    const bool created = sqlite3_changes(db.get()) > 0;

    Statement select(db.get(), "SELECT id FROM repositories WHERE normalized_url=?");
    select.bind(1, parsed.normalized);
    if (!select.step_row()) throw std::runtime_error("Repository insert/select failed");
    return {select.integer(0), created};
}

std::vector<ImportedRepository> Database::import_repositories(const std::vector<ParsedRepositoryUrl>& parsed_urls) {
    std::vector<ImportedRepository> results;
    results.reserve(parsed_urls.size());
    Connection db(path_);
    db.exec("BEGIN IMMEDIATE;");
    try {
        Statement insert(db.get(), R"SQL(
INSERT OR IGNORE INTO repositories(
 original_url, normalized_url, host, owner, name, storage_relpath, status,
 paused, is_github, inserted_at, modified_at
) VALUES(?,?,?,?,?,?,'queued',0,?,?,?)
)SQL");
        Statement select(db.get(), "SELECT id, storage_relpath FROM repositories WHERE normalized_url=?");
        const auto now = now_utc();
        for (const auto& parsed : parsed_urls) {
            insert.bind(1, parsed.original);
            insert.bind(2, parsed.normalized);
            insert.bind(3, parsed.host);
            insert.bind(4, parsed.owner);
            insert.bind(5, parsed.name);
            insert.bind(6, parsed.relative_storage_path.generic_string());
            insert.bind(7, parsed.github ? 1 : 0);
            insert.bind(8, now);
            insert.bind(9, now);
            insert.step_done();
            const bool created = sqlite3_changes(db.get()) > 0;
            insert.reset();

            select.bind(1, parsed.normalized);
            if (!select.step_row()) throw std::runtime_error("Repository insert/select failed");
            const std::int64_t id = select.integer(0);
            std::string storage_relpath = select.text(1);
            select.reset();

            if (created) enqueue_job_locked(db.get(), id, "clone", "Imported from web UI", {});
            results.push_back({id, created, std::move(storage_relpath)});
        }
        db.exec("COMMIT;");
    } catch (...) {
        db.exec("ROLLBACK;");
        throw;
    }
    return results;
}

std::vector<Repository> Database::list_repositories() const {
    Connection db(path_);
    Statement stmt(db.get(), std::string("SELECT ") + repository_columns +
        " FROM repositories ORDER BY host COLLATE NOCASE, owner COLLATE NOCASE, name COLLATE NOCASE");
    std::vector<Repository> result;
    while (stmt.step_row()) result.push_back(read_repository(stmt));
    return result;
}

namespace {

constexpr const char* kRepositoryFilterWhere = R"SQL(
WHERE (?1 = '' OR owner LIKE '%'||?1||'%' ESCAPE '\' OR name LIKE '%'||?1||'%' ESCAPE '\'
       OR host LIKE '%'||?1||'%' ESCAPE '\' OR description LIKE '%'||?1||'%' ESCAPE '\')
  AND (?2 = '' OR status = ?2)
  AND (?3 = '' OR (is_github = 1 AND owner LIKE '%'||?3||'%' ESCAPE '\'))
  AND (?4 = '' OR EXISTS (
        SELECT 1 FROM refs WHERE refs.repo_id = repositories.id AND refs.type = 'tag'
          AND refs.name LIKE '%'||?4||'%' ESCAPE '\'
      ))
  AND (?5 = '' OR importance = CAST(?5 AS INTEGER))
)SQL";

void bind_repository_filter(Statement& stmt, const RepositoryFilter& filter) {
    stmt.bind(1, filter.search);
    stmt.bind(2, filter.status);
    stmt.bind(3, filter.account);
    stmt.bind(4, filter.tag);
    stmt.bind(5, filter.importance);
}

} // namespace

RepositoryPage Database::list_repositories_page(const RepositoryFilter& filter, std::size_t page,
                                                std::size_t per_page) const {
    Connection db(path_);
    Statement count_stmt(db.get(), std::string("SELECT count(*) FROM repositories ") + kRepositoryFilterWhere);
    bind_repository_filter(count_stmt, filter);
    RepositoryPage result;
    result.total = count_stmt.step_row() ? static_cast<std::size_t>(count_stmt.integer(0)) : 0;

    const std::size_t page_count = result.total == 0 ? 1 : (result.total + per_page - 1) / per_page;
    page = std::max<std::size_t>(1, std::min(page, page_count));

    Statement stmt(db.get(), std::string("SELECT ") + repository_columns + " FROM repositories " +
        kRepositoryFilterWhere +
        " ORDER BY host COLLATE NOCASE, owner COLLATE NOCASE, name COLLATE NOCASE LIMIT ?6 OFFSET ?7");
    bind_repository_filter(stmt, filter);
    stmt.bind(6, static_cast<std::int64_t>(per_page));
    stmt.bind(7, static_cast<std::int64_t>((page - 1) * per_page));
    while (stmt.step_row()) result.items.push_back(read_repository(stmt));
    return result;
}

std::optional<Repository> Database::get_repository(std::int64_t id) const {
    Connection db(path_);
    Statement stmt(db.get(), std::string("SELECT ") + repository_columns + " FROM repositories WHERE id=?");
    stmt.bind(1, id);
    if (!stmt.step_row()) return std::nullopt;
    return read_repository(stmt);
}

std::optional<Repository> Database::get_repository_by_url(const std::string& normalized_url) const {
    Connection db(path_);
    Statement stmt(db.get(), std::string("SELECT ") + repository_columns + " FROM repositories WHERE normalized_url=?");
    stmt.bind(1, normalized_url);
    if (!stmt.step_row()) return std::nullopt;
    return read_repository(stmt);
}

bool Database::set_repository_paused(std::int64_t id, bool paused) {
    Connection db(path_);
    Statement stmt(db.get(), "UPDATE repositories SET paused=?, modified_at=? WHERE id=?");
    stmt.bind(1, paused ? 1 : 0);
    stmt.bind(2, now_utc());
    stmt.bind(3, id);
    stmt.step_done();
    return sqlite3_changes(db.get()) > 0;
}

bool Database::set_repository_importance(std::int64_t id, int importance) {
    Connection db(path_);
    Statement stmt(db.get(), "UPDATE repositories SET importance=?, modified_at=? WHERE id=?");
    stmt.bind(1, importance);
    stmt.bind(2, now_utc());
    stmt.bind(3, id);
    stmt.step_done();
    return sqlite3_changes(db.get()) > 0;
}

void Database::update_repository_status(std::int64_t id, const std::string& status, const std::string& error) {
    Connection db(path_);
    Statement stmt(db.get(), "UPDATE repositories SET status=?, last_error=?, modified_at=? WHERE id=?");
    stmt.bind(1, status);
    stmt.bind(2, error);
    stmt.bind(3, now_utc());
    stmt.bind(4, id);
    stmt.step_done();
}

void Database::sync_repository_refs_and_stats(std::int64_t id, const std::vector<RefRecord>& refs,
                                              const std::string& default_branch, const std::string& head_oid,
                                              std::int64_t branches, std::int64_t tags, std::int64_t objects,
                                              bool fetched) {
    Connection db(path_);
    db.exec("BEGIN IMMEDIATE;");
    try {
        Statement remove(db.get(), "DELETE FROM refs WHERE repo_id=?");
        remove.bind(1, id);
        remove.step_done();
        Statement insert(db.get(), "INSERT INTO refs(repo_id,type,name,target_oid,updated_at) VALUES(?,?,?,?,?)");
        const auto now = now_utc();
        for (const auto& ref : refs) {
            insert.bind(1, id);
            insert.bind(2, ref.type);
            insert.bind(3, ref.name);
            insert.bind(4, ref.target_oid);
            insert.bind(5, now);
            insert.step_done();
            insert.reset();
        }

        const std::string sql =
            "UPDATE repositories SET status='ready', default_branch=?, head_oid=?, branch_count=?, tag_count=?, object_count=?, "
            "last_fetch_at=CASE WHEN ?=1 THEN ? ELSE last_fetch_at END, last_success_at=?, last_error='', modified_at=? WHERE id=?";
        Statement stmt(db.get(), sql);
        stmt.bind(1, default_branch);
        stmt.bind(2, head_oid);
        stmt.bind(3, branches);
        stmt.bind(4, tags);
        stmt.bind(5, objects);
        stmt.bind(6, fetched ? 1 : 0);
        stmt.bind(7, now);
        stmt.bind(8, now);
        stmt.bind(9, now);
        stmt.bind(10, id);
        stmt.step_done();

        db.exec("COMMIT;");
    } catch (...) {
        db.exec("ROLLBACK;");
        throw;
    }
}

void Database::update_repository_health(std::int64_t id, bool healthy, const std::string& error) {
    Connection db(path_);
    Statement stmt(db.get(), "UPDATE repositories SET status=?, last_health_at=?, last_error=?, modified_at=? WHERE id=?");
    stmt.bind(1, healthy ? "ready" : "unhealthy");
    const auto now = now_utc();
    stmt.bind(2, now);
    stmt.bind(3, error);
    stmt.bind(4, now);
    stmt.bind(5, id);
    stmt.step_done();
}

void Database::update_github_metadata(std::int64_t id, std::int64_t github_id,
                                      const std::string& default_branch, const std::string& description,
                                      const std::string& homepage, const std::string& html_url,
                                      const std::string& license, bool archived, bool fork,
                                      std::int64_t stars, std::int64_t forks,
                                      std::int64_t open_issues, const std::string& created_at,
                                      const std::string& updated_at, const std::string& pushed_at,
                                      const std::string& raw_json) {
    Connection db(path_);
    Statement stmt(db.get(), R"SQL(
UPDATE repositories SET github_repo_id=?, default_branch=CASE WHEN ?='' THEN default_branch ELSE ? END,
 description=?, homepage=?, html_url=?, license=?, archived=?, is_fork=?, stars=?, forks=?,
 open_issues=?, created_at=?, updated_at=?, pushed_at=?, metadata_fetched_at=?,
 github_metadata_json=?, modified_at=? WHERE id=?
)SQL");
    stmt.bind(1, github_id);
    stmt.bind(2, default_branch);
    stmt.bind(3, default_branch);
    stmt.bind(4, description);
    stmt.bind(5, homepage);
    stmt.bind(6, html_url);
    stmt.bind(7, license);
    stmt.bind(8, archived ? 1 : 0);
    stmt.bind(9, fork ? 1 : 0);
    stmt.bind(10, stars);
    stmt.bind(11, forks);
    stmt.bind(12, open_issues);
    stmt.bind(13, created_at);
    stmt.bind(14, updated_at);
    stmt.bind(15, pushed_at);
    const auto now = now_utc();
    stmt.bind(16, now);
    stmt.bind(17, raw_json);
    stmt.bind(18, now);
    stmt.bind(19, id);
    stmt.step_done();
}

bool Database::enqueue_job(std::optional<std::int64_t> repo_id, const std::string& type, const std::string& message,
                           const std::string& payload) {
    Connection db(path_);
    db.exec("BEGIN IMMEDIATE;");
    try {
        const bool created = enqueue_job_locked(db.get(), repo_id, type, message, payload);
        db.exec("COMMIT;");
        return created;
    } catch (...) {
        db.exec("ROLLBACK;");
        throw;
    }
}

std::size_t Database::enqueue_all(const std::string& type) {
    const auto repositories = list_repositories();
    std::size_t count = 0;
    Connection db(path_);
    db.exec("BEGIN IMMEDIATE;");
    try {
        for (const auto& repo : repositories) {
            if (repo.paused) continue;
            if (type == "fetch" && repo.status != "ready" && repo.status != "unhealthy" && repo.status != "error" && repo.status != "missing") continue;
            if (type != "clone" && repo.status == "queued") continue;
            if (enqueue_job_locked(db.get(), repo.id, type, {}, {})) ++count;
        }
        db.exec("COMMIT;");
    } catch (...) {
        db.exec("ROLLBACK;");
        throw;
    }
    return count;
}

std::optional<Job> Database::claim_next_job() {
    Connection db(path_);
    db.exec("BEGIN IMMEDIATE;");
    try {
        Statement select(db.get(), R"SQL(
SELECT j.id,j.repo_id,j.type,j.status,j.queued_at,j.started_at,j.finished_at,j.message,j.payload,j.output,j.error,j.attempts
FROM jobs j LEFT JOIN repositories r ON r.id=j.repo_id
WHERE j.status='queued' AND (j.repo_id IS NULL OR r.paused=0)
 AND (j.scheduled_at = '' OR j.scheduled_at <= datetime('now'))
 AND (j.repo_id IS NULL OR NOT EXISTS (
   SELECT 1 FROM jobs running WHERE running.repo_id=j.repo_id AND running.status='running'
 ))
ORDER BY j.id LIMIT 1
)SQL");
        if (!select.step_row()) {
            db.exec("COMMIT;");
            return std::nullopt;
        }
        Job job = read_job(select);
        Statement update(db.get(), "UPDATE jobs SET status='running',started_at=?,attempts=attempts+1 WHERE id=? AND status='queued'");
        update.bind(1, now_utc());
        update.bind(2, job.id);
        update.step_done();
        if (sqlite3_changes(db.get()) != 1) {
            db.exec("ROLLBACK;");
            return std::nullopt;
        }
        db.exec("COMMIT;");
        job.status = "running";
        ++job.attempts;
        return job;
    } catch (...) {
        db.exec("ROLLBACK;");
        throw;
    }
}

void Database::finish_job(std::int64_t job_id, bool success, const std::string& output, const std::string& error) {
    Connection db(path_);
    Statement stmt(db.get(), "UPDATE jobs SET status=?,finished_at=?,output=?,error=? WHERE id=?");
    stmt.bind(1, success ? "success" : "failed");
    stmt.bind(2, now_utc());
    stmt.bind(3, output.substr(0, 1024 * 1024));
    stmt.bind(4, error.substr(0, 65536));
    stmt.bind(5, job_id);
    stmt.step_done();
}

void Database::requeue_job(std::int64_t job_id, const std::string& message, int delay_seconds) {
    Connection db(path_);
    // The delay is computed in SQL (datetime('now', '+N seconds')) rather than formatted
    // in C++, so it's guaranteed to use the exact same "YYYY-MM-DD HH:MM:SS" format that
    // claim_next_job compares scheduled_at against — now_utc() produces a different
    // (ISO 8601 'T'/'Z') format that would silently never compare as due.
    Statement stmt(db.get(),
        "UPDATE jobs SET status='queued', started_at='', message=?, "
        "scheduled_at = CASE WHEN ?2 > 0 THEN datetime('now', '+' || ?2 || ' seconds') ELSE '' END "
        "WHERE id=?3");
    stmt.bind(1, message);
    stmt.bind(2, delay_seconds);
    stmt.bind(3, job_id);
    stmt.step_done();
}

void Database::delay_pending_github_jobs(int delay_seconds) {
    Connection db(path_);
    // Called when one job discovers the GitHub API rate limit has been hit, so every
    // other job that would otherwise immediately retry the same exhausted quota backs
    // off too, instead of each independently burning an attempt to rediscover it.
    Statement stmt(db.get(),
        "UPDATE jobs SET scheduled_at = datetime('now', '+' || ?1 || ' seconds') "
        "WHERE type IN ('metadata','account') AND status='queued' "
        "AND (scheduled_at = '' OR scheduled_at < datetime('now', '+' || ?1 || ' seconds'))");
    stmt.bind(1, delay_seconds);
    stmt.step_done();
}

void Database::update_job_message(std::int64_t job_id, const std::string& message) {
    Connection db(path_);
    Statement stmt(db.get(), "UPDATE jobs SET message=? WHERE id=?");
    stmt.bind(1, message);
    stmt.bind(2, job_id);
    stmt.step_done();
}

JobPage Database::recent_jobs_page(const std::string& status_filter, std::size_t page, std::size_t per_page) const {
    Connection db(path_);
    Statement count_stmt(db.get(), "SELECT count(*) FROM jobs WHERE ?1 = '' OR status = ?1");
    count_stmt.bind(1, status_filter);
    JobPage result;
    result.total = count_stmt.step_row() ? static_cast<std::size_t>(count_stmt.integer(0)) : 0;

    const std::size_t page_count = result.total == 0 ? 1 : (result.total + per_page - 1) / per_page;
    page = std::max<std::size_t>(1, std::min(page, page_count));

    Statement stmt(db.get(), R"SQL(
SELECT id,repo_id,type,status,queued_at,started_at,finished_at,message,payload,output,error,attempts
FROM jobs WHERE ?1 = '' OR status = ?1
ORDER BY id DESC LIMIT ?2 OFFSET ?3
)SQL");
    stmt.bind(1, status_filter);
    stmt.bind(2, static_cast<std::int64_t>(per_page));
    stmt.bind(3, static_cast<std::int64_t>((page - 1) * per_page));
    while (stmt.step_row()) result.items.push_back(read_job(stmt));
    return result;
}

std::int64_t Database::active_job_count() const {
    Connection db(path_);
    Statement stmt(db.get(), "SELECT count(*) FROM jobs WHERE status='running'");
    return stmt.step_row() ? stmt.integer(0) : 0;
}

std::int64_t Database::queued_job_count() const {
    Connection db(path_);
    Statement stmt(db.get(), "SELECT count(*) FROM jobs WHERE status='queued'");
    return stmt.step_row() ? stmt.integer(0) : 0;
}

std::vector<RefRecord> Database::list_refs(std::int64_t repo_id, const std::string& type) const {
    Connection db(path_);
    const std::string sql = type.empty()
        ? "SELECT type,name,target_oid FROM refs WHERE repo_id=? ORDER BY type,name COLLATE NOCASE"
        : "SELECT type,name,target_oid FROM refs WHERE repo_id=? AND type=? ORDER BY name COLLATE NOCASE";
    Statement stmt(db.get(), sql);
    stmt.bind(1, repo_id);
    if (!type.empty()) stmt.bind(2, type);
    std::vector<RefRecord> refs;
    while (stmt.step_row()) refs.push_back({stmt.text(0), stmt.text(1), stmt.text(2)});
    return refs;
}

void Database::replace_releases(std::int64_t repo_id, const std::vector<ReleaseRecord>& releases) {
    Connection db(path_);
    db.exec("BEGIN IMMEDIATE;");
    try {
        Statement remove(db.get(), "DELETE FROM releases WHERE repo_id=?");
        remove.bind(1, repo_id);
        remove.step_done();
        Statement insert(db.get(), "INSERT INTO releases(repo_id,github_id,tag_name,name,html_url,published_at,prerelease,draft) VALUES(?,?,?,?,?,?,?,?)");
        for (const auto& release : releases) {
            insert.bind(1, repo_id);
            insert.bind(2, release.github_id);
            insert.bind(3, release.tag_name);
            insert.bind(4, release.name);
            insert.bind(5, release.html_url);
            insert.bind(6, release.published_at);
            insert.bind(7, release.prerelease ? 1 : 0);
            insert.bind(8, release.draft ? 1 : 0);
            insert.step_done();
            insert.reset();
        }
        db.exec("COMMIT;");
    } catch (...) {
        db.exec("ROLLBACK;");
        throw;
    }
}

std::vector<ReleaseRecord> Database::list_releases(std::int64_t repo_id) const {
    Connection db(path_);
    Statement stmt(db.get(), "SELECT github_id,tag_name,name,html_url,published_at,prerelease,draft FROM releases WHERE repo_id=? ORDER BY published_at DESC");
    stmt.bind(1, repo_id);
    std::vector<ReleaseRecord> releases;
    while (stmt.step_row()) {
        releases.push_back({stmt.integer(0), stmt.text(1), stmt.text(2), stmt.text(3), stmt.text(4), stmt.int_value(5) != 0, stmt.int_value(6) != 0});
    }
    return releases;
}

} // namespace gitcube
