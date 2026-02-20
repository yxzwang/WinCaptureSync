
# agents.md

## 项目名称

WinCaptureSync —— 低延迟同步录屏 + 键鼠输入记录（Windows）

## 项目目标

实现一个 Windows 桌面端工具：用户按下自定义全局快捷键后，同时开始：

1. **录屏**（屏幕视频）
2. **记录输入**：键盘输入、鼠标按键/滚轮、以及**鼠标移动距离/轨迹**

并且在启动时记录：

* 录屏开始的**具体时间戳**
* 键鼠记录开始的**具体时间戳**
  用于后续对齐与计算延迟差。

录屏结果与键鼠结果必须分别写入**不同文件**。系统要求：**Windows**，**延迟尽可能低**。

---

## 关键约束与优先级

### P0（必须满足）

* 自定义全局快捷键（支持用户配置）一键启动/停止。
* 录屏与键鼠记录**尽可能同时**开始，并分别记录各自的开始时间戳。
* 鼠标事件要包含：

  * 移动（至少包含每次移动的 dx/dy 或绝对坐标 + 计算出的距离）
  * 按键（左/右/中/侧键等）
  * 滚轮（垂直/水平如可行）
* 键盘事件要包含：按下/抬起、虚拟键码/扫描码、修饰键状态。
* 两类输出分别落盘：例如 `capture.mp4` + `input.jsonl`（或其他约定格式）。
* 尽可能低延迟：录屏用系统级捕获 API，输入用低层 hook，避免轮询。

### P1（强烈建议）

* 高精度统一时间基准：使用 **QueryPerformanceCounter (QPC)** 作为内部单调时钟；同时记录与 **UTC（系统时间）** 的映射，便于跨进程/跨文件对齐。
* 支持选择录制显示器/窗口/区域（至少支持“主显示器全屏”）。
* 录制过程中写文件不能造成明显掉帧：事件记录采用缓冲队列 + 后台写盘。

### P2（可选）

* 支持覆盖层提示（录制中红点/计时）。
* 支持暂停/恢复。
* 支持自动分段文件、压缩、或生成对齐报告。

---

## 推荐技术路线（建议按此实现）

### 录屏

优先使用 **Windows Graphics Capture (WGC)**（Win10 1903+）或 Desktop Duplication（兼容旧系统）。

* 目标：低延迟、高帧率、对 GPU 友好。
* 编码：建议 H.264（硬件优先：Media Foundation / D3D11 + MFT / 或 FFmpeg 作为可选后端）。
* 记录每个视频帧的时间戳（如果可获得帧到达时间/捕获时间，保留在 sidecar 元数据文件也行）。

### 键鼠记录

使用 **Low-level hooks**：

* `WH_KEYBOARD_LL` + `WH_MOUSE_LL`
* 记录事件发生的时间：用 QPC 采样，避免依赖 `GetMessageTime` 之类的低精度/不一致来源。
* 鼠标移动：

  * 记录绝对屏幕坐标 (x,y)
  * 同时计算 dx/dy 与累计距离（欧氏距离或曼哈顿距离二选一，建议欧氏距离；都可记录）

### 快捷键

* 使用 `RegisterHotKey` 注册全局热键（可配置：例如 `Ctrl+Alt+F9`）。
* 热键触发后：严格按“**同一个控制线程**”顺序启动两个子系统，尽量减少抖动。

---

## 时间戳与对齐策略（必须严格实现）

### 时间基准

* 内部时间：`qpc_ticks`（QueryPerformanceCounter）
* 频率：`qpc_freq`（QueryPerformanceFrequency）
* 同时记录：`utc_epoch_ns`（或 `FILETIME`）与采样时刻的 `qpc_ticks`，用于把 qpc 映射到 UTC。

### 启动时必须写入的字段

在开始录制瞬间（尽量同一时刻采样）写入：

* `screen_start_qpc`
* `input_start_qpc`
* `screen_start_utc`（可选但建议）
* `input_start_utc`（可选但建议）
* `qpc_freq`
* 版本、系统信息（Windows 版本、DPI、屏幕分辨率、录制参数）

> 重要：录屏模块与输入模块各自“真正开始”的时刻要独立采样；不要只在总控层写一个时间戳就假设同步。

---

## 输出文件设计（建议）

### 1) 录屏文件

* `video.mp4`（H.264/AAC；音频可先不做）
* 同目录生成：`video_meta.json`

  * 包含录制参数（分辨率、fps、编码、比特率）
  * 包含 `screen_start_qpc` / `qpc_freq` 等关键字段
  * （可选）每帧时间戳列表（如果实现成本过高可先不做）

### 2) 输入记录文件

建议使用 **JSON Lines**（一行一个事件，便于流式写入）：

* `input.jsonl`

每条事件字段建议：

* `t_qpc`：事件发生时 QPC tick
* `type`：`key_down | key_up | mouse_move | mouse_down | mouse_up | wheel`
* 键盘字段：`vk`, `scan`, `flags`, `is_extended`, `mods`
* 鼠标字段：`x`, `y`, `dx`, `dy`, `distance`, `button`, `wheel_delta`
* （可选）`injected` 标记（区分系统注入输入）

并在文件头（第一行）写一个 `session_header` 事件，包含：

* `input_start_qpc`
* `qpc_freq`
* `utc_anchor`（qpc->utc 映射信息）
* 屏幕信息（分辨率、缩放、虚拟屏幕边界）

---

## 性能与低延迟要求（实现细则）

* Hook 回调里禁止做重 IO / 重计算：

  * 回调仅做：采样 QPC、复制必要字段、推入 lock-free/轻量队列。
  * 后台线程负责批量写盘（例如每 5~20ms flush，或达到 N 条事件）。
* 录屏线程与编码线程分离，避免编码阻塞采集。
* 尽量减少跨线程同步点：用事件/信号量，不用频繁锁竞争。
* 停止录制时保证：

  * 输入文件 flush 完成
  * 视频封装正常结束（mp4 moov 写入等）
  * 会话元数据完整落盘

---

## 目录结构建议

* `/src`

  * `main`（入口、热键、状态机）
  * `capture`（WGC/DXGI 捕获）
  * `encode`（Media Foundation/FFmpeg 后端）
  * `input`（hook、事件结构、队列、writer）
  * `time`（QPC、UTC anchor、转换工具）
* `/docs`

  * `formats.md`（事件格式与字段定义）
  * `sync.md`（对齐与延迟测量方法）
* `/tests`

  * 单元测试（时间转换、事件序列写入）
  * 简单集成测试（启动/停止、文件存在性）

---

## 状态机（建议实现）

* `Idle`
* `Arming`（按下热键 -> 初始化资源）
* `Recording`
* `Stopping`（停止热键 -> flush/close）
* `Error`（错误处理与资源回收）

热键行为：

* 第一次按下：从 Idle -> Recording
* 再次按下：Recording -> Stopping -> Idle
  （可选）支持两个热键：Start/Stop 分开。

---

## 验收标准（Definition of Done）

* 在 Windows 10/11 上可运行。
* 按下热键后，生成：

  * `video.mp4`
  * `video_meta.json`
  * `input.jsonl`
* `video_meta.json` 与 `input.jsonl` 头部都包含 `qpc_freq` 与各自 `*_start_qpc`。
* `input.jsonl` 中鼠标移动事件包含可用于计算“移动距离”的信息（dx/dy 或 distance）。
* 连续录制 5 分钟不崩溃、不卡死；事件不丢失到不可用程度（允许极少量丢失但必须记录丢失计数）。
* 停止录制后文件可正常播放/解析。

---

## 开发任务拆解（Agent 应按顺序完成）

1. 建立工程骨架（C++ 或 C#；优先 C++ 以利 WGC + hook + 性能）
2. 实现时间模块：QPC、频率、UTC anchor
3. 实现输入 hook + 事件队列 + JSONL writer
4. 实现全局热键 + 状态机
5. 实现录屏捕获（WGC 优先）+ 基础编码输出 mp4
6. 实现元数据文件写入（含 start timestamps）
7. 集成启动同步：热键触发时尽量同一控制流启动两模块，并记录各自 start
8. 测试与性能调优（减少回调阻塞、控制 flush 策略）
9. 文档补齐：formats.md、sync.md、build/run 说明

---

## 风险与注意事项

* WGC 可用性与系统版本有关；需要在启动时检测并回退到 Desktop Duplication（如支持）。
* Low-level hook 运行在消息循环线程；必须有稳定的 message pump。
* DPI 缩放：鼠标坐标与屏幕捕获坐标可能存在缩放差异，需在元数据中记录 DPI 与虚拟屏幕边界，便于后处理校正。
* MP4 封装需要正确结束写入，否则文件可能不可播放；Stop 时必须等待封装完成。

---

## 构建与运行（写在 README 的要点）

* 目标：Windows 10/11
* 依赖：Windows SDK、Media Foundation（或可选 FFmpeg）
* 运行：启动后后台驻留，热键控制开始/停止
* 输出：默认在可执行文件同目录 `./captures/<timestamp>/...`，或支持参数指定目录

