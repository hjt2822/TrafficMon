# 流量台 TrafficMon

Windows 本机网络流量监控工具。界面为纯 C + Win32/GDI 自绘，采集与存储核心为 C++17，两者通过 `ui/bridge.h` 的 C ABI 连接。

- 实时显示网卡下载/上传速度
- 按应用（进程）统计上传/下载流量
- 历史日/月查询与柱状图
- 系统托盘、轻量模式浮窗、4 套主题

所有数据仅保存在本机，不上传服务器，不采集窗口标题、剪贴板或文件内容。

## 快速开始

双击 `build/trafficmon_ui.exe` 即可。首次启动会在 exe 同目录创建 `traffic.sqlite3`。

按应用统计需要管理员权限（ETW 内核 provider 订阅要求）。非管理员运行时网卡速度照常显示，应用区域提示“需要管理员权限”，轻量模式的程序行显示“—”。

## 命令行参数

`trafficmon_ui.exe`：

| 参数 | 说明 |
|---|---|
| `--db <路径>` | 指定数据库文件，默认为 exe 同目录的 `traffic.sqlite3` |
| `--mini` | 直接以轻量模式启动 |
| `--no-etw` | 不订阅 ETW，仅网卡计数（用于无管理员权限或复现降级界面） |
| `--exit-after <秒>` | 运行指定秒数后自动退出（测试用，1–86399） |

`trafficmon_m1.exe`（控制台版）：

```
trafficmon_m1 [1..86400 秒] [--no-etw] [--db <路径>]
              [--day YYYY-MM-DD | --month YYYY-MM]
```

## 构建

### Visual Studio 2022（新增 CMake 构建）

安装 Visual Studio 2022 17.0+，勾选 **使用 C++ 的桌面开发**，包含 MSVC、Windows 10/11 SDK 和 **用于 Windows 的 C++ CMake 工具**。不需要 MFC。

在 VS 中选择 **文件 → 打开 → 文件夹**，打开项目根目录，选择 `vs2022-x64` 预设，将生成配置切为 `Release`，启动目标选择 `trafficmon_ui.exe`。

也可在 Developer PowerShell 中生成解决方案并编译：

```powershell
cmake --preset vs2022-x64
cmake --build --preset vs2022-release
ctest --preset vs2022-release
```

解决方案位于 `build/vs2022/trafficmon.sln`，Release 界面版位于 `build/vs2022/bin/trafficmon_ui.exe`。编译 Debug 版使用 `cmake --build --preset vs2022-debug`。Debug/Release 共用 `bin` 输出目录，不要同时构建两种配置。

仅构建界面版（不生成控制台版和核心测试）：

```powershell
cmake --preset vs2022-x64 -DTRAFFICMON_BUILD_TOOLS=OFF -DTRAFFICMON_BUILD_TESTS=OFF
cmake --build --preset vs2022-release --target trafficmon_ui
```

UI 文件仍按 C11 编译，核心与桥接按 C++17 编译；SQLite 从仓库源码静态构建，不需要预先生成 `.o`。MSVC 使用 `/utf-8`、静态 CRT 和 Release 链接优化，清单由资源文件嵌入。

**验证范围：** 本机未安装 Visual Studio，尚未实测 MSVC 编译；CMake 构建已使用 MinGW-w64 验证，不能据此视为 MSVC 已验证。

### MinGW-w64

依赖 gcc / g++ / windres。原有脚本仍可使用：

```bash
bash build.sh      # 编译 sqlite3.o、控制台版与核心测试，并运行测试
bash build_ui.sh   # 编译 Release 界面版 build/trafficmon_ui.exe
```

`build_ui.sh` 依赖 `build/sqlite3.o`，需先执行一次 `build.sh`。界面版为 Release 构建（`-O2 -DNDEBUG`，链接时 `-s` 去除符号）。

推荐用 CMake 独立构建（另需 CMake 3.21+、Ninja，且 gcc/g++/windres 在 PATH 中）：

```bash
cmake --preset mingw-release
cmake --build --preset mingw-release
ctest --preset mingw-release
```

产物位于 `build/mingw-release/bin/`。CMake 在 Release 下也保留核心测试的断言，避免测试被 `NDEBUG` 编译成空检查。

## 测试

```bash
python tests/ui_smoke.py          # 界面冒烟：ETW 归属、退出落盘、schema v3、--no-etw 降级
python tests/integration_test.py  # 采集落盘、重启累计、日/月查询、短命进程归属
```

测试 CMake 生成的界面版，可通过环境变量指定 exe（PowerShell 示例）：

```powershell
$env:TRAFFICMON_UI_EXE = "$PWD/build/vs2022/bin/trafficmon_ui.exe"
python tests/ui_smoke.py
```

默认不设置该变量时，冒烟测试仍使用 `build/trafficmon_ui.exe`。

冒烟测试中 ETW 部分需要管理员权限，且运行前需关闭正在运行的流量台（单实例互斥）。若测试被强制结束，可能残留 `TrafficMon-*` ETW 会话，可用 `logman query -ets` 查看、`logman stop <名称> -ets` 清理。

## 项目结构

```
src/                 采集、聚合、存储核心（C++17）
  nic_collector      网卡计数轮询（GetIfTable2）
  proc_collector     ETW Kernel-Network 订阅
  connection_collector 连接表降级路径
  traffic_aggregator 内存聚合与定时提交
  storage            SQLite 封装、schema 迁移
include/             核心头文件
ui/                  纯 C Win32 界面
  main.c             WinMain、窗口过程、布局、轻量模式
  theme.c            皮肤调色板、DPI、字体
  draw.c             双缓冲绘制原语与图表
  tray.c             托盘图标与菜单
  format.c           字节/速率格式化
  bridge.h/.cpp      C ABI 桥接 C 界面与 C++ 核心
tests/               冒烟测试、集成测试、流量生成脚本
design-system-signal-paper/  设计 token 与视觉预览
docs/                产品架构与研发方案
vendor/sqlite/       SQLite amalgamation
```

## 数据口径

- **网卡总量**来自系统网卡计数器，包含所有进程、系统服务与协议开销。
- **应用流量**来自 ETW 逐进程事件，局域网/互联网分别累计。
- 两者**不能相加**：各应用之和通常小于网卡总量，界面中分别标注。
- 无法分类的局域网/互联网拆分显示“—”，不显示 0。
- 小时桶以 UTC 存储，同时写入本地日历标签；`--day`/`--month` 按本地自然日/月查询。

