#!/usr/bin/env bash
# Builds the pure-C Win32 UI (trafficmon_ui.exe) against the C++ core.
# The UI itself is C11; bridge.cpp is the only C++ in the UI layer.
set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
cd "$root"
mkdir -p build

cc="${CC:-gcc}"
cxx="${CXX:-g++}"
windres="${WINDRES:-windres}"

if [ ! -f build/sqlite3.o ]; then
  echo "build/sqlite3.o is missing; run build.sh once to produce it." >&2
  exit 1
fi

"$windres" -I"$root" -I"$root/ui" -I"$root/include" \
  ui/app.rc -O coff -o build/ui_res.o

cflags=(-std=c11 -O2 -DNDEBUG -Wall -Wextra -DUNICODE -D_UNICODE -Iui -Iinclude)
cxxflags=(-std=c++17 -O2 -DNDEBUG -Wall -Wextra -DUNICODE -D_UNICODE -Iui -Iinclude -Ivendor/sqlite)

for f in ui/main.c ui/theme.c ui/format.c ui/draw.c ui/tray.c; do
  obj="build/$(basename "${f%.c}").o"
  "$cc" "${cflags[@]}" -c "$f" -o "$obj"
done

"$cxx" "${cxxflags[@]}" -c ui/bridge.cpp -o build/bridge.o

core_objs=()
for f in src/nic_collector.cpp src/proc_collector.cpp src/storage.cpp \
         src/traffic_aggregator.cpp src/local_time.cpp; do
  obj="build/$(basename "${f%.cpp}").o"
  "$cxx" "${cxxflags[@]}" -c "$f" -o "$obj"
  core_objs+=("$obj")
done

"$cxx" -mwindows -static -s -specs=ui/no-default-manifest.specs \
  -o build/trafficmon_ui.exe \
  build/main.o build/theme.o build/format.o build/draw.o build/tray.o \
  build/bridge.o "${core_objs[@]}" build/sqlite3.o build/ui_res.o \
  -liphlpapi -ladvapi32 -lws2_32 -lgdi32 -luser32 -lshell32 -lcomctl32 -lole32 -luuid \
  -lmingw32 -lgcc -lstdc++

echo "built: $root/build/trafficmon_ui.exe"