# SurfaceFlinger 总览与启动（Android 10 / cells-android10）

> 适用工程：`cells-android10` / AOSP `android-10.0.0_r33`（Columbia Cells 树，Pixel 3a XL / coral）
> 源码根：`frameworks/native/services/surfaceflinger/`（SF 进程本体）+ `frameworks/native/libs/renderengine/`（GPU 绘制）+ `kernel`（图形栈、HWC 驱动）
> 本文定位：**架构总览 + 启动链 + 六大组件拆解**，对标 `WMS_总览与启动_Android10.md` / `IMS_总览与启动_Android10.md`。合成逐帧时序见兄弟文档 `SurfaceFlinger 合成流程详解_Android10.md`，方法清单见 `SurfaceFlinger_方法流程分析_Android10.md`。

---

## 0. 一图总览（进程与六大组件）

```
               ┌──────────────────────────────────────────────────────────┐
               │             surfaceflinger 独立进程 (system)               │
               │  rc: surfaceflinger.rc  class hal graphics                 │
               └──────────────────────────────────────────────────────────┘
                                     │
   main()  ──► createSurfaceFlinger() ──► onFirstRef() ──► init() ──► run() ──► MessageQueue 主循环
   (main_surfaceflinger.cpp:84)      (SF.cpp:285)    (SF.cpp:408) (SF.cpp:621)(SF.cpp:1493) (onMessageReceived:1759)

   六大组件（职责边界，全部在本仓实测行号）：
   ┌─────────────┐  ┌──────────────┐  ┌───────────────┐  ┌────────────────────┐
   │ ① Layer 树   │  │ ② DisplayDevice│  │ ③ HWComposer   │  │ ④ Scheduler+VSync   │
   │(Layer.h:97) │  │(DispDev.cpp:57)│  │(HWC.cpp:74)   │  │(Scheduler.cpp:60)  │
   │ 内容层：谁要 │  │ 物理屏抽象：   │  │ HWC2 封装：     │  │ VSync 时钟源与分发 │
   │ 画、画什么   │  │ 一块屏+缓冲区 │  │ device/client  │  │ DispSync+EventThread│
   └─────────────┘  └──────────────┘  └───────────────┘  └────────────────────┘
          │                 │                  │                      │
          └─────────────────┴──────────────────┴──────────────────────┘
                                     ▼
   ┌──────────────────────────────────────────────────────────────────────┐
   │ ⑤ CompositionEngine（合成引擎抽象，Android 10 新架构核心）             │
   │   CompositionEngine.h:41 / createCompositionEngine(src:30)             │
   │   持有：Output(Display) / OutputLayer / LayerFE 接口                    │
   │   职责：plan(合成策略) → validate → present(上屏)                      │
   └──────────────────────────────────────────────────────────────────────┘
                                     │ 需要 GPU 走图元绘制时
                                     ▼
   ┌──────────────────────────────────────────────────────────────────────┐
   │ ⑥ RenderEngine（GPU 绘制后端）                                         │
   │   libs/renderengine/RenderEngine.h:61 / drawLayers(:214) / GLESRenderEngine│
   └──────────────────────────────────────────────────────────────────────┘
```

**一句话**：SurfaceFlinger 是独立进程里的「合成总管」，收齐各 Layer（App 的 Surface）的图元，问 DisplayDevice/HWC 决定哪些交给硬件合成、哪些自己用 RenderEngine(GPU) 画，最后在 VSync 节拍下把一帧送显。Android 10 把「怎么合成、怎么上屏」从 `SurfaceFlinger.cpp` 抽成独立的 **CompositionEngine** 模块（这是相对 8.1 最大的架构变化，详见兄弟文档）。

---

## 1. 结论前置

1. **SurfaceFlinger 是独立 native 进程**，由 `surfaceflinger.rc` 拉起，`main_surfaceflinger.cpp:84` 入口，通过 `createSurfaceFlinger()`（`SurfaceFlinger.cpp:285` 工厂）建实例；它不是 system_server 的一部分，而是和 system_server 平级的 Binder 服务端（service 名 `SurfaceFlinger`）。
2. **启动顺序**：`main()` → `SurfaceFlinger` 构造（`SF.cpp:285`）→ `onFirstRef()`（`SF.cpp:408`，搭 Binder/Looper）→ `init()`（`SF.cpp:621`，装配 HWC、Scheduler、Display）→ `run()`（`SF.cpp:1493`）进 `MessageQueue` 主循环。真正「显示就绪」在 `bootFinished()`（`SF.cpp:533`，AMS 启动完第一屏后回调）。
3. **Android 10 架构核心变化**：引入 `CompositionEngine`（在 `CompositionEngine/` 子目录）把「合成策略 + 上屏」从 `SurfaceFlinger` 大类剥离成一组接口（`CompositionEngine` / `Output` / `OutputLayer` / `LayerFE`），`SurfaceFlinger` 退化为「事务管理 + 驱动 CE」。读 8.1 老博客必须按这个边界修正。
4. **六大组件职责分明**：Layer=内容、DisplayDevice=屏、HWComposer=硬件合成决策、Scheduler=VSync 时钟、CompositionEngine=合成编排、RenderEngine=GPU 绘制。排查卡顿/花屏时按这张职责表定位，不要一上来就翻 `SurfaceFlinger.cpp` 的 6400 行。
5. **VSync 是整条链的时间基准**：硬件 VSync → `DispSync`（`DispSync.cpp`，软件相位校正）→ `EventThread`（`EventThread.cpp`）→ 同时驱动「App 渲染」与「SF 合成」两条路径，保证生产/消费同拍。

---

## 2. 总览

### 2.1 进程模型与 Binder 角色

- **独立进程**：`main_surfaceflinger.cpp` 里 `ProcessState::self()->setThreadPoolMaxThreadCount(4)`（线程池上限 4，避免线程膨胀），`sp<SurfaceFlinger> flinger = surfaceflinger::createSurfaceFlinger()`（`:30`）后 `flinger->init()` / `flinger->run()`。
- **Binder 服务端**：对上（app/WMS）暴露 `ISurfaceComposer`（`setTransactionState` 等事务入口）；对下通过 `Hwc2::Composer`（HIDL）与 HAL 层 `IComposer` 通信，再经 kernel `hwcomposer`/DRM 驱动送硬件。
- **与 WMS 的关系（呼应 WMS 篇）**：WMS 管「窗口树 / 焦点 / 令牌」，SF 管「窗口背后的 Surface 如何合成上屏」。WMS 通过 `SurfaceControl`/`Transaction` 向 SF 下单（创建 Layer、设位置/层级/Z），SF 不管窗口语义，只认 Layer 和 Buffer。

### 2.2 主循环：MessageQueue 双消息驱动

`SurfaceFlinger::run()`（`SF.cpp:1493`）→ `waitForEvent()`（`SF.cpp:1460`）→ `MessageQueue` 阻塞在 `INVALIDATE` / `REFRESH` 两类消息。分发入口统一是 `onMessageReceived`（`SF.cpp:1759`）：

- `INVALIDATE` 阶段：处理事务（`handleMessageTransaction`）、拿新 Buffer（`handlePageFlip` / latch）、判断本帧是否要刷新。
- `REFRESH` 阶段：真正合成（`handleMessageRefresh` → `rebuildLayerStacks` → `doComposition` → `postFrame`）。

> 细节见 `SurfaceFlinger 合成流程详解_Android10.md`。本文只把这条主循环作为「启动后一直在做什么」的锚点。

### 2.3 六大组件速览表

| 组件 | 核心文件 | 关键行 | 一句话职责 |
|---|---|---|---|
| ① Layer（内容层） | `Layer.h` / `BufferQueueLayer.cpp` 等 | `Layer.h:97`（基类） | 一个可合成的视觉单元；持有一块 GraphicBuffer 及位置/透明度/Z 等状态 |
| ② DisplayDevice（屏抽象） | `DisplayDevice.cpp` | `DisplayDevice.cpp:57`（ctor） | 一块物理显示设备 + 它的合成目标（DisplaySurface） |
| ③ HWComposer（HWC2） | `DisplayHardware/HWComposer.cpp` | `HWComposer.cpp:74`（ctor） | 封装 HWC2 HAL，决定每个 Layer 走「硬件叠加」还是「GPU 客户端合成」 |
| ④ Scheduler + VSync | `Scheduler/Scheduler.cpp` `DispSync.cpp` `EventThread.cpp` | `Scheduler.cpp:60`（ctor） | 产生并分发 VSync 节拍，对齐 App 渲染与 SF 合成 |
| ⑤ CompositionEngine | `CompositionEngine/`（src + include） | `CompositionEngine.h:41`（抽象类） | 合成编排：plan/validate/present，Android 10 新抽离 |
| ⑥ RenderEngine | `libs/renderengine/` | `RenderEngine.h:61`（抽象类） | GPU 图元绘制（client 合成路径的「画笔」） |

---

## 3. 分层拆解

### 3.1 ① 启动链：main → onFirstRef → init → run

| 步骤 | 文件:行 | 做了什么 |
|---|---|---|
| `main()` | `main_surfaceflinger.cpp:84` | 拉起伴随服务（如 GraphicsAllocator），设 Binder 线程池上限 4，建 `SurfaceFlinger` 实例 |
| `createSurfaceFlinger()` | `SurfaceFlinger.cpp:285`（工厂 `SurfaceFlinger(Factory&)`） | 经 `Factory` 注入各组件构造器（Columbia Cells 树的依赖装配点） |
| `onFirstRef()` | `SurfaceFlinger.cpp:408` | 初始化 Binder 通信、`MessageQueue`、Looper；此时还没碰硬件 |
| `init()` | `SurfaceFlinger.cpp:621` | 装配 `HWComposer`、`Scheduler`（含 `mPrimaryDispSync`）、`CompositionEngine`、`DisplayDevice`；注册 HWC 回调（`onVsync`/`onHotplug`） |
| `bootFinished()` | `SurfaceFlinger.cpp:533` | 由 system_server（AMS）在系统首屏就绪后回调，触发第一次全量刷新 |
| `run()` → `waitForEvent()` | `SF.cpp:1493` / `:1460` | 进入 `MessageQueue` 主循环，等 INVALIDATE/REFRESH 消息 |

> 与 `SF启动链与mini_compositor映射.md` 的教学版逐方法对照可互为补充。

### 3.2 ② Layer 层级：内容的最小合成单元

Android 10 的 Layer 是一棵「类型化继承树」，基类 `Layer`（`Layer.h:97`，实现 `compositionengine::LayerFE` 接口，即「Layer Front-End」）：

```
Layer  (Layer.h:97, 抽象基类, 持位置/Z/透明度/Buffer 状态)
 ├─ BufferLayer            (BufferLayer.h:48)         —— 有像素内容的层基类
 │   ├─ BufferQueueLayer   (BufferQueueLayer.h:32)     —— 经典 BufferQueue 驱动（legacy 路径）
 │   └─ BufferStateLayer   (BufferStateLayer.h:34)     —— Android 10 新路径：状态经 Binder 由 client 推送
 ├─ ColorLayer             (ColorLayer.h:26)           —— 纯色填充层（如 dim 遮罩）
 └─ ContainerLayer         (ContainerLayer.h:26)       —— 仅做层级容器，无自身内容（如某些 SurfaceView 容器）
```

- **关键差异（面试高频）**：`BufferQueueLayer` 依赖 `BufferQueue`（生产者-消费者队列，dequeue/queue/acquire/fence 那套）；`BufferStateLayer` 把 Buffer 状态改成由 client 通过 `Transaction` 直接「推」过来（`onFrameAvailable` 在 `BufferQueueLayer.cpp:442` 仍是 BQ 路径的回调，BSL 不复用此路径）。Android 10 起新窗口默认走 `BufferStateLayer`，这是向「无锁、状态快照」合成过渡的关键。
- **latch（取帧）**：`Layer::prepareClientLayer`（`Layer.cpp:517` 系列重载）决定本帧用哪块 Buffer、裁剪区是什么；client 合成层最终交给 `RenderEngine::drawLayers` 画进目标。

### 3.3 ③ DisplayDevice：一块物理屏的抽象

- 构造：`DisplayDevice::DisplayDevice(DisplayDeviceCreationArgs&&)`（`DisplayDevice.cpp:57`）。构造时即向 `CompositionEngine` 要一块 `Display`（`mCompositionDisplay = mFlinger->getCompositionEngine().createDisplay(...)`，`DisplayDevice.cpp:62`），并创建 `RenderSurface`（`DisplayDevice.cpp:69`）、`DisplayColorProfile`（`DisplayDevice.cpp:76`）。
- 姿态/裁剪：`DisplayDevice::setProjection`（`DisplayDevice.cpp:203`）设 orientation、viewport、frame——决定了 Layer 坐标如何映射到物理像素。
- 持有：HWC 句柄（`mHwc`）+ `DisplaySurface`（与硬件缓冲区/DRM 的连接点）。SF 对所有「屏」只认 `DisplayDevice`，不直连驱动。

### 3.4 ④ HWComposer（HWC2）：硬件合成决策

- 封装：`HWComposer::HWComposer(std::unique_ptr<Hwc2::Composer>)`（`HWComposer.cpp:74`），下层是 HIDL `IComposer`（再落 kernel）。
- 两个核心动作：
  - `prepare(DisplayId, const compositionengine::Output&)`（`HWComposer.cpp:402`）：把 CE 算好的「每层合成方式」下发给 HWC2，HWC 回写每层最终是 `DEVICE`（硬件叠加）还是 `CLIENT`（需 GPU 画）。
  - `presentAndGetReleaseFences`（`HWComposer.cpp:560`）：真正提交这一帧到显示硬件，并取回 release fence（Buffer 何时可被生产者复用）。
  - `setClientTarget`（`HWComposer.cpp:390`）：当某层是 CLIENT 合成时，SF 用 `RenderEngine` 先把所有 CLIENT 层画进一块 target buffer，再通过它交给 HWC 叠加。
- **关键认知**：HWC 决定「哪几层能叠加」是性能分水岭。能 DEVICE 叠加就不要 GPU 画——这是流畅度优化的第一杠杆。

### 3.5 ⑤ Scheduler + VSync：整条链的时间基准

VSync 信号流（注意跨三层）：

```
硬件 VSync (display panel / HWC)  ──回调──►  HWComposer::onVsyncReceived
        │
        ▼
   DispSync  (Scheduler/DispSync.cpp, DispSyncThread:62, mPeriod:92)
        │ 软件相位校正：把硬件 VSync 和 present 时间对齐，补偿 offset
        ▼
   DispSyncSource (Scheduler.cpp:160)  —— 带 phaseOffset 的 VSync 源
        │
        ▼
   EventThread (Scheduler/EventThread.cpp:157)  —— 每个 connection 一个分发线程
        │
        ├─► App 侧 EventThread：驱动 App 在 VSync 节拍里渲染（Choreographer 同源）
        └─► SF 侧 EventThread ：驱动 SF 在 VSync 节拍里合成（MessageQueue::INVALIDATE/REFRESH）
```

- `Scheduler::Scheduler(...)`（`Scheduler.cpp:60`），`mPrimaryDispSync`（`Scheduler.cpp:72`）是主屏的软时钟；`createConnection`（`Scheduler.cpp:134`）为 App / SF 各建一条带不同 `phaseOffset` 的 VSync 连接（App 先渲染、SF 后合成，错峰避免抢拍）。
- `resyncToHardwareVsync` / `setVsyncPeriod` 负责刷新率切换（90/120Hz 时这里的周期会变）。

### 3.6 ⑥ CompositionEngine + RenderEngine：合成编排与 GPU 绘制

**CompositionEngine（Android 10 新架构核心）**
- 抽象类 `CompositionEngine`（`CompositionEngine/include/compositionengine/CompositionEngine.h:41`），实现在 `CompositionEngine/src/CompositionEngine.cpp`（`createCompositionEngine()` 在 `:30`）。
- 工厂方法：`createDisplay`（`impl/CompositionEngine.h:28`）、`createLayer`（`:30`）——SF 不直接 new 显示/层对象，而是向 CE 要，依赖倒置。
- 关键接口对象：`Output`（一块合成目标的抽象，`Display` 继承它，`Display.h:35`）、`OutputLayer`、`LayerFE`（Layer 前端的接口，见 3.2）。
- 职责三段式：**plan**（每帧算「谁 DEVICE / 谁 CLIENT」）→ **validate**（校验合成可行性，必要时回退到全 CLIENT）→ **present**（把结果上屏，内部调用 HWC `present` 或 `RenderEngine::drawLayers`）。

**RenderEngine（GPU 画笔）**
- 抽象类 `RenderEngine`（`libs/renderengine/include/renderengine/RenderEngine.h:61`），实现 `GLESRenderEngine`（`libs/renderengine/gl/GLESRenderEngine.cpp`）。
- 核心绘制入口：`drawLayers(const DisplaySettings&, const std::vector<LayerSettings>&, ANativeWindowBuffer*, ...)`（`RenderEngine.h:214`）——把所有「CLIENT 合成层」一次性绘制进目标 buffer，再交 HWC 叠加。
- 辅助：`fillRegionWithColor`（`:107`，ColorLayer/dim 用）、`drawMesh`（`:171`，基础图元）。

---

## 4. 速查表（组件 → 文件 → 关键行）

| 关注点 | 文件 | 行号 | 备注 |
|---|---|---|---|
| 进程入口 | `main_surfaceflinger.cpp` | `:84` | `main()` |
| 实例工厂 | `SurfaceFlinger.cpp` | `:285` | `createSurfaceFlinger`（Factory 注入） |
| Binder/Looper 装配 | `SurfaceFlinger.cpp` | `:408` | `onFirstRef()` |
| 硬件/调度装配 | `SurfaceFlinger.cpp` | `:621` | `init()` |
| 首屏就绪回调 | `SurfaceFlinger.cpp` | `:533` | `bootFinished()` |
| 主循环 | `SurfaceFlinger.cpp` | `:1493` / `:1460` | `run()` / `waitForEvent()` |
| 消息分发 | `SurfaceFlinger.cpp` | `:1759` | `onMessageReceived()` |
| 一帧合成 | `SurfaceFlinger.cpp` | `:2577` / `:2609` | `doComposition()` / `postFramebuffer()` |
| Layer 基类 | `Layer.h` | `:97` | `class Layer : public LayerFE` |
| BufferQueueLayer | `BufferQueueLayer.h` | `:32` | 旧 BufferQueue 路径 |
| BufferStateLayer | `BufferStateLayer.h` | `:34` | Android 10 新状态推送路径 |
| 取帧 | `Layer.cpp` | `:517` | `prepareClientLayer()` |
| DisplayDevice 构造 | `DisplayDevice.cpp` | `:57` / `:62` | 含向 CE 要 Display |
| DisplayDevice 姿态 | `DisplayDevice.cpp` | `:203` | `setProjection()` |
| HWC 构造 | `DisplayHardware/HWComposer.cpp` | `:74` | |
| HWC prepare | `DisplayHardware/HWComposer.cpp` | `:402` | 下发合成方式 |
| HWC present | `DisplayHardware/HWComposer.cpp` | `:560` | 提交硬件 + 取 release fence |
| Scheduler 构造 | `Scheduler/Scheduler.cpp` | `:60` / `:72` | 含 `mPrimaryDispSync` |
| VSync 连接 | `Scheduler/Scheduler.cpp` | `:134` / `:160` | `createConnection` / `DispSyncSource` |
| DispSync 软时钟 | `Scheduler/DispSync.cpp` | `:62` / `:92` | `DispSyncThread` / `mPeriod` |
| EventThread | `Scheduler/EventThread.cpp` | `:157` | VSync 分发 |
| CE 抽象类 | `CompositionEngine/include/compositionengine/CompositionEngine.h` | `:41` | |
| CE 工厂 | `CompositionEngine/src/CompositionEngine.cpp` | `:30` | `createCompositionEngine()` |
| CE 建 Display/Layer | `CompositionEngine/include/compositionengine/impl/CompositionEngine.h` | `:28` / `:30` | |
| RenderEngine 抽象 | `libs/renderengine/include/renderengine/RenderEngine.h` | `:61` | |
| RenderEngine 绘制 | `libs/renderengine/include/renderengine/RenderEngine.h` | `:214` | `drawLayers()` |

---

## 5. 调试验证

| 目的 | 命令 / 入口 | 说明 |
|---|---|---|
| 整体状态 | `adb shell dumpsys SurfaceFlinger` | 看 Layer 数、HWC 是否 DEVICE 合成、刷新率、fps |
| 仅列图层 | `adb shell dumpsys SurfaceFlinger --list` | 列出当前所有 Layer 名 |
| 图层明细 | `adb shell dumpsys SurfaceFlinger layers` | 每层的 z、size、format、合成方式(DEVICE/CLIENT) |
| 截图 | `adb shell screencap -p /data/local/tmp/a.png` | 抓当前合成结果（注意抓的是合成后帧，非单 Layer） |
| 帧耗时 | `systrace` / `perfetto` 勾选 `gfx` / `surfaceflinger` | 看 VSync、SF 主线程、HWC 提交各阶段耗时 |
| 显示服务存活 | `adb shell service list \| grep SurfaceFlinger` | 确认 Binder service 已注册 |
| 事务追踪 | `SurfaceFlinger` 内 `SurfaceTracing`（`SurfaceTracing.cpp`） | 抓 Transaction 序列，定位某 Layer 何时被改 |

**排查套路**：花屏/叠加错 → 看 `dumpsys` 里该层是 DEVICE 还是 CLIENT；卡顿 → `perfetto` 看 SF `REFrame` 是否等 VSync 或 HWC 提交慢；黑屏 → 先确认 `DisplayDevice` 是否 `isValid`（`DisplayDevice.cpp:82` 校验点）。

---

## 6. 踩坑清单（Android 10 专属 / 高频误区）

1. **CompositionEngine 是 Android 10 才独立抽出的**：老博客（8.1 及更早）把 `prepareFrame`/`postFrame`/`composeSurfaces` 都写在 `SurfaceFlinger.cpp`；Android 10 已下沉到 `CompositionEngine`（`SurfaceFlinger.cpp:1877` 处有注释提示）。拿 8.1 的行号硬套 10 会全盘错位——务必以本仓 `CompositionEngine/` 为准。
2. **`BufferStateLayer` ≠ `BufferQueueLayer`**：新窗口默认走 BSL（状态经 Binder 推送、无 BQ 队列语义），老代码里的 `onFrameAvailable`（`BufferQueueLayer.cpp:442`）对 BSL 不适用。问「Buffer 怎么来的」先分清是哪种 Layer。
3. **SurfaceFlinger 是独立进程，不是 system_server 的一部分**：改 SF 要重编 `surfaceflinger` 模块并 push 到 `/system/lib[64]/...` + 重启服务，跟改 `services.jar` 是两码事；它和 WMS 通过 `ISurfaceComposer` Binder 通信，跨进程。
4. **VSync 有「软件相位」**：App 和 SF 的 VSync 是 `DispSync` 经不同 `phaseOffset` 错峰出来的（`Scheduler.cpp:160`），不是裸硬件 VSync。刷新率切换（90/120Hz）时 `mPeriod`（`DispSync.cpp:92`）会变，动画掉帧别只怀疑 App。
5. **DEVICE 合成 vs CLIENT 合成是性能分水岭**：HWC `prepare`（`HWComposer.cpp:402`）决定每层归属。能 DEVICE 叠加就别 GPU 画；过度使用 `SurfaceView`/圆角/模糊会强制转 CLIENT，掉帧先查这里。
6. **Layer 不认识「窗口」语义**：SF 只认 Layer + Buffer + Z，不知道 Activity/Window。WMS 管窗口树，SF 管 Layer 合成——两者通过 `SurfaceControl`/`Transaction` 衔接，排查「窗口位置错」要去 WMS 篇，`SurfaceFlinger 总览` 不解决窗口层级问题。
7. **`bootFinished` 前不真正出图**：`SurfaceFlinger.cpp:533` 由 AMS 在系统就绪后回调，在此之前首屏是 bootanimation。首屏黑屏但 service 正常，先确认 boot 流程是否走到这里。
8. **HWC 提交有 release fence**：`presentAndGetReleaseFences`（`HWComposer.cpp:560`）返回的 fence 决定生产者何时可复用 Buffer。Buffer 泄漏/卡在「已提交未释放」往往和 fence 流转有关，不是简单的引用计数 bug。

---

## 7. 与既有 SF 文档的分工

| 文档 | 侧重 | 本文角色 |
|---|---|---|
| `SF启动链与mini_compositor映射.md` | 启动链逐方法 + 教学版对照 | 互补：本文给「六大组件职责地图」 |
| `SurfaceFlinger_方法流程分析_Android10.md` | 三类方法清单（生命周期/双阶段/外部入口） | 互补：本文给「每个组件是什么」 |
| `SurfaceFlinger 合成流程详解_Android10.md` | INVALIDATE/REFRESH 逐帧时序 | 下游：本文的 3.1/2.2 是它的大前提 |
| 本文 `SurfaceFlinger 总览与启动_Android10.md` | **架构总览 + 六大组件拆解 + 启动链** | 入口级，先读本文再下钻上述三篇 |

> 下一步可继续：模块 A「`BufferStateLayer` vs `BufferQueueLayer` 取帧全路径对比」、模块 B「CompositionEngine 的 plan/validate/present 三段式逐帧细节」、模块 C「VSync-DispSync-EventThread 相位对齐数学」。需要哪个我接着写。
