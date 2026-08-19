# ThreadedRenderer 渲染与消息队列全链路分析

> 来源：`C:\D\SDK\sources\android-31\android\view\ThreadedRenderer.java`（708 行）
> 以及父类 `C:\D\SDK\sources\android-31\android\graphics\HardwareRenderer.java`（1397 行）
> 版本：Android 12 (API 31)

---

## 一、整体架构

```
UI Thread                          RenderThread (Native)
──────────                         ──────────────────────
ViewRootImpl.performDraw()
  │
  ▼
ThreadedRenderer.draw()             ┌──────────────────┐
  │                                 │  RenderThread     │
  ├─ 1. updateRootDisplayList()     │  Looper (Native)  │
  │     在UI线程录制 DisplayList    │                    │
  │                                 │  Message Queue     │
  ├─ 2. syncAndDrawFrame() ─────────┼─→ [DrawFrameTask]  │
  │     nSyncAndDrawFrame() (JNI)   │     ↓              │
  │     将渲染任务投递到RT队列      │  同步DisplayList    │
  │                                 │  执行GPU绘制        │
  ├─ 3. 检查返回值                  │  提交到Surface      │
  │     SYNC_LOST_SURFACE?          │     ↓              │
  │     SYNC_REDRAW_REQUESTED?      │  FrameBuffer       │
  ▼                                 └──────────────────┘
继续UI线程后续工作
```

### 线程模型要点

- **UI 线程可以阻塞在 RenderThread 上**（例如 `fence()`、`setWaitForPresent(true)`）
- **RenderThread 绝不能阻塞在 UI 线程上**
- 所有 `HardwareRenderer` 实例共享同一个 RenderThread
- RenderThread 持有 GPU 上下文与资源（EGL/Vulkan），首个实例创建时承担创建 GPU 上下文的开销

---

## 二、第一步：UI 线程录制 DisplayList

`ThreadedRenderer.updateRootDisplayList()` 在 UI 线程执行，将 View 树的绘制命令录制为 `RenderNode` 的 DisplayList。

```java
// ThreadedRenderer.java 第 538-574 行
private void updateRootDisplayList(View view, DrawCallbacks callbacks) {
    Trace.traceBegin(Trace.TRACE_TAG_VIEW, "Record View#draw()");
    // 1. 递归更新整棵 View 树的 DisplayList
    updateViewTreeDisplayList(view);
    //    → view.mRecreateDisplayList 由 PFLAG_INVALIDATED 决定
    //    → 调用 view.updateDisplayListIfDirty()（脏节点才重建）

    // 2. 消费并设置帧回调（在 onPostDraw 之前设置，保证滚动期间的回调更新也作用于本帧）
    if (mNextRtFrameCallbacks != null) {
        final ArrayList<FrameDrawingCallback> frameCallbacks = mNextRtFrameCallbacks;
        mNextRtFrameCallbacks = null;
        setFrameCallback(frame -> {
            for (int i = 0; i < frameCallbacks.size(); ++i) {
                frameCallbacks.get(i).onFrameDraw(frame);
            }
        });
    }

    // 3. 录制 Root RenderNode（仅在需要更新或无 DisplayList 时）
    if (mRootNodeNeedsUpdate || !mRootNode.hasDisplayList()) {
        RecordingCanvas canvas = mRootNode.beginRecording(mSurfaceWidth, mSurfaceHeight);
        try {
            final int saveCount = canvas.save();
            canvas.translate(mInsetLeft, mInsetTop);
            callbacks.onPreDraw(canvas);                       // 预绘制回调（不可发绘制命令）

            canvas.enableZ();
            canvas.drawRenderNode(view.updateDisplayListIfDirty()); // 子树挂到 Root
            canvas.disableZ();

            callbacks.onPostDraw(canvas);                      // 后绘制回调（可安全发命令）
            canvas.restoreToCount(saveCount);
            mRootNodeNeedsUpdate = false;
        } finally {
            mRootNode.endRecording();                          // 结束录制
        }
    }
    Trace.traceEnd(Trace.TRACE_TAG_VIEW);
}
```

**关键点**：
- 这一步完全在 UI 线程执行，产出的是 `RenderNode` 的 DisplayList（绘制命令序列）
- 只有脏节点（`PFLAG_INVALIDATED`）才重建 DisplayList，非脏节点直接复用
- `mRootNodeNeedsUpdate` 由 `invalidateRoot()` 置位，仅当根节点内容变化时重录

---

## 三、第二步：投递渲染任务到 RenderThread 消息队列（核心）

`ThreadedRenderer.draw()` 调用 `syncAndDrawFrame()`，这是 UI 线程与 RenderThread 的桥接点。

```java
// ThreadedRenderer.java 第 613-645 行
void draw(View view, AttachInfo attachInfo, DrawCallbacks callbacks) {
    attachInfo.mViewRootImpl.mViewFrameInfo.markDrawStart();

    updateRootDisplayList(view, callbacks);                    // ① 录制（UI线程）

    // 注册渲染器创建前就开始动画的 RenderNode（典型：首次绘制前启动的 animator）
    if (attachInfo.mPendingAnimatingRenderNodes != null) {
        final int count = attachInfo.mPendingAnimatingRenderNodes.size();
        for (int i = 0; i < count; i++) {
            registerAnimatingRenderNode(attachInfo.mPendingAnimatingRenderNodes.get(i));
        }
        attachInfo.mPendingAnimatingRenderNodes.clear();
        attachInfo.mPendingAnimatingRenderNodes = null;
    }

    final FrameInfo frameInfo = attachInfo.mViewRootImpl.getUpdatedFrameInfo();

    int syncResult = syncAndDrawFrame(frameInfo);              // ② 投递到RT队列
    if ((syncResult & SYNC_LOST_SURFACE_REWARD_IF_FOUND) != 0) {
        Log.w("OpenGLRenderer", "Surface lost, forcing relayout");
        attachInfo.mViewRootImpl.mForceNextWindowRelayout = true;
        attachInfo.mViewRootImpl.requestLayout();              // ③ Surface丢失 → 强制重布局
    }
    if ((syncResult & SYNC_REDRAW_REQUESTED) != 0) {
        attachInfo.mViewRootImpl.invalidate();                 // ④ RT请求重绘 → 下一帧
    }
}
```

```java
// HardwareRenderer.java 第 455-457 行
@SyncAndDrawResult
public int syncAndDrawFrame(@NonNull FrameInfo frameInfo) {
    return nSyncAndDrawFrame(mNativeProxy, frameInfo.frameInfo, frameInfo.frameInfo.length);
}
```

`nSyncAndDrawFrame` 是 JNI native 方法（HardwareRenderer.java 第 1308 行）：

```
nSyncAndDrawFrame(mNativeProxy, frameInfo, size)
    │
    ├── 1. 将 frameInfo（Vsync时间、窗口信息等）打包
    ├── 2. 构造 DrawFrameTask 对象
    ├── 3. 通过 RenderThread 的 Looper 投递 Message
    │      └── post(message) → 加入 RenderThread 的 MessageQueue
    ├── 4. 返回 syncResult（不等待绘制完成，异步）
    │
    └── RenderThread Looper 取出 Message 后执行：
         ├── a. syncFrameState()  — 同步 UI 线程的 DisplayList 到 RT
         ├── b. canDraw()         — 判断是否可以绘制
         ├── c. draw()            — GPU 绘制（OpenGL/Vulkan/Skia）
         ├── d. swapBuffers()     — 提交到 SurfaceFlinger
         └── e. 如果有 FrameCallback → onFrameDraw()
```

> **核心答案**：`nSyncAndDrawFrame`（通过 `syncAndDrawFrame` 调用）是将渲染任务加入 RenderThread 消息队列的关键节点。它将 UI 线程录制的 DisplayList + FrameInfo 打包为一个 DrawFrameTask，投递到 RenderThread 的 Native Looper 队列中，RenderThread 异步取出后执行 GPU 绘制和 Buffer 提交。

### 3.1 调用点位置定位

```java
int syncResult = syncAndDrawFrame(frameInfo);   // ThreadedRenderer.java L634
```

| 维度 | 信息 |
|------|------|
| 所在文件 | `ThreadedRenderer.java` |
| 所在方法 | `draw(View view, AttachInfo attachInfo, DrawCallbacks callbacks)`（L613–645，方法体共 33 行） |
| 所在行 | L634，处于方法体中部，`updateRootDisplayList()` 调用之后、返回值处理之前 |
| 方法可见性 | `void draw(...)` 包内私有（无修饰符），仅限 `android.view` 包调用 |

### 3.2 三层调用链定位图

```
【调用方】 ViewRootImpl.java:4418  (performDraw() 方法内)
    mAttachInfo.mThreadedRenderer.draw(mView, mAttachInfo, this);
        │
        ▼
【当前方法】 ThreadedRenderer.java:613  draw(View, AttachInfo, DrawCallbacks)
    │  614   markDrawStart()                    — 记录帧绘制开始时间
    │  616   updateRootDisplayList()            — ① UI线程录制 DisplayList
    │  620   registerAnimatingRenderNode()      — ② 注册动画 RenderNode
    │  632   getUpdatedFrameInfo()              — ③ 取 FrameInfo（含Vsync时间）
    │  634   syncAndDrawFrame(frameInfo)  ◄─────── 本行
    │  635   SYNC_LOST_SURFACE → requestLayout() — ④ 返回值处理
    │  642   SYNC_REDRAW_REQUESTED → invalidate() — ⑤ 请求下一帧
        │
        ▼
【被调用方】 HardwareRenderer.java:455  syncAndDrawFrame()（父类，ThreadedRenderer 继承）
    return nSyncAndDrawFrame(mNativeProxy, ...) — JNI 投递任务到 RenderThread 队列
```

### 3.3 所在方法 `ThreadedRenderer.draw()` 逐段详解

| 行号 | 代码 | 作用 |
|------|------|------|
| 614 | `attachInfo.mViewRootImpl.mViewFrameInfo.markDrawStart()` | 标记帧开始绘制时间，供 `FrameInfo` 统计各阶段耗时 |
| 616 | `updateRootDisplayList(view, callbacks)` | **UI 线程**上录制整棵 View 树的 DisplayList（脏节点才重建） |
| 620–630 | `registerAnimatingRenderNode(...)` | 注册在渲染器创建前就已启动的动画 RenderNode（一次性迁移，后续动画直接走 `attachRenderNodeAnimator`） |
| 632 | `getUpdatedFrameInfo()` | 从 `mViewFrameInfo` 取出打包好的 `FrameInfo`（含 Vsync 时间、输入事件、绘制开始时间等 17 个时间戳槽位） |
| **634** | **`syncAndDrawFrame(frameInfo)`** | **核心：将渲染任务投递到 RenderThread 消息队列** |
| 635–641 | `SYNC_LOST_SURFACE_REWARD_IF_FOUND` | Surface 丢失（如锁屏、窗口销毁），置 `mForceNextWindowRelayout = true` 并 `requestLayout()` 强制下一帧重新布局拿新 Surface |
| 642–644 | `SYNC_REDRAW_REQUESTED` | RenderThread 请求 UI 线程下一帧重绘（动画仍在进行，RT 无法自驱动），调用 `invalidate()` |

> 注意 `draw()` 方法内部**不等待**渲染完成——`syncAndDrawFrame` 是异步投递，立即返回 `syncResult`。

### 3.4 被调用方 `HardwareRenderer.syncAndDrawFrame()` 详解

```java
// HardwareRenderer.java 第 449-457 行
/**
 * Syncs the RenderNode tree to the render thread and requests a frame to be drawn.
 * @hide
 */
@SyncAndDrawResult
public int syncAndDrawFrame(@NonNull FrameInfo frameInfo) {
    return nSyncAndDrawFrame(mNativeProxy, frameInfo.frameInfo, frameInfo.frameInfo.length);
}
```

| 要点 | 说明 |
|------|------|
| 声明位置 | `HardwareRenderer.java:455`，`ThreadedRenderer` 的**父类**，通过继承调用（`ThreadedRenderer extends HardwareRenderer`） |
| 可见性 | `public` 但标注 `@hide`，仅系统内部可用 |
| `@SyncAndDrawResult` | 自定义注解，限定返回值必须为 `SYNC_*` 常量之一 |
| 参数 1 `mNativeProxy` | long 型 Native 代理句柄，构造渲染器时由 `nCreateProxy()` 创建，是 JNI 层 RenderProxy 的引用 |
| 参数 2 `frameInfo.frameInfo` | `long[FRAME_INFO_SIZE=17]` 数组，即上文 `getUpdatedFrameInfo()` 的产物 |
| 参数 3 `frameInfo.length` | 数组长度 17，传给 native 层做越界校验 |
| 返回值 | `int` syncResult，就是本行代码接收的 `syncResult` |

### 3.5 JNI 层 `nSyncAndDrawFrame`（消息队列投递）

`nSyncAndDrawFrame` 声明于 `HardwareRenderer.java:1308`，native 实现在 `libhwui` 的 `RenderProxy.cpp`，其投递流程见第三节开头的 JNI 调用图：

```
nSyncAndDrawFrame(proxy, frameInfo, size)
  ├── 1. frameInfo 打包（Vsync 时间、输入事件时间、绘制开始时间等）
  ├── 2. 构造 DrawFrameTask
  ├── 3. RenderThread.getInstance().getLooper().post(task)  ← 加入RT消息队列
  ├── 4. 立即返回 syncResult（异步）
  │
  └── RenderThread 取出任务后执行：
       ├── syncFrameState()  同步 DisplayList
       ├── canDraw()         判断 Surface/资源可用性
       ├── draw()            GPU 绘制
       └── swapBuffers()     提交 SurfaceFlinger
```

### 3.6 上游调用者 `ViewRootImpl.performDraw()`

`ViewRootImpl.java:4418`：

```java
if (isHardwareEnabled()) {
    ...
    mAttachInfo.mThreadedRenderer.draw(mView, mAttachInfo, this);
    ...
}
```

前置条件（L4380–4417）：
- 脏区域非空 / 动画中 / 无障碍焦点变化 → 才执行硬件绘制
- `invalidateRoot()` 置位时先通知渲染器根节点需要重录
- `mReportNextDraw` 时先 `setStopped(false)` 恢复渲染

### 3.7 一句话总结

> `ThreadedRenderer.draw()`（L613，由 `ViewRootImpl.performDraw()` 每帧调用）中的 L634 是**帧产出的投递点**：它调用继承自 `HardwareRenderer` 的 `syncAndDrawFrame()`（L455），后者通过 JNI `nSyncAndDrawFrame` 将 UI 线程录制好的 DisplayList 与 FrameInfo 打包成 DrawFrameTask 投递到 RenderThread 的 Native 消息队列，并同步返回 `SYNC_*` 结果码用于后续的 Surface 恢复与重绘调度。

---

## 四、第三步：返回值处理

| 返回值 | 值 | 含义 | 调用方处理 |
|--------|----|------|-----------|
| `SYNC_OK` | 0 | 正常，渲染任务已投递 | 无 |
| `SYNC_REDRAW_REQUESTED` | 1<<0 | RT 请求下一帧重绘（动画进行中，RT 无法自驱动） | `invalidate()` |
| `SYNC_LOST_SURFACE_REWARD_IF_FOUND` | 1<<1 | Surface 丢失（如 `Surface.release()`） | `mForceNextWindowRelayout = true` + `requestLayout()` |
| `SYNC_CONTEXT_IS_STOPPED` | 1<<2 | 渲染器已停止（内容已同步但未产帧） | 无 |
| `SYNC_FRAME_DROPPED` | 1<<3 | 帧被丢弃（本 Vsync 已渲染过或 RT 跑赢消费端，RT 内部会重新调度下一帧） | 无 |

```java
// ThreadedRenderer.java 第 634-644 行
int syncResult = syncAndDrawFrame(frameInfo);

if ((syncResult & SYNC_LOST_SURFACE_REWARD_IF_FOUND) != 0) {
    attachInfo.mViewRootImpl.mForceNextWindowRelayout = true;
    attachInfo.mViewRootImpl.requestLayout();
}
if ((syncResult & SYNC_REDRAW_REQUESTED) != 0) {
    attachInfo.mViewRootImpl.invalidate();
}
```

---

## 五、另一种路径：FrameRenderRequest.syncAndDraw()

`SimpleRenderer`（ThreadedRenderer 内部类，仅用于 Magnifier 等同步渲染场景）展示了直接 API 路径：

```java
// ThreadedRenderer.java 第 697-705 行
public void draw(final FrameDrawingCallback callback) {
    final long vsync = AnimationUtils.currentAnimationTimeMillis() * 1000000L;
    if (callback != null) {
        setFrameCallback(callback);
    }
    createRenderRequest()          // ① 创建（复用实例，每次 reset）
        .setVsyncTime(vsync)       // ② 设置 Vsync 时间（CLOCK_MONOTONIC 纳秒）
        .syncAndDraw();            // ③ 投递到 RenderThread 队列
}
```

```java
// HardwareRenderer.java 第 332-431 行（FrameRenderRequest 内部类）
public final class FrameRenderRequest {
    private FrameInfo mFrameInfo = new FrameInfo();
    private boolean mWaitForPresent;

    private void reset() {
        mWaitForPresent = false;
        mRenderRequest.setVsyncTime(
                AnimationUtils.currentAnimationTimeMillis() * TimeUtils.NANOS_PER_MS);
    }

    public @NonNull FrameRenderRequest setVsyncTime(long vsyncTime) {
        mFrameInfo.setVsync(vsyncTime, vsyncTime, FrameInfo.INVALID_VSYNC_ID, Long.MAX_VALUE,
                vsyncTime, -1);
        mFrameInfo.addFlags(FrameInfo.FLAG_SURFACE_CANVAS);
        return this;
    }

    public @NonNull FrameRenderRequest setFrameCommitCallback(@NonNull Executor executor,
            @NonNull Runnable frameCommitCallback) {
        setFrameCompleteCallback(frameNr -> executor.execute(frameCommitCallback));
        return this;
    }

    public @NonNull FrameRenderRequest setWaitForPresent(boolean shouldWait) {
        mWaitForPresent = shouldWait;
        return this;
    }

    @SyncAndDrawResult
    public int syncAndDraw() {
        int syncResult = syncAndDrawFrame(mFrameInfo);   // 同样走 nSyncAndDrawFrame
        if (mWaitForPresent && (syncResult & SYNC_FRAME_DROPPED) == 0) {
            fence();                                     // 阻塞等待帧提交到 Surface
        }
        return syncResult;
    }
}
```

**注意**：`FrameRenderRequest` 非线程安全，不可持有超过一帧；系统内部复用实例以减少分配开销。

---

## 六、消息队列相关 Native 方法一览

| Java 方法 | Native 方法 | 作用 |
|-----------|------------|------|
| `syncAndDrawFrame()` | `nSyncAndDrawFrame` | **投递渲染任务到 RT 队列** |
| `fence()` | `nFence` | 阻塞等待 RT 队列中所有任务完成 |
| `notifyFramePending()` | `nNotifyFramePending` | 通知 RT 即将有一帧到来（帮助 RT 调度自驱动动画，避免同一 Vsync 产多帧） |
| `pause()` | `nPause` | 暂停 RT 渲染（返回是否有未完成任务；Surface 变更前使用） |
| `stopDrawing()` | `nStopDrawing` | 停止 RT 绘制（DisplayList 中的 Functor 指针不再安全时使用，如 WebView） |
| `setStopped()` | `nSetStopped` | 硬停止 RT（sync 仍执行但返回 `SYNC_CONTEXT_IS_STOPPED`，不产帧） |
| `setFrameCallback()` | `nSetFrameCallback` | 设置帧绘制回调（RT 线程执行，仅触发一次） |
| `setFrameCompleteCallback()` | `nSetFrameCompleteCallback` | 帧完成回调 |

---

## 七、总结流程图

```
Choreographer Vsync 信号
    │
    ▼
ViewRootImpl.performTraversals()
    │
    ▼
ViewRootImpl.performDraw()
    │
    ▼
ThreadedRenderer.draw(view, attachInfo, callbacks)
    │
    ├─ updateRootDisplayList()        [UI Thread] 录制 DisplayList
    │    ├─ updateViewTreeDisplayList()  递归遍历 View 树（脏节点重建）
    │    └─ mRootNode.beginRecording()   Root 节点录制 + onPreDraw/onPostDraw
    │
    ├─ syncAndDrawFrame(frameInfo)    [UI Thread → JNI → RenderThread]
    │    └─ nSyncAndDrawFrame()
    │         ├─ 打包 frameInfo + DisplayList 引用
    │         ├─ 构造 DrawFrameTask
    │         ├─ 投递到 RenderThread MessageQueue  ← 加入消息队列
    │         └─ 返回 syncResult (异步，不阻塞)
    │
    └─ 检查 syncResult
         ├─ SYNC_REDRAW_REQUESTED → invalidate() 请求下一帧
         └─ SYNC_LOST_SURFACE → requestLayout() 重新布局
```

---

## 八、关键源码位置索引

| 内容 | 文件 | 行号 |
|------|------|------|
| `ThreadedRenderer.draw()` | ThreadedRenderer.java | 613-645 |
| `updateRootDisplayList()` | ThreadedRenderer.java | 538-574 |
| `updateViewTreeDisplayList()` | ThreadedRenderer.java | 529-536 |
| `invalidateRoot()` | ThreadedRenderer.java | 603-605 |
| `syncAndDrawFrame()` | HardwareRenderer.java | 455-457 |
| `FrameRenderRequest.syncAndDraw()` | HardwareRenderer.java | 424-430 |
| `createRenderRequest()` | HardwareRenderer.java | 444-447 |
| `SimpleRenderer.draw()` | ThreadedRenderer.java | 697-705 |
| `nSyncAndDrawFrame` 声明 | HardwareRenderer.java | 1308 |
| `SYNC_*` 返回值定义 | HardwareRenderer.java | 88-122 |
| `nFence` 声明 | HardwareRenderer.java | 1335 |

---

## 九、Native 全链路补遗（基于本仓库 Android 10 源码）

> 以下基于 `c:/D/android_project/cells-android10` 中 Android 10（coral）的 `frameworks/base`
> 实际源码，对第三节「异步、不等待绘制完成」的说法做精确化修正，并补全从 JNI 到
> SurfaceFlinger 的完整路径。

### 9.1 JNI 桥 `android_view_ThreadedRenderer_syncAndDrawFrame`

文件：`frameworks/base/core/jni/android_view_ThreadedRenderer.cpp:686`

```cpp
static int android_view_ThreadedRenderer_syncAndDrawFrame(JNIEnv* env, jobject clazz,
        jlong proxyPtr, jlongArray frameInfo, jint frameInfoSize) {
    LOG_ALWAYS_FATAL_IF(frameInfoSize != UI_THREAD_FRAME_INFO_SIZE,
            "Mismatched size expectations, given %d expected %d",
            frameInfoSize, UI_THREAD_FRAME_INFO_SIZE);
    RenderProxy* proxy = reinterpret_cast<RenderProxy*>(proxyPtr);
    env->GetLongArrayRegion(frameInfo, 0, frameInfoSize, proxy->frameInfo()); // 拷贝 FrameInfo 到 proxy
    return proxy->syncAndDrawFrame();
}
```

JNI 注册（`android_view_ThreadedRenderer.cpp:1107`）：
`{ "nSyncAndDrawFrame", "(J[JI)I", (void*) android_view_ThreadedRenderer_syncAndDrawFrame }`

要点：先断言 `frameInfo` 数组长度等于 `UI_THREAD_FRAME_INFO_SIZE`，再把主线程
`Choreographer.mFrameInfo` 的帧时间戳（drawStart、vsync 等）通过 `GetLongArrayRegion`
拷进 `RenderProxy` 持有的 `frameInfo()` 缓冲（`DrawFrameTask` 内），随后转调
`RenderProxy::syncAndDrawFrame()`，将其返回值（sync 结果位掩码）原样返回 Java。

### 9.2 `RenderProxy::syncAndDrawFrame` → `DrawFrameTask::drawFrame`

文件：`frameworks/base/libs/hwui/renderthread/RenderProxy.cpp:124`

```cpp
int RenderProxy::syncAndDrawFrame() {
    return mDrawFrameTask.drawFrame();
}
```

文件：`frameworks/base/libs/hwui/renderthread/DrawFrameTask.cpp:68`

```cpp
int DrawFrameTask::drawFrame() {
    LOG_ALWAYS_FATAL_IF(!mContext, "Cannot drawFrame with no CanvasContext!");
    mSyncResult = SyncResult::OK;
    mSyncQueued = systemTime(CLOCK_MONOTONIC);  // 记录入队时刻，供帧耗时统计
    postAndWait();                              // 投到 RT 队列并等 sync 阶段完成
    return mSyncResult;                         // 返回 sync 结果位掩码
}
```

### 9.3 `postAndWait`：UI 主线程在此阻塞（修正第三章「异步」说法）

文件：`frameworks/base/libs/hwui/renderthread/DrawFrameTask.cpp:78`

```cpp
void DrawFrameTask::postAndWait() {
    AutoMutex _lock(mLock);
    mRenderThread->queue().post([this]() { run(); });  // 把 run() 投递到 RT 消息队列（对 RT 非阻塞）
    mSignal.wait(mLock);                               // UI 主线程持锁阻塞，直到 RT 发信号
}
```

**关键修正**：第三节第 148、206、241 行称「异步、不等待绘制完成」只说对了一半——
对**渲染线程**是 `post` 非阻塞派发，但对**UI 主线程**是通过 `mSignal.wait(mLock)`
阻塞等待的。阻塞仅覆盖「sync 阶段」，不等真正的出帧（draw/上屏）。

`run()` 的唤醒时机（`DrawFrameTask.cpp:84`）：

```cpp
void DrawFrameTask::run() {
    bool canUnblockUiThread;
    bool canDrawThisFrame;
    {
        TreeInfo info(TreeInfo::MODE_FULL, *mContext);
        canUnblockUiThread = syncFrameState(info);   // sync 显示列表/图层/prepareTree
        canDrawThisFrame = info.out.canDrawThisFrame;
        ...
    }
    if (canUnblockUiThread) {
        unblockUiThread();                            // ← 唤醒 UI 主线程的点（draw 之前）
    }
    if (CC_LIKELY(canDrawThisFrame)) {
        context->draw();                              // 真正出帧（RT 上，主线程已返回）
    } else {
        context->waitOnFences();
    }
    if (!canUnblockUiThread) {
        unblockUiThread();
    }
}
```

即 UI 主线程被阻塞的时长只到 `syncFrameState()`（把显示列表 sync 成 GPU 资源、
`prepareTree`）完成即被唤醒，`context->draw()` 在 RT 上继续跑。这样 `drawFrame()`
返回时 `mSyncResult` 已完整就位（对应 `ThreadedRenderer.java:680` 对
`SYNC_LOST_SURFACE_REWARD_IF_FOUND` / `SYNC_REDRAW_REQUESTED` 的判断），又把主线程
等待压到最短。

`syncFrameState()` 写入的返回位（`DrawFrameTask.cpp:128`）：
- `LostSurfaceRewardIfFound`（`:146`）—— Surface 丢失
- `ContextIsStopped`（`:149`）—— 有 Surface 但 context 停了
- `UIRedrawRequired`（`:156`）—— 有动画且需 UI 重绘
- `FrameDropped`（`:160`）—— 本帧不能画

（位定义见 `DrawFrameTask.h:42`：OK=0, UIRedrawRequired=1<<0, LostSurfaceRewardIfFound=1<<1,
ContextIsStopped=1<<2, FrameDropped=1<<3，与第四章 Java 侧 `SYNC_*` 一一对应。）

### 9.4 `context->draw()`：把 buffer 提交给 SurfaceFlinger

文件：`frameworks/base/libs/hwui/renderthread/CanvasContext.cpp:433`

```cpp
void CanvasContext::draw() {
    SkRect dirty;
    mDamageAccumulator.finish(&dirty);
    if (dirty.isEmpty() && Properties::skipEmptyFrames && !surfaceRequiresRedraw()) {
        mCurrentFrameInfo->addFlag(FrameInfoFlags::SkippedFrame);
        return;                                              // 空帧跳过
    }
    mCurrentFrameInfo->markIssueDrawCommandsStart();
    Frame frame = mRenderPipeline->getFrame();               // ① 向 BufferQueue dequeue 一块 buffer
    setPresentTime();
    SkRect windowDirty = computeDirtyRect(frame, &dirty);
    bool drew = mRenderPipeline->draw(frame, windowDirty, dirty, ...); // ② Skia 渲染进该 buffer
    waitOnFences();
    bool requireSwap = false;
    bool didSwap = mRenderPipeline->swapBuffers(frame, drew, windowDirty,
                                                mCurrentFrameInfo, &requireSwap); // ③ 交还 BufferQueue
    ...
    if (didSwap) { for (auto& func : mFrameCompleteCallbacks) std::invoke(func, ...); } // 触发 onFrameComplete
    mJankTracker.finishFrame(*mCurrentFrameInfo);            // 帧耗时统计
}
```

以默认 `SkiaOpenGLPipeline` 为例（`frameworks/base/libs/hwui/pipeline/skia/SkiaOpenGLPipeline.cpp`）：

① `getFrame()` → `EglManager::beginFrame()`（`EglManager.cpp:425`）调用
   `eglBeginFrame` → 内部 `ANativeWindow::dequeueBuffer` 从 BufferQueue 取一块
   空闲 GraphicBuffer 作为 EGLSurface 后端存储。

② `draw()`（`:71`）用 Skia 把 `renderNodes`（显示列表）渲染到以该 GraphicBuffer 为
   后备的 `SkSurface`（FBO0），光栅化进 buffer。

③ `swapBuffers()`（`:124`）→ `EglManager::swapBuffers()`（`EglManager.cpp:454`）：

```cpp
bool EglManager::swapBuffers(const Frame& frame, const SkRect& screenDirty) {
    ...
    eglSwapBuffersWithDamageKHR(mEglDisplay, frame.mSurface, rects,
                                screenDirty.isEmpty() ? 0 : 1);  // 内部 ANativeWindow::queueBuffer
    ...
}
```

`eglSwapBuffersWithDamageKHR` 内部调用 `ANativeWindow::queueBuffer`，把刚画好的
GraphicBuffer 通过 BufferQueue 提交给**消费者 SurfaceFlinger**。queue 后 SF 在下一轮
合成中 `acquireBuffer` 拿到该帧，经 HWC 合成上屏。

`CanvasContext::draw()` 收尾（`:457`）在 swap 成功后把 `DequeueBufferDuration` /
`QueueBufferDuration` 写进 FrameInfo（`CanvasContext.cpp:488-489`），并触发
`FrameComplete` 回调——这些都是 Systrace / GPU 渲染模式条形图 / 掉帧分析的来源。

### 9.5 完整提交链路一图

```
android_view_ThreadedRenderer_syncAndDrawFrame (JNI)
  └─ RenderProxy::syncAndDrawFrame()
       └─ DrawFrameTask::drawFrame()
            └─ DrawFrameTask::postAndWait()        ← UI 主线程在此阻塞（仅到 sync 阶段）
                 ├─ queue().post(run)  →  RenderThread
                 └─ mSignal.wait() 被 run() 中的 unblockUiThread() 唤醒
            └─（返回 mSyncResult 给 Java）

RenderThread 侧 run()：
  syncFrameState()   → 设 mSyncResult 各位，unblockUiThread() 唤醒 UI 线程
  context->draw()    → CanvasContext::draw()
       ├─ getFrame()            → EGL dequeueBuffer（从 BufferQueue 取 GraphicBuffer）
       ├─ draw()                → Skia 渲染进 GraphicBuffer
       └─ swapBuffers()         → EGL queueBuffer（还给 BufferQueue）
                                        │
                                        ▼
                              SurfaceFlinger（BufferQueue 消费者）
                                acquireBuffer → HWC 合成上屏
```

> **结论**：hwui 并不主动调用 SurfaceFlinger，而是通过 `ANativeWindow` 这对
> BufferQueue 的 producer/consumer 接口完成 `dequeue → 渲染 → queue`；SurfaceFlinger
> 作为消费者被动收到 buffer，再由其合成循环（见 `flinger->init()` / `main()` 那条链）
> 走 HWC 上屏。之前第三章「异步、不等待」应精确理解为：对渲染线程非阻塞派发，对 UI
> 主线程阻塞到 sync 阶段完成即返回，出帧在 RT 上异步继续。

