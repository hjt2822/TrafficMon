#!/usr/bin/env bash
set -euo pipefail
root="$(dirname "$0")"
mkdir -p "$root/build"
gcc -O2 -DSQLITE_THREADSAFE=1 -c "$root/vendor/sqlite/sqlite3.c" -o "$root/build/sqlite3.o"
g++ -std=c++17 -O2 -Wall -Wextra -I"$root/include" -I"$root/vendor/sqlite" \
  "$root"/src/*.cpp "$root/build/sqlite3.o" -o "$root/build/trafficmon_m1.exe" \
  -static -liphlpapi -ladvapi32 -lws2_32
g++ -std=c++17 -O2 -Wall -Wextra -I"$root/include" \
  "$root/tests/connection_tests.cpp" "$root/src/connection_collector.cpp" \
  -o "$root/build/connection_tests.exe" -static -liphlpapi -lws2_32
g++ -std=c++17 -O2 -Wall -Wextra -I"$root/include" -I"$root/vendor/sqlite" \
  "$root/tests/storage_tests.cpp" "$root/src/storage.cpp" "$root/src/traffic_aggregator.cpp" \
  "$root/src/local_time.cpp" "$root/build/sqlite3.o" -o "$root/build/storage_tests.exe" -static
"$root/build/connection_tests.exe"
"$root/build/storage_tests.exe"
