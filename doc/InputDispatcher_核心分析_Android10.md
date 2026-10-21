# InputDispatcher 核心分析（Android 10 / android-10.0.0_r33）

> 范围：本仓 `frameworks/native/services/inputflinger/InputDispatcher.{h,cpp}`（cpp 5246 行 + h 1305 行，Q 为扁平布局，头实现同目录）。本篇讲 **派发中枢的内部机制**：数据模型、事件入队、目标命中、政策闸门、发布周期、锁模型。
> 边界：ANR 的触发/上报/裁决全流程见姊妹篇 `doc/inputDispatcher_anr.md`，本篇只把 ANR 作为「目标未就绪」的一环带过并交叉引用，不重复展开。
> 定位：它是 `InputReader` 的下游（承接 `NotifyKey/MotionArgs`），是 `WMS/App` 的上游（经 `InputChannel` socketpair 把事件发给窗口）。此前 `doc/输入事件处理流程_Android10.md` 给了链路概览，本篇钻入 Dispatcher 单体的 `find / intercept / publish / timeout` 四块。

---

## 0. 结论前置

1. **单线程 + 一把锁 + Looper 唤醒**。`InputDispatcher` 全部状态在 `mLock`（`InputDispatcher.h:919`，`std::mutex`）之后，跑在 `InputDispatcherThread`（`InputDispatcher.h:1292`）单线程上；入口 `dispatchOnce`（`InputDispatcher.cpp:265`）每轮 `scoped_lock` → 派发 → 跑命令 → `mLooper->pollOnce(timeoutMillis)` 定时休眠。线程安全靠「状态归单线程 + 其他线程投递 Command」，不靠细粒度锁。
2. **两套队列模型贯穿全程**：入侧 `mInboundQueue`（原生事件排队，`h:926`）；出侧每个 `Connection` 各持 `outboundQueue`（待发）+ `waitQueue`（已发未确认，`h:885/889`）。一个事件从进 `mInboundQueue` 到发完，会依次挂到若干 Connection 的两段队列上——**这是背压与 ANR 判定的物理基础**。
3. **命中（hit-test）与焦点（focus）是两条路**：指针类（触摸）走 `findTouchedWindowTargetsLocked`（`cpp:1258`）→ 底层 `findTouchedWindowAtLocked`（`cpp:511`）按 Z 序自上而下找未被遮挡（`isWindowObscuredAtPointLocked:1761`）的可触摸窗口；非指针类（键盘/轨迹球）走 `findFocusedWindowTargetsLocked`（`cpp:1195`）取当前焦点窗口。**触摸点命中 ≠ 焦点**，这是排查「点了没反应」的第一分岔。
4. **政策（policy）在两个点介入，且从不持锁调用**：入队前 `interceptKey/MotionBeforeQueueing`（`cpp:2685/2760`）、派发前 `interceptKeyBeforeDispatching`（`cpp:4163`）。头文件 `h:394-410` 明文写出不变式——「Dispatcher 绝不在持 `mLock` 时回调 policy」，靠 **CommandEntry（`h:648`）+ LockedInterruptible** 机制实现：先入命令队列，跑命令时先 `mLock.unlock()` 再回调 policy 再 `lock()`（如 `doNotifyANRLockedInterruptible`、`doInterceptKeyBeforeDispatchingLockedInterruptible`）。
5. **发布是「有确认的写」**：`startDispatchCycleLocked`（`cpp:2160`）把事件 `publishKeyEvent/publishMotionEvent`（`cpp:2185/2230`）写入 socket，成功后挪进 `waitQueue` 等 App 的 finished 回执；若 socket 满（`WOULD_BLOCK`）置 `inputPublisherBlocked`（`cpp:2265`）暂停发、等 App 消费。发布坐标还会按 `DispatchEntry` 的 scale/offset 重新缩放（`cpp:2200-2227`），所以窗口收到的坐标是窗口本地坐标而非物理坐标。
6. **目标 5 秒不消费 = Input ANR**：`handleTargetsNotReadyLocked`（`cpp:1044`）以 `DEFAULT_INPUT_DISPATCHING_TIMEOUT=5000ms`（`cpp:74`）计时，超时 `onANRLocked`（`cpp:4070`）截 dump + 投递 `doNotifyANR` → `mPolicy->notifyANR`（`cpp:4140`）→ Java `InputManagerService.notifyANR`（`L1839`）→ `mWindowManagerCallbacks.notifyANR`（`L1840`）进 WMS/AMS。

---

## 1. 总览：它在输入管线里的位置

```
内核 evdev
  → EventHub.getEvents (epoll)              [EventHub_分析_Android10.md]
  → InputReader.loopOnce                     [InputReader.cpp_解析.md]
  → QueuedInputListener.flush
  → InputClassifier（异步分类, 追加 MotionClassification）  [InputClassifier_分析解读_Android10.md]
  ⇒ InputDispatcher.notifyKey/notifyMotion   ← 本篇起点（cpp:2647/2724）
      ├─ policy.intercept*BeforeQueueing     （cpp:2685/2760）
      ├─ enqueueInboundEventLocked           （cpp:443 → mInboundQueue）
      └─ Looper 唤醒 → dispatchOnce          （cpp:265）
          └─ dispatchOnceInnerLocked         （cpp:290）
              ├─ dispatchKeyLocked / dispatchMotionLocked   （cpp:790/905）
              │    ├─ policy.interceptKeyBeforeDispatching （cpp:4163）
              │    ├─ find{Touched/Focused}WindowTargetsLocked（cpp:1258/1195, hit-test cpp:511）
              │    ├─ addGlobalMonitoringTargetsLocked     （cpp:1714, 抄送给 monitor）
              │    └─ dispatchEventLocked（cpp:1018）→ 每个目标
              │         └─ prepare/startDispatchCycleLocked（cpp:2160）
              │              ├─ inputPublisher.publish{Key/Motion}Event（cpp:2185/2230）
              │              ├─ 成功→waitQueue / 满→inputPublisherBlocked（cpp:2278/2265）
              │              └─ 目标未就绪 5s → onANRLocked（cpp:4070）
              └─ App 侧 ViewRootImpl 读 socket（InputChannel 非 Binder）
                   └─ 回执 finished → waitQueue 出队
```

一条 **原生触摸**从内核到 App 主线程的三进程路径：
`system_server`（InputReader/Classifier/Dispatcher 都在此，两条 native 线程）→ socketpair（`InputChannel`）→ App 进程 `ViewRootImpl`。政策拦截回调则反向：Dispatcher（native）持锁外回调 `InputManagerService`（Java，同 system_server）→ `PhoneWindowManager`。

---

## 2. 核心数据模型（`InputDispatcher.h`）

### 2.1 EventEntry 家族 —— 入队事件的抽象

`EventEntry`（`h:478`，`Link<EventEntry>` 双向链表节点）是 `mInboundQueue` 里排队的单位，四类派生：

| 类型 | 行 | 关键字段 | 说明 |
|---|---|---|---|
| `ConfigurationChangedEntry` | `h:507` | — | 配置变化，永不被 drop（`cpp:377`） |
| `DeviceResetEntry` | `h:515` | `deviceId` | 设备重置，永不被 drop（`cpp:385`） |
| `KeyEntry` | `h:525` | `keyCode/scanCode/repeatCount/downTime/interceptKeyResult` | `interceptKeyResult` 缓存政策拦截结果（UNKNOWN/SKIP/CONTINUE/TRY_AGAIN_LATER，`h:539-546`） |
| `MotionEntry` | `h:559` | `pointerCount/pointerCoords[MAX_POINTERS]/classification` | 携带 `MotionClassification`（上一步 Classifier 打的标签，`h:569`）与多指坐标 |

公共字段：`sequenceNum`（`h:486`，全局唯一，回执按它匹配）、`refCount`（引用计数，事件被命令/发布引用时 +1）、`policyFlags`、`dispatchInProgress`（`h:493`，防止同一事件被二次派发）。

### 2.2 DispatchEntry —— 事件 × 目标 的派发记录

`DispatchEntry`（`h:593`）描述「把 eventEntry 派发给某 Connection 且已定的目标参数」：

- `seq`（`h:594`，`nextSeq()` 自增，**永不复用且 ≠0**）：这条派发的唯一号，App 回执用它对号。
- `targetFlags`（`h:597`）：含 `FLAG_FOREGROUND`（`h:614` `hasForegroundTarget()`）与 `FLAG_SPLIT`（`h:618` `isSplit()`，多指分到多个窗口时置位）。
- 坐标变换：`xOffset/yOffset/globalScaleFactor/windowXScale/windowYScale`（`h:598-602`），在 `startDispatchCycleLocked` 里对指针坐标重缩放（`cpp:2200-2227`），使 App 收到的是窗口本地坐标。
- `resolvedAction/resolvedFlags`（`h:606-607`）：入队时解析定稿的 action/flags。

### 2.3 CommandEntry —— 异步政策回调的载体

`CommandEntry`（`h:648`）+ 函数指针 `Command`（`h:645`，`void (InputDispatcher::*)(CommandEntry*)`）。它把「需要回调 policy、不能持锁做」的动作推迟到 dispatch 循环的安全点执行。命令可 **自愿 unlock→调 policy→relock**（`h:628-643` 注释即 LockedInterruptible 不变式）；典型命令：`doInterceptKeyBeforeDispatchingLockedInterruptible`、`doNotifyANRLockedInterruptible`、`doNotifyConfigurationChanged`（`cpp:4105`，`unlock`→`mPolicy->notifyConfigurationChanged`→`lock`）。`mCommandQueue`（`h:928`）挂这些命令；`dispatchOnce` 里「先派发、再跑命令」的顺序（`cpp:273-281`）保证临界派发先完成。

### 2.4 Connection —— 一个窗口的派发句柄

`Connection`（`h:860`，`RefBase`）= 一条 `InputChannel` 在 Dispatcher 侧的镜像（注册入口 `registerInputChannel` `cpp:3828`）：

| 字段 | 行 | 含义 |
|---|---|---|
| `status` | `h:874` | `STATUS_NORMAL/BROKEN/ZOMBIE`（`h:865-872`），连接状态机 |
| `inputChannel` / `inputPublisher` | `h:875/877` | 持有的 channel 与写端 |
| `inputState` | `h:878` | `InputState`（`h:766`），记录已发的键/触摸 memento，用于**断连时合成 cancel 事件**（`synthesizeCancelationEvents` `h:789`） |
| `inputPublisherBlocked` | `h:882` | socket 满、暂停发，等 App 消费 |
| `outboundQueue` | `h:885` | 待发布的派发条目 |
| `waitQueue` | `h:889` | 已发布、等 App finished 回执的条目 |

### 2.5 全局状态成员（`h:916-1113`）

- **锁与唤醒**：`mLock`（`h:919`）、`mDispatcherIsAlive`（`h:921`，heartbeat `monitor()` 用它判 Dispatcher 是否卡死）、`mLooper`（`h:923`）。
- **入侧队列**：`mPendingEvent`（`h:925`，当前正在派发的那条）、`mInboundQueue`（`h:926`）、`mRecentQueue`（`h:927`，保留近期事件供 dump 排查）、`mCommandQueue`（`h:928`）。
- **窗口/目标注册表**：`mConnectionsByFd`（`h:962`，按 channel fd 索引 Connection）、`mInputChannelsByToken`（`h:969`）、`mWindowHandlesByDisplay`（`h:1044`）、`mFocusedWindowHandlesByDisplay`（`h:1055`，按屏焦点窗口）、`mTouchStatesByDisplay`（`h:1105`，当前触摸会话 `TouchState`，`h:1074`，含 `windows`/`portalWindows`/`gestureMonitors`）。
- **monitor 注册表**：`mGlobalMonitorsByDisplay`（`h:979`，全量抄送）/ `mGestureMonitorsByDisplay`（`h:986`，手势抢占，能 `pilferPointers` 抢走指针流）。
- **开关**：`mDispatchEnabled`（`h:1040`）/ `mDispatchFrozen`（`h:1041`，冻结时 `dispatchOnceInnerLocked` 直接 return `cpp:301`）/ `mInputFilterEnabled`（`h:1042`，事件改送 policy 的 filter 而非直接派发）。
- **ANR 计时**：`mInputTargetWaitCause/StartTime/TimeoutTime/TimeoutExpired/ApplicationToken`（`h:1142-1146`）、`mLastANRState`（`h:1116`，最近一次 ANR 的 dump 快照）。

---

## 3. 事件生命周期分步拆解

### 3.1 入队：`notifyKey/notifyMotion` → `enqueueInboundEventLocked`

- `notifyKey`（`cpp:2647`）：先 `mPolicy->interceptKeyBeforeQueueing`（`cpp:2685`）让政策改 policyFlags（例如把 HOME/音量等标成非 `PASS_TO_USER`），再 new `KeyEntry` 入队。
- `notifyMotion`（`cpp:2724`）：先 `mPolicy->interceptMotionBeforeQueueing`（`cpp:2760`），new `MotionEntry`（带 `classification`，来自上游 Classifier）入队。
- `enqueueInboundEventLocked`（`cpp:443`）：`needWake = mInboundQueue.isEmpty()`（`cpp:444`），`enqueueAtTail`（`cpp:445`）。**返回值告诉调用方是否要 `mLooper->wake()`**——只有从空变非空才唤醒，避免高频触摸每帧都 wake。
- `injectInputEvent`（`cpp:2843`）：App/测试注入走这里，内部先走政策 `intercept*BeforeQueueing`（`cpp:2885/2916`），可挂 `InjectionState`（`h:462`）阻塞等待结果。

**丢弃判定**（`dispatchOnceInnerLocked` `cpp:361-424` 的 `dropReason` 状态机）：
`POLICY`（无 `PASS_TO_USER` 标，`cpp:363`）/ `DISABLED`（`mDispatchEnabled==false`，`cpp:364`）/ `APP_SWITCH`（切 App 键已到期，`cpp:396`）/ `STALE`（`isStaleEvent`，`cpp:400`）/ `BLOCKED`（`mNextUnblockedEvent` 挡着，`cpp:403`）。枚举 `DropReason`（`h:907`）。真丢时 `dropInboundEventLocked`（`cpp:576`）并用 `InputState` 合成 cancel。

### 3.2 主循环：`dispatchOnce` → `dispatchOnceInnerLocked`

`dispatchOnce`（`cpp:265`）外层（锁模型核心）：
```
scoped_lock mLock;                 // cpp:268
mDispatcherIsAlive.notify_all();   // cpp:269  heartbeat：告诉 monitor() 我还活着
if (!haveCommandsLocked())          // cpp:273
    dispatchOnceInnerLocked(&nextWakeupTime);   // cpp:274  跑一轮派发
if (runCommandsLockedInterruptible())            // cpp:279  跑政策命令
    nextWakeupTime = LONG_LONG_MIN;             // cpp:280  强制立刻醒
// 出锁后：
mLooper->pollOnce(timeoutMillis);   // cpp:287  按 nextWakeupTime 定时休眠
```
**设计要点**：所有状态访问在锁内；`pollOnce` 在锁外做阻塞等待，`timeoutMillis` 由「最近一次需要被处理的时刻」决定（键重复时间 `cpp:333`、ANR 超时 `cpp:1108`、app switch 到期 `cpp:313`），所以 Dispatcher 既能被事件 wake，也能按 timer 醒来处理超时——这是「5s ANR 不需额外线程」的关键。

`dispatchOnceInnerLocked`（`cpp:290`）一轮：
1. 非交互态复位键重复（`cpp:296-298`）；冻结态直接 return（`cpp:301`）。
2. 无 pending 就从 `mInboundQueue` 取一条（`cpp:344`），或合成键重复（`cpp:330`）。
3. `PASS_TO_USER` 的 `pokeUserActivityLocked`（`cpp:349`）。
4. 按 `type` 分发：`dispatchKeyLocked`（`cpp:406`）/ `dispatchMotionLocked`（`cpp:422`），配置/设备重置也在此列。
5. `done` 则释放 pending（`cpp:438`）并 `nextWakeupTime=LONG_LONG_MIN` 强制下轮立刻醒（`cpp:439`）。

### 3.3 目标查找：hit-test vs focus

- **指针类** `dispatchMotionLocked`（`cpp:905`）判 `isPointerEvent`（`cpp:922`）→ `findTouchedWindowTargetsLocked`（`cpp:1258`）。底层 `findTouchedWindowAtLocked`（`cpp:511`）在该屏窗口 Z 序里从上往下找**未被遮挡**（`isWindowObscuredAtPointLocked` `cpp:1378/1448` 判定 `cpp:1761`）且可接受触摸的窗口；`findTouchedWindowTargetsLocked` 再据此组装目标、处理 `TouchState`（多指、`portalWindows` 跨屏嵌入）。
- **非指针类**（键盘/轨迹球）→ `findFocusedWindowTargetsLocked`（`cpp:1195`）：直接取 `mFocusedWindowHandlesByDisplay`（`h:1055`）里该屏的焦点窗口。
- 两条路都追加全局 monitor：`addGlobalMonitoringTargetsLocked`（`cpp:1714`）把同屏 `mGlobalMonitorsByDisplay` 的目标（`h:979`）加入 `inputTargets`（monitor 只旁观不消费，回执也忽略）。
- 目标未就绪（窗口/App 还没准备好收）→ `handleTargetsNotReadyLocked`（`cpp:1044`）开 5s 计时（`cpp:1096` 到点 `onANRLocked` `cpp:1098`），返回 `INJECTION_PENDING`（`cpp:1104`），本轮先不派、等 App ready 或超时。

### 3.4 政策拦截闸门（LockedInterruptible 的两处落点）

- **入队前**：`interceptKey/MotionBeforeQueueing`（`cpp:2685/2760`）——同步、持锁内改 policyFlags（这类政策调用不阻塞）。
- **派发前**（按键专属）：`dispatchKeyLocked`（`cpp:790`）在 `interceptKeyResult==UNKNOWN` 且 `PASS_TO_USER` 时，`postCommandLocked(&doInterceptKeyBeforeDispatchingLockedInterruptible)`（`cpp:844`）然后**返回 false 等命令**（`cpp:854`）。命令真正执行时（`cpp:4163` `mPolicy->interceptKeyBeforeDispatching`）：先 `mLock.unlock()` → 调政策（政策可能卡 WMS/AMS 查焦点、可返回 `TRY_AGAIN_LATER`）→ `mLock.lock()`。政策判 SKIP 则 `dropReason=POLICY`（`cpp:858-860`）。**这就是 HOME/Back/音量等系统键在派发前被吃掉的机制**——排查「App 收不到某个键」的第一站。

### 3.5 发布周期：`dispatchEventLocked` → `startDispatchCycleLocked`

`dispatchEventLocked`（`cpp:1018`）遍历 `inputTargets`：每个 target 取回其 `Connection`（`getConnectionIndexLocked` `cpp:1030`，查 `mConnectionsByFd`），调 `prepareDispatchCycleLocked`（入 `outboundQueue`，`enqueueDispatchEntriesLocked` `cpp:1999`）。

`startDispatchCycleLocked`（`cpp:2160`）逐条发：
- 取 `outboundQueue` 头（`cpp:2174`）按事件类型 `publishKeyEvent`（`cpp:2185`）/ `publishMotionEvent`（`cpp:2230`）写 socket；motion 坐标在此按 `DispatchEntry` 的 scale/offset 重缩放（`cpp:2200-2227`）——**所以窗口收到的是本地坐标**。
- 成功：`outboundQueue` 出队（`cpp:2276`）、挪进 `waitQueue`（`cpp:2278`）等回执。
- `WOULD_BLOCK`（socket 满）：`waitQueue` 非空则置 `inputPublisherBlocked=true`（`cpp:2265`）暂停发，等 App 消费完 `waitQueue` 再恢复；`waitQueue` 空还满=异常 → `abortBrokenDispatchCycleLocked`（`cpp:2256/2270`）置 `STATUS_BROKEN` 并通知政策。
- 回执处理（`finishDispatchCycleLocked` `cpp:2283` 及 socket readable 分支）：App 发 finished，按 `seq` 对号 `findWaitQueueEntry`（`h:898`）出 `waitQueue`。

### 3.6 ANR 与背压（交叉引用）

- 计时：`handleTargetsNotReadyLocked`（`cpp:1044`），`mInputTargetWaitTimeoutTime`（`h:1144`）初值 `DEFAULT_INPUT_DISPATCHING_TIMEOUT=5000ms`（`cpp:74`，窗口可覆写 `getDispatchingTimeout` `cpp:1069`）。
- 到点 `onANRLocked`（`cpp:4070`）：记 `mLastANRState` dump 快照（`cpp:4087-4095`）→ `postCommandLocked(&doNotifyANR)`（`cpp:4097`）→ `mPolicy->notifyANR`（`cpp:4140`）。
- 落到 Java：`InputManagerService.notifyANR`（`L1839`）→ `mWindowManagerCallbacks.notifyANR`（`L1840`）→ WMS → 最终 ANR 对话框/杀进程。
- **`onANRLocked` 不直接杀 App**，它只上报；真正的 force-stop 由 AMS 侧决定，`newTimeout`（`mPolicy->notifyANR` 返回 `cpp:4140`）>0 时可延长等待（`resumeAfterTargetsNotReadyTimeoutLocked` `cpp:1121`）。
- 完整触发/裁决/「为什么 5s 从 eventTime 算」见 `doc/inputDispatcher_anr.md`。

### 3.7 注册与连接状态机

`registerInputChannel`（`cpp:3828`）/ `registerInputMonitor`（`cpp:3858`）：新窗口（App 的 `ViewRootImpl`）在 attach 时注册，Dispatcher 建 `Connection`（`status=STATUS_NORMAL`）并挂进 `mConnectionsByFd`。`unregisterInputChannel`（`cpp:3892`）置 `STATUS_ZOMBIE`。`pilferPointers`（`h:390`）供手势 monitor 抢占当前指针流（`mGestureMonitorsByDisplay` `h:986`）。

---

## 4. 方法 → 文件 → 行号 速查表

| 关注点 | 方法 | 文件:行 |
|---|---|---|
| 类/锁模型不变式 | `class InputDispatcher` + Locked 注释 | `InputDispatcher.h:411 / 394-410` |
| 数据模型 | `EventEntry/Key/Motion` | `h:478/525/559` |
| | `DispatchEntry` | `h:593` |
| | `CommandEntry` | `h:648` |
| | `Connection`（outbound/wait） | `h:860 / 885 / 889` |
| | `TouchState` / `InputState` | `h:1074 / 766` |
| 入口 | `notifyKey / notifyMotion` | `cpp:2647 / 2724` |
| | `injectInputEvent` | `cpp:2843` |
| | `enqueueInboundEventLocked` | `cpp:443` |
| 主循环 | `dispatchOnce` | `cpp:265` |
| | `dispatchOnceInnerLocked` | `cpp:290` |
| 派发 | `dispatchKeyLocked / dispatchMotionLocked` | `cpp:790 / 905` |
| | `dispatchEventLocked` | `cpp:1018` |
| 目标查找 | `findTouchedWindowTargetsLocked` | `cpp:1258` |
| | `findTouchedWindowAtLocked`（hit-test） | `cpp:511` |
| | `findFocusedWindowTargetsLocked` | `cpp:1195` |
| | `isWindowObscuredAtPointLocked` | `cpp:1761` |
| | `addGlobalMonitoringTargetsLocked` | `cpp:1714` |
| 政策闸门 | `interceptKeyBeforeQueueing`（调用点） | `cpp:2685` |
| | `interceptKeyBeforeDispatching`（命令内） | `cpp:4163` |
| 发布 | `startDispatchCycleLocked` | `cpp:2160` |
| | `publishKeyEvent / publishMotionEvent` | `cpp:2185 / 2230` |
| | `enqueueDispatchEntriesLocked` | `cpp:1999` |
| | `finishDispatchCycleLocked` | `cpp:2283` |
| ANR/背压 | `handleTargetsNotReadyLocked` | `cpp:1044` |
| | `onANRLocked` → `mPolicy->notifyANR` | `cpp:4070 / 4140` |
| | `DEFAULT_INPUT_DISPATCHING_TIMEOUT` | `cpp:74` |
| 注册 | `registerInputChannel / Monitor / unregister` | `cpp:3828 / 3858 / 3892` |

---

## 5. 调试验证

```bash
# 1) Dispatcher 状态：各窗口通道 + 焦点 + 触摸会话 + monitor + 最近事件
adb shell dumpsys input
# 关注：Focus / Window / 每个 InputChannel 的 outbound/wait 长度、"recent events"、Last ANR

# 2) 抓原生事件（对照 hit-test 是否命中预期窗口）
adb shell getevent -l          # 看 raw code

# 3) trace 一条触摸的派发轨迹
# 开 input dispatch 相关 ATRACE：
adb shell setprop debug.stagefright.?  (不相关,略)
adb shell atc /data/misc/trace/input.txt
adb shell setprop debug.stagefright... # 略
# 更直接：在真机 logcat 过滤
adb logcat | grep -E "InputDispatcher|InputReader|WindowManager.*ANR"

# 4) 复现「点了没反应」定位路径
#   a. dumpsys input 看该坐标落在哪个可触摸窗口（对应 findTouchedWindowAtLocked）
#   b. 若窗口在 TouchState.windows 里，说明命中了但被 obscured（isWindowObscuredAtPointLocked）
#   c. 若焦点窗口与触摸窗口不一致（键盘/轨迹球类），走 findFocusedWindowTargetsLocked

# 5) 复现「5s 后 ANR」
#   App 主线程卡住不消费 InputChannel → 观察 logcat "Application is not responding"（onANRLocked L4076）
#   dumpsys input 的 "Last ANR" 段 = mLastANRState（cpp:4087-4095）
```

logcat 锚点：`"Application is not responding"`（`cpp:4076`）、`"channel '%s' ~ Could not publish event because the pipe is full"`（`cpp:2251`）、`"channel '%s' ~ startDispatchCycle"`（`cpp:2168`，需 `DEBUG_DISPATCH_CYCLE`）。

---

## 6. 踩坑清单

1. **触摸命中 ≠ 焦点**：指针事件按 Z 序 hit-test（`findTouchedWindowAtLocked`），非指针按焦点（`findFocusedWindowTargetsLocked`）。一个窗口能收触摸但不持焦点（反之亦然）。「App 收不到按键」先确认它是不是焦点窗口。
2. **obscured 判定**：命中被上层遮挡窗口挡掉（`isWindowObscuredAtPointLocked` `cpp:1378`）时该窗口不成为 target，事件落到下一个可见可触摸窗口——排查「点上层弹不出下层」看这里。
3. **政策拦截在派发前**（`interceptKeyBeforeDispatching` `cpp:4163`）：HOME/Back/音量等被政策消费，App 侧 `dispatchKeyEvent` 根本收不到。别在 App 层找，回 Dispatcher 政策闸门。
4. **Dispatcher 绝不持 `mLock` 回调 policy**（`h:394-410` 不变式）：一切政策回调都走 Command 的 unlock→call→lock（`cpp:4105` 范式）。自定义 policy 若在这条路径上死锁，就是破坏了该不变式。
5. **`enqueueInboundEventLocked` 只在「空变非空」时 wake**（`cpp:444`）：高频触摸不会每帧唤醒 Looper。改派发节奏别在这里加 wake。
6. **App 收到的坐标是窗口本地坐标**：`startDispatchCycleLocked` 按 `DispatchEntry` 的 scale/offset 重缩放（`cpp:2200-2227`），物理坐标≠窗口坐标，跨屏/多 DPI 场景尤其易错。
7. **`waitQueue` 有背压语义**：socket 满（`WOULD_BLOCK`）置 `inputPublisherBlocked`（`cpp:2265`）暂停发；若 App 迟迟不消费 → 5s ANR。backpressure 与 ANR 是同一物理现象的两面。
8. **`onANRLocked` 不杀进程**：只 dump + `notifyANR` 上报（`cpp:4070/4140`），force-stop 由 AMS 决定，`notifyANR` 返回的 `newTimeout` 还能延长（`cpp:4140` → `resumeAfterTargetsNotReadyTimeoutLocked` `cpp:1121`）。别把「5s 超时」等同「立刻杀」。

---

## 7. 与既有文档分工

| 文档 | 侧重 |
|---|---|
| `InputDispatcher_核心分析_Android10.md`（本篇） | 单体内机制：数据模型 / 入队 / 目标查找 / 政策闸门 / 发布周期 / 锁模型 |
| `inputDispatcher_anr.md` | ANR 触发、上报、裁决、5s 计时细节（本篇 §3.6 交叉引用，不重复） |
| `输入事件处理流程_Android10.md` | 全链路概览（内核→…→View），本篇是其中 Dispatcher 段放大 |
| `EventHub_分析_Android10.md` / `InputReader.cpp_解析.md` / `InputClassifier_分析解读_Android10.md` | 上游 EventHub / Reader / 分类器 |
