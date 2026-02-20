# WinCaptureSync

低延迟同步录屏 + 键鼠输入记录（Windows）。

## 当前实现范围

- 全局热键一键开始/停止（默认 `Ctrl+Alt+F9`，可配置）
- GUI 界面（源类型选择、窗口列表、实时预览、开始/停止）
- 屏幕录制输出 `video.mp4`（H.264，Media Foundation）
- 输入记录输出 `input.jsonl`（`WH_KEYBOARD_LL` + `WH_MOUSE_LL`）
- 会话元数据 `video_meta.json`
- 统一时间基准：QPC + UTC anchor 映射
- 启停状态机：`Idle -> Arming -> Recording -> Stopping -> Idle`
- 输入写盘采用队列 + 后台线程，回调线程不做重 IO

## 目录结构

```text
src/
  main/      入口、热键、状态机、配置
  time/      QPC 与 UTC 映射
  input/     低层 hook、事件队列、JSONL writer
  capture/   屏幕采集与元数据
  encode/    Media Foundation H.264 编码
docs/
  formats.md 输出格式
  sync.md    对齐策略
tests/
  time_tests.cpp
```

## 环境要求

- Windows 10/11
- Visual Studio 2022 (含 C++ 桌面开发工具集)
- Windows SDK（包含 Media Foundation）
- CMake 3.21+
- Ninja（可选）

## 构建

```powershell
cmake -S . -B build -G "Ninja"
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

## 运行

```powershell
./build/WinCaptureSync.exe
```

打开 GUI 后可选择：

- `主显示器全屏`
- `窗口`（可刷新窗口列表并在预览区实时查看）

可点击按钮或按全局热键切换开始/停止。输出默认在：

```text
./captures/<YYYYMMDD_HHMMSS>/
  video.mp4
  video_meta.json
  input.jsonl
```

## 配置

首次启动会自动生成 `config.ini`，示例：

```ini
hotkey_modifiers=CTRL+ALT
hotkey_vk=F9
capture_fps=60
capture_bitrate=12000000
capture_width=0
capture_height=0
input_queue_capacity=32768
input_batch_size=512
input_flush_interval_ms=10
output_root=captures
```

- `capture_width/height=0` 表示主屏全屏
- `hotkey_modifiers` 支持 `CTRL/ALT/SHIFT/WIN` 组合
- `hotkey_vk` 支持 `A-Z`、`0-9`、`F1-F24`

## 已知限制

- 当前录屏后端优先使用 WGC（不可用时回退到 GDI）+ Media Foundation。
- 暂未实现音频、暂停恢复、覆盖层。

## 视频时间轴对齐

项目内置了时间轴对齐脚本：	ools/align_timeline.ps1，用于把 input.jsonl 事件时间转换到视频时间轴。

### 用法

`powershell
# 对齐最新一次会话
powershell -ExecutionPolicy Bypass -File tools/align_timeline.ps1

# 对齐指定会话目录
powershell -ExecutionPolicy Bypass -File tools/align_timeline.ps1 -SessionDir captures\20260217_212443

# 自定义输出前缀（默认 aligned）
powershell -ExecutionPolicy Bypass -File tools/align_timeline.ps1 -SessionDir captures\20260217_212443 -OutputPrefix aligned_custom
`

### 输出文件

脚本会在会话目录下生成：

- <prefix>_input.jsonl：原输入事件 + 对齐字段
- <prefix>_events.csv：便于筛选/作图的表格
- <prefix>_report.json：对齐摘要（包含启动偏差）

### 关键对齐字段

- 	_video_ms：事件相对视频起点（screen_start_qpc）的毫秒时间
- 	_input_ms：事件相对输入起点（input_start_qpc）的毫秒时间
- 	_utc_epoch_ns：事件对应的 UTC 纳秒时间戳
- input_minus_screen_ms：输入启动相对视频启动的偏差（毫秒）

## Timeline Alignment

Use `tools/align_timeline.ps1` to align `input.jsonl` events to the video timeline.

### Commands

```powershell
# Align the latest capture session
powershell -ExecutionPolicy Bypass -File tools/align_timeline.ps1

# Align a specific session
powershell -ExecutionPolicy Bypass -File tools/align_timeline.ps1 -SessionDir captures\20260217_212443

# Custom output file prefix (default: aligned)
powershell -ExecutionPolicy Bypass -File tools/align_timeline.ps1 -SessionDir captures\20260217_212443 -OutputPrefix aligned_custom
```

### Generated Files

The script generates these files inside the session directory:

- `<prefix>_input.jsonl`: original input events plus aligned fields
- `<prefix>_events.csv`: flattened table for filtering/charting
- `<prefix>_report.json`: alignment summary

### Key Fields

- `t_video_ms`: event time in milliseconds relative to `screen_start_qpc`
- `t_input_ms`: event time in milliseconds relative to `input_start_qpc`
- `t_utc_epoch_ns`: event UTC timestamp in nanoseconds
- `input_minus_screen_ms`: startup offset between input and screen capture

## Dual Window Recording

Window/Monitor source mode supports recording two sources at the same time.

- `Window 1`: primary required source (window or monitor)
- `Window 2`: optional second source (window or monitor, `<None>` to disable)
- Output files:
- `Window 1` -> `video.mp4` + `video_meta.json`
- `Window 2` -> `video_2.mp4` + `video_2_meta.json` (only when selected)

Each metadata file includes:

- `capture.source_type`
- `capture.source_hwnd`
- `screen_start_qpc`, `input_start_qpc`, `qpc_freq`

## Codec Configuration

Video codec is configurable through `config.ini`:

```ini
capture_codec=h264
```

Supported values:

- `h264` (default)
- `hevc`

Notes:

- Output container remains `.mp4`
- If HEVC encoder is unavailable on the system, encoder falls back to H.264 automatically
- `video_meta.json` writes the actual codec in `capture.codec`

## Input Diagnostic Mode

Enable raw input diagnostics through `config.ini`:

```ini
input_diagnostic_mode=1
```

When enabled, each session also writes:

- `input_diag.jsonl`: raw-input registration and per-message diagnostics

This file is intended for debugging cases where a specific game does not emit expected mouse events.

## Video Capture Backend

Recorder now prefers `Windows Graphics Capture (WGC)` for single-window/single-monitor recording.

- Cursor is captured by the system compositor pipeline (`IsCursorCaptureEnabled=true`)
- Captured frame matches on-screen composition without application-side cursor repaint
- `video_meta.json` includes `capture.backend` (`wgc` or `gdi`)
- 默认使用 WGC 录制能录制到鼠标。
- 某些游戏窗口录制支持可能不行，需要全屏后使用录制屏幕功能。
- 为了保证游戏全屏中也能录制键盘鼠标输入，需要启动时开启管理员权限。

If WGC is unavailable for the selected source, recorder falls back to the previous GDI path.
