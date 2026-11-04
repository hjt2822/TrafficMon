# Signal Paper

**A quiet instrument-paper interface for a local network traffic monitor.**

Signal Paper is built for one job: let someone glance at their own machine and understand what the network is doing, without turning a measurement tool into a marketing dashboard. It borrows its cues from bench instruments and chart recorders — cool paper, hairline rules, tabular numerals, two unmistakable data channels — rather than from SaaS card kits.

The interface has exactly one memorable element: the full-width **60-second signal band** at the top of the dashboard. Everything else stays deliberately quiet so that band can be read at a glance. There are no shadows, no gradients, no illustration, no decorative icons.

---

## Color System

The palette is two-channel: ink-green for download, copper for upload. Chrome (navigation, cards, borders) never borrows either channel color.

| Token | Hex | Swatch | Usage |
|---|---|---|---|
| background | `#F2F4F1` | ████ | Window / paper |
| surface | `#FFFFFF` | ████ | Cards, tables, plot background |
| surface-muted | `#E8EDE9` | ████ | Inactive chip wells, table header, chart grid |
| border | `#D7DFDA` | ████ | 1px card and table rules |
| border-strong | `#B9C6C0` | ████ | Hover border on chips and arrow buttons |
| text-primary | `#16211D` | ████ | Headings and values |
| text-secondary | `#55645E` | ████ | Labels and column headers |
| text-tertiary | `#66756E` | ████ | Axis labels, footer |
| primary / download | `#0E5E56` | ████ | Active nav, download stroke, focus ring |
| primary-soft | `#DCEAE5` | ████ | Download area fill |
| secondary / upload | `#A85F2A` | ████ | Upload stroke, copper accent |
| secondary-soft | `#F0E3D8` | ████ | Upload area fill |
| success | `#2F7D5A` | ████ | Live status dot |
| warning | `#9A6B12` | ████ | Degraded (non-admin) mode |
| error | `#B0362D` | ████ | Startup failure |

**Do**
- Use ink-green for anything that means *down/in*, copper for *up/out*.
- Keep ≥ 4.5:1 contrast for body text; the palette is tuned for it on `#F2F4F1` and `#FFFFFF`.
- Separate surfaces with a 1px border, never a shadow.

**Don't**
- Don't use the channel colors for buttons, links, or decoration.
- Don't tint text with channel colors except the two large live-rate numbers.
- Don't introduce a third series color; if a third series is ever needed, use `text-secondary`, not a new hue.

---

## Typography

Three system faces, each with a job. No web-font dependency: this ships as a native Windows app.

- **Bahnschrift** — display and large numerals. Semi-condensed, technical, slightly engineered. Used for the window title, the two 34px live rates, and chart totals.
- **Microsoft YaHei UI** — Chinese body and labels. Humanist, highly legible at 11–15px.
- **Consolas** — all tabular data: table numbers, axis values, date labels, footer path.

Scale (logical pixels at 96 DPI; multiply by DPI/96):

| Token | Size / line | Face | Where |
|---|---|---|---|
| xs | 11 / 14 | YaHei UI | axis labels, column headers, footer |
| sm | 12 / 16 | YaHei UI / Consolas | status pill, table numbers |
| base | 13 / 20 | YaHei UI | body, app names, navigation |
| lg | 15 / 22 | YaHei UI | card titles |
| xl | 18 / 24 | Bahnschrift | window title |
| 2xl | 20 / 26 | Bahnschrift | history totals |
| 3xl | 34 / 38 | Bahnschrift | live download/upload |

**Rules**
- Numbers are right-aligned and tabular; names are left-aligned.
- Only two weights in practice: 400 and 600. Avoid 700 — Bahnschrift semibold is the heaviest this system needs.
- No all-caps labels, no letter-spacing tricks, no italic emphasis.
- Chinese and Latin text share the same line; set Latin data in Consolas only when it is a value, never a label.

---

## Spacing & Layout

Base unit **4px**. The scale is 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 64.

- Window margin: 28px.
- Header: 64px, with a 1px bottom rule.
- Metrics row: 16px label gap, 20px below.
- Gap between cards: 16px.
- Card padding: 20px; card radius: 10px; border: 1px.
- Table header 28px; rows 36px; row rules only under the header and between rows (surface-muted).
- Footer: 32px, anchored to the bottom of the window.

Layout skeleton (dashboard):

```
┌──────────────────────────────────────────────────────────┐
│ mark  流量台   [● 实时监控]                    仪表盘 历史 │ header 64
├──────────────────────────────────────────────────────────┤
│ 下载            上传              今日         本月        │
│ 1.24 MB/s       86.0 KB/s         12.4 GB      210 GB     │ metrics
│                                                          │
│ ┌──────────────────────────────────────────────────────┐ │
│ │ 最近 60 秒                        ● 下载   ● 上传     │ │
│ │                                                      │ │
│ │   ╱╲      ╱╲                                         │ │ signal
│ │  ╱  ╲____╱  ╲____                                    │ │ band
│ │ -60s              -30s                       现在     │ │
│ └──────────────────────────────────────────────────────┘ │
│ ┌───────────────────────┐ ┌────────────────────────────┐ │
│ │ 网卡                  │ │ 今日应用                    │ │
│ │ 以太网   1.2M   40K   │ │ Chrome   820M  12M   —      │ │
│ │ Wi-Fi    0      0     │ │ Code     210M   2M   18%    │ │
│ └───────────────────────┘ └────────────────────────────┘ │
│ 数据仅保存在本机 · …                      网卡与应用不可相加 │ footer
└──────────────────────────────────────────────────────────┘
```

History skeleton:

```
period chips:  [今天] [本月]     ‹  2026年11月3日  ›        今日合计 …
┌──────────────────────────────────────────────────────────┐
│ 每小时下载 / 上传（网卡口径）                              │
│  ▂▄▆█▅▃▂  bar pairs, 24 buckets, grid + labels              │
└──────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────┐
│ 应用排行（应用口径）                                       │
└──────────────────────────────────────────────────────────┘
```

Alignment: everything is left-aligned except numeric columns (right) and the two live-rate values (left within their own fixed column). No centered body text.

---

## Component Patterns

### Status pill
Rounded full, surface-muted background, 6px dot. Green dot + "实时监控" when ETW is active; warning dot + "连接表模式 · 应用统计不可用" when degraded. Never color the pill background red for a degraded mode — degradation is not an error.

### Metric block
Small label above, 34px Bahnschrift value, baseline-aligned unit in 13px tertiary. Download value ink-green, upload value copper. Values change in place once per second; no animation, no rolling digits.

### Card
White, 1px border, 10px radius, 20px padding, 36px header row. Title 15px medium. A quiet note in 11px tertiary may sit next to the title; actions go to the far right. No shadow, ever.

### Signal band
The one bold element. Two filled areas (soft fills) with 2px strokes, 4 horizontal grid lines, no vertical grid except the endpoints. Max value printed in Consolas 11px at the top-left of the plot. Time labels `-60s / -30s / 现在` under the plot. Data is 60 one-second samples, newest at the right edge; the y-scale is the max of both channels over the window, floored at 4 KiB/s so an idle line stays flat rather than amplifying noise.

### History bars
Grouped bars per bucket: download bar then upload bar, 2px gap, 2px top radius, 60% of the bucket width total. No axis line on the left; a single hairline baseline. Labels every 4–5 buckets, never all 24/31. Zero buckets are a 1px stub in `surface-muted`, so absence reads as zero rather than as a gap.

### Table
Header 28px, 11px secondary labels, hairline under. Rows 36px with a surface-muted rule. App name 13px primary; numbers Consolas 12px secondary, right-aligned. If fewer rows than the card can hold, do not stretch rows; leave empty space. When empty, show one 13px tertiary sentence explaining why (e.g. "需要管理员权限才能按应用统计" or "今天还没有应用流量").

### Chips and arrows
Chips are 28px tall, full radius, 1px border. Active chip fills primary with white text; inactive is transparent with secondary text. Arrow buttons are 28×28, borderless, surface-muted on hover. Keyboard focus is a 2px primary outline with 2px offset.

### Tray menu
Native Win32 popup menu, default system styling. Items: 打开主窗口, 退出. No custom owner-draw in v1; the window carries the identity.

---

## Motion

Only state feedback: 120ms ease-out on hover fills and chip switches; 160ms on view changes. No page-load animation, no chart draw-on animation, no pulsing live dot. A single discrete change per second (the signal band shift) is the only ambient motion.

---

## AI Agent Instructions

When generating or modifying this UI:

1. Read `design-tokens.json` first. Never invent a color, size, radius, or font. If a value is missing, add a token and document its usage.
2. Download is `primary` (`#0E5E56`); upload is `secondary` (`#A85F2A`). This mapping is semantic, not decorative.
3. Surfaces are flat: white card, 1px `border`, 10px radius, no shadow. Do not add elevation.
4. Use exactly three faces: Bahnschrift (display/large numerals), Microsoft YaHei UI (Chinese body), Consolas (tabular values). Weights 400 and 600 only.
5. Numbers right-align and use tabular figures. Names left-align. No centered body text.
6. The signal band is the only expressive element. Keep the rest quiet.
7. Never merge NIC totals with application totals. Label each measurement basis at the point where both appear.
8. Degraded mode (non-admin) is a warning, not an error: show the pill in `warning`, keep NIC data live, and show application rows as unavailable — never fabricate zeros.
9. Empty states are one plain sentence telling the user what is missing or what to do next.
10. Targets are native controls: 120/160ms hover transitions, 28px chips, 36px rows, keyboard focus visible.

---

## Windows / GDI Mapping

| Design token | GDI implementation |
|---|---|
| background | `FillRect` with `RGB(0xF2,0xF4,0xF1)` |
| card | `RoundRect(radius 10)`, white brush, 1px `#D7DFDA` pen |
| color | `RGB(r,g,b)` constants in `ui/theme.h`; no GDI+ |
| type | `CreateFontW` with height `-MulDiv(px, dpi, 96)`; face fallback probed via `EnumFontFamiliesExW` |
| spacing | logical px scaled with `MulDiv(v, dpi, 96)` |
| DPI | PerMonitorV2 manifest; rebuild fonts and backbuffer on `WM_DPICHANGED` |
| chart | `Polyline` / `Polygon` with solid fills; no alpha, so soft tokens are opaque pre-blends |
| text | `DrawTextW` with `DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX`, `DT_END_ELLIPSIS` where truncation is possible |
| tray | `Shell_NotifyIconW` with a generated 16/32px ICO resource |

GDI has no subpixel text or rounded clipping; keep radii at 10px or below and never clip text to a rounded path.

---

*Signal Paper v1.0 — 2026-11-03.*