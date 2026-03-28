# 输出格式说明

## 1) `video.mp4`

- 容器：MP4
- 视频编码：H.264（Media Foundation Sink Writer）
- 默认帧率：60 FPS（可配）
- 默认码率：12 Mbps（可配）

## 2) `video_meta.json`

示例字段：

```json
{
  "version": 1,
  "video_file": "video.mp4",
  "capture": {
    "width": 1920,
    "height": 1080,
    "fps": 60,
    "bitrate": 12000000,
    "backend": "wgc",
    "source_type": "window",
    "source_hwnd": 1311768465173141112
  },
  "screen_start_qpc": 1234567890,
  "input_start_qpc": 1234567901,
  "screen_start_utc_epoch_ns": 1700000000000000000,
  "input_start_utc_epoch_ns": 1700000000000010000,
  "qpc_freq": 10000000,
  "utc_anchor": {
    "qpc_ticks": 1234500000,
    "utc_epoch_ns": 1700000000000000000
  },
  "stats": {
    "written_frames": 600,
    "dropped_frames": 0
  }
}
```

## 3) `input.jsonl`

每行一个 JSON 事件。第一行是 `session_header`。

### `session_header`

```json
{
  "type": "session_header",
  "input_start_qpc": 1234567901,
  "qpc_freq": 10000000,
  "utc_anchor": {
    "qpc_ticks": 1234500000,
    "utc_epoch_ns": 1700000000000000000
  },
  "screen": {
    "width": 1920,
    "height": 1080,
    "virtual_left": 0,
    "virtual_top": 0,
    "virtual_width": 1920,
    "virtual_height": 1080,
    "dpi": 96,
    "dpi_scale": 1.0
  }
}
```

### 键盘事件

- `type`: `key_down | key_up`
- `t_qpc`: 事件 QPC tick
- `vk`, `scan`, `flags`, `is_extended`
- `mods`: `shift/ctrl/alt/win`
- `injected`: 是否注入事件

### 鼠标事件

- 移动：`type=mouse_move`，字段 `x/y/dx/dy/distance`
- 按键：`type=mouse_down | mouse_up`，字段 `button`
- 滚轮：`type=wheel`，字段 `wheel_delta`，`button=vertical|horizontal`

### 统计事件

停止时追加：

```json
{
  "type": "stats",
  "t_qpc": 1239999999,
  "mods": {"shift": false, "ctrl": false, "alt": false, "win": false},
  "injected": false,
  "dropped_events": 0
}
```

### Gamepad Events

The same `input.jsonl` stream also contains gamepad events (XInput).

- `type=gamepad_connected | gamepad_disconnected`
  - `gamepad_index`
  - `packet`
- `type=gamepad_button_down | gamepad_button_up`
  - `gamepad_index`
  - `packet`
  - `control` (readable name, e.g. `a`, `b`, `left_shoulder`, `dpad_up`)
- `type=gamepad_axis`
  - `gamepad_index`
  - `packet`
  - `control` (`left_trigger`, `right_trigger`, `left_stick_x`, `left_stick_y`, `right_stick_x`, `right_stick_y`)
  - `value`
  - `prev_value`

## Dual Window Separate Outputs

When dual-window recording is enabled, files are split by source:

- `video.mp4` + `video_meta.json` for `Window 1`
- `video_2.mp4` + `video_2_meta.json` for `Window 2`

Each `*_meta.json` keeps the same schema and describes only its own source.

For monitor sources, metadata also includes:

- `capture.source_type`: `primary_monitor` or `monitor`
- `capture.source_hmonitor`: monitor handle value
- `capture.source_rect`: monitor bounds in virtual screen coordinates

Codec field:

- `capture.codec`: `h264` or `hevc` (actual encoder used)

## Mouse Signal In Games

For fullscreen/locked-cursor games, `mouse_move` is captured from raw input movement signal.

- `dx` / `dy`: relative movement signal per event
- `distance`: Euclidean distance computed from `dx` / `dy`
- `x` / `y`: integrated signal coordinates (not OS cursor absolute position in locked-cursor mode)
- Input capture is raw-input-only (`WM_INPUT`) for keyboard and mouse events
- Low-level hook paths are intentionally disabled to avoid cursor recenter jitter contamination

## Diagnostic Output (Optional)

If `input_diagnostic_mode=1`, an additional `input_diag.jsonl` is generated.

- `diag_info`: initialization and raw-input registration status
- `diag_raw_keyboard`: per raw keyboard packet summary
- `diag_raw_mouse`: per raw mouse packet summary (`us_flags`, `button_flags`, `last_x`, `last_y`)
- `diag_raw_error`: raw-input data read failures
- `diag_summary`: aggregate packet counters for quick root-cause analysis
