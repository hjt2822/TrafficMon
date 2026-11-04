#include "storage.h"
#include "local_time.h"
#include "sqlite3.h"
#include <stdexcept>

namespace {
const int kSchemaVersion = 3;

void Check(sqlite3* db, int rc) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db));
}
void Exec(sqlite3* db, const char* sql) { Check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr)); }

class Statement {
public:
    sqlite3* db;
    sqlite3_stmt* stmt = nullptr;
    Statement(sqlite3* d, const char* sql) : db(d) {
        Check(db, sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr));
    }
    ~Statement() { sqlite3_finalize(stmt); }
    void Number(int i, int64_t v) { Check(db, sqlite3_bind_int64(stmt, i, v)); }
    void Text(int i, const std::string& v) { Check(db, sqlite3_bind_text(stmt, i, v.c_str(), -1, SQLITE_TRANSIENT)); }
    int Step() { int rc = sqlite3_step(stmt); Check(db, rc); return rc; }
    void Reset() { Check(db, sqlite3_reset(stmt)); Check(db, sqlite3_clear_bindings(stmt)); }
};

void Validate(const TrafficRecord& r) {
    if (r.utcHour < 0 || r.utcHour % 3600 || r.localDay <= 0 || r.identity.empty() ||
        r.up < 0 || r.down < 0)
        throw std::invalid_argument("Invalid traffic record");
    if (r.lanSplit && (r.upLan < 0 || r.downLan < 0 || r.upLan > r.up || r.downLan > r.down))
        throw std::invalid_argument("LAN split exceeds totals");
}

const char* kCreateHourly =
    "CREATE TABLE nic_hourly(utc_hour INTEGER NOT NULL,local_day INTEGER NOT NULL,"
    "identity TEXT NOT NULL,"
    "up INTEGER NOT NULL CHECK(typeof(up)='integer' AND up>=0),"
    "down INTEGER NOT NULL CHECK(typeof(down)='integer' AND down>=0),"
    "up_lan INTEGER CHECK(up_lan IS NULL OR (typeof(up_lan)='integer' AND up_lan>=0)),"
    "down_lan INTEGER CHECK(down_lan IS NULL OR (typeof(down_lan)='integer' AND down_lan>=0)),"
    "PRIMARY KEY(utc_hour,local_day,identity));"
    "CREATE TABLE application_hourly(utc_hour INTEGER NOT NULL,local_day INTEGER NOT NULL,"
    "identity TEXT NOT NULL,"
    "up INTEGER NOT NULL CHECK(typeof(up)='integer' AND up>=0),"
    "down INTEGER NOT NULL CHECK(typeof(down)='integer' AND down>=0),"
    "up_lan INTEGER CHECK(up_lan IS NULL OR (typeof(up_lan)='integer' AND up_lan>=0)),"
    "down_lan INTEGER CHECK(down_lan IS NULL OR (typeof(down_lan)='integer' AND down_lan>=0)),"
    "PRIMARY KEY(utc_hour,local_day,identity));";

const char* kCreateSessions =
    "CREATE TABLE collection_session(id INTEGER PRIMARY KEY,started INTEGER NOT NULL,"
    "committed INTEGER NOT NULL,mode TEXT NOT NULL CHECK(mode IN ('etw','connections')),"
    "closed INTEGER NOT NULL DEFAULT 0);";

// Rebuilds pre-v3 hourly tables, backfilling local_day from each row's UTC hour using the
// machine timezone at migration time (approximation: rows are not re-localized per historical rule).
void MigrateHourly(sqlite3* db, const char* table) {
    std::string create = std::string("CREATE TABLE ") + table + "_v3(utc_hour INTEGER NOT NULL,"
        "local_day INTEGER NOT NULL,identity TEXT NOT NULL,"
        "up INTEGER NOT NULL CHECK(typeof(up)='integer' AND up>=0),"
        "down INTEGER NOT NULL CHECK(typeof(down)='integer' AND down>=0),"
        "up_lan INTEGER CHECK(up_lan IS NULL OR (typeof(up_lan)='integer' AND up_lan>=0)),"
        "down_lan INTEGER CHECK(down_lan IS NULL OR (typeof(down_lan)='integer' AND down_lan>=0)),"
        "PRIMARY KEY(utc_hour,local_day,identity));";
    Exec(db, create.c_str());
    {
        std::string select = std::string("SELECT utc_hour,identity,up,down FROM ") + table;
        std::string insert = std::string("INSERT INTO ") + table + "_v3 VALUES(?1,?2,?3,?4,?5,NULL,NULL)";
        Statement in(db, select.c_str());
        Statement out(db, insert.c_str());
        while (in.Step() == SQLITE_ROW) {
            int64_t hour = sqlite3_column_int64(in.stmt, 0);
            out.Number(1, hour);
            out.Number(2, LocalDayForUtc(hour));
            out.Text(3, reinterpret_cast<const char*>(sqlite3_column_text(in.stmt, 1)));
            out.Number(4, sqlite3_column_int64(in.stmt, 2));
            out.Number(5, sqlite3_column_int64(in.stmt, 3));
            out.Step(); out.Reset();
        }
    }
    std::string drop = std::string("DROP TABLE ") + table + ";ALTER TABLE " + table +
                       "_v3 RENAME TO " + table + ";";
    Exec(db, drop.c_str());
}
}

Storage::Storage(const std::string& path) {
    try {
        Check(db_, sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr));
        Open();
    } catch (...) {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

Storage::Storage(const std::wstring& path) {
    try {
        Check(db_, sqlite3_open16(path.c_str(), &db_));
        Open();
    } catch (...) {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

void Storage::Open() {
    {
        Check(db_, sqlite3_busy_timeout(db_, 2000));
        int version;
        { Statement s(db_, "PRAGMA user_version"); s.Step(); version = sqlite3_column_int(s.stmt, 0); }
        if (version < 0 || version > kSchemaVersion)
            throw std::runtime_error("Unsupported database schema version");
        Exec(db_, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
        if (version == 0) {
            std::string create = std::string("BEGIN IMMEDIATE;") + kCreateHourly + kCreateSessions +
                                 "PRAGMA user_version=3; COMMIT;";
            Exec(db_, create.c_str());
        } else if (version < kSchemaVersion) {
            Exec(db_, "BEGIN IMMEDIATE;");
            try {
                if (version < 2) Exec(db_, kCreateSessions);
                MigrateHourly(db_, "nic_hourly");
                MigrateHourly(db_, "application_hourly");
                Exec(db_, "PRAGMA user_version=3; COMMIT;");
            } catch (...) {
                sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                throw;
            }
        }
    }
}

Storage::~Storage() { sqlite3_close(db_); }

void Storage::Write(const TrafficBatch& batch) {
    for (const auto& r : batch.nic) Validate(r);
    for (const auto& r : batch.application) Validate(r);
    Exec(db_, "BEGIN IMMEDIATE");
    try {
        auto write = [&](const std::vector<TrafficRecord>& rows, const char* sql) {
            Statement s(db_, sql);
            for (const auto& r : rows) {
                s.Number(1, r.utcHour); s.Number(2, r.localDay); s.Text(3, r.identity);
                s.Number(4, r.up); s.Number(5, r.down);
                if (r.lanSplit) { s.Number(6, r.upLan); s.Number(7, r.downLan); }
                else {
                    Check(db_, sqlite3_bind_null(s.stmt, 6));
                    Check(db_, sqlite3_bind_null(s.stmt, 7));
                }
                s.Step(); s.Reset();
            }
        };
        write(batch.nic, "INSERT INTO nic_hourly VALUES(?1,?2,?3,?4,?5,?6,?7) "
                         "ON CONFLICT(utc_hour,local_day,identity) DO UPDATE SET "
                         "up=up+excluded.up,down=down+excluded.down,"
                         "up_lan=CASE WHEN up_lan IS NULL OR excluded.up_lan IS NULL THEN NULL "
                                     "ELSE up_lan+excluded.up_lan END,"
                         "down_lan=CASE WHEN down_lan IS NULL OR excluded.down_lan IS NULL THEN NULL "
                                       "ELSE down_lan+excluded.down_lan END");
        write(batch.application, "INSERT INTO application_hourly VALUES(?1,?2,?3,?4,?5,?6,?7) "
                                 "ON CONFLICT(utc_hour,local_day,identity) DO UPDATE SET "
                                 "up=up+excluded.up,down=down+excluded.down,"
                                 "up_lan=CASE WHEN up_lan IS NULL OR excluded.up_lan IS NULL THEN NULL "
                                             "ELSE up_lan+excluded.up_lan END,"
                                 "down_lan=CASE WHEN down_lan IS NULL OR excluded.down_lan IS NULL THEN NULL "
                                               "ELSE down_lan+excluded.down_lan END");
        if (session_) {
            Statement s(db_, "UPDATE collection_session SET committed="
                             "max(started,CAST(strftime('%s','now') AS INTEGER)) WHERE id=?1");
            s.Number(1, session_); s.Step();
        }
        Exec(db_, "COMMIT");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

std::vector<TrafficRecord> Storage::Query(bool application, int32_t beginDay, int32_t endDay,
                                          const std::string& identity) const {
    if (!ValidDay(beginDay) || !ValidDay(endDay) || endDay <= beginDay)
        throw std::invalid_argument("Query requires a valid half-open local-day range");
    std::string sql = "SELECT utc_hour,local_day,identity,up,down,up_lan,down_lan FROM ";
    sql += application ? "application_hourly" : "nic_hourly";
    sql += " WHERE local_day>=?1 AND local_day<?2 AND (?3='' OR identity=?3) "
           "ORDER BY local_day,utc_hour,identity";
    Statement s(db_, sql.c_str());
    s.Number(1, beginDay); s.Number(2, endDay); s.Text(3, identity);
    std::vector<TrafficRecord> result;
    while (s.Step() == SQLITE_ROW) {
        TrafficRecord r;
        r.utcHour = sqlite3_column_int64(s.stmt, 0);
        r.localDay = sqlite3_column_int(s.stmt, 1);
        r.identity = reinterpret_cast<const char*>(sqlite3_column_text(s.stmt, 2));
        r.up = sqlite3_column_int64(s.stmt, 3);
        r.down = sqlite3_column_int64(s.stmt, 4);
        r.lanSplit = sqlite3_column_type(s.stmt, 5) != SQLITE_NULL &&
                     sqlite3_column_type(s.stmt, 6) != SQLITE_NULL;
        if (r.lanSplit) { r.upLan = sqlite3_column_int64(s.stmt, 5); r.downLan = sqlite3_column_int64(s.stmt, 6); }
        result.push_back(std::move(r));
    }
    return result;
}

void Storage::BeginSession(bool etw, int64_t started) {
    if (session_ || started < 0) throw std::invalid_argument("Invalid session start");
    Statement s(db_, "INSERT INTO collection_session(started,committed,mode) VALUES(?1,?1,?2)");
    s.Number(1, started); s.Text(2, etw ? "etw" : "connections"); s.Step();
    session_ = sqlite3_last_insert_rowid(db_);
}

void Storage::CloseSession() {
    if (!session_) return;
    Statement s(db_, "UPDATE collection_session SET closed=1 WHERE id=?1");
    s.Number(1, session_); s.Step(); session_ = 0;
}

std::vector<Coverage> Storage::Sessions(int64_t begin, int64_t end) const {
    Statement s(db_, "SELECT id,started,committed,mode,closed FROM collection_session "
                     "WHERE started<?2 AND committed>=?1 ORDER BY started,id");
    s.Number(1, begin); s.Number(2, end);
    std::vector<Coverage> result;
    while (s.Step() == SQLITE_ROW)
        result.push_back({sqlite3_column_int64(s.stmt,0),sqlite3_column_int64(s.stmt,1),
            sqlite3_column_int64(s.stmt,2),reinterpret_cast<const char*>(sqlite3_column_text(s.stmt,3)),
            sqlite3_column_int(s.stmt,4) != 0});
    return result;
}