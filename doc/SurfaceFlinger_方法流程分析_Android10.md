# SurfaceFlinger 方法与流程分析（Android 10 / cells-android10）

> 源文件：`frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp`（约 6400 行）
> 配套：`main_surfaceflinger.cpp`、`Scheduler/MessageQueue.cpp`、`Scheduler/EventThread.cpp`、`Layer.cpp/h`、`DisplayDevice.cpp/h`

---

## 一、总览：三类方法

`SurfaceFlinger.cpp` 的方法按角色分三层：

1. **生命周期与主循环**：构造（285）、`onFirstRef`（408）、`init`（621）、`run`（1493）、`waitForEvent`（1460）、`onMessageReceived`（1759）。
2. **一帧的两个阶段**：INVALIDATE 阶段的 `handleMessageTransaction` / `handleTransaction` / `handleTransactionLocked` / `handleMessageInvalidate` / `handlePageFlip`；REFRESH 阶段的 `handleMessageRefresh` / `preComposition` / `rebuildLayerStacks` / `calculateWorkingSet` / `beginFrame` / `prepareFrame` / `doComposition` / `postFrame` / `postComposition`。
3. **外部入口与回调**：Binder 侧的 `setTransactionState` / `applyTransactionState` / `setClientStateLocked`；HWC2 回调 `onVsyncReceived` / `onHotplugReceived` / `onRefreshReceived`。

贯穿全局的两个状态对象与一把锁：

- `mCurrentState`：客户端写入的"当前状态"。
- `mDrawingState`：本帧实际用于合成的"绘制状态"。
- `mStateLock`：保护上述状态；外加一组事务标志位 `eTransactionNeeded`、`eTraversalNeeded`、`eDisplayTransactionNeeded`、`eTransactionFlushNeeded`。

---

## 二、启动流程

`main_surfaceflinger.cpp` 的 `main()` 是全进程入口，顺序如下：

```
OtherSystemServiceLoopRun()            （本项目定制，见第八节）
signal(SIGPIPE, SIG_IGN)
配置 HIDL 线程池
启动 Graphics Allocator HAL（V3 失败回退 V2）
限制 Binder 线程池为 4 并启动
createSurfaceFlinger()
setpriority(PRIORITY_URGENT_DISPLAY) / set_sched_policy(SP_FOREGROUND) / set_cpuset_policy(SP_SYSTEM)
flinger->init()                        （必须在客户端能连接之前完成）
ServiceManager::addService（DUMP_FLAG_PRIORITY_CRITICAL | DUMP_FLAG_PROTO）
注册 DisplayService
主线程设 SCHED_FIFO，优先级 2
flinger->run()                         （主线程从此成为 SF 主循环，不再返回）
```

构造函数只做属性与配置读取：`hasSyncFramework`、`dispSyncPresentTimeOffset`、`useHwcForRgbToYuv`、`maxFrameBufferAcquiredBuffers`、`hasWideColorDisplay`、`useColorManagement`、`useContextPriority`、默认合成 Dataspace/PixelFormat、`primary_display_orientation`，以及一批 `debug.sf.*` 调试开关（`showupdates`、`disable_backpressure`、`enable_hwc_vds`、`luma_sampling`、`max_igbp_list_size` 等）。末尾 `mVsyncModulator.setPhaseOffsets(early, gl, late, threshold)` 建立 app/SF 相位偏移。

`onFirstRef()` 只做一件事：`mEventQueue->init(this)`，创建 `Looper(true)` 与 `Handler`。

### 2.1 onFirstRef 的触发机制（RefBase 引用计数）

`onFirstRef()` 不是业务代码显式调用的，而是由 **RefBase 的引用计数在"对象第一次被强指针 `sp<>` 持有"时自动回调**（所有 `RefBase` 子类如 `Layer`、`DisplayDevice`、`BBinder` 都遵循同一规则）。`weakref_impl` 构造时 `mStrong` 初值是 `INITIAL_STRONG_VALUE`（`1<<28`，`RefBase.cpp:159`），因此"第一次 `incStrong`"可被识别：

```411:430:system/core/libutils/RefBase.cpp
void RefBase::incStrong(const void* id) const
{
    weakref_impl* const refs = mRefs;
    refs->incWeak(id);
    refs->addStrongRef(id);
    const int32_t c = refs->mStrong.fetch_add(1, std::memory_order_relaxed);
    ...
    if (c != INITIAL_STRONG_VALUE)  {
        return;
    }
    int32_t old __unused = refs->mStrong.fetch_sub(INITIAL_STRONG_VALUE, std::memory_order_relaxed);
    ALOG_ASSERT(old > INITIAL_STRONG_VALUE, "0x%x too small", old);
    refs->mBase->onFirstRef();
}
```

强引用计数从 `INITIAL_STRONG_VALUE` 跳到真实计数 1 的那一次 `incStrong` 会回调 `onFirstRef()`；此后所有 `incStrong`（c 已是 1、2、3…）直接 `return`，因此**每个对象生命周期内只调用一次**。（例外：`forceIncStrong` 在 `c==0` 时也会回调，用于 wp 提升/resurrect 场景，见 `RefBase.cpp:480-487`。）

**SurfaceFlinger 的实际调用链**：

```
main_surfaceflinger.cpp:113  sp<SurfaceFlinger> flinger = surfaceflinger::createSurfaceFlinger();
    → SurfaceFlingerFactory.cpp:142   return new SurfaceFlinger(factory);
        → 裸指针隐式转成返回类型 sp<SurfaceFlinger>
            → StrongPointer.h:141-145  sp<T>::sp(T*) : m_ptr(other) { other->incStrong(this); }
                → RefBase::incStrong()  → SurfaceFlinger::onFirstRef()
```

`sp<T>` 的构造函数会立即对非空指针调 `incStrong(this)`（`StrongPointer.h:140-145`），正是这一步触发了 `onFirstRef()`。

**确切时机**：在 `SurfaceFlinger` 构造函数（285-283 行，末尾 `mEventQueue(mFactory.createMessageQueue())`）**执行完毕之后**——即 `createSurfaceFlinger()` 返回、裸指针被 `sp` 接住的那一刻。在 `main()` 中的相对顺序是：**构造函数 → `onFirstRef()`（创建 Looper/Handler）→ `flinger->init()`（621，建 Scheduler、EventThread、HWComposer、RenderEngine…）→ `addService` → `flinger->run()`（1493）**。所以 `onFirstRef` 早于 `init`，更远早于主循环。

**为什么不放进构造函数**：其一，`mEventQueue->init(this)` 要把 `this` 交给 `MessageQueue` 内部的 `Looper`/`Handler` 长期持有，构造函数尚未返回时发布自身指针不安全；其二，在构造函数里若对 `this` 取强引用（`sp<SurfaceFlinger>(this)`），引用计数归零时会 `delete this`，而对象还没构造完——`onFirstRef` 的语义正是"对象已完整构造且已被至少一个强引用持有"，是做这类初始化的唯一安全窗口。

对称地，最后一次 `decStrong` 使计数降到 0 时回调 `onLastStrongRef()`，随后在 `OBJECT_LIFETIME_STRONG` 模式下 `delete this`（`RefBase.cpp:442-450`）。

`init()`（621-776）顺序严格：

1. 创建 `Scheduler`（带 `setPrimaryVsyncEnabled` 回调与 `getVsyncPeriod` 重同步回调）。
2. 建立两条 EventThread 连接：`app`（应用 VSYNC）与 `sf`（合成 VSYNC，各自带相位偏移）。
3. `mEventQueue->setEventConnection(mScheduler->getEventConnection(mSfConnectionHandle))` —— 把 SF 的 VSYNC 接到主线程 Looper。
4. `mVsyncModulator.setSchedulerAndHandles`。
5. 启动 `RegionSamplingThread`。
6. 创建 `RenderEngine`（特性位由色彩管理/高优先级上下文/保护内容决定）。
7. 创建并注册 `HWComposer`，`registerCallback(this, sequenceId)`；`processDisplayHotplugEventsLocked()` 处理初始热插拔；`LOG_ALWAYS_FATAL_IF` 断言内屏存在且已连接。
8. 可选创建 `VrFlinger`。
9. `mDrawingState = mCurrentState` 并 `initializeDisplays()`。
10. `getRenderEngine().primeCache()` 预热着色器。
11. 启动 `StartPropertySetThread`（带 `presentFenceReliable` 参数）。
12. 注册刷新率变更/当前刷新率查询/VSYNC 周期三类回调。
13. `mRefreshRateConfigs.populate(getHwComposer().getConfigs(...))` 填充刷新率表。

---

## 三、主循环与消息驱动

```1493:1497:frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
void SurfaceFlinger::run() {
    do {
        waitForEvent();
    } while (true);
}
```

`waitForEvent()` 即 `mEventQueue->waitMessage()`，本质是 `mLooper->pollOnce(-1)` 永久阻塞。

驱动主线程的有三类来源：

**一是 VSYNC**。`EventThread`（sf 连接）通过 BitTube 把 `DISPLAY_EVENT_VSYNC` 送到 Looper，`MessageQueue::eventReceiver()` 读到后调 `mHandler->dispatchInvalidate()`，在 Looper 上投递 `INVALIDATE` 消息。

**二是进程内主动触发**。`signalLayerUpdate()`（1470，新帧排队、backpressure 重排）与 `signalTransaction()`（1464，事务到达）都会先 `mScheduler->resetIdleTimer()`，再 `mEventQueue->invalidate()`，后者本质是 `mEvents->requestNextVsync()` —— 即**请求下一个 VSYNC**，而不是立刻发消息。而 `signalRefresh()`（1474）设置 `mRefreshPending` 后 `mEventQueue->refresh()`，直接向 Looper 投递 `REFRESH` 消息，不经过 VSYNC。

**三是 `postMessageAsync` / `postMessageSync`**，把任意 lambda 抛到主线程执行（同步版本会 `msg->wait()`）。

于是 `onMessageReceived(int32_t what)`（1759）只有两个分支，构成 Android 10 经典的 **INVALIDATE → REFRESH 两段式**一帧。

---

## 四、INVALIDATE 阶段：事务 + 取帧

`case INVALIDATE`（1762-1831）的执行顺序：

1. `populateExpectedPresentTime()`：缓存本帧预期 present 时间（若 SF 相位指向下一个 VSYNC，则再加一个周期），保证本帧所有图层看到同一参考时间。
2. `previousFrameMissed(graceTimeForPresentFenceMs)`：用 `mPreviousPresentFences[0/1]` 判断上一帧 present fence 是否仍未 signal（可选 1ms 宽限等待），累加 `mFrameMissedCount` / `mHwcFrameMissedCount` / `mGpuFrameMissedCount` 并写 ATRACE。
3. 若开启 backpressure（`mPropagateBackpressure`）且确认丢帧，直接 `signalLayerUpdate()` 重排并 `break` —— **本帧放弃合成**，把压力回传给应用。
4. `mUseSmart90ForVideo` 时 `mScheduler->updateFpsBasedOnContent()` 做视频帧率探测。
5. `performSetActiveConfig()` 处理待生效的刷新率切换，成功则本帧结束。
6. `updateVrFlinger()`（1641，必要时整体换 HWC 实例）。
7. 两个核心调用：

```1818:1824:frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
            bool refreshNeeded = handleMessageTransaction();
            refreshNeeded |= handleMessageInvalidate();

            updateCursorAsync();
            updateInputFlinger();

            refreshNeeded |= mRepaintEverything;
```

### handleMessageTransaction()（1843）

先 `peekTransactionFlags()` 原子读标志、`flushTransactionQueues()` 刷新延迟事务队列，然后判断
`transactionFlags && (flags != eTransactionFlushNeeded || flushedATransaction)`，成立才调 `handleTransaction(eTransactionMask)`，否则只 `getTransactionFlags(eTransactionFlushNeeded)` 清标志，避免反复唤醒。

### handleTransaction()（2668）

一个易被忽略的细节：先在锁外拷贝一份 `State drawingState(mDrawingState)`，注释说明是为了让 `State` 赋值的副作用（析构旧状态）**不在持有 `mStateLock` 时发生**，避免死锁。然后加锁 → `mVsyncModulator.onTransactionHandled()` → `getTransactionFlags(eTransactionMask)`（读并清零）→ `handleTransactionLocked(flags)` → `invalidateHwcGeometry()`。

### handleTransactionLocked()（2958）

事务真正的落地点，五步：

1. `mCurrentState.traverseInZOrder` 通知所有 layer 可用帧（`notifyAvailableFrames`）。
2. 若有 `eTraversalNeeded`：逐 layer `doTransaction(0)`，返回标志带 `eVisibleRegion` 则置 `mVisibleRegionsDirty`，带 `eInputInfoChanged` 则置 `mInputInfoChanged`。
3. 若有 `eDisplayTransactionNeeded`：`processDisplayChangesLocked()` + `processDisplayHotplugEventsLocked()`。
4. 若显示层栈变化：遍历所有 layer 重算 transform hint。注释解释：必须放在 `rebuildLayerStacks` 之前，因为要在 acquire buffer 之前定好 hint。
5. 处理 `mLayersAdded` / `mLayersRemoved`（置脏区、对移除的 layer `invalidateLayerStack`）。
6. `commitInputWindowCommands()` + `commitTransaction()`。

### commitTransaction()（3178）

处理待移除 layer（记录 buffering 统计、必要时 `latchAndReleaseBuffer()` 释放 buffer、无父节点者放入 `mOffscreenLayers`）；`mAnimCompositionPending = mAnimTransactionPending`；在 `withTracingLock` 内执行 **`mDrawingState = mCurrentState`**（这就是"提交"的本质）、`commitChildList()`、`commitOffscreenLayers()`；最后 `mTransactionPending = false` 并 `mTransactionCV.broadcast()` 唤醒等待同步事务的客户端。

### handleMessageInvalidate()（1933）与 handlePageFlip()（3410）

`handlePageFlip()` 先在锁外遍历 `mDrawingState` 收集 `hasReadyFrame()` 的 layer，用 `layer->shouldPresentNow(expectedPresentTime)` 判断是否该在本帧呈现：命中者进入 `mLayersWithQueuedFrames`，未命中者 `useEmptyDamage()`。注释解释了为什么要先快照集合再 latch —— 两个 producer 共享同一命令流时，边遍历边 latch 会导致互相等待的死锁。

随后持 `mStateLock` 对每个 layer `latchBuffer(visibleRegions, latchTime)`，成功者进 `mLayersPendingRefresh`。若"有排队帧但这一帧没 latch 到"，`signalLayerUpdate()` 下一帧再来。首次 latch 成功会把 `mBootStage` 从 `BOOTLOADER` 推进到 `BOOTANIMATION`。返回值是 `!mLayersWithQueuedFrames.empty() && newDataLatched`。

回到 `handleMessageInvalidate()`：若 `mVisibleRegionsDirty` 则 `computeLayerBounds()`；对 `mLayersPendingRefresh` 中每个 layer 调 `invalidateLayerStack(layer, visibleReg)` 把脏区写到对应显示设备。

最后 `updateCursorAsync()`（异步更新鼠标位置）与 `updateInputFlinger()`（3093）：后者在 `mVisibleRegionsDirty || mInputInfoChanged` 时调 `updateInputWindowInfo()`，否则若请求了 `syncInputWindows` 则立刻 `setInputWindowsFinished()`。

若 `refreshNeeded` 且不在 BOOTLOADER 阶段，就 `signalRefresh()`。

---

## 五、REFRESH 阶段：真正的合成

```1886:1908:frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
    const bool repaintEverything = mRepaintEverything.exchange(false);
    preComposition();
    rebuildLayerStacks();
    calculateWorkingSet();
    for (const auto& [token, display] : mDisplays) {
        beginFrame(display);
        prepareFrame(display);
        doDebugFlashRegions(display, repaintEverything);
        doComposition(display, repaintEverything);
    }

    logLayerStats();
    postFrame();
    postComposition();
```

**`preComposition()`（2105）**：记录 `mRefreshStartTime`，遍历调 `layer->onPreComposition()`，若任一 layer 需要额外 invalidate 则 `signalLayerUpdate()`。

**`rebuildLayerStacks()`（2344）**：只在 `mVisibleRegionsDirty` 时真正工作。先 `invalidateHwcGeometry()`；对每个显示设备 `computeVisibleRegions()`（3260，自顶向下遍历，累加 `aboveOpaqueLayers` / `aboveCoveredLayers`，算出每个 layer 的 visible/covered/transparent region 与屏幕脏区）；再按 Z 序遍历所有 layer，用 `display->belongsInOutput(...)` 判断是否属于该屏，把可见者 `getOrCreateOutputLayer()` 装配成 `OutputLayer`；同时算出 `undefinedRegion`（未被不透明区域覆盖的部分）并入 `dirtyRegion`。

**`calculateWorkingSet()`（1956）**：Android 10 新增的一层。若 `mGeometryInvalid`，为每个 output layer 分配 `z`、重置 `forceClientComposition`、调 `updateCompositionState(true)` 与 `writeStateToHWC(true)` 把几何状态写给 HWC；然后处理色彩（colorMatrix、`pickColorMode` 选 ColorMode/Dataspace/RenderIntent），并对 HDR（PQ/HLG 且 profile 不支持）、Y410、圆角等情况强制 client 合成，其余 layer 调 `setPerFrameData()` 写入每帧数据。

**逐屏四步**：

| 方法 | 行号 | 作用 |
|---|---|---|
| `beginFrame` | 2532 | `mustRecompose = dirty && !(empty && wasEmpty)`，调 RenderSurface 的 `beginFrame` |
| `prepareFrame` | 2564 | `getRenderSurface()->prepareFrame()` |
| `doDebugFlashRegions` | 2061 | 仅 `mDebugRegion` 开启时闪脏区并 `postFramebuffer` |
| `doComposition` | 2577 | 取脏区 → `doDisplayComposition()` → 清脏区 → `flip()` |

`doDisplayComposition()`（3485）内部走 `doComposeSurfaces()`（3505）：`dequeueBuffer` → 用 RenderEngine 渲染所有 CLIENT 合成层 → `queueBuffer(readyFence)`。

**`postFramebuffer()`（2609）** 是本帧与硬件交接的关键：`getHwComposer().presentAndGetReleaseFences(*displayId)` 触发 HWC present；然后对每个 output layer 取 HWC 的 release fence（若上一帧是 CLIENT 合成，还要与 client target acquire fence 合并），调 `layerFE.onLayerDisplayed(releaseFence)` 把 buffer 还回应用；最后 `clearReleaseFences`。

**`postFrame()`（2597）** 做周期性帧统计日志。

**`postComposition()`（2180）**：释放被替换的 buffer（`releasePendingBuffer(dequeueReadyTime)`）；记录 GL 合成完成 fence 与 present fence；更新 `mPreviousPresentFences`；更新 compositor timing（`updateCompositorTiming` / `setCompositorTimingSnapped` 把 composite→present 延迟"对齐"到 VSYNC 整数倍以消除抖动）；上报 TimeStats 与 transaction 回调。

末尾统计本帧是否包含 client/device 合成，交 `mVsyncModulator.onRefreshed(mHadClientComposition)` 调整下一帧的 SF/App 相位，并清空 `mLayersWithQueuedFrames`。

---

## 六、方法索引表

| 方法 | 行号 | 说明 |
|---|---|---|
| `SurfaceFlinger(factory)` | 285 | 属性/配置读取 |
| `onFirstRef` | 408 | `mEventQueue->init(this)` |
| `init` | 621 | 全流程初始化 |
| `run` / `waitForEvent` | 1493 / 1460 | 主循环 |
| `signalTransaction` / `signalLayerUpdate` / `signalRefresh` | 1464 / 1469 / 1474 | 唤醒源 |
| `postMessageAsync` / `postMessageSync` | 1479 / 1484 | 主线程抛消息 |
| `onVsyncReceived` | 1515 | HWC VSYNC → DispSync 采样 |
| `onHotplugReceived` | 1574 | 热插拔 |
| `onRefreshReceived` | 1600 | HWC 请求整屏重绘 |
| `setPrimaryVsyncEnabled` | 1608 | 硬件 VSYNC 开关 |
| `updateVrFlinger` | 1641 | VR 合成器切换 |
| `previousFrameMissed` / `populateExpectedPresentTime` | 1724 / 1745 | 丢帧判定 / 期望 present 时间 |
| `onMessageReceived` | 1759 | INVALIDATE / REFRESH 分发 |
| `handleMessageTransaction` | 1843 | 事务处理入口 |
| `handleMessageRefresh` | 1879 | 一帧合成总入口 |
| `handleMessageInvalidate` | 1933 | 取帧入口 |
| `calculateWorkingSet` | 1956 | Z 序 / 合成类型 / 写 HWC |
| `doDebugFlashRegions` | 2061 | 调试闪脏区 |
| `preComposition` | 2105 | 合成前准备 |
| `postComposition` | 2180 | 合成后收尾 |
| `rebuildLayerStacks` | 2344 | 重建可见层栈 |
| `getBestDataspace` / `pickColorMode` | 2439 / 2482 | 色彩选择 |
| `beginFrame` / `prepareFrame` | 2532 / 2564 | 起帧 / 准备帧 |
| `doComposition` / `postFrame` / `postFramebuffer` | 2577 / 2597 / 2609 | 合成 / 统计 / 提交 |
| `handleTransaction` | 2668 | 事务外层（锁外拷贝 State） |
| `processDisplayHotplugEventsLocked` | 2696 | 热插拔落库 |
| `setupNewDisplayDeviceInternal` | 2739 | 创建 DisplayDevice |
| `processDisplayChangesLocked` | 2816 | 显示增删变更 |
| `handleTransactionLocked` | 2958 | 事务真正落地 |
| `updateInputFlinger` / `updateInputWindowInfo` | 3093 / 3113 | 输入窗口同步 |
| `commitInputWindowCommands` | 3152 | 提交输入命令 |
| `latchAndReleaseBuffer` | 3170 | latch 后释放 |
| `commitTransaction` | 3178 | `mDrawingState = mCurrentState` |
| `withTracingLock` / `commitOffscreenLayers` | 3228 / 3248 | 追踪同步 / 离屏层提交 |
| `computeVisibleRegions` / `invalidateLayerStack` | 3260 / 3401 | 可见区 / 脏区 |
| `handlePageFlip` | 3410 | latch 新帧 |
| `doDisplayComposition` / `doComposeSurfaces` | 3485 / 3505 | GPU 合成 |
| `setTransactionState` | 3850 | Binder 事务入口 |
| `applyTransactionState` | 3898 | 事务应用 |
| `setDisplayStateLocked` | 4013 | 显示状态 |
| `setClientStateLocked` | 4072 | 图层状态 |

---

## 七、客户端事务入口

应用 / WMS 通过 Binder 调 `setTransactionState()`（3850）：先检查权限与非法状态；若带 `eAnimation` 标志且该 `applyToken` 已有排队事务，在 `mTransactionCV` 上等待（最多 5 秒）前序动画帧被应用；若仍需排队（已有 pending 或 `!transactionIsReadyToBeApplied(desiredPresentTime, states)`），压入 `mTransactionQueues[applyToken]` 并置 `eTransactionFlushNeeded` 后返回 —— 这就是**延迟/定时事务**；否则直接 `applyTransactionState()`（3898）。

`applyTransactionState()` 依次：

1. `setDisplayStateLocked()`（4013）：处理 surface / layerStack / projection / size 变更，置 `eDisplayTransactionNeeded`。
2. 注册 listener 回调到 `mTransactionCompletedThread`。
3. 对每个 `ComposerState` 调 `setClientStateLocked()`（4072）：逐项处理 `ePositionChanged`、`eLayerChanged`、`eRelativeLayerChanged`、alpha/matrix/crop 等；改动 Z 序时还要在 `mCurrentState.layersSortedByZ` 中移除再插入，并置 `eTransactionNeeded | eTraversalNeeded`。
4. 合并 `addInputWindowCommands`；处理 uncache buffer。
5. 空但同步 / 动画的事务强制置 `eTransactionNeeded`（作为 flush 或模拟背压）。
6. 主线程调用时清掉 `eTraversalNeeded`，改置 `mTraversalNeededMainThread`。
7. `setTransactionFlags(transactionFlags, start)`（start 由 `eEarlyWakeup` 决定 EARLY / NORMAL），这才唤醒主循环。
8. 同步事务在 `mTransactionCV` 上等待 —— **仅非主线程等待**，主线程永不被阻塞。

---

## 八、HWC 回调与显示管理

`onVsyncReceived()`（1515）：校验 `sequenceId` → `getHwComposer().onVsync()` → 非内屏 VSYNC 直接忽略 → 内屏则 `mScheduler->addResyncSample(timestamp, &periodFlushed)` 喂给 DispSync 做软件 VSYNC 建模；周期被刷新时通知 `mVsyncModulator`。

`onHotplugReceived()`（1574）：把事件压入 `mPendingHotplugEvents`，用 `ConditionalLock` 避免主线程重复加锁（主线程已进入则直接 `processDisplayHotplugEventsLocked()`），并置 `eDisplayTransactionNeeded`。

`processDisplayHotplugEventsLocked()`（2696）：CONNECTED 则创建 `mPhysicalDisplayTokens` 与 `DisplayDeviceState`；DISCONNECTED 则移除；随后 `processDisplayChangesLocked()`（2816）用 `isIdenticalTo` 快速跳过无变化的情况，分别处理"移除的屏"（`disconnect()` + `dispatchDisplayHotplugEvent`）和"新增的屏"（创建 BufferQueue，物理屏走 `FramebufferSurface`，虚拟屏走 `VirtualDisplaySurface`，再由 `setupNewDisplayDeviceInternal()`（2739）组装 `DisplayDeviceCreationArgs` 并创建 DisplayDevice）。

`onRefreshReceived()`（1600）：直接 `repaintEverythingForHWC()`。

---

## 九、本项目（cells）的定制点

这份代码不是原生 AOSP，有多处"多 cell / 多容器"改造：

**1. 图层归属判定加入 cell 维度**。`belongsInOutput()` 被扩展为四参数版本，除 layerStack 外还比较 `displayDevice->getActiveSystemName()` 与 `layer->getSystemName()` 以及 `layer->getPrimaryDisplayOnly()`。调用点分布在：

- 2335：`computeLayerBounds`
- 2380：`rebuildLayerStacks`
- 3031：transform hint 计算
- 3278：`computeVisibleRegions`
- 3404：`invalidateLayerStack`

**2. 输入窗口按 cell 分发**。`updateInputWindowInfo()`（3113）被改写为按 cell 分组：定义 `MAX_CONTEXT 6`，遍历 layer 时用 `sscanf(layer->systemName(), "cell%d", &i)` 解析出 cell 编号，把 `InputWindowInfo` 分别装入 `inputHandles[i]`；i = 0 发给本进程的 inputflinger；i > 0 则通过 `OtherServiceManager(i)` 拿到**其它 cell 的 ServiceManager**，取出各自的 `inputflinger` 服务并 `setInputWindows`。也就是说每个 cell 有独立的 inputflinger 与独立的窗口信息通道。

**3. 其它**。`main()` 开头的 `OtherSystemServiceLoopRun()` 属于同一改造；`Layer` 新增 `mSystemName` / `mPrimaryDisplayOnly` 字段，`DisplayDevice` 新增 `getActiveSystemName()`。

---

## 十、调试与排障入口

- `dumpsys SurfaceFlinger` 走 `dump()`。
- `logLayerStats()`（2091）在开启 layer stats 时对主屏输出 proto 图层统计。
- `mDebugRegion`（`debug.sf.showupdates`）触发 `doDebugFlashRegions` 闪脏区。
- `mPropagateBackpressure` / `mPropagateBackpressureClientComposition` 控制丢帧回压。
- VSYNC 相关：`enableVSyncInjections`（1306）+ `injectVSync`（1341）注入。
- `getVsyncPeriod()`（1499）返回当前周期。
- `previousFrameMissed` 写出的 `FrameMissed` / `HwcFrameMissed` / `GpuFrameMissed` 三个 ATRACE 计数，是看丢帧最直接的信号。

---

## 十一、一帧时序图

```
[VSYNC]  EventThread(sf) → BitTube → MessageQueue::eventReceiver
             → dispatchInvalidate() → Looper 投递 INVALIDATE

[INVALIDATE]
  onMessageReceived(INVALIDATE)
    populateExpectedPresentTime()
    previousFrameMissed()  → 丢帧 + backpressure? → signalLayerUpdate() 并放弃本帧
    performSetActiveConfig()
    updateVrFlinger()
    handleMessageTransaction()
        flushTransactionQueues()
        handleTransaction() → handleTransactionLocked()
            doTransaction() per layer → mVisibleRegionsDirty
            processDisplayChangesLocked()
            commitInputWindowCommands() / commitTransaction()
                mDrawingState = mCurrentState
    handleMessageInvalidate()
        handlePageFlip() → latchBuffer() per layer
        computeLayerBounds()
        invalidateLayerStack()
    updateCursorAsync() / updateInputFlinger()
    → signalRefresh()

[REFRESH]
  onMessageReceived(REFRESH) → handleMessageRefresh()
    preComposition()
    rebuildLayerStacks()      → computeVisibleRegions() + 装配 OutputLayer
    calculateWorkingSet()     → Z 序 / 合成类型 / writeStateToHWC / setPerFrameData
    per display:
        beginFrame() → prepareFrame() → doDebugFlashRegions() → doComposition()
            doDisplayComposition() → doComposeSurfaces()（GPU） → queueBuffer
            postFramebuffer() → presentAndGetReleaseFences() → onLayerDisplayed()
    logLayerStats()
    postFrame() / postComposition()
    mVsyncModulator.onRefreshed(mHadClientComposition)
    mLayersWithQueuedFrames.clear()
```

---

## 十二、小结

理解这份代码的关键在三点：一是 **INVALIDATE / REFRESH 两段式**，"改状态"与"合成"严格分离，中间靠 `signalRefresh()` 衔接；二是 **双状态模型**，`mCurrentState` 收客户端写入、`mDrawingState` 用于本帧合成，`commitTransaction()` 中的那次赋值就是事务生效的瞬间；三是 **唤醒语义差别**，`signalLayerUpdate/signalTransaction` 只是请求下一个 VSYNC，而 `signalRefresh` 直接投递 REFRESH 消息。在此之上，本项目叠加了按 cell 划分显示与输入的多容器改造，凡涉及图层归属与 inputflinger 分发的地方都需要留意。
