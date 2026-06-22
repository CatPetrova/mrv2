# Alt+Left/Right Seek 性能优化

## 问题描述

在 mrv2 中，`Alt+Left` / `Alt+Right` 快捷键实现了向后/向前 seek 2 秒（`seekRelativeSeconds(±2.0)`）。
按住或连续按下时，**画面经常要等 1~2 秒才会动**，明显不如 IINA 那样"跟随手"。

### 触发链路

```
Alt+Left/Right
 └─ TimelineViewportEvents.cpp:1923/1937   kFrameStep2SecondsFwd/Back
     └─ TimelinePlayer::seekRelativeSeconds(±2.0)        mrvTimelinePlayer.cpp:389
         └─ (1/30s 去抖 timeout) → applyPendingRelativeSeek → seek(time)
             └─ tlRender Player::seek                     Player.cpp:642
                 ├─ clearRequests = true   (取消进行中的预读请求)
                 └─ resetAudioTime()
```

## 根因分析

### 瓶颈 1：未命中缓存时画面干等不动

`Player::updateVideoData()`（`tlRender/lib/tlTimeline/Player.cpp:404`）只在目标帧命中
`videoDataCache` 时才更新画面；未命中时**完全不更新** `currentVideoData`，画面卡在旧帧
不动，干等后台 IO 线程把目标帧解码进缓存、下一个 tick 才显示：

```cpp
const auto i = p.thread.videoDataCache.find(p.thread.currentTime);
if (i != p.thread.videoDataCache.end())
    p.mutex.currentVideoData = i->second;   // 命中→立即显示
else if (playback != Stop) { ... }          // 未命中→不更新画面
else { /* 未命中 + Stop → 不更新画面，干等 */ }
```

这是"等 1~2 秒画面才动"的直接原因：seek 后目标帧不在缓存里，`currentVideoData`
保持旧值，UI 不知道"已经响应、正在解码"，体感像卡死。

### 瓶颈 2：后向缓存窗口太小，向后跳必然未命中

`Player::Private::cacheUpdate()`（`tlRender/lib/tlTimeline/PlayerPrivate.cpp:281`）只缓存
一个窄窗口，默认值来自 `PlayerCacheOptions`（`PlayerOptions.h:40,43`）：

```cpp
readAhead  = RationalTime(5.0, 1.0)   // 前方 5 秒
readBehind = RationalTime(0.5, 1.0)   // 后方仅 0.5 秒
videoRange = [currentTime - readBehind, currentTime + readAhead]
```

- **Alt+Left 向后 2 秒**：`2.0s > readBehind(0.5s)` → 目标帧**必定不在缓存** → 触发重新解码。
- **Alt+Right 向前 2 秒**：理论在 `readAhead(5s)` 内，但若刚停下播放、缓存未预填到 +2s，
  或 seek 时 `clearRequests` 取消了正在向前的预读，同样未命中。

`SettingsObject.cpp:123-124` 用 `PlayerCacheOptions()` 默认值（readBehind=0.5s）作为用户
默认设置；`App::cacheUpdate()`（`mrvApp.cpp:2511`）对长片/多音轨/NDI 还会把 readBehind
压到 `0`，进一步恶化向后跳。

### 瓶颈 3（次因）：FFmpeg 层 seek 丢帧 + 关键帧重解 GOP

缓存未命中后落到 `FFmpegReadVideo::seek`（`tlRender/lib/tlIO/FFmpegReadVideo.cpp:1222`）：

```cpp
avcodec_flush_buffers(_avCodecContext[_avStream]);   // 丢弃解码器内所有帧
av_seek_frame(..., AVSEEK_FLAG_BACKWARD);            // 定位到目标之前最近的关键帧
_buffer.clear();                                     // 丢弃已读 packet
```

随后 `process()` 从该关键帧逐帧解码到目标帧。对 H.264/H.265 大 GOP 视频（keyint 常见
48~250 帧），要解码整个 GOP 才能显示目标帧——延迟 ≈ 一个 GOP 的解码耗时，1~2 秒完全吻合。

### 与 IINA 的差距

IINA 底层是 mpv，优势在于：
1. demuxer 压缩包缓存 + 解码帧缓存都远大于 mrv2 的 0.5s/5s 窗口，seek 到已缓存区域几乎瞬时；
2. mpv 对精确 seek 有 frame-drop 与解码调度优化，且**不在每次 seek 时 flush 全部已解码帧**；
3. mrv2/tlRender 每次 FFmpeg seek 都 `avcodec_flush_buffers` + `_buffer.clear()`，**无跨 seek
   的帧复用**，向后跳每次都从零重解 GOP。

## 解决方案

采用性价比最高的三步组合（思路 C-1 + A-1 + B-3/C-2），目标：命中缓存时瞬时，未命中时
也有即时反馈且只等单帧而非整个 GOP。

### 改动 1（C-1）：未命中缓存时给即时反馈，而非画面干等

`Player::updateVideoData()` 未命中 + Stop 分支当前不更新 `currentVideoData`。改为：未命中时
**保持旧帧**（不闪黑），并标记一个 `seekPending` 状态，供 UI/viewport 在 redraw 时叠加
"loading" 提示，让用户知道"已响应、正在解码"，而非以为卡死。命中后自动清除该标记。

这独立于缓存大小，立即消除"卡死"观感。

### 改动 2（A-1）：提高默认 readBehind 到 2 秒

`SettingsObject.cpp:123-124` 将默认 `Cache/ReadBehind` 从 `PlayerCacheOptions()` 默认值
（0.5s）改为 `2.0`，使 Alt+Left 向后 2 秒落入后向缓存窗口，直接命中缓存、无需 FFmpeg seek。

### 改动 3（B-3/C-2）：seek 后插队解码目标帧

`Player::seek` 设 `clearRequests=true` 后，新位置的目标帧要排队等预读线程按方向线性补满
窗口才轮到。改为：seek 时记录 `seekTime`，`cacheUpdate` 把目标帧单帧作为最高优先级请求
插到队列最前，先解出这一帧显示，再慢慢补周围——把未命中最坏情况从"等整个 GOP 窗口"降到
"等单帧解码"。

## 已知遗留 / 后续

- 思路 D（FFmpeg 层 `avformat_seek_file` 精确 seek + 保留最近 GOP 解码帧做 LRU 复用）治本
  但工作量大，作为后续优化。
- 思路 A-2（浏览模式动态切换 readAhead/Behind）可进一步平衡内存。
