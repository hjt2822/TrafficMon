#include "traffic_aggregator.h"
#include "local_time.h"
#include "sqlite3.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
TrafficDelta Delta(const char* id, int64_t utc, int64_t up, int64_t down,
                   bool split = false, int64_t upLan = 0, int64_t downLan = 0) {
    TrafficDelta d;
    d.identity = id; d.utcSeconds = utc; d.up = up; d.down = down;
    d.lanSplit = split; d.upLan = upLan; d.downLan = downLan;
    return d;
}
}

int main() {
    Storage db(":memory:");
    TrafficAggregator a;
    const int64_t hour = 3600 * 10;
    const int32_t day = LocalDayForUtc(hour);
    assert(day > 0 && NextDay(day) > day && MonthStartOf(day) % 100 == 1);
    a.Add(false, Delta("nic-A", hour, 10, 20));
    a.Add(false, Delta("nic-A", hour + 1, 5, 7));
    a.Add(false, Delta("nic-A", hour + 3600, 30, 40));
    a.Add(true, Delta("app-A", hour, 100, 200, true, 60, 150));
    a.Add(true, Delta("app-A", hour + 1, 1, 2, true, 0, 2));
    a.Flush(db);
    assert(a.Pending() == 0);
    a.Flush(db);
    auto nic = db.Query(false, day, NextDay(day));
    assert(nic.size() == 2);
    assert(nic[0].utcHour == hour && nic[0].up == 15 && nic[0].down == 27 && !nic[0].lanSplit);
    assert(nic[1].up == 30);
    auto app = db.Query(true, day, NextDay(day), "app-A");
    assert(app.size() == 1 && app[0].up == 101 && app[0].down == 202);
    assert(app[0].lanSplit && app[0].upLan == 60 && app[0].downLan == 152);
    assert(db.Query(false, NextDay(day), NextDay(NextDay(day))).empty());

    a.Add(true, Delta("mixed", hour, 5, 5, true, 1, 1));
    a.Add(true, Delta("mixed", hour, 5, 5));
    a.Flush(db);
    auto mixed = db.Query(true, day, NextDay(day), "mixed")[0];
    assert(!mixed.lanSplit && mixed.up == 10 && mixed.upLan == 0);

    a.Add(false, Delta("overflow", hour, std::numeric_limits<int64_t>::max(), 0));
    a.Flush(db);
    a.Add(false, Delta("overflow", hour, 1, 0));
    a.Add(false, Delta("nic-A", hour, 1, 0));
    bool failed = false;
    try { a.Flush(db); } catch (const std::exception&) { failed = true; }
    assert(failed && a.Pending() == 2);
    assert(db.Query(false, day, NextDay(day), "nic-A")[0].up == 15);

    {
        Storage db2(":memory:");
        TrafficAggregator c;
        c.Add(false, Delta("nic", hour, 1, 2));
        c.Flush(db2);
        c.Add(false, Delta("nic", hour, 3, 4));
        c.Flush(db2);
        auto r = db2.Query(false, day, NextDay(day))[0];
        assert(!r.lanSplit && r.up == 4 && r.down == 6);
        c.Add(true, Delta("app", hour, 5, 5, true, 2, 3));
        c.Flush(db2);
        c.Add(true, Delta("app", hour, 5, 5, true, 1, 4));
        c.Flush(db2);
        auto split = db2.Query(true, day, NextDay(day))[0];
        assert(split.lanSplit && split.upLan == 3 && split.downLan == 7);
        c.Add(true, Delta("app", hour, 1, 1));
        c.Flush(db2);
        auto mixedAgain = db2.Query(true, day, NextDay(day))[0];
        assert(!mixedAgain.lanSplit && mixedAgain.up == 11);
    }

    const char* file = "build/storage-test.sqlite3";
    std::remove(file);
    {
        Storage disk(file);
        TrafficAggregator b;
        b.Add(false, Delta("adapter", hour, 9, 12));
        b.Flush(disk);
    }
    {
        Storage disk(file);
        assert(disk.Query(false, day, NextDay(day))[0].down == 12);
    }

    // Build a v2 database (old schema, no local_day) and migrate it.
    sqlite3* raw = nullptr;
    assert(sqlite3_open(file, &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw,
        "DROP TABLE nic_hourly; DROP TABLE application_hourly; DROP TABLE collection_session;"
        "CREATE TABLE nic_hourly(utc_hour INTEGER,identity TEXT,up INTEGER,down INTEGER);"
        "CREATE TABLE application_hourly(utc_hour INTEGER,identity TEXT,up INTEGER,down INTEGER);"
        "CREATE TABLE collection_session(id INTEGER PRIMARY KEY,started INTEGER,committed INTEGER,"
        "mode TEXT,closed INTEGER DEFAULT 0);"
        "INSERT INTO nic_hourly VALUES(72000,'legacy',3,4);"
        "PRAGMA user_version=2;", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
    {
        Storage migrated(file);
        auto rows = migrated.Query(false, LocalDayForUtc(72000), NextDay(LocalDayForUtc(72000)), "legacy");
        assert(rows.size() == 1 && rows[0].up == 3 && !rows[0].lanSplit);
        assert(migrated.Sessions(0, 4102444800LL).empty());
        migrated.BeginSession(false, 100);
        TrafficAggregator empty;
        empty.Flush(migrated);
        auto sessions = migrated.Sessions(0, 4102444800LL);
        assert(sessions.size() == 1 && sessions[0].mode == "connections" && !sessions[0].closed);
        assert(sessions[0].committed >= sessions[0].started);
        migrated.CloseSession();
        assert(migrated.Sessions(0,4102444800LL)[0].closed);
        migrated.BeginSession(true, 200);
    }
    {
        Storage reopened(file);
        auto sessions = reopened.Sessions(0,4102444800LL);
        assert(sessions.size() == 2 && !sessions[1].closed && sessions[1].committed == 200);
    }
    assert(sqlite3_open(file, &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw, "PRAGMA user_version=99", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
    failed = false;
    try { Storage future(file); } catch (const std::exception&) { failed = true; }
    assert(failed);
    std::remove(file);
    std::puts("storage/aggregation local-day, LAN split, rollback, migration v2->v3, schema tests passed");
}