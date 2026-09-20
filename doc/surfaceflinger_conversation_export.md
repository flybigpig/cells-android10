# SurfaceFlinger 源码解析 · 会话历史导出

> 仓库路径：`d:/framework/cells-android10/`
> 分析对象：Android 10 SurfaceFlinger（`frameworks/native/services/surfaceflinger` 与 `frameworks/native/libs/gui`）
> 导出时间：2026-08-23

---

## 目录

1. SurfaceFlinger 各个组件解析
2. BufferQueue 机制
3. Scheduler 与 MessageQueue
4. App / SF 刷新信号驱动
5. BufferQueueProducer::queueBuffer 核心分析
6. queueBuffer 中的 mMutex 互斥锁
7. App.draw → SurfaceFlinger.refresh 全链路
8. EventThread 的 VSync 分发与 requestNextVsync 的 pending 机制
9. BitTube 传输通道
10. 一帧渲染流水线核对（10 步表验证）

---

## 1. SurfaceFlinger 各个组件解析

SurfaceFlinger 是 Android 图形系统的核心合成服务，主要组件如下：

- **SurfaceFlinger（核心）**：继承 `BnSurfaceComposer`，运行在独立进程，接收各 App 的 `Surface` 事务，按 Z-order 合成各 Layer 并送显。
- **Client**：每个 App 进程通过 `SurfaceComposerClient` 在 SF 侧对应一个 `Client` 对象，负责创建 `Surface`（即 Layer）。
- **Layer 体系**：
  - `BufferLayer`：持有 `BufferQueue`，最常见的 UI 图层。
  - `BufferQueueLayer`：由 BufferQueue 驱动的图层。
  - `BufferStateLayer`：由客户端直接 setBuffer 的图层。
  - `ColorLayer` / `ContainerLayer`：纯色 / 容器图层。
- **DisplayDevice**：抽象一块物理或虚拟显示设备（含 `DisplaySurface`、HWC 连接）。
- **HWComposer（HWC）**：封装 HAL `ComposerHal`，负责硬件叠加与 VSync 信号源。
- **CompositionEngine**：Android 10 引入，承担"如何合成"的决策（planes 分配、RenderEngine 回退）。
- **Scheduler**：调度 VSync，内含 `EventThread`（App / SF 各一）、`DispSync`、`MessageQueue`。
- **Effects**：如 blur、dimming 等特效层。

入口：`frameworks/native/services/surfaceflinger/main_surfaceflinger.cpp`
`startGraphicsAllocatorService → createSurfaceFlinger → init → addService → run`

---

## 2. BufferQueue 机制

BufferQueue 是 App（生产者）与 SF（消费者）之间传递 `GraphicBuffer` 的桥梁。

- **BufferQueueCore（mCore）**：共享状态中心，保存 `BufferSlot[64]` 数组与队列。
- **BufferQueueProducer / BufferQueueConsumer**：分别挂在生产、消费端。
- **BufferSlot 状态机**：
  - `FREE` → `DEQUEUED`（生产者拿到）→ `QUEUED`（生产者填充完）→ `ACQUIRED`（消费者拿到）→ `FREE`。
  - 另有 `SHARED` 用于跨进程共享。
- **Fence 同步**：`acquireFence` / `releaseFence` 标记 buffer 何时可被 GPU/CPU 安全访问。
- **异步模式（async）**：`ST_FLAG`（非阻塞）下若队列满可直接丢弃最旧帧（`droppable`）。
- **同进程优化**：当 `consumerIsSurfaceFlinger` 时跳过 Binder 跨进程开销。

关键文件：
- `frameworks/native/libs/gui/BufferQueue.h/.cpp`
- `BufferQueueCore.h`、`BufferQueueProducer.h/.cpp`、`BufferQueueConsumer.h`
- `BufferSlot.h`、`BufferItem.h`、`BufferQueueDefs.h`

---

## 3. Scheduler 与 MessageQueue

- **MessageQueue** 是 SF 主线程的消息泵，处理两类事件：
  - `INVALIDATE`：触发事务/可见性更新与重合成。
  - `REFRESH`：执行实际合成并送显。
- 内部基于 **Looper + BitTube fd**：VSync 脉冲经 BitTube 唤醒 `eventReceiver()`。
- **原子掩码去重**：`dispatchInvalidate` 用 `atomic` mask 防止重复排队。
- 调用链：`MessageQueue::invalidate()`（cpp:158）→ `eventReceiver`（:171）→ `setEventThread`（:94）→ `waitMessage`（:126）。

关键文件：
- `frameworks/native/services/surfaceflinger/Scheduler/Scheduler.h`
- `Scheduler/DispSync.h`
- `Scheduler/MessageQueue.h/.cpp`

---

## 4. App / SF 刷新信号驱动

VSync 信号的来源与分发：

- **DispSync**：基于硬件 VSync（来自 HWC）做软件相位对齐，输出**单一相位**信号。
- **DispSyncSource + EventThread（两个实例）**：
  - `app` EventThread：`phaseOffset = 0`（或较小），驱动 App 渲染。
  - `sf`  EventThread：`phaseOffset` 较大（如 1 帧偏移），驱动 SF 合成。
- **PhaseOffsets**：通过 `SurfaceFlingerProperties` 配置两路相位差。
- **requestNextVsync 的 pending 机制**（见第 8 节）。
- **ResyncCallback**：当屏幕刷新率变化时重新对齐 DispSync。

> 注意：DispSync **不是**直接发射"VSync-app / VSync-sf"两路，而是两个 EventThread 各配一个带不同 `phaseOffset` 的 `DispSyncSource`。

---

## 5. BufferQueueProducer::queueBuffer 核心分析

入口：`frameworks/native/libs/gui/BufferQueueProducer.cpp :: queueBuffer()`（cpp:763–1033）

核心流程：

1. **加锁**：`std::lock_guard<std::mutex> lock(mCore->mMutex);`（:807）保护共享状态。
2. **校验 slot / fence**：确认该 slot 处于 `DEQUEUED`。
3. **判断可丢弃帧（droppable）**：async 模式下若 `mCore->mQueue` 已满且当前帧可丢弃（:888–891）。
4. **入队分支**（:908–955）：
   - 正常：`mQueue.push` 将 slot 状态置 `QUEUED`。
   - 丢弃：复用最旧 `QUEUED` buffer。
5. **触发消费者唤醒**：`onFrameAvailable` 回调（通过 `mCore->mConsumerListener`）通知 SF。
6. **EGL 限流**：`waitForever`（:1025–1030）等待消费者释放 buffer，防止生产者过快。
7. 返回 `QUEUE_OK` 给 App。

相邻方法：`dequeueBuffer()` 在 cpp:356。

---

## 6. queueBuffer 中的 mMutex 互斥锁

```cpp
std::lock_guard<std::mutex> lock(mCore->mMutex);  // BufferQueueProducer.cpp:807
```

- **保护对象**：`mCore`（即 `BufferQueueCore`），内含 `mSlots[64]`、`mQueue`、`mFreeBuffers`、`mActiveBuffers` 等。
- **为什么必须加锁**：
  - 生产者（App 线程）与消费者（SF 主线程，经 `BufferQueueConsumer`）并发访问同一份 `BufferSlot`。
  - 不加锁会导致 slot 状态机错乱（如 double-free、越界入队）。
- **锁粒度**：仅覆盖"修改共享状态 + 入队"的临界区，唤醒回调前解锁，避免回调中死锁。
- **RAII 风格**：`lock_guard` 析构自动解锁，异常安全。

---

## 7. App.draw → SurfaceFlinger.refresh 全链路

```
App: dequeueBuffer → 绘制(Skia/HWUI/GL) → queueBuffer
        │ (Binder)
        ▼
SF: onFrameAvailable → signalTransaction/invalidate
        │ (预留 NEXT VSync)
        ▼
Scheduler: DispSync → EventThread → BitTube → MessageQueue
        ▼
SF 主线程: onMessageReceived(INVALIDATE) → handleMessageTransaction
        → handleMessageInvalidate (acquireBuffer)
        → handleMessageRefresh (HWC prepare)
        → CompositionEngine::present
        → HWComposer::presentAndGetReleaseFences
        ▼
HAL: 面板翻转 framebuffer → 回传 release fence → buffer 回收复用
```

关键点：
- `queueBuffer` 后并非立即合成，而是**登记事务并等待下一个 VSync** 触发 `INVALIDATE`。
- `acquireBuffer` 在 `BufferLayerConsumer::updateTexImage()` 内完成（非 `acquireNextBuffer`）。

---

## 8. EventThread 的 VSync 分发与 requestNextVsync 的 pending 机制

文件：`frameworks/native/services/surfaceflinger/Scheduler/EventThread.cpp`

- **分发路径**：`onVSyncEvent()`（:290）→ `threadMain()`（:312）→ 通过 `BitTube` 写给各 `Connection`。
- **状态机**：`Idle` / `VSync` / `SyntheticVSync`。
- **createEventConnection()**（:212）：App/SF 各自建立 `Connection`，持有独立 `BitTube`。
- **requestNextVsync()**（:257）：客户端请求"下一次 VSync"。三种策略：
  - `Single`：只发一次脉冲。
  - `Periodic`：持续每帧发（如动画）。
  - `None`：不主动发。
- **pending 机制**：当 `requestNextVsync` 在两次 VSync 之间到达时，EventThread 将其记为 `pending`，待下一个 `onVSyncEvent` 到来时统一派发，避免丢帧。
- **shouldConsumeEvent()**（:405）：决定是否向某 Connection 投递该次 VSync。

---

## 9. BitTube 传输通道

文件：`frameworks/native/libs/gui/BitTube.cpp/.h`

- **本质**：一对 `AF_UNIX`（`SOCK_SEQPACKET`）`socketpair`，单向/双向传递小数据包（VSync 事件）。
- **缓冲**：默认 4KB，足以承载 `VSyncEvent` Parcel。
- **关键 API**：
  - `init()`（:47–64）：创建 socketpair。
  - `sendObjects()` / `recvObjects()`：序列化对象收发。
  - `stealReceiveChannel()`：消费者接管读端 fd。
  - `writeToParcel()`（:113–130）：将 fd `dup` 跨 Binder 传递给对端（Parcelable fd）。
- **在 SF 中的角色**：EventThread 写端 → MessageQueue 读端，唤醒 SF 主线程处理 `INVALIDATE`/`REFRESH`。

---

## 10. 一帧渲染流水线核对（10 步表验证）

用户提供的 10 步表与源码核对结果：

| 步 | 调用方 → 被调方 | 用户方法 / 路径 | 核对 |
|----|----------------|----------------|------|
| ① | App → BufferQueue | `BufferQueueProducer.cpp::dequeueBuffer()` | ✅ 正确 |
| ② | App 本地 | Skia/HWUI/GL 渲染 | ✅ 正确 |
| ③ | App → SF | `BufferQueueProducer::queueBuffer()` (Binder) | ⚠️ 修正：`queueBuffer` 后通过 `onFrameAvailable` 唤醒，SF 仅**登记事务并预留 NEXT VSync**，并非直接合成 |
| ④ | HWC/Queue → SF | `SurfaceFlinger.cpp::onMessageInvalidate()/acquireNextBuffer()` | ❌ 错误：无 `onMessageInvalidate`；实际为 `onMessageReceived(INVALIDATE)` → `handleMessageInvalidate()` → `BufferLayerConsumer::updateTexImage()` → `acquireBuffer()` |
| ⑤ | SF 本地 | `SurfaceFlinger.cpp::handleMessageRefresh()` | ⚠️ 部分正确：脏区/可见性/Z-order 计算在 `rebuildLayerStacks()` 中 |
| ⑥ | Scheduler → App/SF | `DispSync.cpp` | ❌ 概念错误：DispSync 单一相位；App/SF 两路来自**两个 EventThread**（各自 `DispSyncSource(phaseOffset)`） |
| ⑦ | SF → CE | `CompositionEngine.cpp::present()` | ✅ 正确 |
| ⑧ | CE → HWC | `HWC2.cpp::set()/presentDisplay()` | ⚠️ 更准确路径：`HWComposer.cpp::presentAndGetReleaseFences()` |
| ⑨ | HWC → Panel | Hardware Composer HAL | ✅ 正确 |
| ⑩ | Panel → SF | `presentAndGetReleaseFence()` | ⚠️ 应为复数 `presentAndGetReleaseFences()` |

### 修正后的 12 步完整链路

1. `dequeueBuffer`：App 申请可写 buffer。
2. App 本地渲染（Skia/HWUI/GL）。
3. `queueBuffer` → `onFrameAvailable` → SF 登记事务并预留 NEXT VSync。
4. DispSync（单相位）→ 两个 EventThread（app/sf，不同 phaseOffset）。
5. EventThread → BitTube → MessageQueue → 唤醒 SF 主线程。
6. `onMessageReceived(INVALIDATE)` → `handleMessageTransaction`。
7. `handleMessageInvalidate` → `acquireBuffer`（取最新 buffer）。
8. `handleMessageRefresh` → HWC `prepare`（layers 决策）。
9. `CompositionEngine::present()`。
10. `HWComposer::presentAndGetReleaseFences()`。
11. HAL 翻转 framebuffer（面板显示）。
12. 回传 present fence + release fence → buffer 回收复用。

---

## 附：关键源码路径索引

```
frameworks/native/services/surfaceflinger/
  main_surfaceflinger.cpp
  SurfaceFlinger.h / .cpp          (onMessageReceived cpp:1743)
  Scheduler/
    Scheduler.h
    DispSync.h
    EventThread.cpp
    MessageQueue.h / .cpp
  DisplayHardware/HWComposer.h / .cpp
  BufferLayer.cpp / BufferLayerConsumer.h
  CompositionEngine/src/CompositionEngine.cpp

frameworks/native/libs/gui/
  BufferQueue.h / .cpp
  BufferQueueCore.h
  BufferQueueProducer.h / .cpp      (queueBuffer cpp:763, dequeueBuffer cpp:356)
  BufferQueueConsumer.h
  BufferSlot.h / BufferItem.h / BufferQueueDefs.h
  BitTube.cpp / .h
```

---

*本文件由会话历史导出整理，所有结论均基于 `d:/framework/cells-android10` 仓库实际源码核对。*
