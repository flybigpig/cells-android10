

-----






结合当前 workspace 是 Android 10 源码,我来定位 `InputDispatcher` 中 `dispatchOnce` 与 `processAnrsLocked` 的实现并分析。
Android 10 的 inputflinger 目录结构是单层(非 Q 之后的分层),头文件应该在同目录。先并行读取关键代码段。
继续定位 `processAnrsLocked` 及 ANR 相关辅助函数。
该分支树里没有名为 `processAnrsLocked` 的函数(那是 Android 12+ 重构后的命名),Android 10 对应的实现是 `onANRLocked` 链路。我读取 ANR 检测与超时处理完整逻辑。
补充读取 dispatchOnceInnerLocked 中 key/motion 分支与超时设定逻辑,以及 doNotifyANRLockedInterruptible 全文。
我已理清本仓库(Android 10 Q 分支)的完整逻辑。最后确认一下 `dispatchOnce` 的调用线程起点。
补一个确认:驱动 `dispatchOnce` 的线程入口在哪。
做最后一轮精确取证,锁定各函数行号与关键片段。
分析完成。先纠正一个关键命名:你当前的仓库是 **Android 10(Q)分支**,其中**并不存在 `processAnrsLocked`**——这个函数名是 Android 12+ 把 InputDispatcher 重构后引入的入口(新版本 `dispatchOnceInnerLocked` 每轮一开始就调 `processAnrsLocked` 扫描哪些连接等待超时)。Q 分支的等价逻辑是 **`handleTargetsNotReadyLocked` + `onANRLocked` + 命令队列回调**。下面基于本仓库实际代码展开。

## 一、线程骨架:`dispatchOnce` 从哪来、靠什么醒来

InputDispatcher 有自己专属的 `InputDispatcherThread`,死循环反复调用 `dispatchOnce`:

```5239:5242:frameworks/native/services/inputflinger/InputDispatcher.cpp
bool InputDispatcherThread::threadLoop() {
    mDispatcher->dispatchOnce();
    return true;
}
```

`dispatchOnce` 的结构决定了整个事件循环的节奏:

```265:288:frameworks/native/services/inputflinger/InputDispatcher.cpp
void InputDispatcher::dispatchOnce() {
    nsecs_t nextWakeupTime = LONG_LONG_MAX;
    { // acquire lock
        std::scoped_lock _l(mLock);
        mDispatcherIsAlive.notify_all();
        // Run a dispatch loop if there are no pending commands.
        if (!haveCommandsLocked()) {
            dispatchOnceInnerLocked(&nextWakeupTime);
        }
        // Run all pending commands if there are any.
        // If any commands were run then force the next poll to wake up immediately.
        if (runCommandsLockedInterruptible()) {
            nextWakeupTime = LONG_LONG_MIN;
        }
    } // release lock
    nsecs_t currentTime = now();
    int timeoutMillis = toMillisecondTimeoutDelay(currentTime, nextWakeupTime);
    mLooper->pollOnce(timeoutMillis);
}
```

要点:每轮先持有 `mLock` 通知 watchdog(`mDispatcherIsAlive.notify_all()`,`monitor()` 会用它做死锁探活);有 pending 命令就先不跑派发(命令优先),命令执行完强制 `LONG_LONG_MIN` 立即再转一圈。末尾 `pollOnce` 是唯一的睡眠点,唤醒来源只有三种——已注册的 input channel fd 有数据到达(通过 `mLooper->addFd` + `handleReceiveCallback` 注册)、定时器到期(即 `nextWakeupTime`,ANR 截止时间正是靠它传导)、以及其它线程(InputReader 等)调 `mLooper->wake()`。

## 二、`dispatchOnceInnerLocked`:一轮派发做什么

```290:291:frameworks/native/services/inputflinger/InputDispatcher.cpp
void InputDispatcher::dispatchOnceInnerLocked(nsecs_t* nextWakeupTime) {
    nsecs_t currentTime = now();
```

它按序处理:dispatch 冻结检查(`mDispatchFrozen` 直接短路)→ app switch 到期提前唤醒→ 若无在派发事件且 `mInboundQueue` 有货则出队一条新事件并合成 key repeat → `pokeUserActivity` → 各种丢弃判定(POLICY 标志不允许传给用户、dispatch 被禁用、被"app switch 抢占"标记 `mNextUnblockedEvent` 之前的旧事件、超过 `STALE_EVENT_TIMEOUT`(10s)的陈旧事件)→ 按 `switch(event->type)` 分派到 `dispatchKeyLocked` / `dispatchMotionLocked` / `dispatchFocusLocked` 等 → 无事可做则派发结束并置唤醒。每条事件出队后到真正落窗口之间,还要经过 `resetANRTimeoutsLocked` 把上一轮残留的 ANR 等待现场复位(见第四节)。

## 三、ANR 的检测链(Q 版):目标窗口"不 ready"是唯一入口

`dispatchKeyLocked`/`dispatchMotionLocked` 先做目标查找:
- 键盘类事件走 `findFocusedWindowTargetsLocked`,拿不到焦点窗口/焦点窗口不可用;
- 触摸走 `findTouchedWindowTargetsLocked`。

两者内部逐窗口调用 `checkWindowReadyForMoreInputLocked`,检查该窗口对应 connection 是否就绪(上一个事件是否已消费、connection 是否 alive、是否在等 timeout 判定等),不满足则拼出原因字符串(形如 "Waiting because the touched window has not finished processing the input events that were previously delivered to it."),然后 goto Unresponsive 汇入同一个处理函数:

```1044:1048:frameworks/native/services/inputflinger/InputDispatcher.cpp
int32_t InputDispatcher::handleTargetsNotReadyLocked(nsecs_t currentTime,
        const EventEntry* entry,
        const sp<InputApplicationHandle>& applicationHandle,
        const sp<InputWindowHandle>& windowHandle,
        nsecs_t* nextWakeupTime, const char* reason) {
```

这里区分两种等待原因(也就是你说的 ANR 语义核心):
- **SYSTEM_NOT_READY**:window 和 application 都是空(例如系统还没亮出任何窗口树)——**永远不会 ANR**,无限期等待,直到窗口树建立后事件重新派发;
- **APPLICATION_NOT_READY**:窗口或应用存在但没准备好——这才是真 ANR,记录 `mInputTargetWaitStartTime` 与 `mInputTargetWaitTimeoutTime = now + window 的 dispatchingTimeout`(窗口默认即 WMS 下发的 input dispatching timeout,常为 5s),并把 `nextWakeupTime` 指向该截止时间,让主线程在 deadline 精确醒来。

两个典型调用现场:应用启动中还没有窗口但存在 focused application(1210-1214,等待应用起窗口,同样不会 ANR,只等不加超时)、以及焦点窗口不 ready(1234-1235)。触摸目标分支同理在 1538-1540。**因此 Q 版"每轮去查谁超时"这件事并不存在,而是"谁在等待,谁就把自己的 deadline 塞进下一轮的 nextWakeupTime",这是 Q 与新版 `processAnrsLocked` 最本质的结构差异。**

## 四、超时爆发:`onANRLocked` 与策略回调

当分发函数发现等待原因非 NONE 且 `now() >= mInputTargetWaitTimeoutTime`,便调用:

```4068:4101:frameworks/native/services/inputflinger/InputDispatcher.cpp
void InputDispatcher::onANRLocked(
        nsecs_t currentTime, const sp<InputApplicationHandle>& applicationHandle,
        const sp<InputWindowHandle>& windowHandle,
        nsecs_t eventTime, nsecs_t waitStartTime, const char* reason) {
    float dispatchLatency = (currentTime - eventTime) * 0.000001f;
    float waitDuration = (currentTime - waitStartTime) * 0.000001f;
    ALOGI("Application is not responding: %s.  "
            "It has been %0.1fms since event, %0.1fms since wait started.  Reason: %s",
            getApplicationWindowLabel(applicationHandle, windowHandle).c_str(),
            dispatchLatency, waitDuration, reason);
    // Capture a record of the InputDispatcher state at the time of the ANR.
    ...
    mLastANRState.clear();
    mLastANRState += INDENT "ANR:\n";
    ...
    dumpDispatchStateLocked(mLastANRState);

    CommandEntry* commandEntry = postCommandLocked(
            & InputDispatcher::doNotifyANRLockedInterruptible);
    commandEntry->inputApplicationHandle = applicationHandle;
    commandEntry->inputChannel = windowHandle != nullptr ?
            getInputChannelLocked(windowHandle->getToken()) : nullptr;
    commandEntry->reason = reason;
}
```

这里做了三件事:打 logcat 记录"距事件 X ms、距开始等待 Y ms、原因 Z";**把整个 dispatcher 当前状态快照进 `mLastANRState`**(之后任何时候 `dumpsys input` 都能看到 "at time of last ANR" 的诊断现场,这是排查卡死的黄金数据);随后只是入队一个命令就返回,事件仍保持 PENDING 不丢。真正的外部动作发生在下一轮 `runCommandsLockedInterruptible()` 里——它先解锁再执行,避免持锁 binder 调用:

```4134:4147:frameworks/native/services/inputflinger/InputDispatcher.cpp
void InputDispatcher::doNotifyANRLockedInterruptible(
        CommandEntry* commandEntry) {
    mLock.unlock();

    nsecs_t newTimeout = mPolicy->notifyANR(
            commandEntry->inputApplicationHandle,
            commandEntry->inputChannel ? commandEntry->inputChannel->getToken() : nullptr,
            commandEntry->reason);

    mLock.lock();

    resumeAfterTargetsNotReadyTimeoutLocked(newTimeout,
            commandEntry->inputChannel);
}
```

`notifyANR` 的调用链是 `NativeInputManager → InputManagerService.nativeNotifyANR → AMS`,由 **AMS 决定**是弹 ANR 对话框、继续等还是杀进程;InputDispatcher 自己只负责"探测并上报"。AMS 的裁决以返回值 `newTimeout` 传回:大于 0 表示续期(如 ANR 对话框等待期间用户决定"Wait")。

`resumeAfterTargetsNotReadyTimeoutLocked`(1121 行起)据此收尾:续期则把 deadline 顺延到 `now + newTimeout` 继续等;放弃(超时到期仍无答复)则置 `mInputTargetWaitTimeoutExpired`,按 token 移除该窗口,并向该 connection 合成 CANCEL 事件(原因 "application not responding"),让卡住的派发周期以注入超时(`INPUT_EVENT_INJECTION_TIMED_OUT`)收场,后续新事件重新寻找目标窗口,从而绕开已死的窗口。

## 五、配套机制:复位与"用户点走"

`resetANRTimeoutsLocked`(1159 行起)把 `mInputTargetWaitCause/StartTime/TimeoutTime/Expired/ApplicationHandle` 整组复位为 NONE。窗口树变化、焦点变化、事件派发完成等路径都会触发它。另有一个不依赖超时的逃生通道——**app switch**:当处于 APPLICATION_NOT_READY 等待时,如果用户触摸了**另一个**应用的窗口(事件源 app 不同),dispatcher 会把该触摸事件记为 `mNextUnblockedEvent`,丢弃它之前排队的全部旧事件,把输入流直接切到新应用(例如 ANR 弹窗期间用户点别的界面,无需等 5s)。

## 六、排查建议

抓 input ANR 时优先级依次看:`logcat -s InputDispatcher` 里的 "Application is not responding" 行(含原因、等待时长);随后立即 `dumpsys input`,先看 "Input Dispatcher State at time of last ANR" 快照段,再核对当前段的 `WaitQueue`、`mInputTargetWaitCause` 与每连接 `OutboundQueue/WaitQueue` 是否堆积;若等待原因是 focused/touched window 未消费,则该 app 主线程(UI 线程事件循环)大概率被卡住,配合 `dumpsys activity` / ANR trace 看主线程栈即可闭环。

一句话总结:Q 版把 ANR 做成"**事件在派发时发现目标不 ready → 向主循环登记 deadline → 主循环到点自动醒来 → onANRLocked 上报策略层 → 策略返回续期或放弃**"的拉模式;`processAnrsLocked` 只是新版本把同一套语义改成每轮开头集中扫描的推模式,排查思路完全通用。












----




继续在 Android 10 分支把"事件分发完整链路 + ANR"讲透。先定位分发链路上其余关键函数。
读取分发目标判定、投递与回调回收的关键代码段。
再读投递循环后续与回调回收部分。
补充确认两个关键超时常量的实际取值。
奇怪,刚才明明在文件中见过该常量,再对文件本身复查定义位置。
STREAM_AHEAD 常量是 0.5s。读取文件头部常量区与 `handleTargetsNotReadyLocked` 完整实现,把 ANR 计时细节坐实。
补读 `dispatchKeyLocked` 主体与 `handleTargetsNotReadyLocked` 的计时设置段,拿到 ANR 判定的精确现场。
证据链已完整。下面给出 InputDispatcher 事件分发全链路 + ANR 机制的完整分析(基于本仓库 Android 10 分支 `InputDispatcher.cpp`,所有引用均为真实行号)。

## 一、一次输入事件的生命周期总览

```text
InputReader(设备线程) → notifyKey/notifyMotion(入队)
  → InputDispatcher 主线程 dispatchOnce 循环
      → 出队 mInboundQueue → dispatchKeyLocked/dispatchMotionLocked
          → findFocused/TouchedWindowTargetsLocked(目标决策)
              → [不 ready] handleTargetsNotReadyLocked(登记等待+ANR 截止时间)
          → dispatchEventLocked → addWindowTargetLocked
          → enqueueDispatchEntryLocked(入 outboundQueue)
          → startDispatchCycleLocked(publish 写共享内存+socket, 移入 waitQueue)
  → 应用侧 InputChannel 消费 → 写回 finished 信号
  → handleReceiveCallback(fd 可读) → finishDispatchCycleLocked(清 waitQueue)
  → 事件 refCount 归零 → 从 mPendingEvent 释放 → 下一轮循环
```

整条链被一把 `mLock` + 一条条件变量(`mInboundQueueNotEmpty`)+ 一组 fd/epoll(`pollOnce`)串起。

## 二、分发的入口与节奏

主循环 `dispatchOnce` 每轮持有锁跑 `dispatchOnceInnerLocked`,没有 pending 命令时先执行一轮派发,再 `pollOnce(timeoutMillis)` 睡眠;唤醒源是 input channel fd 有数据(应用回了 finished 信号)、`nextWakeupTime` 定时到点(ANR 截止时间就是走这条)、或 InputReader 入队后的显式 wake。`nextWakeupTime` 是整个循环中"下一件必须在何时处理的事"的唯一时钟源。

## 三、目标查找:谁来决定事件去哪

以按键为例,`dispatchKeyLocked`(790 行起)的流程是:先做 key repeat 预处理(793-827,硬件自动连发 vs 软件合成、LONG_PRESS 标志),处理策略层上一轮的 `TRY_AGAIN_LATER` 与 `interceptKeyBeforeDispatching`(830-862,如 HOME/POWER 被系统消费就置 SKIP),确认不丢弃后进入真正的目标阶段:

```872:889:frameworks/native/services/inputflinger/InputDispatcher.cpp
    // Identify targets.
    std::vector<InputTarget> inputTargets;
    int32_t injectionResult = findFocusedWindowTargetsLocked(currentTime,
            entry, inputTargets, nextWakeupTime);
    if (injectionResult == INPUT_EVENT_INJECTION_PENDING) {
        return false;
    }
    ...
    // Dispatch the key.
    dispatchEventLocked(currentTime, entry, inputTargets);
```

key 走 `findFocusedWindowTargetsLocked`(焦点窗口);触摸走 `findTouchedWindowTargetsLocked`(根据 DOWN 事件命中的触摸目标,后续 MOVE/UP 跟随同一目标)。触摸有一个与 key 完全不同的投递哲学,注释讲得很清楚(1852-1866):触摸事件按"用户当时看到什么就摸什么"投递,不做严格的队列串行,以便跟手;而 key 必须等该窗口之前所有事件完成(1833-1850),因为先前事件可能转移焦点,按键要送到新焦点。

## 四、ready 检查:ANR 的第一现场

目标窗口找到后,逐个经过 `checkWindowReadyForMoreInputLocked`(1800 行)的体检,任一不过就拿到一条 reason 字符串,随后 goto 汇聚到 `handleTargetsNotReadyLocked`。这份体检表(每条 reason 都会原样出现在 logcat 和 dumpsys 里):

窗口 paused(1804)、input channel 未注册(1809,窗口正在被移除)、connection 状态非 NORMAL(1819,如已被判定 broken)、**输入管道满**(`inputPublisherBlocked`,1826——这是最常见的 ANR 现场:应用不消费导致共享内存环写满)、key 事件遇到残留的 outbound/waitQueue(1845)、**非 key 事件发现 waitQueue 头部事件已经 0.5s 没被 finish**(1867-1869,靠 `STREAM_AHEAD_EVENT_TIMEOUT = 500ms`,88-89 行)。最后这条是关键设计:触摸事件平时允许"超前流送"(app 边画边补),但如果最早的未完成事件滞留超过 0.5s,就停止继续投递——目的是**让 ANR 能被可靠、及时地测出来**,而不是让队列无限堆。

## 五、等待登记与 ANR 计时

`handleTargetsNotReadyLocked`(1044 行)只做一件事:给"等待"建档并设闹钟。它分两种语义:

```1049:1089:frameworks/native/services/inputflinger/InputDispatcher.cpp
    if (applicationHandle == nullptr && windowHandle == nullptr) {
        ... SYSTEM_NOT_READY: waitStart=now, waitTimeout=LONG_LONG_MAX ...
    } else {
        ...
        nsecs_t timeout;
        if (windowHandle != nullptr) {
            timeout = windowHandle->getDispatchingTimeout(DEFAULT_INPUT_DISPATCHING_TIMEOUT);
        } else if (applicationHandle != nullptr) {
            timeout = applicationHandle->getDispatchingTimeout(
                    DEFAULT_INPUT_DISPATCHING_TIMEOUT);
        } else {
            timeout = DEFAULT_INPUT_DISPATCHING_TIMEOUT;
        }
        mInputTargetWaitCause = INPUT_TARGET_WAIT_CAUSE_APPLICATION_NOT_READY;
        mInputTargetWaitStartTime = currentTime;
        mInputTargetWaitTimeoutTime = currentTime + timeout;
```

窗口和应用都不存在(如开机早期窗口树未建)→ `SYSTEM_NOT_READY`,无限期等、永不 ANR;存在应用/窗口但不 ready → `APPLICATION_NOT_READY`,取该窗口(可被 WMS 定制)的 dispatchingTimeout,缺省 `DEFAULT_INPUT_DISPATCHING_TIMEOUT = 5s`(74 行),并把 deadline 同步给主循环的 `nextWakeupTime`(1107-1108),于是主线程在 deadline 处精确醒来复查。

## 六、超时爆发与策略裁决

到点后窗口仍不 ready,同一入口处触发:

```1096:1111:frameworks/native/services/inputflinger/InputDispatcher.cpp
    if (currentTime >= mInputTargetWaitTimeoutTime) {
        // input target wait timeout expired ,input anr
        onANRLocked(currentTime, applicationHandle, windowHandle,
                entry->eventTime, mInputTargetWaitStartTime, reason);
        if (mInputTargetWaitCause == INPUT_TARGET_WAIT_CAUSE_APPLICATION_NOT_READY) {
            // Force poll loop to wake up immediately on next iteration once we get the
            // ANR response back from the policy.
            *nextWakeupTime = LONG_LONG_MIN;
            return INPUT_EVENT_INJECTION_PENDING;
        }
    }
    ...
    if (mInputTargetWaitTimeoutExpired) {
        return INPUT_EVENT_INJECTION_TIMED_OUT;
    }
```

`onANRLocked`(上一轮已展示,4068 行)的职责是"上报+留档":打 `Application is not responding` logcat、把全量 dispatcher 状态快照进 `mLastANRState`(之后 dumpsys input 首屏可见)、然后只 post 一个命令 `doNotifyANRLockedInterruptible` 就返回。命令在下一轮 `runCommandsLockedInterruptible` 里**解锁后**执行(4134-4147):`mPolicy->notifyANR(app, channelToken, reason)` 经 NativeInputManager/InputManagerService 直达 AMS,由 AMS 决定弹框、杀进程还是等待,并把结果以纳秒 `newTimeout` 返回。

回到锁内,`resumeAfterTargetsNotReadyTimeoutLocked`(1121 行)收尾:

```1121:1148:frameworks/native/services/inputflinger/InputDispatcher.cpp
void InputDispatcher::resumeAfterTargetsNotReadyTimeoutLocked(nsecs_t newTimeout,
        const sp<InputChannel>& inputChannel) {
    if (newTimeout > 0) {
        // Extend the timeout.
        mInputTargetWaitTimeoutTime = now() + newTimeout;
    } else {
        // Give up.
        mInputTargetWaitTimeoutExpired = true;
        ...
        if (token != nullptr) {
            removeWindowByTokenLocked(token);
        }
        if (connection->status == Connection::STATUS_NORMAL) {
            CancelationOptions options(CancelationOptions::CANCEL_ALL_EVENTS,
                    "application not responding");
            synthesizeCancelationEventsForConnectionLocked(connection, options);
        }
    }
}
```

续期(比如 ANR 对话框用户点 Wait)就把 deadline 顺延;放弃则三连:置 `mInputTargetWaitTimeoutExpired` → 按 token 移除该窗口(触摸状态也一并清)→ 给该 connection 合成 `"application not responding"` 的 CANCEL 事件。此后该事件走 `1092-1094` 以 `INPUT_EVENT_INJECTION_TIMED_OUT` 收场,后续新事件因窗口已移除而重新寻路,系统得以绕过卡死的 app。

## 七、真正"投递"的动作与回环

目标就绪后:`dispatchEventLocked` 对每个 target 调 `enqueueDispatchEntryLocked`(2030 行),构造 `DispatchEntry`,做 `inputState.trackKey/trackMotion` 一致性记账(如"不存在的 DOWN 会被跳过"、HOVER 补齐、动作改写 OUTSIDE/HOVER_EXIT 等,2054-2120),然后 `connection->outboundQueue.enqueueAtTail`。`startDispatchCycleLocked`(2160 行)在每轮把 outbound 队列尽量 flush:publish 前打上 `deliveryTime = currentTime`(2175,ANR 判龄的起点),key/motion 分别经 `inputPublisher.publishKeyEvent/publishMotionEvent` 写入共享内存槽并触发 socket 通知;若环满返回 `WOULD_BLOCK`,置 `inputPublisherBlocked = true` 等应用消化(2248-2266);成功后该 DispatchEntry 从 outbound 移到 `waitQueue`(2276-2279)。waitQueue 就是 dumpsys input 里能看到的应用"已收未答"清单。

应用处理完调用 `InputConsumer.consume`/`sendFinishedSignal` 写回(实际是往 socket 写 finished 字节)。dispatcher 侧该 connection 的 fd 变可读,触发注册过的 `handleReceiveCallback`(2340 行):

```2365:2377:frameworks/native/services/inputflinger/InputDispatcher.cpp
            for (;;) {
                uint32_t seq;
                bool handled;
                status = connection->inputPublisher.receiveFinishedSignal(&seq, &handled);
                if (status) {
                    break;
                }
                d->finishDispatchCycleLocked(currentTime, connection, seq, handled);
                gotOne = true;
            }
            if (gotOne) {
                d->runCommandsLockedInterruptible();
```

`finishDispatchCycleLocked`(2283 行)按 seq 从 waitQueue 摘掉对应条目、清 `inputPublisherBlocked`,走到 `onDispatchCycleFinishedLocked` 决定是否需要唤醒(比如事件处理完发现 waitQueue 清空,之前被阻塞的 key 可以继续投递,或该事件所属流的后续事件可以解锁)。至此一轮完整的分发周期闭合。

## 八、常量表(理解 ANR 阈值的钥匙)

```72:92:frameworks/native/services/inputflinger/InputDispatcher.cpp
// Default input dispatching timeout ...
constexpr nsecs_t DEFAULT_INPUT_DISPATCHING_TIMEOUT = 5000 * 1000000LL; // 5 sec
// Amount of time to allow for all pending events to be processed when an app switch ...
constexpr nsecs_t APP_SWITCH_TIMEOUT = 500 * 1000000LL; // 0.5sec
// Amount of time to allow for an event to be dispatched ...
constexpr nsecs_t STALE_EVENT_TIMEOUT = 10000 * 1000000LL; // 10sec
// Amount of time to allow touch events to be streamed out ...
constexpr nsecs_t STREAM_AHEAD_EVENT_TIMEOUT = 500 * 1000000LL; // 0.5sec
// Log a warning when an event takes longer than this to process, even if an ANR does not occur.
constexpr nsecs_t SLOW_EVENT_PROCESSING_WARNING_TIMEOUT = 2000 * 1000000LL; // 2sec
```

## 九、Q 版 ANR 架构特点与调试入口

Q 分支没有独立的 "ANR 扫描" 函数(那是新版 `processAnrsLocked` 的做法),ANR 是**惰性触发**:事件在某轮循环撞上不 ready 窗口 → 登记 deadline → 主循环到点自动醒来复查 → 不 ready 就上报。所有等待状态收敛在三个成员上:`mInputTargetWaitCause`(`NONE/SYSTEM_NOT_READY/APPLICATION_NOT_READY`)、`mInputTargetWaitStartTime`、`mInputTargetWaitTimeoutTime`,`resetANRTimeoutsLocked`(1159 行)在事件完成/窗口树变化等时机把它们清回 NONE。配套的逃生通道是 app switch:用户在 ANR 等待中点别的应用时,新事件会触发丢弃 `mNextUnblockedEvent` 之前的积压事件,输入流立刻切走,不用等超时。

实际排查顺序建议:logcat 里 `InputDispatcher` 的 "Application is not responding ... Reason:" 行先定性;随后 `dumpsys input` 看首屏 "Input Dispatcher State at time of last ANR" 快照(ANR 瞬间全量状态)与当前 "WaitQueue" 堆积、各 connection 的 `inputPublisherBlocked`;结合 `dumpsys window windows` 里该窗口的 dispatchingTimeout 是否被改过、以及 `dumpsys activity` 主线程是否卡在消息循环,即可完成从"事件堵在哪"到"app 为什么没消费"的闭环定位。

---


结合前几轮的上下文,你问的应该是 **`dispatchOnce` 里"超时"到底是怎么实现和调度的**——即 `nextWakeupTime` 机制下有哪些软定时器、到点如何处理。我先把本文件所有超时写点 grep 出来做完整枚举。
用 grep 拿到了 `dispatchOnce` 体系内所有"设置下次唤醒时刻"的写点,共 8 处(递归进 `findFocused/TouchedWindowTargetsLocked` 后还会经由 `handleTargetsNotReadyLocked` 再补两处)。结合此前通读,可把这个话题完整讲透。

## 一、本质:dispatchOnce 里只有"一个闹钟",没有一堆定时器

```265:288:frameworks/native/services/inputflinger/InputDispatcher.cpp
void InputDispatcher::dispatchOnce() {
    nsecs_t nextWakeupTime = LONG_LONG_MAX;
    { // acquire lock
        std::scoped_lock _l(mLock);
        mDispatcherIsAlive.notify_all();
        // Run a dispatch loop if there are no pending commands.
        if (!haveCommandsLocked()) {
            dispatchOnceInnerLocked(&nextWakeupTime);
        }
        // Run all pending commands if there are any.
        // If any commands were run then force the next poll to wake up immediately.
        if (runCommandsLockedInterruptible()) {
            nextWakeupTime = LONG_LONG_MIN;
        }
    } // release lock
    nsecs_t currentTime = now();
    int timeoutMillis = toMillisecondTimeoutDelay(currentTime, nextWakeupTime);
    mLooper->pollOnce(timeoutMillis);
}
```

`nextWakeupTime` 是**本文件唯一的"软定时器"时钟源**。每轮以 `LONG_LONG_MAX`(语义:没有要醒的事)初始化,派发路径中任何关心"某时刻必须再被叫醒"的逻辑都把自己的 deadline 与它取小写回;命令被执行过则直接赋 `LONG_LONG_MIN` 强迫 0 毫秒超时立即转下一轮。最后由 `toMillisecondTimeoutDelay` 换算交给 `pollOnce` 阻塞。换算存在三个特例:`LONG_LONG_MAX` → timeout=-1(**无限阻塞**),`diff <= 0` → 0(立即返回),正常 → 向上取整毫秒。所以 pollOnce 这一睡,醒来只有三种原因:应用侧 channel fd 有 finished 信号、闹钟到点、其它线程 `wake()`。

## 二、一轮派发内会把哪些"时刻"塞进闹钟(枚举)

对照上面 grep 出的 8 个写点:

| 写点 | 塞入什么 | 语义 |
|---|---|---|
| 279-280 | `LONG_LONG_MIN` | 有命令刚执行完,不睡,立即再转一圈处理后续命令 |
| 312-313 | `mAppSwitchDueTime` | **App switch 缓冲到期**:等待中的事件队列允许被"用户点走"抢占,到点丢弃旧流 |
| 332-333 | `mKeyRepeatState.nextRepeatTime` | **长按 repeat 闹钟**:到点若仍按住则 synthesize 一条 repeat(上轮 798-807 登记的就是它) |
| 438-439 | `LONG_LONG_MIN` | 事件被丢弃/处理完释放 pending 后,立即醒来取下一个事件 |
| 832-833 | `entry->interceptKeyWakeupTime` | **policy 拦截重试**:policy 让 dispatcher "稍后再来问一次"(TRY_AGAIN_LATER),到点重新进 dispatchKey 重试 |
| 1102-1103 | `LONG_LONG_MIN` | ANR 已上报,立刻醒来等策略层回话(命令队列) |
| 1107-1108 | `mInputTargetWaitTimeoutTime` | **ANR deadline**(默认 5s 后):窗口不 ready 时注册的唤醒点 |

其中 1107 正是 ANR 的核心——`handleTargetsNotReadyLocked` 把 `now + dispatchingTimeout` 塞进闹钟,主线程睡到那一刻复查目标是否恢复。还有一类"分发中顺带处理"的时限检查不依赖闹钟、每轮即时判断:事件在入队处被判 stale 超过 10s 丢弃、motion 超前流送超过 0.5s 就停发(`STREAM_AHEAD_EVENT_TIMEOUT`)、事件处理超过 2s 只打慢处理警告不阻断。

## 三、"超时到点"如何被执行:惰性求值,没有回调线程

整个文件没有"定时器到期 → 自动回调"的机制。超时触发一律是**到点后,下一次重新进入同一段代码时用 `now() >= deadline` 就地判断**:

keyRepeat 在 `dispatchOnceInnerLocked` 取事件前(327-336)判断到点即 synthesize;intercept 重试在 `dispatchKeyLocked` 开头(830-839)判断;ANR 在 `handleTargetsNotReadyLocked` 开头:

```1096:1108:frameworks/native/services/inputflinger/InputDispatcher.cpp
    if (currentTime >= mInputTargetWaitTimeoutTime) {
        // input target wait timeout expired ,input anr
        onANRLocked(currentTime, applicationHandle, windowHandle,
                entry->eventTime, mInputTargetWaitStartTime, reason);
        if (mInputTargetWaitCause == INPUT_TARGET_WAIT_CAUSE_APPLICATION_NOT_READY) {
            // Force poll loop to wake up immediately on next iteration once we get the
            // ANR response back from the policy.
            *nextWakeupTime = LONG_LONG_MIN;
            return INPUT_EVENT_INJECTION_PENDING;
        }
    }
    ...
    if (mInputTargetWaitTimeoutTime < *nextWakeupTime) {
        *nextWakeupTime = mInputTargetWaitTimeoutTime;
    }
```

到点后不会重复爆发:上报完策略立刻把自己唤醒点改成 `LONG_LONG_MIN` 去等命令结果,命令回来 `resumeAfterTargetsNotReadyTimeoutLocked` 决定续期或放弃(移除窗口 + CANCEL),再把新 deadline(或不再等)写回闹钟。**每个超时只爆发一次,处理完必须自己重新注册下一个闹钟**,这就是"睡到点 → 检查 → 处理 → 重设或不再设"的闭环。

## 四、超时只是"事件不再继续推进",不是事件自动消失

必须澄清:ANR/拦截超时到点,**事件不会因为超时被自动删除**。被卡住的事件一直占着 `mPendingEvent`,期间新事件排队在 `mInboundQueue`;只有两种情况事件才退出派发:策略裁决放弃后(窗口被移除、CANCEL 已合成),或事件最终以 `INPUT_EVENT_INJECTION_TIMED_OUT` 结束;另一种是 dispatch 被冻结时(`mDispatchFrozen`)事件直接丢弃。真正清理动作集中在 438 行附近:事件走完自己的生命周期 → `releasePendingEventLocked()` → `LONG_LONG_MIN` 立刻取下一个。而等待注入结果的调用方(如 `adb shell input`,或 `InputManager.injectInputEvent` 带等待语义时)则阻塞在自己的条件变量上,由 `setInjectionResult` 唤醒;若 dispatcher 一直不结束该事件,注入方会按等待超时自行返回(这就是注入侧看到的 "injection timed out")。

## 五、调试视角

超时没有集中日志,但每类到点都有特征可查:`dumpsys input` 里当前 `mPendingEvent`、各 connection 的 WaitQueue 长度与滞留时间、`InputDispatcher` logcat 的 "Application is not responding"(ANR 到点)、"Channel is unrecoverably broken"(连接超时判死)、"Stale event"(10s 陈旧丢弃);而 `--input-dispatcher` 的 poll 阻塞时长则间接反映闹钟远近——若 pollOnce 长期被设成接近 0(反复空转),通常是命令风暴或 `LONG_LONG_MIN` 路径过多。

一句话总结:`dispatchOnce` 的"分发超时"是**单闹钟(share 一个 `nextWakeupTime`)+ 惰性判定(到点后按 `now() >= deadline` 就地处理)+ 事件生命周期推进(超时不删事件,只停止推进)**三者组合的结果;ANR 只是其中一个把 deadline 注册进这个闹钟的参与者,keyRepeat、policy 拦截重试、app switch 缓冲走的都是同一条路。

----

用户给出的是 **Android 12/13+ 新版** ANR 调用链(SF 侧 `processAnrsLocked`、`mAnrTracker`、`AnrController`、`notifyWindowUnresponsive` 等都是新架构命名),要求我把后半段"参照写出本仓库的 AOSP 10(Q)版本"。我先并行取证 Q 版各层真实实现,再输出完整对照。
本仓库确实是 Q 的扁平布局且 `IMS.notifyANR` 已是 Q 两参版。继续取 WMS/AMS/AppErrors 层的真实实现。
继续定位 `NativeInputManager.cpp` 的真实路径,并读 Q 版 AMS.inputDispatchingTimedOut 与 AppErrors 调用。
逐个符号在 `am/` 目录全局定位。
找到 Q 的真实结构:ANR 处理在 AMS(18612)与 ProcessRecord.appNotResponding(1407,本仓库无 AppErrors)。读取这两段主体,并全仓定位 `NativeInputManager`。
补读 `ProcessRecord.appNotResponding` 内部结构,并全仓搜索 `NativeInputManager`。
再取两个关键点行号:Q 版"无焦点窗口"的丢弃文案,与 AMS 按 pid 的 3 参 `inputDispatchingTimedOut` 重载。
取证完毕。你贴的流程是 **Android 12+/13 的新版 input-ANR 架构**,其中 `processAnrsLocked`/`mAnrTracker`/`notifyNoFocusedWindowAnr`/`notifyWindowUnresponsive`/`AnrController`/`cancelEventsForAnrLocked` 在本仓库(Q)里**全部不存在**。下面把该流程逐行翻译成本仓库 Android 10 的真实代码链,并标注差异。

## 一、SF 侧(触发方式完全不同)

新版是"每轮 `processAnrsLocked` 集中扫 `mAnrTracker`,谁超时处理谁";Q 是**惰性单事件等待 + 命令队列上报**,前几轮已证:`handleTargetsNotReadyLocked`(1044)只登记 `mInputTargetWaitTimeoutTime`,主循环到点复查,超时才进 `onANRLocked`(4068)→ post 命令 `doNotifyANRLockedInterruptible`(4134):

```4134:4147:frameworks/native/services/inputflinger/InputDispatcher.cpp
void InputDispatcher::doNotifyANRLockedInterruptible(
        CommandEntry* commandEntry) {
    mLock.unlock();
    nsecs_t newTimeout = mPolicy->notifyANR(
            commandEntry->inputApplicationHandle,
            commandEntry->inputChannel ? commandEntry->inputChannel->getToken() : nullptr,
            commandEntry->reason);
    mLock.lock();
    resumeAfterTargetsNotReadyTimeoutLocked(newTimeout,
            commandEntry->inputChannel);
}
```

对应新版的两个点:reason 文案 Q 里来自 `checkWindowReadyForMoreInputLocked` 的既有字符串(没有 "<channel> is not responding. Waited Nms" 的生成器);**没有** `cancelEventsForAnrLocked`/无聚焦窗口专用分支——Q 里"没有焦点"是直接丢事件,不是 ANR:

```1218:1221:frameworks/native/services/inputflinger/InputDispatcher.cpp
        ALOGI("Dropping event because there is no focused window or focused application in display "
                "%" PRId32 ".", displayId);
        injectionResult = INPUT_EVENT_INJECTION_FAILED;
        goto Failed;
```

而"系统还没窗口树"属 `SYSTEM_NOT_READY`,无限等待永不 ANR。abort 后的清理由 `resumeAfterTargetsNotReadyTimeoutLocked`(1121)完成:`newTimeout > 0` 续期,否则置 `expired` + `removeWindowByTokenLocked` + 合成 "application not responding" 的 CANCEL,对应新版 `cancelEventsForAnrLocked` 的语义。

## 二、JNI 回调层

新版 `notifyWindowUnresponsive(token, pid, reason)` → 旧版对应是 **`notifyANR`**,且本仓库 Q 的 `IMS.notifyANR` 已是两参(新版的"按 window/activity/pid 三路分流"逻辑在这里下沉到了 WMS):

```1839:1842:frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
    private long notifyANR(IBinder token, String reason) {
        return mWindowManagerCallbacks.notifyANR(
                token, reason);
    }
```

(Q 中 JNI 桥 `NativeInputManager.cpp` 不在本仓库树内,属该 AOSP 分支未收录的 native 侧文件,但其职责即"把 SF 的 `notifyANR(app, token, reason)` 转成对 IMS 这个两参方法的回调,并把返回值纳秒转回 `nsecs_t newTimeout`"。)

## 三、WMS 侧(你贴的 AnrController 在 Q 里是 InputManagerCallback,且没有 dumpAnrState 的"预 dump"概念)

新版 `AnrController.notifyWindowUnresponsive` 的整套动作在 Q 中是 `WindowManagerCallback 实现 InputManagerCallback.notifyANR(IBinder token, String reason)`(注意:Q 尚未把 callback 从 IMS 挪进 WMS 类内部,它已独立成 wm/ 下文件):

```71:130:frameworks/base/services/core/java/com/android/server/wm/InputManagerCallback.java
    public long notifyANR(IBinder token, String reason) {
        AppWindowToken appWindowToken = null;
        WindowState windowState = null;
        boolean aboveSystem = false;
        synchronized (mService.mGlobalLock) {
            if (token != null) {
                windowState = mService.windowForClientLocked(null, token, false);
                if (windowState != null) {
                    appWindowToken = windowState.mAppToken;
                }
            }
            ...
            mService.saveANRStateLocked(appWindowToken, windowState, reason);
        }
        // All the calls below need to happen without the WM lock held since they call into AM.
        mService.mAtmInternal.saveANRState(reason);

        if (appWindowToken != null && appWindowToken.appToken != null) {
            ...
            final boolean abort = appWindowToken.keyDispatchingTimedOut(reason,
                    (windowState != null) ? windowState.mSession.mPid : -1);
            if (!abort) {
                ...
                return appWindowToken.mInputDispatchingTimeoutNanos;
            }
        } else if (windowState != null) {
            ...
            long timeout = mService.mAmInternal.inputDispatchingTimedOut(
                    windowState.mSession.mPid, aboveSystem, reason);
            if (timeout >= 0) {
                ...
                return timeout * 1000000L; // nanoseconds
            }
        }
        return 0; // abort dispatching
    }
```

对照你贴的新版:状态快照对等物是 102 行 `saveANRStateLocked` + 106 行 `mAtmInternal.saveANRState`(Q 存 ANR 现场在 `mService.mLastANRState` / ATMS,无 AnrController 的 preDumpIfLockTooSlow);`[有 Activity]` 分支对等 AppWindowToken→ActivityRecord;`[无 Activity/仅 pid]` 分支对等 else-if windowState → `mAmInternal.inputDispatchingTimedOut(pid, aboveSystem, reason)`。返回纳秒是对 SF 的续期:非 0 时 `resumeAfterTargetsNotReadyTimeoutLocked` 顺延 deadline,0 则放弃。

## 四、Activity/AMS 侧(Q 的裁决链与 9/13 的差异)

有 Activity 分支经 `AppWindowToken.keyDispatchingTimedOut(reason, pid)`(1939,abort 语义)→ `ActivityRecord.keyDispatchingTimedOut`(2463):先 `getWaitingHistoryRecordLocked` 找"真凶"(stopped 的 activity 不算),再判断超时窗口是否属于该 activity 同进程(`windowFromSameProcessAsActivity`,2470-2471);同进程走 6 参 `mAmInternal.inputDispatchingTimedOut(..., reason)`,否则走 pid-only 3 参并把返回 `<0` 当作 abort(2481-2482)。

`AMS.inputDispatchingTimedOut` 6 参实现(Q,非 9 的 mHandler.post、也非 13 的 mAnrHelper):

```18612:18645:frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
    boolean inputDispatchingTimedOut(ProcessRecord proc, String activityShortComponentName,
            ApplicationInfo aInfo, String parentShortComponentName,
            WindowProcessController parentProcess, boolean aboveSystem, String reason) {
        if (checkCallingPermission(FILTER_EVENTS) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Requires permission " + FILTER_EVENTS);
        }
        final String annotation;
        if (reason == null) {
            annotation = "Input dispatching timed out";
        } else {
            annotation = "Input dispatching timed out (" + reason + ")";
        }
        if (proc != null) {
            synchronized (this) {
                if (proc.isDebugging()) {
                    return false;
                }
                if (proc.getActiveInstrumentation() != null) {
                    Bundle info = new Bundle();
                    info.putString("shortMsg", "keyDispatchingTimedOut");
                    info.putString("longMsg", annotation);
                    finishInstrumentationLocked(proc, Activity.RESULT_CANCELED, info);
                    return true;
                }
            }
            proc.appNotResponding(activityShortComponentName, aInfo,
                    parentShortComponentName, parentProcess, aboveSystem, annotation);
        }
        return true;
    }
```

关键差异:你注释行里 `[Android 9] mHandler.post` 的异步投递,在 Q 里变成了**同步直调**,且目标从 AMS 内的 `appNotResponding` 变成了 **`ProcessRecord.appNotResponding`**(1407,Q 把 ANR 主体逻辑迁进了 ProcessRecord;`AppErrors` 类尚未出现):

```1407:1448:frameworks/base/services/core/java/com/android/server/am/ProcessRecord.java
    void appNotResponding(String activityShortComponentName, ApplicationInfo aInfo,
            String parentShortComponentName, WindowProcessController parentProcess,
            boolean aboveSystem, String annotation) {
        ArrayList<Integer> firstPids = new ArrayList<>(5);
        SparseArray<Boolean> lastPids = new SparseArray<>(20);

        mWindowProcessController.appEarlyNotResponding(annotation, () -> kill("anr", true));
        long anrTime = SystemClock.uptimeMillis();
        if (isMonitorCpuUsage()) {
            mService.updateCpuStatsNow();
        }
        synchronized (mService) {
            if (mService.mAtmInternal.isShuttingDown()) { ... return; }
            else if (isNotResponding()) { ... return; }   // 重复 ANR 去重
            ...
            setNotResponding(true);
            EventLog.writeEvent(EventLogTags.AM_ANR, userId, pid, processName, info.flags,
                    annotation);
            firstPids.add(pid);
            if (!isSilentAnr()) { ... 组 firstPids: 父进程 / MY_PID / persistent / IME / lastPids ... }
```

之后(1476 起)收集各进程 traces,后台/不可见 silent ANR 直接 `kill`,前台才弹 ANR 对话框——裁决以该函数为界全部完成,`return true/false` 再沿 ActivityRecord→AppWindowToken 回传,决定 WMS 是否给 SF 续期。

## 五、一张对照总表

| 新版(A12+)/你贴的 | AOSP 10(Q)/本仓库 |
|---|---|
| `dispatchOnce → processAnrsLocked` 集中扫描 | `dispatchOnce → dispatchKey/MotionLocked` 遇到不 ready 才登记,无扫描器 |
| `mAnrTracker.firstTimeout()` 每连接独立超时 | `mInputTargetWaitTimeoutTime` 单点 deadline(1107) |
| 无聚焦窗口分支 `processNoFocusedWindowAnrLocked` | 无该概念:直接 drop 事件(1218-1221) |
| `onAnrLocked` + `notifyNoFocusedWindowAnr` | `onANRLocked`(4068)→ `mPolicy->notifyANR`(4134) |
| `sendWindowUnresponsiveCommandLocked` | `postCommandLocked(doNotifyANRLockedInterruptible)` |
| `notifyWindowUnresponsive(token,pid,reason)` | `IMS.notifyANR(IBinder token, String reason)`(1839) |
| `AnrController`(wm/) | `InputManagerCallback`(wm/,71),存现场在 `saveANRStateLocked`/`mAtmInternal.saveANRState` |
| `mAtmService.inputDispatchingTimedOut(reason,pid)` | `AppWindowToken/ActivityRecord.keyDispatchingTimedOut`(1939/2463)→ `mAmInternal.inputDispatchingTimedOut`(18385/18393) |
| `mAnrHelper.appNotResponding`(13)/`mHandler.post`(9) | **同步** `proc.appNotResponding(...)`(AMS 18640),主体在 `ProcessRecord`(1407) |
| `cancelEventsForAnrLocked` 丢弃该连接事件 | `resumeAfterTargetsNotReadyTimeoutLocked`:`removeWindowByToken`+CANCEL("application not responding") |

一句话结论:Q 的 input-ANR 是把"谁卡住、等多久、报不报、续不续期"全部揉进**事件分发循环 + 一条策略回调**里的早期设计,AMS 端则是**同步直调 ProcessRecord 内的 appNotResponding**;你贴的新版把同一职责拆成了独立的 ANR 扫描器、每连接超时跟踪器与 AnrController/AMS 的异步编排,但两端语义("探测→上报→AMS 裁决续期或中止→中止则移除窗口并 CANCEL")是完全对应的。



```
[Native] InputDispatcher::dispatchOnce()                     // frameworks/native/.../dispatcher/InputDispatcher.cpp
  └─ processAnrsLocked()
       ├─ 无聚焦窗口分支:
       │    mNoFocusedWindowTimeoutTime 到期
       │    └─ processNoFocusedWindowAnrLocked()              // 二次校验 focused app 未变
       │         └─ onAnrLocked(mAwaitedFocusedApplication)
       │              └─ postCommand → mPolicy->notifyNoFocusedWindowAnr(app)
       └─ connection 分发超时分支:
            mAnrTracker.firstTimeout() 到期
            └─ onAnrLocked(connection)
                 ├─ if (connection->waitQueue.empty()) return   // ⚠️ 已恢复则放弃报 ANR
                 ├─ reason = "<channel> is not responding. Waited Nms for <event>"
                 ├─ processConnectionUnresponsiveLocked()
                 │    └─ sendWindowUnresponsiveCommandLocked()
                 │         └─ mPolicy->notifyWindowUnresponsive(token, pid, reason)   // 解锁后回调
                 └─ cancelEventsForAnrLocked(connection)         // 丢弃该连接后续事件

[JNI 回调 Java] InputManagerService.notifyWindowUnresponsive()   // frameworks/base/.../input/InputManagerService.java
  └─ mWindowManagerCallbacks.notifyWindowUnresponsive()          // 实现类 InputManagerCallback (wm/)
       └─ WindowManagerService.mAnrController.notifyWindowUnresponsive(token, pid, reason)   // wm/AnrController.java
            ├─ preDumpIfLockTooSlow()          // debug 版：AM/WM 锁 1s 内拿不到 → 预 dump system_server+SF 现场
            ├─ dumpAnrStateLocked()            // mService.saveANRStateLocked + mAtmService.saveANRState
            ├─ [有 ActivityRecord] activity.inputDispatchingTimedOut(reason, pid)
            └─ [无 Activity/仅 pid] mService.mAmInternal.inputDispatchingTimedOut(pid, aboveSystem, reason)   // ATMS.LocalService
                 └─ AMS.inputDispatchingTimedOut(proc, ...)      // services/core/.../am/ActivityManagerService.java
                      ├─ proc.isDebugging() → return false        // 断点调试豁免，不中止输入分发
                      ├─ proc.getActiveInstrumentation() != null → finishInstrumentationLocked
                      └─ [Android 13] mAnrHelper.appNotResponding(proc, activityShortComponentName, aInfo, ...)
                           └─ [Android 9]  mHandler.post(() -> mAppErrors.appNotResponding(proc, activity, parent, aboveSystem, annotation))
```