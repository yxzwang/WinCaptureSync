
# WinCaptureSync

Low-latency synchronized screen recording + Keyboard/Mouse input logging (Windows).

> **⚠️ Important Tips:**
> - Some game windows may not support direct window capture; please use the **Full Screen** recording feature in such cases.
> - To ensure keyboard and mouse inputs are captured while in full-screen games, the application must be launched with **Administrator Privileges**.
> - The compiled `.exe` in the "Releases" section can be run directly.
> - Global hotkeys remain active in the background; you can minimize the GUI to avoid obscuring the screen during recording.

## Features & Scope

- **Global Hotkey:** One-key start/stop (Default: `Ctrl+Alt+F9`, configurable).
- **GUI Interface:** Source selection, window list, real-time preview, and manual controls.
- **Video Output:** `video.mp4` (H.264, via Windows Media Foundation).
- **Input Logging:** `input.jsonl` for keyboard, mouse, and gamepad input.
- **Metadata:** `video_meta.json` for session details.
- **Unified Timebase:** QPC (Query Performance Counter) mapped to UTC anchors for high-precision sync.
- **State Machine:** Robust flow: `Idle -> Arming -> Recording -> Stopping -> Idle`.
- **Efficient I/O:** Inputs are handled via a queue and background thread to prevent blocking the hook callback.

## Directory Structure

```text
src/
  main/      Entry point, hotkeys, state machine, configuration
  time/      QPC and UTC mapping logic
  input/     Low-level hooks, event queue, JSONL writer
  capture/   Screen capture logic and metadata
  encode/    Media Foundation H.264 encoding
docs/
  formats.md Output format specifications
  sync.md    Synchronization strategies
tests/
  time_tests.cpp

```

## Requirements

* Windows 10/11
* Visual Studio 2022 (with Desktop Development with C++ workload)
* Windows SDK (including Media Foundation)
* CMake 3.21+
* Ninja (Optional)

## Build Instructions

```powershell
cmake -S . -B build -G "Ninja"
cmake --build build --config Release
ctest --test-dir build --output-on-failure

```

## Usage

1. Run the executable:
```powershell
./build/WinCaptureSync.exe

```


2. Select your source in the GUI:
* `Primary Monitor Fullscreen`
* `Window` (Refresh the list to see active windows and check the real-time preview).


3. Toggle recording via the UI button or the global hotkey.

Outputs are saved to:
`./captures/<YYYYMMDD_HHMMSS>/`

* `video.mp4`
* `video_meta.json`
* `input.jsonl`

## Configuration

A `config.ini` file is automatically generated on the first launch. Example:

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

* `capture_width/height=0`: Defaults to primary monitor resolution.
* `hotkey_modifiers`: Supports `CTRL`, `ALT`, `SHIFT`, and `WIN` combinations.
* `hotkey_vk`: Supports `A-Z`, `0-9`, and `F1-F24`.

## Known Limitations

* The capture backend prioritizes WGC (falls back to GDI) + Media Foundation.
* Audio recording, pause/resume, and overlays are not yet implemented.

## Video Timeline Alignment

The project includes a utility script: `tools/align_timeline.ps1`, used to map `input.jsonl` event times to the video timeline.

### Usage

```powershell
# Align the most recent session
powershell -ExecutionPolicy Bypass -File tools/align_timeline.ps1

# Align a specific session directory
powershell -ExecutionPolicy Bypass -File tools/align_timeline.ps1 -SessionDir captures\20260217_212443

# Custom output prefix (Default: "aligned")
powershell -ExecutionPolicy Bypass -File tools/align_timeline.ps1 -SessionDir captures\20260217_212443 -OutputPrefix aligned_custom

```

### Output Files

The script generates the following in the session directory:

* `<prefix>_input.jsonl`: Original events with added alignment fields.
* `<prefix>_events.csv`: Tabular format for easy filtering/plotting.
* `<prefix>_report.json`: Alignment summary (including startup offsets).

### Key Alignment Fields

* `t_video_ms`: Event time relative to video start (`screen_start_qpc`).
* `t_input_ms`: Event time relative to input logging start (`input_start_qpc`).
* `t_utc_epoch_ns`: UTC nanosecond timestamp of the event.
* `input_minus_screen_ms`: Offset between input start and video start.

## Dual Window Recording

The Window/Monitor source mode supports recording two sources simultaneously.

* **Window 1:** Primary required source.
* **Window 2:** Optional secondary source (Select `<None>` to disable).
* **Output Files:**
* Window 1 -> `video.mp4` + `video_meta.json`
* Window 2 -> `video_2.mp4` + `video_2_meta.json` (Only if selected).



## Encoder Configuration

The video codec can be modified in `config.ini`:

```ini
capture_codec=h264

```

Supported values:

* `h264` (Default)
* `hevc`

**Note:** The output container remains `.mp4`. If no HEVC encoder is found on the system, the program will automatically fall back to H.264. The actual codec used is recorded in `video_meta.json` under `capture.codec`.

## Input Diagnostics

Enable raw input diagnostic mode in `config.ini`:

```ini
input_diagnostic_mode=1

```

When enabled, an additional file `input_diag.jsonl` is created per session, containing raw registration info and per-message diagnostic data. This is useful for debugging cases where specific games fail to send expected mouse events.

## Stability And Crash Diagnostics

The app now includes process-level crash diagnostics and runtime logs.

- Runtime log file: `./logs/wincapturesync_<timestamp>_<pid>.log`
- Crash dump file (on unhandled crash): `./logs/crash_<timestamp>_<pid>.dmp`

Logs record critical lifecycle stages and recorder start/stop failures.  
The dump file can be opened with Visual Studio or WinDbg for root-cause analysis.

## Gamepad Input Recording

Gamepad input is recorded through `XInput` and written into the same `input.jsonl`.

- Supports up to 4 controllers (`gamepad_index` 0-3).
- Records connection lifecycle:
  - `gamepad_connected`
  - `gamepad_disconnected`
- Records button events with readable control names:
  - `gamepad_button_down`
  - `gamepad_button_up`
- Records analog changes:
  - `gamepad_axis` for `left_trigger`, `right_trigger`, `left_stick_x`, `left_stick_y`, `right_stick_x`, `right_stick_y`

### Stick Axis Event Format

Left and right sticks are recorded as `gamepad_axis` events:

- Left stick: `left_stick_x`, `left_stick_y`
- Right stick: `right_stick_x`, `right_stick_y`

Fields:

- `type`: `gamepad_axis`
- `gamepad_index`: controller index (`0-3`)
- `packet`: XInput packet number
- `control`: axis name
- `value`: current axis value (`-32768` to `32767`, deadzone is normalized to `0`)
- `prev_value`: previous axis value
- `t_qpc`: event timestamp in QPC ticks

Example:

```json
{"type":"gamepad_axis","t_qpc":1234567890,"mods":{"shift":false,"ctrl":false,"alt":false,"win":false},"injected":false,"gamepad_index":0,"packet":2048,"control":"left_stick_x","value":13420,"prev_value":0}
{"type":"gamepad_axis","t_qpc":1234567902,"mods":{"shift":false,"ctrl":false,"alt":false,"win":false},"injected":false,"gamepad_index":0,"packet":2048,"control":"left_stick_y","value":-8421,"prev_value":-120}
{"type":"gamepad_axis","t_qpc":1234567920,"mods":{"shift":false,"ctrl":false,"alt":false,"win":false},"injected":false,"gamepad_index":0,"packet":2049,"control":"right_stick_x","value":22100,"prev_value":21980}
{"type":"gamepad_axis","t_qpc":1234567935,"mods":{"shift":false,"ctrl":false,"alt":false,"win":false},"injected":false,"gamepad_index":0,"packet":2049,"control":"right_stick_y","value":-30000,"prev_value":-29700}
```

## Capture Backends

The recorder prioritizes **Windows Graphics Capture (WGC)** for high-performance recording.

* Cursors are captured via the system compositor pipeline (`IsCursorCaptureEnabled=true`).
* Frames match the composited content on screen without needing application-level cursor re-rendering.
* The `capture.backend` field in `video_meta.json` will indicate `wgc` or `gdi`.
* If the selected source is incompatible with WGC, the recorder automatically falls back to the GDI path.
