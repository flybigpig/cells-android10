> 本文基于 AOSP 8.1 (android-8.1.0\_r1) 源码分析

### 前置知识

建议先阅读本系列前几篇笔记：

-   01-架构总览：了解 SurfaceFlinger 在 Android 图形系统中的角色
-   02-[BufferQueue](https://zhida.zhihu.com/search?content_id=272075327&content_type=Article&match_order=1&q=BufferQueue&zhida_source=entity)：了解生产者-消费者模型和 Buffer 流转机制
-   03-VSYNC 机制：了解 DispSync、EventThread 和 VSYNC 分发

本文将深入分析 SurfaceFlinger 收到 VSYNC 信号后，如何将各个应用的图层合成为最终画面并送显的完整流程。

___

### 正文

### 1\. 合成触发机制

SurfaceFlinger 的合成流程由 VSYNC-sf 信号驱动，整个触发链路如下：

1.  **硬件 VSYNC** 到达 → DispSync 模型产生软件 VSYNC
2.  **EventThread(sf)** 监听 SF VSYNC 事件，唤醒等待的连接
3.  **MessageQueue** 收到事件后向 Looper 发送 `INVALIDATE` 消息
4.  **SurfaceFlinger::onMessageReceived()** 处理 `INVALIDATE` 消息
5.  如果有新内容需要合成，发送 `REFRESH` 消息
6.  **handleMessageRefresh()** 执行实际的合成操作

### 源码：onMessageReceived()

```php
// frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp:1418-1453
void SurfaceFlinger::onMessageReceived(int32_t what) {
    ATRACE_CALL();
    switch (what) {
        case MessageQueue::INVALIDATE: {
            bool frameMissed = !mHadClientComposition &&
                    mPreviousPresentFence != Fence::NO_FENCE &&
                    (mPreviousPresentFence->getSignalTime() ==
                            Fence::SIGNAL_TIME_PENDING);
            ATRACE_INT("FrameMissed", static_cast<int>(frameMissed));
            if (mPropagateBackpressure && frameMissed) {
                signalLayerUpdate();  // 丢帧时跳过本次合成，等下一个 VSYNC
                break;
            }
            

            updateVrFlinger();

            bool refreshNeeded = handleMessageTransaction();
            refreshNeeded |= handleMessageInvalidate();
            refreshNeeded |= mRepaintEverything;
            if (refreshNeeded) {
                signalRefresh();  // 触发 REFRESH 消息
            }
            break;
        }
        case MessageQueue::REFRESH: {
            handleMessageRefresh();
            break;
        }
    }
}
```

注意第 1422-1430 行的**背压（Backpressure）机制**：如果上一帧的 present fence 尚未 signal（说明上一帧还没有显示完毕），且当前帧不涉及 Client 合成，则跳过本次合成。这是 Android 8.0 引入的防止掉帧雪崩的机制。

```rust
flowchart LR
    A["硬件 VSYNC"] --> B["DispSync 模型"]
    B --> C["EventThread(sf)"]
    C --> D["MessageQueue"]
    D --> E["INVALIDATE 消息"]
    E --> F{"handleMessageTransaction()\n+ handleMessageInvalidate()"}
    F -->|refreshNeeded = true| G["signalRefresh()"]
    G --> H["REFRESH 消息"]
    H --> I["handleMessageRefresh()"]
    F -->|refreshNeeded = false| J["本帧无需合成"]
```

### handleMessageInvalidate() 与 handleMessageTransaction()

```cpp
// SurfaceFlinger.cpp:1455-1467
bool SurfaceFlinger::handleMessageTransaction() {
    uint32_t transactionFlags = peekTransactionFlags();
    if (transactionFlags) {
        handleTransaction(transactionFlags);  // 处理待提交的事务（窗口属性变更等）
        return true;
    }
    return false;
}

bool SurfaceFlinger::handleMessageInvalidate() {
    ATRACE_CALL();
    return handlePageFlip();  // 锁定新 Buffer
}
```

___

### 2\. handlePageFlip – 锁定新 Buffer

`handlePageFlip()` 是连接 BufferQueue 和合成流程的桥梁。它的核心任务是：遍历所有有新帧排队的 Layer，通过 `latchBuffer()` 从 BufferQueue 中获取最新的缓冲区。

### 源码分析

```rust
// SurfaceFlinger.cpp:2495-2547
bool SurfaceFlinger::handlePageFlip()
{
    ALOGV("handlePageFlip");

    nsecs_t latchTime = systemTime();

    bool visibleRegions = false;
    bool frameQueued = false;
    bool newDataLatched = false;

    // 第一步：收集有新帧的 Layer
    mDrawingState.traverseInZOrder([&](Layer* layer) {
        if (layer->hasQueuedFrame()) {
            frameQueued = true;
            if (layer->shouldPresentNow(mPrimaryDispSync)) {
                mLayersWithQueuedFrames.push_back(layer);
            } else {
                layer->useEmptyDamage();
            }
        } else {
            layer->useEmptyDamage();
        }
    });

    // 第二步：锁定每个 Layer 的新 Buffer
    for (auto& layer : mLayersWithQueuedFrames) {
        const Region dirty(layer->latchBuffer(visibleRegions, latchTime));
        layer->useSurfaceDamage();
        invalidateLayerStack(layer, dirty);
        if (layer->isBufferLatched()) {
            newDataLatched = true;
        }
    }

    mVisibleRegionsDirty |= visibleRegions;

    // 如果有排队的帧但没有 latch 成功，安排下次再试
    if (frameQueued && (mLayersWithQueuedFrames.empty() || !newDataLatched)) {
        signalLayerUpdate();
    }

    // 只有确实有新数据 latch 时才返回 true，触发 REFRESH
    return !mLayersWithQueuedFrames.empty() && newDataLatched;
}
```

### Layer::latchBuffer() 关键流程

```cpp
// Layer.cpp:2178-2236（关键片段）
Region Layer::latchBuffer(bool& recomputeVisibleRegions, nsecs_t latchTime)
{
    // ... 前置检查 ...

    // 核心：通过 SurfaceFlingerConsumer 获取最新 Buffer 并绑定为 GL 纹理
    status_t updateResult = mSurfaceFlingerConsumer->updateTexImage(&r,
            mFlinger->mPrimaryDispSync, &mAutoRefresh, &queuedBuffer,
            mLastFrameNumberReceived);

    // 处理各种返回状态：PRESENT_LATER、BUFFER_REJECTED 等
    // ...
}
```

`updateTexImage()` 内部会调用 `acquireBufferLocked()` 从 BufferQueue 获取 buffer，然后通过 `bindTextureImage()` 将其绑定为 OpenGL ES 纹理，以备后续 GPU 合成使用。

___

### 3\. handleMessageRefresh() – 六步合成

这是 SurfaceFlinger 合成流程的核心入口，依次执行六个步骤：

```rust
// SurfaceFlinger.cpp:1469-1481
void SurfaceFlinger::handleMessageRefresh() {
    ATRACE_CALL();
    mRefreshPending = false;
    nsecs_t refreshStartTime = systemTime(SYSTEM_TIME_MONOTONIC);

    preComposition(refreshStartTime);       // Step 1: 预合成
    rebuildLayerStacks();                    // Step 2: 重建图层栈
    setUpHWComposer();                       // Step 3: 配置 HWC
    doDebugFlashRegions();                   // Step 4: 调试闪烁区域
    doComposition();                         // Step 5: 执行合成
    postComposition(refreshStartTime);       // Step 6: 后处理

    mPreviousPresentFence = mHwc->getPresentFence(HWC_DISPLAY_PRIMARY);

    mHadClientComposition = false;
    for (size_t displayId = 0; displayId < mDisplays.size(); ++displayId) {
        const sp<DisplayDevice>& displayDevice = mDisplays[displayId];
        mHadClientComposition = mHadClientComposition ||
                mHwc->hasClientComposition(displayDevice->getHwcDisplayId());
    }

    mLayersWithQueuedFrames.clear();
}
flowchart TD
    A["handleMessageRefresh()"] --> B["Step 1: preComposition(refreshStartTime)"]
    B --> C["Step 2: rebuildLayerStacks()"]
    C --> D["Step 3: setUpHWComposer()"]
    D --> E["Step 4: doDebugFlashRegions()"]
    E --> F["Step 5: doComposition()"]
    F --> G["Step 6: postComposition(refreshStartTime)"]
    G --> H["保存 PresentFence\n记录 ClientComposition 状态\n清理 mLayersWithQueuedFrames"]
```

___

### Step 1: preComposition()

```cpp
// SurfaceFlinger.cpp:1539-1553
void SurfaceFlinger::preComposition(nsecs_t refreshStartTime)
{
    ATRACE_CALL();
    ALOGV("preComposition");

    bool needExtraInvalidate = false;
    mDrawingState.traverseInZOrder([&](Layer* layer) {
        if (layer->onPreComposition(refreshStartTime)) {
            needExtraInvalidate = true;
        }
    });

    if (needExtraInvalidate) {
        signalLayerUpdate();
    }
}
```

**核心逻辑：**

-   按 Z-order 遍历 `mDrawingState` 中所有 Layer
-   调用每个 Layer 的 `onPreComposition(refreshStartTime)`

-   该方法检查 Layer 是否还有待处理的帧（如动画尚未结束）
-   如果 `mRefreshPending` 为 true 或 `mQueuedFrames > 0`，返回 true

-   如果任何 Layer 需要额外的 invalidate（`needExtraInvalidate = true`），调用 `signalLayerUpdate()` 安排下一次 VSYNC 周期再做合成

**面试要点：** preComposition 的核心目的是确保动画流畅性。如果某个 Layer 还有后续帧需要显示，就提前预约下一次合成机会，避免动画卡顿。

___

### Step 2: rebuildLayerStacks()

```rust
// SurfaceFlinger.cpp:1717-1765
void SurfaceFlinger::rebuildLayerStacks() {
    ATRACE_CALL();

    if (CC_UNLIKELY(mVisibleRegionsDirty)) {
        mVisibleRegionsDirty = false;
        invalidateHwcGeometry();

        for (size_t dpy=0 ; dpy<mDisplays.size() ; dpy++) {
            Region opaqueRegion;
            Region dirtyRegion;
            Vector<sp<Layer>> layersSortedByZ;
            const sp<DisplayDevice>& displayDevice(mDisplays[dpy]);
            const Transform& tr(displayDevice->getTransform());
            const Rect bounds(displayDevice->getBounds());

            if (displayDevice->isDisplayOn()) {
                // 核心：计算每个 Layer 的可见区域
                computeVisibleRegions(displayDevice, dirtyRegion, opaqueRegion);

                // 收集有可见区域的 Layer
                mDrawingState.traverseInZOrder([&](Layer* layer) {
                    if (layer->belongsToDisplay(displayDevice->getLayerStack(),
                                displayDevice->isPrimary())) {
                        Region drawRegion(tr.transform(
                                layer->visibleNonTransparentRegion));
                        drawRegion.andSelf(bounds);
                        if (!drawRegion.isEmpty()) {
                            layersSortedByZ.add(layer);
                        } else {
                            layer->destroyHwcLayer(
                                    displayDevice->getHwcDisplayId());
                        }
                    }
                });
            }
            displayDevice->setVisibleLayersSortedByZ(layersSortedByZ);
            displayDevice->undefinedRegion.set(bounds);
            displayDevice->undefinedRegion.subtractSelf(
                    tr.transform(opaqueRegion));
            displayDevice->dirtyRegion.orSelf(dirtyRegion);
        }
    }
}
```

**computeVisibleRegions() 详解**

这是可见区域计算的核心算法，位于 `SurfaceFlinger.cpp:2354-2484`：

```rust
void SurfaceFlinger::computeVisibleRegions(
        const sp<const DisplayDevice>& displayDevice,
        Region& outDirtyRegion, Region& outOpaqueRegion)
{
    Region aboveOpaqueLayers;
    Region aboveCoveredLayers;
    Region dirty;

    // 从 Z-order 最高层向下遍历（逆序）
    mDrawingState.traverseInReverseZOrder([&](Layer* layer) {
        // ...
        Region opaqueRegion;    // 该层的不透明区域
        Region visibleRegion;   // 该层的可见区域
        Region coveredRegion;   // 该层被上层覆盖的区域
        Region transparentRegion; // 透明区域提示

        if (CC_LIKELY(layer->isVisible())) {
            const bool translucent = !layer->isOpaque(s);
            Rect bounds(layer->computeScreenBounds());
            visibleRegion.set(bounds);

            // 计算不透明区域：alpha==1.0 且不透明 且无复杂变换
            if (s.alpha == 1.0f && !translucent &&
                    ((layerOrientation & Transform::ROT_INVALID) == false)) {
                opaqueRegion = visibleRegion;
            }
        }

        // 被上层所有区域覆盖的部分
        coveredRegion = aboveCoveredLayers.intersect(visibleRegion);
        aboveCoveredLayers.orSelf(visibleRegion);

        // 减去上层不透明区域 → 得到真正可见的区域
        visibleRegion.subtractSelf(aboveOpaqueLayers);

        // 计算脏区域
        // ...

        // 更新上层不透明区域累积
        aboveOpaqueLayers.orSelf(opaqueRegion);

        // 保存计算结果
        layer->setVisibleRegion(visibleRegion);
        layer->setCoveredRegion(coveredRegion);
        layer->setVisibleNonTransparentRegion(
                visibleRegion.subtract(transparentRegion));
    });

    outOpaqueRegion = aboveOpaqueLayers;
}
```

**核心算法思路：**

```rust
flowchart TD
    A["从最上层 Layer 开始\n(Z-order 最高)"] --> B["计算该 Layer 的 bounds"]
    B --> C{"Layer 是否可见?"}
    C -->|是| D["visibleRegion = bounds"]
    C -->|否| E["visibleRegion = empty"]
    D --> F["coveredRegion = aboveCoveredLayers & visibleRegion"]
    E --> F
    F --> G["visibleRegion -= aboveOpaqueLayers\n(减去被上层不透明区域遮挡的部分)"]
    G --> H["更新 aboveCoveredLayers\n更新 aboveOpaqueLayers"]
    H --> I["保存 visibleRegion / coveredRegion"]
    I --> J{"还有下一层?"}
    J -->|是| B
    J -->|否| K["完成"]
```

**关键概念：**

| 区域名称 | 含义 |
|-------------------|-------------------------------|
| visibleRegion | Layer 在屏幕上可见的区域（未被上层不透明区域遮挡） |
| coveredRegion | Layer 被上层覆盖的区域（包括半透明覆盖） |
| opaqueRegion | Layer 完全不透明的区域（可以遮挡下层） |
| transparentRegion | 应用声明的透明区域（优化提示） |
| aboveOpaqueLayers | 所有上层不透明区域的累积，用于剔除被完全遮挡的 Layer |

___

### Step 3: setUpHWComposer()

```cpp
// SurfaceFlinger.cpp:1829-1937
void SurfaceFlinger::setUpHWComposer() {
    // ... 判断是否需要重新合成 ...

    // 如果几何信息发生变化，重建 HWC Layer
    if (CC_UNLIKELY(mGeometryInvalid)) {
        mGeometryInvalid = false;
        for (size_t dpy=0 ; dpy<mDisplays.size() ; dpy++) {
            sp<const DisplayDevice> displayDevice(mDisplays[dpy]);
            const auto hwcId = displayDevice->getHwcDisplayId();
            if (hwcId >= 0) {
                const Vector<sp<Layer>>& currentLayers(
                        displayDevice->getVisibleLayersSortedByZ());
                for (size_t i = 0; i < currentLayers.size(); i++) {
                    const auto& layer = currentLayers[i];
                    if (!layer->hasHwcLayer(hwcId)) {
                        // 创建 HWC Layer
                        if (!layer->createHwcLayer(mHwc.get(), hwcId)) {
                            layer->forceClientComposition(hwcId);
                            continue;
                        }
                    }
                    // 设置几何信息：位置、大小、变换、裁剪、Z-order、alpha
                    layer->setGeometry(displayDevice, i);
                }
            }
        }
    }

    // 设置每帧数据
    for (size_t displayId = 0; displayId < mDisplays.size(); ++displayId) {
        // ...
        for (auto& layer : displayDevice->getVisibleLayersSortedByZ()) {
            // 设置 buffer handle、acquire fence、visible region 等
            layer->setPerFrameData(displayDevice);
        }

        // 调用 HWC validate
        status_t result = displayDevice->prepareFrame(*mHwc);
    }
}
```

这一步分为三个阶段：

**阶段 1：创建/更新 HWC Layer 几何信息**

`Layer::setGeometry()` (Layer.cpp:641) 设置的属性包括：

-   BlendMode（混合模式：None / Premultiplied / Coverage）
-   DisplayFrame（显示位置和大小）
-   SourceCrop（源裁剪区域）
-   Transform（变换矩阵）
-   Z-order
-   Alpha

**阶段 2：设置每帧数据**

`Layer::setPerFrameData()` (Layer.cpp:858) 设置的属性包括：

-   VisibleRegion（可见区域）
-   SurfaceDamage（损坏区域，增量更新优化）
-   Buffer handle 和 Acquire Fence
-   Dataspace（色彩空间）
-   Composition Type（建议的合成类型）

在 `setPerFrameData()` 中有一段重要的合成类型决策逻辑：

```rust
// Layer.cpp:894-910
// 强制 Client 合成的情况
if (hwcInfo.forceClientComposition ||
        (mActiveBuffer != nullptr && mActiveBuffer->handle == nullptr)) {
    setCompositionType(hwcId, HWC2::Composition::Client);
    return;
}

// 无 Buffer 的层 → SolidColor
if (mActiveBuffer == nullptr) {
    setCompositionType(hwcId, HWC2::Composition::SolidColor);
    // ...
    return;
}

// 默认请求 Device 合成
setCompositionType(hwcId, HWC2::Composition::Device);
```

**阶段 3：HWC Validate**

`displayDevice->prepareFrame(*mHwc)` 最终调用 HWC2 的 `validate()` 接口。HWC 硬件会评估所有 Layer，决定哪些能由硬件合成，哪些需要回退到 GPU（Client）合成。validate 后，每个 Layer 的合成类型可能被 HWC 修改。

___

### Step 4: doDebugFlashRegions()

```javascript
// SurfaceFlinger.cpp:1495-1537
void SurfaceFlinger::doDebugFlashRegions()
{
    if (CC_LIKELY(!mDebugRegion))
        return;
    // 仅在调试模式下生效，闪烁显示被更新的区域
    // 帮助开发者识别哪些区域在每帧中被重绘
    // ...
}
```

这是一个纯粹的调试功能，可通过 `adb shell service call SurfaceFlinger 1002` 或系统属性 `debug.sf.showupdates` 开启。开启后屏幕上被更新的区域会闪烁红色/绿色，便于排查过度绘制问题。正常运行时此步骤直接跳过。

___

### Step 5: doComposition()

```rust
// SurfaceFlinger.cpp:1939-1958
void SurfaceFlinger::doComposition() {
    ATRACE_CALL();
    const bool repaintEverything = android_atomic_and(0, &mRepaintEverything);
    for (size_t dpy=0 ; dpy<mDisplays.size() ; dpy++) {
        const sp<DisplayDevice>& hw(mDisplays[dpy]);
        if (hw->isDisplayOn()) {
            const Region dirtyRegion(hw->getDirtyRegion(repaintEverything));
            doDisplayComposition(hw, dirtyRegion);  // 执行实际合成

            hw->dirtyRegion.clear();
            hw->flip(hw->swapRegion);    // 标记交换区域
            hw->swapRegion.clear();
        }
    }
    postFramebuffer();  // 提交帧到 HWC
}
```

**doDisplayComposition() 内部流程：**

```javascript
// SurfaceFlinger.cpp:2555-2603
void SurfaceFlinger::doDisplayComposition(
        const sp<const DisplayDevice>& displayDevice,
        const Region& inDirtyRegion)
{
    // 根据 DisplayDevice 的能力计算实际脏区域
    // SWAP_RECTANGLE / PARTIAL_UPDATES / 全屏
    // ...

    if (!doComposeSurfaces(displayDevice, dirtyRegion)) return;

    displayDevice->swapRegion.orSelf(dirtyRegion);
    displayDevice->swapBuffers(getHwComposer());  // EGL swapBuffers
}
```

**doComposeSurfaces() – 核心渲染逻辑：**

```rust
// SurfaceFlinger.cpp:2605-2753
bool SurfaceFlinger::doComposeSurfaces(
        const sp<const DisplayDevice>& displayDevice, const Region& dirty)
{
    const auto hwcId = displayDevice->getHwcDisplayId();
    bool hasClientComposition = mHwc->hasClientComposition(hwcId);

    if (hasClientComposition) {
        // 有需要 GPU 合成的 Layer
        // 1. 设置 EGL Context
        displayDevice->makeCurrent(mEGLDisplay, mEGLContext);

        // 2. 清除 framebuffer
        if (hasDeviceComposition) {
            // 有混合合成时，framebuffer 需要全透明背景
            mRenderEngine->clearWithColor(0, 0, 0, 0);
        } else {
            // 纯 Client 合成，清除 letterbox 等未定义区域
            drawWormhole(displayDevice, region);
        }
    }

    // 3. 遍历所有可见 Layer，按合成类型分别处理
    for (auto& layer : displayDevice->getVisibleLayersSortedByZ()) {
        const Region clip(dirty.intersect(
                displayTransform.transform(layer->visibleRegion)));
        if (!clip.isEmpty()) {
            switch (layer->getCompositionType(hwcId)) {
                case HWC2::Composition::Device:
                case HWC2::Composition::SolidColor:
                case HWC2::Composition::Sideband:
                case HWC2::Composition::Cursor:
                    // HWC 硬件合成的 Layer → 跳过 GPU 绘制
                    // 但如果 clearClientTarget 标记，需要清除对应区域
                    if (layer->getClearClientTarget(hwcId) && !firstLayer &&
                            layer->isOpaque(state) && (state.alpha == 1.0f)
                            && hasClientComposition) {
                        layer->clearWithOpenGL(displayDevice);
                    }
                    break;
                case HWC2::Composition::Client:
                    // GPU 合成的 Layer → 使用 RenderEngine 绘制
                    layer->draw(displayDevice, clip);
                    break;
            }
        }
    }
    return true;
}
sequenceDiagram
    participant SF as SurfaceFlinger
    participant DD as DisplayDevice
    participant RE as RenderEngine<br/>(GLES20)
    participant L as Layer
    participant HWC as HWComposer

    SF->>SF: doComposition()
    loop 每个 DisplayDevice
        SF->>DD: getDirtyRegion()
        SF->>SF: doDisplayComposition(hw, dirtyRegion)
        SF->>SF: doComposeSurfaces(hw, dirtyRegion)

        alt 有 Client 合成的 Layer
            SF->>DD: makeCurrent(EGLDisplay, EGLContext)
            SF->>RE: clearWithColor() / drawWormhole()
        end

        loop 每个可见 Layer (Z-order)
            alt Composition::Client
                SF->>L: draw(displayDevice, clip)
                L->>L: onDraw() -- bindTextureImage
                L->>RE: drawMesh() -- OpenGL ES 绘制
            else Composition::Device
                Note over SF,L: 跳过 GPU 绘制<br/>HWC 硬件直接处理
            end
        end

        SF->>DD: swapBuffers() -- EGL swap
        SF->>DD: flip(swapRegion)
    end

    SF->>SF: postFramebuffer()
    loop 每个 DisplayDevice
        SF->>HWC: presentAndGetReleaseFences(hwcId)
        SF->>DD: onSwapBuffersCompleted()
        loop 每个可见 Layer
            alt Client 合成
                SF->>L: onLayerDisplayed(clientTargetAcquireFence)
            else Device 合成
                SF->>L: onLayerDisplayed(hwcLayerReleaseFence)
            end
        end
        SF->>HWC: clearReleaseFences(hwcId)
    end
```

___

### Step 5.5: postFramebuffer()

`postFramebuffer()` 是 `doComposition()` 的最后一步，负责将合成结果提交给 HWC 并处理 fence 分发。

```rust
// SurfaceFlinger.cpp:1961-2003
void SurfaceFlinger::postFramebuffer()
{
    const nsecs_t now = systemTime();
    mDebugInSwapBuffers = now;

    for (size_t displayId = 0; displayId < mDisplays.size(); ++displayId) {
        auto& displayDevice = mDisplays[displayId];
        if (!displayDevice->isDisplayOn()) {
            continue;
        }
        const auto hwcId = displayDevice->getHwcDisplayId();
        if (hwcId >= 0) {
            // 提交帧到 HWC 硬件，并获取 release fences
            mHwc->presentAndGetReleaseFences(hwcId);
        }
        displayDevice->onSwapBuffersCompleted();
        displayDevice->makeCurrent(mEGLDisplay, mEGLContext);

        // 分发 release fence 给每个 Layer
        for (auto& layer : displayDevice->getVisibleLayersSortedByZ()) {
            sp<Fence> releaseFence = Fence::NO_FENCE;
            if (layer->getCompositionType(hwcId) == HWC2::Composition::Client) {
                // Client 合成：release fence = clientTarget 的 acquire fence
                releaseFence = displayDevice->getClientTargetAcquireFence();
            } else {
                // Device 合成：release fence = HWC 返回的 layer release fence
                auto hwcLayer = layer->getHwcLayer(hwcId);
                releaseFence = mHwc->getLayerReleaseFence(hwcId, hwcLayer);
            }
            // 通知 Layer buffer 已显示完毕，可以被生产者重新使用
            layer->onLayerDisplayed(releaseFence);
        }
        if (hwcId >= 0) {
            mHwc->clearReleaseFences(hwcId);
        }
    }
}
```

**Release Fence 的含义和分发逻辑：**

Release Fence 表示”该 buffer 已经被消费完毕，生产者可以安全重用它”。不同合成方式下 release fence 的来源不同：

| 合成类型 | Release Fence 来源 | 含义 |
|--------------|----------------------------------------------|----------------------------|
| Client (GPU) | displayDevice->getClientTargetAcquireFence() | GPU 完成绘制后的 fence |
| Device (HWC) | mHwc->getLayerReleaseFence(hwcId, hwcLayer) | HWC 硬件释放该层 buffer 后的 fence |

___

### Step 6: postComposition()

```cpp
// SurfaceFlinger.cpp:1615-1715
void SurfaceFlinger::postComposition(nsecs_t refreshStartTime)
{
    // 1. 释放本帧被替换的旧 Buffer
    nsecs_t dequeueReadyTime = systemTime();
    for (auto& layer : mLayersWithQueuedFrames) {
        layer->releasePendingBuffer(dequeueReadyTime);
    }

    // 2. 获取 GL 合成完成时间线
    mGlCompositionDoneTimeline.updateSignalTimes();
    // ...

    // 3. 获取 present fence 并更新时间线
    sp<Fence> presentFence = mHwc->getPresentFence(HWC_DISPLAY_PRIMARY);
    auto presentFenceTime = std::make_shared<FenceTime>(presentFence);
    mDisplayTimeline.push(presentFenceTime);

    // 4. 更新 CompositorTiming（合成到显示的延迟）
    nsecs_t vsyncPhase = mPrimaryDispSync.computeNextRefresh(0);
    nsecs_t vsyncInterval = mPrimaryDispSync.getPeriod();
    updateCompositorTiming(
        vsyncPhase, vsyncInterval, refreshStartTime, presentFenceTime);

    // 5. 通知每个 Layer 合成完成
    mDrawingState.traverseInZOrder([&](Layer* layer) {
        bool frameLatched = layer->onPostComposition(glCompositionDoneFenceTime,
                presentFenceTime, compositorTiming);
        if (frameLatched) {
            recordBufferingStats(layer->getName().string(),
                    layer->getOccupancyHistory(false));
        }
    });

    // 6. 更新 DispSync 模型
    if (presentFenceTime->isValid()) {
        if (mPrimaryDispSync.addPresentFence(presentFenceTime)) {
            enableHardwareVsync();    // 模型需要校准
        } else {
            disableHardwareVsync(false);  // 模型已稳定
        }
    }

    // 7. 动画帧追踪和统计
    // ...
}
```

**核心工作：**

1.  **释放旧 Buffer**：调用 `releasePendingBuffer()` 将被新帧替换的旧 buffer 归还给 BufferQueue
2.  **更新 DispSync 模型**：通过 `addPresentFence()` 将实际显示时间反馈给 DispSync，校准软件 VSYNC 模型
3.  **通知 Layer**：`onPostComposition()` 更新 FrameTimestamps，供应用层通过 `getFrameTimestamps()` 查询帧延迟
4.  **更新 CompositorTiming**：计算合成到显示的延迟（composite-to-present latency），供应用精确预测显示时间

___

### 4\. HWC 合成 vs GPU 合成

Android 支持两种合成方式，它们可以混合使用（同一帧中部分 Layer 由 HWC 合成，部分由 GPU 合成）。

```rust
flowchart TD
    A["SurfaceFlinger 收集所有可见 Layer"] --> B["setUpHWComposer():\n为每个 Layer 设置建议的合成类型"]
    B --> C["Layer::setPerFrameData() 决定初始类型"]
    C --> D{"Layer 状态检查"}
    D -->|"forceClientComposition\n或 buffer handle == null"| E["设置为 Client"]
    D -->|"mActiveBuffer == null"| F["设置为 SolidColor"]
    D -->|"正常 buffer"| G["设置为 Device"]

    E --> H["HWC validate()"]
    F --> H
    G --> H

    H --> I{"HWC 能处理?"}
    I -->|"能"| J["保持 Device/SolidColor/Cursor"]
    I -->|"不能"| K["回退为 Client"]

    J --> L["HWC 硬件直接合成\n(overlay / DMA)"]
    K --> M["GPU(RenderEngine) 绘制到\nclientTarget framebuffer"]
    M --> N["clientTarget 作为整体\n提交给 HWC 显示"]
    L --> O["HWC presentDisplay()\n最终送显"]
    N --> O
```

### 对比表

| 特性 | HWC（Device）合成 | GPU（Client）合成 |
|-----------|-----------------------|-------------------|
| 执行硬件 | 显示控制器专用硬件（overlay） | GPU (OpenGL ES) |
| 功耗 | 低 | 高 |
| 性能 | 高（专用硬件流水线） | 一般（占用 GPU 资源） |
| Layer 数限制 | 有限（通常 4-8 层） | 无限制 |
| 变换支持 | 简单变换（旋转 0/90/180/270） | 任意变换 |
| 特效支持 | 不支持 | 支持（圆角、模糊、色彩变换） |
| 适用场景 | 普通 UI 层、视频层 | 复杂特效层、超出 HWC 能力的层 |

### Fallback 机制详解

HWC 合成到 GPU 合成的回退发生在以下场景：

1.  **HWC 层数超限**：HWC 硬件能处理的 overlay 层数有限，超出部分回退到 Client
2.  **不支持的变换**：某些 HWC 不支持非 90 度倍数的旋转或复杂的缩放
3.  **不支持的像素格式**：某些 HWC 不支持特定的 buffer 格式
4.  **不支持的混合模式**：某些复杂的 alpha 混合 HWC 无法处理
5.  **安全层在非安全显示**：安全内容强制走 Client 合成（Layer.cpp:659-661）
6.  **调试模式强制**：`mDebugDisableHWC` 开启时所有层强制 Client 合成

**混合合成的工作方式：**

当一帧中既有 Device 层又有 Client 层时：

1.  SurfaceFlinger 使用 RenderEngine 将所有 Client 层绘制到一个 clientTarget framebuffer 中
2.  该 clientTarget 作为一个”虚拟层”传递给 HWC
3.  HWC 将 Device 层和 clientTarget 一起合成后送显

___

### 5\. RenderEngine – GPU 合成引擎

RenderEngine 是 SurfaceFlinger 的 GPU 绘制后端，负责执行 Client 合成。

**源码位置：** `frameworks/native/services/surfaceflinger/RenderEngine/`

### 类层次结构

```scss
RenderEngine (基类)
  └── GLES20RenderEngine (实际实现，使用 OpenGL ES 2.0)
```

主要文件：

| 文件 | 职责 |
|------------------------|----------------------------------|
| RenderEngine.cpp | 基类，工厂方法创建合适的实现 |
| GLES20RenderEngine.cpp | OpenGL ES 2.0 实现，负责实际绘制 |
| ProgramCache.cpp | 着色器程序缓存，按 Layer 属性组合缓存不同的 shader |
| Program.cpp | GLSL 着色器程序封装 |
| Description.cpp | 绘制描述符，封装绘制状态 |
| Texture.cpp | 纹理封装 |
| Mesh.cpp | 网格数据（顶点/纹理坐标） |

### Layer::onDraw() – GPU 绘制入口

```rust
// Layer.cpp:1095-1174（关键片段）
void Layer::onDraw(const sp<const DisplayDevice>& hw, const Region& clip,
        bool useIdentityTransform) const
{
    if (CC_UNLIKELY(mActiveBuffer == 0)) {
        // 没有 buffer 时，绘制黑色填充
        // ...
        return;
    }

    // 将当前 buffer 绑定为 GL 纹理
    status_t err = mSurfaceFlingerConsumer->bindTextureImage();

    bool blackOutLayer = isProtected() || (isSecure() && !hw->isSecure());

    RenderEngine& engine(mFlinger->getRenderEngine());

    if (!blackOutLayer) {
        // 设置纹理矩阵
        float textureMatrix[16];
        mSurfaceFlingerConsumer->setFilteringEnabled(useFiltering);
        mSurfaceFlingerConsumer->getTransformMatrix(textureMatrix);
        // ... 设置变换 ...

        // 通过 RenderEngine 绘制纹理网格
        engine.setupLayerTexturing(mTexture);
    } else {
        engine.setupLayerBlackedOut();
    }
    // drawMesh() -- 最终的 GL draw call
}
```

### ProgramCache 着色器缓存

`ProgramCache` 根据 Layer 的属性组合（是否有纹理、是否预乘 alpha、是否需要色彩变换等）生成不同的 GLSL 着色器，并缓存编译后的 GL program，避免运行时频繁编译着色器。

___

### 小结

1.  **VSYNC 驱动的两阶段合成**：INVALIDATE 阶段锁定新 Buffer（handlePageFlip），REFRESH 阶段执行实际合成（handleMessageRefresh 六步流程）。两阶段设计允许在无新内容时跳过合成，节省功耗。
2.  **六步合成流程**：preComposition（预检查/预约下帧）→ rebuildLayerStacks（可见区域计算）→ setUpHWComposer（配置 HWC）→ doDebugFlashRegions（调试）→ doComposition（执行合成 + 送显）→ postComposition（DispSync 校准 + 统计）。
3.  **HWC 与 GPU 混合合成**：SurfaceFlinger 优先使用 HWC 硬件合成以降低功耗。HWC validate 后，无法处理的 Layer 自动回退到 GPU 合成。Client 层先由 RenderEngine 绘制到 clientTarget，再整体提交给 HWC。
4.  **Fence 机制保障同步**：Acquire Fence 确保 buffer 可被读取，Release Fence 确保 buffer 可被重用，Present Fence 标记帧实际显示时间。整个合成流程通过 fence 实现 CPU/GPU/HWC 三方异步流水线。
5.  **背压机制防止掉帧雪崩**：如果上一帧的 present fence 未 signal（说明显示还在处理中），SurfaceFlinger 会跳过当前 INVALIDATE，避免积压导致更严重的掉帧。

___

### 常见面试问题

### Q1: SurfaceFlinger 的一帧合成流程是怎样的？请描述 handleMessageRefresh 的六个步骤

**参考答案要点：**

handleMessageRefresh 依次执行六步：

-   **preComposition**：遍历所有 Layer，检查是否有动画需要继续，有则预约下一帧合成
-   **rebuildLayerStacks**：当可见区域脏标记为 true 时，对每个 DisplayDevice 重新计算所有 Layer 的可见区域，生成按 Z-order 排序的可见 Layer 列表
-   **setUpHWComposer**：为每个 HWC Layer 设置几何信息（setGeometry）和每帧数据（setPerFrameData），然后调用 HWC validate 确定每层的合成方式
-   **doDebugFlashRegions**：调试功能，正常运行时跳过
-   **doComposition**：对 Client 合成的 Layer 使用 RenderEngine (OpenGL ES) 绘制到 framebuffer，然后调用 postFramebuffer 将帧提交给 HWC 硬件，并分发 release fence
-   **postComposition**：释放旧 buffer，更新 DispSync 模型，通知 Layer 更新 FrameTimestamps，记录合成统计

### Q2: HWC 合成和 GPU 合成有什么区别？什么时候会 fallback 到 GPU 合成？

**参考答案要点：**

区别：HWC 合成由显示控制器的 overlay 硬件完成，功耗低、性能高，但支持的层数和变换有限；GPU 合成由 OpenGL ES 通过 RenderEngine 完成，灵活但功耗高。

Fallback 场景：

-   HWC overlay 层数超限（通常 4-8 层）
-   不支持的旋转/缩放/混合模式
-   安全层在非安全显示设备上
-   不支持的像素格式
-   调试模式强制 Client 合成

两者可以混合使用：同一帧中部分 Layer 走 HWC，部分走 GPU。GPU 绘制的结果作为 clientTarget 统一提交给 HWC。

### Q3: 什么是 handlePageFlip？它和 handleMessageRefresh 的关系是什么？

**参考答案要点：**

handlePageFlip 在 INVALIDATE 阶段执行，负责从 BufferQueue 中锁定（latch）每个有新帧的 Layer 的最新 Buffer。它遍历所有 Layer，找出有排队帧且当前应该显示的 Layer，调用 `latchBuffer()` 获取 buffer 并绑定为 GL 纹理。

关系：handlePageFlip 返回 true 表示有新内容被 latch，此时 `onMessageReceived()` 中 `refreshNeeded` 为 true，会调用 `signalRefresh()` 触发 REFRESH 消息，进而执行 handleMessageRefresh 完成实际合成。如果 handlePageFlip 返回 false（无新内容），且没有其他需要刷新的理由，则跳过本帧的合成，节省功耗。

### Q4: SurfaceFlinger 如何决定哪些 Layer 由 HWC 合成，哪些由 GPU 合成？

**参考答案要点：**

决策分两步：

第一步（SF 侧初步决策）：在 `setPerFrameData()` 中，SurfaceFlinger 根据 Layer 状态设置初始合成类型：

-   `forceClientComposition` 标记或 buffer handle 无效 → Client
-   无 activeBuffer → SolidColor
-   其他 → Device

第二步（HWC 侧最终决策）：SF 调用 HWC 的 `validate()`，HWC 硬件评估所有 Layer，如果某些层超出硬件能力，会将其合成类型从 Device 改为 Client。SF 读取 validate 结果后，按最终类型进行合成。

### Q5: postFramebuffer 做了什么？release fence 是如何分发的？

**参考答案要点：**

postFramebuffer 是每帧合成的最后提交环节：

1.  调用 `mHwc->presentAndGetReleaseFences(hwcId)` 将帧提交给 HWC 硬件显示，同时获取每个 Layer 的 release fence
2.  调用 `displayDevice->onSwapBuffersCompleted()` 更新缓冲区状态
3.  遍历每个可见 Layer，根据合成类型分发 release fence：

-   **Client 合成**的 Layer：release fence = `displayDevice->getClientTargetAcquireFence()`（GPU 完成渲染的 fence）
-   **Device 合成**的 Layer：release fence = `mHwc->getLayerReleaseFence(hwcId, hwcLayer)`（HWC 硬件释放 buffer 的 fence）

1.  调用 `layer->onLayerDisplayed(releaseFence)` 通知 Layer，Layer 将 fence 传回 BufferQueue，生产者可以在 fence signal 后安全复用该 buffer
2.  调用 `mHwc->clearReleaseFences(hwcId)` 清理 HWC 的 fence 缓存

___

### 参考源码文件

| 文件路径 | 关键内容 |
|-------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp | 合成流程主逻辑：onMessageReceived (L1418)、handlePageFlip (L2495)、handleMessageRefresh (L1469)、preComposition (L1539)、rebuildLayerStacks (L1717)、setUpHWComposer (L1829)、doComposition (L1939)、doComposeSurfaces (L2605)、postFramebuffer (L1961)、postComposition (L1615)、computeVisibleRegions (L2354) |
| frameworks/native/services/surfaceflinger/Layer.cpp | Layer 合成相关：setGeometry (L641)、setPerFrameData (L858)、onDraw (L1095)、latchBuffer (L2178) |
| frameworks/native/services/surfaceflinger/DisplayDevice.cpp | 显示设备管理：prepareFrame、swapBuffers、flip |
| frameworks/native/services/surfaceflinger/RenderEngine/GLES20RenderEngine.cpp | GPU 合成引擎实现 |
| frameworks/native/services/surfaceflinger/RenderEngine/ProgramCache.cpp | 着色器缓存机制 |
| frameworks/native/services/surfaceflinger/SurfaceFlingerConsumer.cpp | Buffer 获取和纹理绑定：updateTexImage、bindTextureImage |
| frameworks/native/services/surfaceflinger/MessageQueue.cpp | 消息队列：INVALIDATE / REFRESH 消息分发 |
| frameworks/native/services/surfaceflinger/DisplayHardware/HWComposer.cpp | HWC 接口封装：prepare、presentAndGetReleaseFences |