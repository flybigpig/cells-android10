# SurfaceFlinger 合成流程详解（Android 10）

本文基于仓库 `c:\D\android_project\cells-android10`（AOSP android-10.0.0_r*）源码，以经典博文《SurfaceFlinger 合成流程详解》的框架为主线，逐段校正 Android 10 实际实现，重点标注相比 Android 8.1 的架构变化（最显著的是 **CompositionEngine** 的引入）。所有函数位置均给出 `文件:行号` 便于对照源码阅读。

---

## 一、总体框架：两个消息驱动的主循环

SurfaceFlinger 运行在独立进程，主线程通过 `MessageQueue` 接收两类消息驱动合成：

- `MessageQueue::INVALIDATE`：处理事务（transaction）、获取新 Buffer（page flip）、判断是否需要刷新。
- `MessageQueue::REFRESH`：执行真正的一帧合成与上屏。

入口统一在 `SurfaceFlinger::onMessageReceived`（`frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp:1813`）。相比 8.1，Android 10 在该函数开头新增了 `populateExpectedPresentTime()` 缓存本帧预期的 present 时间，以及 Backpressure 判定（`mPropagateBackpressure`），用于丢弃迟到帧与统计 `FrameMissed`/`HwcFrameMissed`/`GpuFrameMissed`（见 `SurfaceFlinger.cpp:1823-1887`）。

```cpp
void SurfaceFlinger::onMessageReceived(int32_t what) {
    switch (what) {
        case MessageQueue::INVALIDATE: {
            populateExpectedPresentTime();
            ...
            bool refreshNeeded = handleMessageTransaction();   // 提交事务
            refreshNeeded |= handleMessageInvalidate();        // latch 新 buffer
            updateCursorAsync();
            updateInputFlinger();
            refreshNeeded |= mRepaintEverything;
            if (refreshNeeded && CC_LIKELY(mBootStage != BootStage::BOOTLOADER)) {
                signalRefresh();                               // 触发 REFRESH 消息
            }
            break;
        }
        case MessageQueue::REFRESH: {
            handleMessageRefresh();                            // 真正合成
            break;
        }
    }
}
```

注意：INVALIDATE 阶段若判定需要刷新，会调用 `signalRefresh()` 往队列再投一个 REFRESH 消息，因此一次 VSync 信号通常会连续走 INVALIDATE → REFRESH 两段。

---

## 二、INVALIDATE 阶段：事务提交与 Buffer 获取

### 2.1 handleMessageTransaction（提交窗口事务）

`SurfaceFlinger::handleMessageTransaction`（`SurfaceFlinger.cpp:1924`）调用 `flushTransactionQueues()` 取出应用通过 `setTransactionState` 提交的事务，再 `handleTransaction()` 计算各 Layer 的可见区域、Z 序、变换矩阵等。若几何发生变化，会置 `mVisibleRegionsDirty=true`，后续 `rebuildLayerStacks` 将重建可见 Layer 列表。

### 2.2 handleMessageInvalidate（Latch 新 Buffer）

`SurfaceFlinger::handleMessageInvalidate`（`SurfaceFlinger.cpp:1984`）核心是 `handlePageFlip()`：遍历有 queued frame 的 Layer，调用 `latchBuffer` 把 BufferQueue 中最新可用的 GraphicBuffer 锁定为当前帧，并取出该 Buffer 的 acquire fence。若 `mVisibleRegionsDirty`，还会 `computeLayerBounds()` 重新计算 Layer 屏幕边界。

### 2.3 何时进入 REFRESH

回到 `onMessageReceived`：只要 `handleMessageTransaction` 或 `handleMessageInvalidate` 返回“需要刷新”，或 `mRepaintEverything` 为真，就 `signalRefresh()`。

---

## 三、REFRESH 阶段：handleMessageRefresh 主流程

`SurfaceFlinger::handleMessageRefresh`（`SurfaceFlinger.cpp:1946`）是整个合成的核心，按固定顺序执行：

```cpp
void SurfaceFlinger::handleMessageRefresh() {
    mRefreshPending = false;
    const bool repaintEverything = mRepaintEverything.exchange(false);

    preComposition();            // 1. 预合成：询问各 layer 是否需要额外 invalidate
    rebuildLayerStacks();        // 2. 重建每个 display 的可见 layer 列表（z序）
    calculateWorkingSet();       // 3. Android 10 新增：计算合成工作集，写 HWC 状态

    for (const auto &[token, display] : mDisplays) {
        beginFrame(display);     // 4. 开始一帧（EGL 上下文/render surface 准备）
        prepareFrame(display);   // 5. 通知 HWC 本帧合成类型（GLES/HWC/MIXED）
        doDebugFlashRegions(display, repaintEverything);
        doComposition(display, repaintEverything); // 6. 真正合成并上屏
    }

    logLayerStats();
    postFrame();
    postComposition();           // 7. 释放旧 buffer、处理 release fence

    // 记录本帧是否包含 client/device 合成，供下一帧 Backpressure 判定
    mHadClientComposition = ...; mHadDeviceComposition = ...;
    mVsyncModulator.onRefreshed(mHadClientComposition);
    mLayersWithQueuedFrames.clear();
}
```

相比 8.1 的 `handleMessageRefresh`，Android 10 在遍历 display 前**新增了 `calculateWorkingSet()` 调用**（见 3.3），并把“向 HWC 写 layer 状态”提前到帧循环外统一处理，这是 CompositionEngine 重构的结果。

### 3.1 preComposition

`SurfaceFlinger::preComposition`（`SurfaceFlinger.cpp:2153`）记录 `mRefreshStartTime`，然后按 Z 序遍历 `mDrawingState` 中每个 Layer 调用 `onPreComposition()`。若某 Layer（例如带有动画或持续绘制的内容）返回 `true`，则 `needExtraInvalidate=true`，函数末尾 `signalLayerUpdate()` 请求下一帧继续刷新（典型如视频、动画层）。

### 3.2 rebuildLayerStacks（Android 10 的 CompositionEngine 版本）

`SurfaceFlinger::rebuildLayerStacks`（`SurfaceFlinger.cpp:2391`）在 `mVisibleRegionsDirty` 为真时执行。与 8.1 直接维护 `mVisibleLayersSortedByZ` 不同，Android 10 通过 `compositionengine::Output` 管理每屏的可见层：

1. `computeVisibleRegions()` 计算每个 Layer 的可见区、不透明区、脏区。
2. 遍历 `mDrawingState` 中每个 Layer，通过 `display->belongsInOutput()` 判断该 Layer 是否属于当前 display 的 layerStack，并且 `drawRegion` 非空。
3. 命中则 `display->getOrCreateOutputLayer(displayId, compositionLayer, layerFE)` 取（或建）对应的 `compositionengine::OutputLayer`，按 Z 序加入 `layersSortedByZ`。
4. 设置 `outputLayerState.visibleRegion`。

最终 `display->setOutputLayersOrderedByZ(std::move(layersSortedByZ))`（`SurfaceFlinger.cpp:2464`）保存排好序的 OutputLayer 列表，并维护 `undefinedRegion`（未被任何不透明层覆盖的区域，用于壁纸/背景）。

> 架构差异：8.1 中 SurfaceFlinger 直接持有 `mVisibleLayersSortedByZ`/`mDrawingState`；Android 10 把“某 display 上参与合成的输出层集合”抽象为 `compositionengine::Output`，Layer 成为 `compositionengine::LayerFE`（Front End），合成状态封装在 `OutputLayerCompositionState`。这是 10 引入 CompositionEngine 的核心解耦。

### 3.3 calculateWorkingSet（Android 10 新增）

`SurfaceFlinger::calculateWorkingSet`（`SurfaceFlinger.cpp:2004`）是 8.1 没有的步骤，负责在合成前把 Layer 的几何/合成状态写入 HWC：

- 若 `mGeometryInvalid`：遍历每个 display 的 `getOutputLayersOrderedByZ()`，对每个 `OutputLayer` 设置 `forceClientComposition`（当无 hwc 能力、或 `mDebugDisableHWC`/`mDebugRegion` 时强制走 GPU），按简单计数器分配 `compositionState.z`（Z 序）；然后 `layer->getLayerFE().latchCompositionState(...)` 拉取前端合成状态，`layer->updateCompositionState(true)` 重算几何，最后 `layer->writeStateToHWC(true)` 把 layer 状态写入 HWC。
- 设置逐帧数据：颜色矩阵（`colorMatrixChanged`）、颜色管理（`useColorManagement` 时 `pickColorMode` + `setColorMode` + `targetDataspace`）、HDR 数据空间（`getBestDataspace`）。

这一步把“决定哪些层走 HWC Overlay、哪些走 GPU Client 合成”的判定与状态下发集中处理，是 8.1 中散落在 `rebuildLayerStacks`/`prepareFrame` 里的逻辑上移统一。

### 3.4 beginFrame / prepareFrame

- `SurfaceFlinger::beginFrame`（`SurfaceFlinger.cpp:2580`）：调用 `display->getRenderSurface()->beginFrame()`，准备 EGL/GPU 渲染上下文（针对 client 合成部分）。
- `SurfaceFlinger::prepareFrame`（`SurfaceFlinger.cpp:2612`）：委托 `display->getRenderSurface()->prepareFrame()`（`CompositionEngine/src/RenderSurface.cpp:113`），后者根据 `hasClientComposition` / `hasDeviceComposition` 计算出合成类型，调用 `DisplaySurface::prepareFrame()` 并传 `COMPOSITION_GLES` / `COMPOSITION_HWC` / `COMPOSITION_MIXED` 三种枚举之一（`RenderSurface.cpp:138`）。这取代了 8.1 中直接调用 `mDisplaySurface->prepareFrame(compositionType)` 的写法，多了一层 RenderSurface 抽象。

### 3.5 doComposition

`SurfaceFlinger::doComposition`（`SurfaceFlinger.cpp:2625`）：

1. `display->getDirtyRegion(repaintEverything)` 取得脏区。
2. `doDisplayComposition(displayDevice, dirtyRegion)`：若需要 GPU 合成（client composition），用 GLES 把各 Layer 绘制到 framebuffer（通过 `plientComposition`/`doComposeSurfaces`）。
3. `display->getRenderSurface()->flip()` 提交绘制结果。
4. `postFramebuffer(displayDevice)`：调用 `getHwComposer().presentAndGetReleaseFences(*displayId)` 让 HWC 真正上屏，并处理 release fence 归还给 Layer（见 3.7）。

---

## 四、HWC 协商：prepare / validate / present

Android 10 仍通过 `HWComposer`（HWC2 接口）与硬件合成器交互，关键函数位于 `DisplayHardware/HWComposer.cpp`：

- `HWComposer::setClientTarget`（`HWComposer.cpp:390`）：把 GPU 合成出的 framebuffer 作为“client target”传给 HWC，供其在 MIXED 合成模式下与 overlay 层叠加。
- `HWComposer::prepare`（`HWComposer.cpp:402`）：接收 `compositionengine::Output`，遍历 OutputLayer 把每个 layer 的属性（buffer、dataspace、transform、可见区、合成类型等）写入 HWC 命令队列，发起 `validate`。HWC 返回每个 layer 的最终合成方式（`HWC` overlay 或 `CLIENT` GPU）。`forceClientComposition` 的层会被强制标记为 CLIENT。
- `HWComposer::presentAndGetReleaseFences`（`HWComposer.cpp:560`）：提交本帧并取回每个 layer 的 release fence，表示 HWC 不再需要该 buffer，可归还给 BufferQueue 生产者。

> 合成类型判定：在 `calculateWorkingSet` + `prepare` 阶段确定。若某 layer 因尺寸/格式/混合/数量超限无法走 overlay，`prepare` 返回值会要求 SurfaceFlinger 用 GPU 合成该层（CLIENT）。真实 dump 中全部可见层走 CLIENT，即本设备 HWC 未给这些层分配 overlay（可能与 `FORCE_HWC_FOR_RBG_TO_YUV` 或厂商 SDM 配置有关，详见末尾“结合真实 dump 的观察”）。

---

## 五、postComposition：Buffer 回收与 fence 处理

`SurfaceFlinger::postComposition`（`SurfaceFlinger.cpp:2227`）在每帧末尾：

1. 遍历 `mLayersWithQueuedFrames`，调用 `layer->releasePendingBuffer(dequeueReadyTime)`，把被本帧替换下来的旧 buffer 释放（使其回到 BufferQueue 的 FREE 状态，生产者可重新 dequeue）。
2. 处理 GL 合成完成 fence（`glCompositionDoneFenceTime`）与 present fence，更新 `mCompositorTiming`，供后续帧的 present 时间计算与抖动消除（`updateCompositorTiming` / `setCompositorTimingSnapped`）。
3. 把各 Layer 的 release fence 通过 `layer->onFrameAvailable`/相关接口送回，形成“生产者—BufferQueue—SurfaceFlinger—HWC—release fence—BufferQueue”的完整闭环。

---

## 六、VSync 节拍与调度器（Scheduler）

Android 10 把 VSync 调度独立为 `Scheduler` 模块（`Scheduler/` 目录）：

- `DispSync`：软件模拟 VSync，对齐硬件 VSync 信号并分发到 app（SF 的 `EventThread` 与应用的 `EventThread`）。
- `EventThread`：事件线程，向监听者（SurfaceFlinger 主线程、应用渲染线程）投递 VSync 回调。SurfaceFlinger 在 VSync 到来时向 `MessageQueue` 投递 INVALIDATE/REFRESH。
- `PhaseOffsets`：配置 SF 与 app 各自的相位偏移（SF 合成需早于 present 一个 offset），`setCompositorTimingSnapped` 即用其计算 deadline。
- `VsyncReactor` / `LayerHistory`：Android 10 新增，用于根据内容（如视频）动态调节刷新率（`mUseSmart90ForVideo` 在 `onMessageReceived` 中调用 `updateFpsBasedOnContent()`）。

---

## 七、结合真实 dumpsys SurfaceFlinger 的观察

对照仓库中 `dumps_surfaceflinger.md`（设备：720×1440 / 60Hz / Qualcomm Adreno 610 / Android 10，前台 `com.yto.customermanmagererp.HomeActivity`），可见上述流程的落地表现：

- 刷新率静态 60Hz（`mRefreshRateOverlay` 无，`phaseOffsets` 标准值），所有合成发生在每个 VSync。
- 三个可见层 `HomeActivity`、`StatusBar`、`NavigationBar` 其 `Composition type` 均为 `CLIENT`，说明 `calculateWorkingSet`/`prepare` 判定它们走 GPU 合成，未使用 HWC Overlay；`HWC composition` 统计为 0，与“全 CLIENT”一致。
- 帧丢失统计：`mGpuFrameMissedCount`（GPU 掉帧）远高于 `mHwcFrameMissedCount`，印证主要负载在 client 合成，GPU 成为瓶颈。
- `BufferQueue` 状态：`HomeActivity` 三缓冲占比 0.457 偏高，提示存在卡顿或合成不及时；`dequeueBuffer`/`queueBuffer` 计数与 dump 中 graphicBuffer 总数（约 28MB 分配）对应。
- `Color` 区域 `forceHwcForRgbToYuv = 1`：解释了为何 RGB 层被强制 client 合成（厂商为兼容 YUV 输出的策略），这与 `calculateWorkingSet` 中 `forceClientComposition` 的触发条件呼应。
- 壁纸层未参与合成：`HomeActivity` 不透明，遮盖了壁纸，故壁纸不进入 `rebuildLayerStacks` 的可见列表（对应 `undefinedRegion` 被完全覆盖时壁纸不可见的逻辑）。

---

## 八、与 Android 8.1 的关键差异小结

| 方面 | Android 8.1 | Android 10 |
|------|-------------|------------|
| 合成状态管理 | SurfaceFlinger 直接持有 `mVisibleLayersSortedByZ` 等 | 抽象为 `compositionengine::Output` + `OutputLayer`，每 display 独立 |
| 主流程 | `handleMessageRefresh` 内 `rebuildLayerStacks` → `setupHardwareComposer` | 新增 `calculateWorkingSet()`，几何/HWC 状态写入上移统一 |
| prepareFrame | 直接 `mDisplaySurface->prepareFrame(type)` | 经 `RenderSurface::prepareFrame` 再转 `DisplaySurface::prepareFrame`（GLES/HWC/MIXED） |
| Layer 抽象 | Layer 即合成单元 | Layer 拆分为 `LayerFE`（前端）+ `compositionengine::Layer`（后端状态） |
| VSync 调度 | `DispSync` + `EventThread` | 新增 `Scheduler` 模块（`VsyncReactor`/`LayerHistory`/`PhaseOffsets`）支持动态帧率（Smart 90） |
| 行号参考 | `handleMessageRefresh` 约 1469 | `handleMessageRefresh` 在 `SurfaceFlinger.cpp:1946` |

---

## 九、阅读路径建议（对照源码）

1. `SurfaceFlinger.cpp:1813` `onMessageReceived` —— 消息总入口。
2. `SurfaceFlinger.cpp:1924` `handleMessageTransaction` / `:1984` `handleMessageInvalidate` —— INVALIDATE 阶段。
3. `SurfaceFlinger.cpp:1946` `handleMessageRefresh` —— REFRESH 主流程，依次看 `preComposition`(2153)、`rebuildLayerStacks`(2391)、`calculateWorkingSet`(2004)、`doComposition`(2625)。
4. `CompositionEngine/src/RenderSurface.cpp:113` `prepareFrame` —— 合成类型下发。
5. `DisplayHardware/HWComposer.cpp:402` `prepare` / `:560` `presentAndGetReleaseFences` —— HWC 协商与上屏。
6. `SurfaceFlinger.cpp:2227` `postComposition` —— buffer 回收与 fence。
7. `Scheduler/`（`DispSync.cpp`、`EventThread.cpp`、`Scheduler.cpp`）—— VSync 节拍。

> 说明：本文“合成类型”“forceClientComposition”等判定逻辑与 `CompositionEngine` 相关接口声明见 `CompositionEngine/include/compositionengine/Output.h`、`OutputLayer.h`、`LayerFE.h`；具体 impl 实现在 `compositionengine::impl::*` 命名空间（头文件位于 `CompositionEngine/include/compositionengine/impl/`）。
