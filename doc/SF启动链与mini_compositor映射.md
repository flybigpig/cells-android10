# SurfaceFlinger 启动链 + mini_compositor 映射学习文档

> 适用工程：cells-android10 / Android 10（目标设备 Pixel 3a xl，源码 android-10.0.0_r33）
> 本文把「真实 SurfaceFlinger 进程启动链」与「教学版 `doc/mini_compositor.cpp`」逐方法对照，帮助从玩具版过渡到真实代码。

---

## 0. 一图总览

```
真实 SurfaceFlinger 进程                          mini_compositor.cpp（教学版）
────────────────────────────────                 ────────────────────────────────
main_surfaceflinger.cpp::main()                  main()
  ├─ onFirstRef()  搭消息泵  (SF.cpp:408)           │  装配阶段
  ├─ init()        装配硬件/调度 (SF.cpp:621)  ◀──┼─ Display screen(...)          持后端
  │    ├─ Scheduler + 2×EventThread (VSync)       │  stack.emplace_back(...)      图层栈
  │    ├─ RenderEngine                            │  auto alias = app            引用计数
  │    ├─ HWComposer + registerCallback          │  screen.markDirty()          脏标记
  │    ├─ processDisplayHotplugEventsLocked       │
  │    ├─ initializeDisplays()                    │
  │    └─ StartPropertySetThread                  │
  ├─ addService() 注册到 ServiceManager            │
  ├─ startDisplayService()                        │
  └─ run()  死循环等事件  (SF.cpp:1477)        ◀──┴─ consumeDirty()? compositeFrame():present()
       └─ waitForEvent() ── VSync ──>                └─ sort by Z → 多态 compose → present
            onMessageReceived → handleMessageRefresh      （一帧合成）
```

真实链三环在 [main_surfaceflinger.cpp:79](../frameworks/native/services/surfaceflinger/main_surfaceflinger.cpp#L79) 注释里明确标注：
`onFirstRef() -> init() -> run()`。

---

## 1. 真实 SurfaceFlinger 启动链详解

### 1.1 main() 入口（[main_surfaceflinger.cpp:84-137](../frameworks/native/services/surfaceflinger/main_surfaceflinger.cpp#L84-L137)）

| 步骤 | 代码 | 作用 |
| --- | --- | --- |
| 系统服务循环 | `OtherSystemServiceLoopRun()` (L85) | 定制系统注入的其它系统服务循环 |
| 信号 | `signal(SIGPIPE, SIG_IGN)` (L87) | 忽略 SIGPIPE（写已关闭 socket 不崩） |
| RPC 线程池 | `configureRpcThreadpool(1, false)` (L89) | HIDL RPC 线程池上限 1 |
| 图形分配服务 | `startGraphicsAllocatorService()` (L95) | 注册 `IAllocator`（V3_0 优先，回退 V2_0） |
| Binder 线程池 | `setThreadPoolMaxThreadCount(4)` + `startThreadPool` (L99-103) | 限制 binder 线程 4 条 |
| 实例化 SF | `createSurfaceFlinger()` (L106) | 工厂模式创建 `SurfaceFlinger` 对象 |
| 调度优先级 | `setpriority(PRIORITY_URGENT_DISPLAY)` (L108) | 紧急显示优先级 |
| 调度策略 | `set_sched_policy(SP_FOREGROUND)` (L110) | 前台调度 |
| CPU 亲和 | `set_cpuset_policy(SP_SYSTEM)` (L115) | 放 system 后台 cpuset，不抢大核 |
| 初始化 | `flinger->init()` (L118) | **核心装配**（见 1.3） |
| 注册服务 | `sm->addService("SurfaceFlinger", flinger, ...)` (L121-123) | 注册到 ServiceManager，DUMP_FLAG_PRIORITY_CRITICAL |
| 显示服务 | `startDisplayService()` (L125) | 依赖 SF 已注册 |
| 实时调度 | `sched_setscheduler(SCHED_FIFO, prio=2)` (L127-131) | 提升为实时 FIFO 调度 |
| 主循环 | `flinger->run()` (L134) | 进入死循环（见 1.4） |

### 1.2 onFirstRef() —— 首次强引用触发（[SurfaceFlinger.cpp:408](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L408)）

```cpp
void SurfaceFlinger::onFirstRef() {
    mEventQueue->init(this);   // 消息队列初始化
}
```

- 触发时机：`sp<SurfaceFlinger>` 第一次被强引用（main 中 L106 `createSurfaceFlinger()` 返回后赋给 `flinger`）。
- 只做一件事：把主线程消息泵 `mEventQueue` 与 `this` 绑定，为后续 `init()`/`run()` 里收发消息打地基。

### 1.3 init() —— 装配重头戏（[SurfaceFlinger.cpp:621-760](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L621-L760)）

| 子步骤 | 代码行 | 作用 |
| --- | --- | --- |
| 加锁 | L627 `mStateLock` | 全程持状态锁 |
| 创建 Scheduler | L629-631 | `setPrimaryVsyncEnabled` 回调绑定 |
| resync 回调 | L632-633 | `getVsyncPeriod` 绑定，供重同步用 |
| 两条 EventThread 连接 | L635-645 | `app` 连接（Choreographer 用）+ `sf` 连接（SF 自己用），各自带 phase offset |
| 绑定事件连接 | L648 | `mEventQueue->setEventConnection(...)` 把 SF 连接交给消息队列 |
| Vsync 调制器 | L650-651 | `mVsyncModulator` 绑定 scheduler 与两条 handle |
| 区域采样线程 | L653-655 | `RegionSamplingThread`（亮度采样等） |
| 创建 RenderEngine | L658-671 | 颜色管理/高优先级上下文/受保护内容 三特性，设置给 CompositionEngine |
| 创建 HWComposer | L675-676 | HWC2 实现 + `registerCallback(this, sequenceId)` |
| 处理热插拔 | L678 `processDisplayHotplugEventsLocked` | 处理初始热插拔，拿到默认 display |
| 取默认 display | L679-682 | 断言内屏存在且已连接 |
| VR Flinger（可选） | L684-706 | `useVrFlinger` 时创建，设请求回调 |
| 同步绘制状态 | L709 `mDrawingState = mCurrentState` | 初始 drawing=当前 |
| 初始化显示 | L712 `initializeDisplays()` | 点亮/unblank 默认屏 |
| RE 缓存预热 | L714 `getRenderEngine().primeCache()` | |
| present fence 可靠性 | L718-719 | 判断 HWC `PresentFenceIsNotReliable` 能力 |
| 属性设置线程 | L720-724 | `StartPropertySetThread->Start()` 设 `ro.surface_flinger.*` 属性 |
| 刷新率回调三类 | L727-754 | `changeRefreshRate` / `getCurrentRefreshRateType` / `getVsyncPeriod` |
| 填充刷新率配置 | L756-757 | `populate(HWC configs)` + `setConfigMode` |

> 关键认知：`init()` 之后，VSync→Scheduler→EventThread→mEventQueue→onMessageReceived 这条「事件驱动一帧」的链路才打通；同时 HWComposer 回调（vsync/hotplug/refresh）也已注册。

### 1.4 run() —— 主循环死等（[SurfaceFlinger.cpp:1477](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L1477)）

```cpp
void SurfaceFlinger::run() {
    do {
        waitForEvent();
    } while (true);
}
```

- SF 主线程在这里**阻塞等待事件**（由 `mEventQueue` 唤醒）。
- 唤醒后进入 `onMessageReceived`（[SF.cpp:1743](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L1743)），分 `INVALIDATE`/`REFRESH` 两路，最终走 `handleMessageRefresh`（[SF.cpp:1863](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L1863)）执行一帧合成。

---

## 2. mini_compositor.cpp 方法注释总结

文件：[doc/mini_compositor.cpp](mini_compositor.cpp)（138 行）
定位：把 SurfaceFlinger 核心结构简化成玩具版，串起 `cpp_learning_plan.md` 全部 12 个模块。注释中"模块 N"即该计划编号。

### 2.1 `Layer` 抽象基类（[L23-L37](mini_compositor.cpp#L23-L37)）— 模块5：纯虚接口+多态

| 成员/方法 | 注释要点 | 模块 |
| --- | --- | --- |
| `int z` / `name` | Z 序（越大越靠上）+ 图层名 | — |
| 构造 `Layer(string,int)` | 初始化列表 + `std::move` | 模块4 |
| `virtual ~Layer() = default` | 基类虚析构（派生正确释放） | 模块5 |
| `virtual void compose() const = 0` | 纯虚：各图层各自实现合成 | 模块5 |
| `= delete` 拷贝构造/赋值 | 禁浅拷贝防双释放 | 模块4 |

### 2.2 `BufferLayer`（[L40-L47](mini_compositor.cpp#L40-L47)）— 模块1/4/5：带缓冲图层

- `compose() override`：模拟 GL draw buffer，输出 `[BufferLayer] ... -> GL draw buffer`。
- 对应真实 SF 的 `BufferQueueLayer`/`BufferStateLayer`。

### 2.3 `ColorLayer`（[L50-L59](mini_compositor.cpp#L50-L59)）— 模块1/4/5：纯色图层

- 成员 `color`（0xRRGGBB），`compose() override` 填充纯色。
- 对应真实 SF 的 `ColorLayer`。

### 2.4 `Display`（[L62-L88](mini_compositor.cpp#L62-L88)）— 模块6/12：后端+原子脏标记

| 方法 | 注释要点 | 模块 |
| --- | --- | --- |
| `mBackBuffer` (unique_ptr) | 独占后端 buffer（string 假装显存） | 模块6 |
| `mDirty` (atomic<bool>) | 标记"需要重绘" | 模块12 |
| `explicit Display(string)` | 初始化列表装配，构造即标脏 | 模块4 |
| `markDirty()` | 原子写标记脏 | — |
| `consumeDirty()` | 原子取值并清零 | 模块12 |
| `present()` | 输出后端 buffer 内容 | — |
| `= delete` 拷贝 / `= default` 移动 | 独占资源禁拷贝、允许移动 | 模块4/10 |

### 2.5 `compositeFrame()`（[L92-L105](mini_compositor.cpp#L92-L105)）— 模块6/7/9：图层栈合成

- `std::sort` + Lambda 比较器，按 `z` **升序**（小在底层）— 模块9。
- 范围 for + auto 遍历 — 模块7。
- `layer->compose()` 多态调用，自动派发到 Buffer/Color — 模块5。

### 2.6 `main()`（[L107-L137](mini_compositor.cpp#L107-L137)）

| 步骤 | 注释要点 | 模块 |
| --- | --- | --- |
| `make_shared` 建三图层 | 智能指针管理，可多处引用 | 模块6 |
| `vector` + `emplace_back` | 图层栈容器、原地构造无临时对象 | 模块7 |
| `auto alias = app` | shared_ptr 拷贝，`use_count=2` | 模块6 |
| `Display screen + markDirty` | 装配显示、标脏 | 模块6/12 |
| `consumeDirty()` 判重绘 | 原子消费脏标记 | 模块12 |
| `compositeFrame + present` | Lambda 排序+多态合成+呈现 | 模块9/5 |
| 末尾 `use_count` | stack 析构后引用归零自动 delete | 模块6 |

---

## 3. mini_compositor ↔ 真实 SurfaceFlinger 映射

| mini_compositor 概念 | 真实 SurfaceFlinger 对应 | 真实代码位置 |
| --- | --- | --- |
| `Display` 持后端 buffer | `init()` 创建 HWComposer/RenderEngine + `initializeDisplays()` | [SF.cpp:621](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L621) |
| `Layer`/Buffer/Color 多态 | `createBufferQueueLayer`/`BufferStateLayer`/`ColorLayer`（工厂） | [SurfaceFlingerFactory.cpp:120-134](../frameworks/native/services/surfaceflinger/SurfaceFlingerFactory.cpp#L120-L134) |
| `mDirty`/`consumeDirty` 脏驱动 | `mEventQueue` + VSync 驱动 invalidate/refresh | [main 注释 L79-83](../frameworks/native/services/surfaceflinger/main_surfaceflinger.cpp#L79-L83) |
| `compositeFrame` 排序+合成 | `handleMessageRefresh`→`rebuildLayerStacks`→`doComposition` | [SF.cpp:1863](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L1863) |
| `present()` 输出 | HWC `present`/`postFrame`（下沉 CompositionEngine） | `CompositionEngine/src/Display.cpp` |
| `run()`-less 死循环跑帧 | `run()` 内 `do { waitForEvent(); } while(true)` | [SF.cpp:1477](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L1477) |
| `make_shared`/`unique_ptr` 资源管理 | `sp`/`wp` RefBase + 工厂 `createSurfaceFlinger` | [SurfaceFlingerFactory.cpp:45](../frameworks/native/services/surfaceflinger/SurfaceFlingerFactory.cpp#L45) |

**一句话映射**：mini_compositor 的 `main()` 跑一帧 = 真实 SF `run()` 死循环里被 VSync 唤醒后跑一帧 `handleMessageRefresh`；mini 的 `Layer` 多态 = 真实 `BufferQueueLayer/ColorLayer` 多态；mini 的 `mDirty` = 真实 VSync 驱动的 invalidate 标志。

---

## 4. 关键代码点速查

| 主题 | 符号 | 位置 |
| --- | --- | --- |
| 进程入口 | `main` | [main_surfaceflinger.cpp:84](../frameworks/native/services/surfaceflinger/main_surfaceflinger.cpp#L84) |
| 工厂创建 SF | `createSurfaceFlinger` | [SurfaceFlingerFactory.cpp:45](../frameworks/native/services/surfaceflinger/SurfaceFlingerFactory.cpp#L45) |
| 首引用搭消息泵 | `onFirstRef` | [SurfaceFlinger.cpp:408](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L408) |
| 装配重头戏 | `init` | [SurfaceFlinger.cpp:621](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L621) |
| 主循环死等 | `run` | [SurfaceFlinger.cpp:1477](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L1477) |
| VSync 入口 | `onVsyncReceived` | [SurfaceFlinger.cpp:1499](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L1499) |
| 单帧合成总入口 | `handleMessageRefresh` | [SurfaceFlinger.cpp:1863](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L1863) |
| 图层收集排序 | `rebuildLayerStacks` | [SurfaceFlinger.cpp:2328](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L2328) |
| 真合成 | `doComposition` | [SurfaceFlinger.cpp:2561](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L2561) |
| boot 完成 | `bootFinished` | [SurfaceFlinger.cpp:533](../frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp#L533) |
| 教学版合成 | `compositeFrame` | [mini_compositor.cpp:92](mini_compositor.cpp#L92) |

---

## 5. 学习路线建议

1. **先跑通 mini_compositor**：`g++ -std=c++17 doc/mini_compositor.cpp -o mini_compositor && ./mini_compositor`，看懂"排序→多态 compose→present"与引用计数/原子脏标记。
2. **对照真实启动链三环**：`onFirstRef`(搭泵) → `init`(装配 HWC/RE/Scheduler) → `run`(死等)，理解 VSync 是怎么把 mini 里的"标脏→合成"驱动起来的。
3. **进单帧**：从 `run` 跳到 `onMessageReceived`→`handleMessageRefresh`→`rebuildLayerStacks`→`doComposition`，把 mini 的 `compositeFrame` 映射进去。
4. **改实验**（参考 `sf_modification_playbook.md`）：先做零风险的 dump 子命令扩展（层面 C），再碰 Binder 接口（层面 B），最后才动 HWC 策略（层面 A3）。

---

## 附：启动链时序（简化）

```
init 进程拉起 surfaceflinger
  └─ main()
      ├─ OtherSystemServiceLoopRun / signal / RPC / Allocator / Binder池
      ├─ createSurfaceFlinger()  ──▶ onFirstRef()  [mEventQueue->init]
      ├─ 优先级/调度/cpuset 设置
      ├─ init()
      │    ├─ Scheduler + EventThread(app/sf)
      │    ├─ RenderEngine
      │    ├─ HWComposer + registerCallback(this)
      │    ├─ processDisplayHotplugEventsLocked → 默认 display
      │    ├─ initializeDisplays()（点亮屏）
      │    └─ StartPropertySetThread + 刷新率回调
      ├─ addService("SurfaceFlinger")
      ├─ startDisplayService()
      ├─ sched_setscheduler(SCHED_FIFO, 2)
      └─ run()  ──▶ do { waitForEvent(); } while(true)
                      │  ◀── VSync 唤醒
                      └─▶ onMessageReceived → handleMessageRefresh（一帧）
```
