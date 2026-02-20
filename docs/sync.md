# 同步与延迟测量

## 时间基准

系统使用单调时钟 `QueryPerformanceCounter`（QPC）作为内部统一时间轴。

- `qpc_ticks`: 当前计数
- `qpc_freq`: 每秒 tick 数

同时记录 UTC 锚点：

- `utc_anchor.qpc_ticks`
- `utc_anchor.utc_epoch_ns`

## QPC -> UTC 映射

转换公式：

```text
utc_ns = anchor_utc_ns + (qpc_ticks - anchor_qpc_ticks) * 1e9 / qpc_freq
```

## 启动关键时间戳

会话中独立记录：

- `screen_start_qpc`（录屏线程真正启动）
- `input_start_qpc`（hook 安装并开始记录）
- `qpc_freq`
- `utc_anchor`

## 两路数据对齐

1. 读取 `video_meta.json` 获取 `screen_start_qpc/qpc_freq`。
2. 读取 `input.jsonl` 首行 `session_header` 获取 `input_start_qpc/qpc_freq`。
3. 任意输入事件相对录屏启动时间：

```text
delta_s = (event_t_qpc - screen_start_qpc) / qpc_freq
```

4. 启动偏移（输入相对录屏）：

```text
start_offset_s = (input_start_qpc - screen_start_qpc) / qpc_freq
```

`start_offset_s > 0` 表示输入记录晚于录屏开始。

## 丢失计数

- 输入事件队列满时计入 `dropped_events`
- 录屏写帧失败计入 `dropped_frames`

两者用于评估长时录制稳定性与后处理可信度。
