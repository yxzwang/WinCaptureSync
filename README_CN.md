# WinCaptureSync

低延迟同步录屏 + 键鼠输入记录（Windows）。

<font color="red"> 重要tips: </font>
- 某些游戏窗口录制支持可能不行，需要全屏后使用录制屏幕功能。
- 为了保证游戏全屏中也能录制键盘鼠标输入，需要启动时开启<font color="red">管理员权限</font>。
- 已经 release 直接使用的 exe 程序可直接运行。
- 快捷键后台有效，可以最小化 GUI 界面避免录屏时遮挡。

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

* Windows 10/11
* Visual Studio 2022 (含 C++ 桌面开发工具集)
* Windows SDK（包含 Media Foundation）
* CMake 3.21+
* Ninja（可选）

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

* `主显示器全屏`
* `窗口`（可刷新窗口列表并在预览区实时查看）

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

* `capture_width/height=0` 表示主屏全屏
* `hotkey_modifiers` 支持 `CTRL/ALT/SHIFT/WIN` 组合
* `hotkey_vk` 支持 `A-Z`、`0-9`、`F1-F24`

## 已知限制

* 当前录屏后端优先使用 WGC（不可用时回退到 GDI）+ Media Foundation。
* 暂未实现音频、暂停恢复、覆盖层。

## 视频时间轴对齐

项目内置了时间轴对齐脚本：`tools/align_timeline.ps1`，用于把 `input.jsonl` 事件时间转换到视频时间轴。

### 用法

```powershell
# 对齐最新一次会话
powershell -ExecutionPolicy Bypass -File tools/align_timeline.ps1

# 对齐指定会话目录
powershell -ExecutionPolicy Bypass -File tools/align_timeline.ps1 -SessionDir captures\20260217_212443

# 自定义输出前缀（默认 aligned）
powershell -ExecutionPolicy Bypass -File tools/align_timeline.ps1 -SessionDir captures\20260217_212443 -OutputPrefix aligned_custom

```

### 输出文件

脚本会在会话目录下生成：

* `<prefix>_input.jsonl`：原输入事件 + 对齐字段
* `<prefix>_events.csv`：便于筛选/作图的表格
* `<prefix>_report.json`：对齐摘要（包含启动偏差）

### 关键对齐字段

* `t_video_ms`：事件相对视频起点（`screen_start_qpc`）的毫秒时间
* `t_input_ms`：事件相对输入起点（`input_start_qpc`）的毫秒时间
* `t_utc_epoch_ns`：事件对应的 UTC 纳秒时间戳
* `input_minus_screen_ms`：输入启动相对视频启动的偏差（毫秒）

## 双窗口录制

窗口/显示器源模式支持同时录制两个源。

* `窗口 1`：主要的必需源（窗口或显示器）
* `窗口 2`：可选的第二个源（窗口或显示器，选择 `<None>` 则禁用）
* 输出文件：
* `窗口 1` -> `video.mp4` + `video_meta.json`
* `窗口 2` -> `video_2.mp4` + `video_2_meta.json`（仅在选择后生成）



每个元数据文件包含：

* `capture.source_type`
* `capture.source_hwnd`
* `screen_start_qpc`, `input_start_qpc`, `qpc_freq`

## 编码器配置

视频编码器可以通过 `config.ini` 进行配置：

```ini
capture_codec=h264

```

支持的值：

* `h264`（默认）
* `hevc`

注意：

* 输出容器统一保持为 `.mp4`
* 如果系统中没有可用的 HEVC 编码器，程序会自动回退到 H.264
* `video_meta.json` 会记录实际使用的编码器于 `capture.codec` 字段

## 输入诊断模式

可以通过 `config.ini` 开启原始输入诊断模式：

```ini
input_diagnostic_mode=1

```

开启后，每个会话还会额外写入：

* `input_diag.jsonl`：记录原始输入注册信息及每条消息的诊断数据

此文件主要用于调试某些特定游戏不发送预期鼠标事件的情况。

## 视频捕获后端

录制器现在优先使用 **Windows Graphics Capture (WGC)** 进行单窗口或单显示器录制。

* 光标由系统合成器流水线捕获（`IsCursorCaptureEnabled=true`）
* 捕获的帧与屏幕上的合成内容一致，无需应用层重新绘制光标
* `video_meta.json` 包含 `capture.backend` 字段（`wgc` 或 `gdi`）
* 默认使用 WGC 录制能够正常录制到鼠标。

如果选定的源不支持 WGC，录制器将自动回退到之前的 GDI 路径。
