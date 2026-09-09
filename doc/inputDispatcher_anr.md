# InputDispatcher 事件分发与 ANR 机制(AOSP 10 / Q 分支)

> **文档范围**:基于本仓库 `Android 10(Q)` 分支源码,分析 `InputDispatcher` 的事件分发链路与 input ANR 触发、上报、裁决全流程。
>
> **核心源码**:`frameworks/native/services/inputflinger/InputDispatcher.cpp`(Q 为扁平布局,头文件与实现同目录,非 Q 之后的 `dispatcher/` 分层)。
>
> **关键结论**:本分支**不存在 `processAnrsLocked`**。该函数是 Android 12+ 重构后引入的入口(新版 `dispatchOnceInnerLocked` 每轮开头调用它集中扫描超时连接)。Q 分支的等价实现是 **`handleTargetsNotReadyLocked` + `onANRLocked` + 命令队列回调**。

## 目录

- [一、线程骨架:dispatchOnce](#一线程骨架dispatchonce)
- [二、一轮派发做什么:dispatchOnceInnerLocked](#二一轮派发做什么dispatchonceinnerlocked)
- [三、事件分发全链路](#三事件分发全链路)
- [四、ANR 计时:handleTargetsNotReadyLocked](#四anr-计时handletargetsnotreadylocked)
- [五、超时爆发与策略裁决](#五超时爆发与策略裁决)
- [六、超时调度模型:单闹钟 + 惰性判定](#六超时调度模型单闹钟--惰性判定)
- [七、关键常量表](#七关键常量表)
- [八、AOSP 10(Q)与 Android 12+ 架构对照](#八aosp-10q与-android-12-架构对照)
- [九、排查建议与调试入口](#九排查建议与调试入口)
- [十、关键函数行号速查](#十关键函数行号速查)

---

## 一、线程骨架:dispatchOnce

`InputDispatcher` 拥有专属线程 `InputDispatcherThread`,其 `threadLoop` 死循环反复调用 `dispatchOnce`:

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
        // The dispatch loop might enqueue commands to run afterwards.
        if (!haveCommandsLocked()) {
            dispatchOnceInnerLocked(&nextWakeupTime);
        }

        // Run all pending commands if there are any.
        // If any commands were run then force the next poll to wake up immediately.
        if (runCommandsLockedInterruptible()) {
            nextWakeupTime = LONG_LONG_MIN;
        }
    } // release lock

    // Wait for callback or timeout or wake.  (make sure we round up, not down)
    nsecs_t currentTime = now();
    int timeoutMillis = toMillisecondTimeoutDelay(currentTime, nextWakeupTime);
    mLooper->pollOnce(timeoutMillis);
}
```

要点:

- 每轮先持有 `mLock` 并 `mDispatcherIsAlive.notify_all()`,供 `monitor()` 做死锁探活。
- 有 pending 命令时**先不跑派发**(命令优先);命令执行完强制 `LONG_LONG_MIN`,下一轮立即再转。
- `pollOnce` 是本循环**唯一的睡眠点**,唤醒来源只有三种:
  1. 已注册的 input channel fd 有数据到达(`mLooper->addFd` + `handleReceiveCallback`);
  2. 定时器到点(即 `nextWakeupTime`,ANR 截止时间正是靠它传导);
  3. 其它线程(如 InputReader)调用 `mLooper->wake()`。

---

## 二、一轮派发做什么:dispatchOnceInnerLocked

```290:291:frameworks/native/services/inputflinger/InputDispatcher.cpp
void InputDispatcher::dispatchOnceInnerLocked(nsecs_t* nextWakeupTime) {
    nsecs_t currentTime = now();
```

按顺序处理:

1. **dispatch 冻结检查**:`mDispatchFrozen` 为真直接短路,不处理超时也不投递新事件。
2. **app switch 到期**:提前唤醒,抢占派发并丢弃积压事件。
3. **取事件**:若当前无在派发事件(`mPendingEvent`)且 `mInboundQueue` 有货则出队;队列空时按需合成 key repeat。
4. **`pokeUserActivity`**:点亮屏幕相关。
5. **丢弃判定**:`POLICY_FLAG` 不允许传给用户、dispatch 被禁用、被 app switch 抢占(`mNextUnblockedEvent` 之前的旧事件)、超过 `STALE_EVENT_TIMEOUT`(10s)的陈旧事件。
6. **按类型分派**:`switch (event->type)` → `dispatchKeyLocked` / `dispatchMotionLocked` / `dispatchFocusLocked` 等。
7. 无事可做则结束本轮并置唤醒。

每条事件出队后到真正落窗口之间,还会调用 `resetANRTimeoutsLocked`,把上一轮残留的 ANR 等待现场复位。

---

## 三、事件分发全链路

### 3.1 生命周期总览

```text
InputReader(设备线程) → notifyKey/notifyMotion(入队)
  → InputDispatcher 主线程 dispatchOnce 循环
      → 出队 mInboundQueue → dispatchKeyLocked/dispatchMotionLocked
          → findFocused/TouchedWindowTargetsLocked(目标决策)
              → [不 ready] handleTargetsNotReadyLocked(登记等待 + ANR 截止时间)
          → dispatchEventLocked → addWindowTargetLocked
          → enqueueDispatchEntryLocked(入 outboundQueue)
          → startDispatchCycleLocked(publish 写共享内存 + socket, 移入 waitQueue)
  → 应用侧 InputChannel 消费 → 写回 finished 信号
  → handleReceiveCallback(fd 可读) → finishDispatchCycleLocked(清 waitQueue)
  → 事件 refCount 归零 → 从 mPendingEvent 释放 → 下一轮循环
```

整条链由一把 `mLock`、条件变量 `mInboundQueueNotEmpty` 与一组 fd/epoll(`pollOnce`)串起。

### 3.2 目标查找

以按键为例,`dispatchKeyLocked`(790 行起)先做 key repeat 预处理(793-827),再处理策略层上一轮的 `TRY_AGAIN_LATER` 与 `interceptKeyBeforeDispatching`(830-862),确认不丢弃后进入目标阶段:

```872:889:frameworks/native/services/inputflinger/InputDispatcher.cpp
    // Identify targets.
    std::vector<InputTarget> inputTargets;
    int32_t injectionResult = findFocusedWindowTargetsLocked(currentTime,
            entry, inputTargets, nextWakeupTime);
    if (injectionResult == INPUT_EVENT_INJECTION_PENDING) {
        return false;
    }

    setInjectionResult(entry, injectionResult);
    if (injectionResult != INPUT_EVENT_INJECTION_SUCCEEDED) {
        return true;
    }

    // Add monitor channels from event's or focused display.
    addGlobalMonitoringTargetsLocked(inputTargets, getTargetDisplayId(entry));

    // Dispatch the key.
    dispatchEventLocked(currentTime, entry, inputTargets);
```

- 键盘事件走 `findFocusedWindowTargetsLocked`(焦点窗口);
- 触摸走 `findTouchedWindowTargetsLocked`(依 DOWN 命中的触摸目标,后续 MOVE/UP 跟随同一目标)。

两者投递哲学不同(见注释 1852-1866):

- **触摸**按"用户当时看到什么就摸什么"投递,不做严格队列串行,以便跟手;
- **按键**必须等该窗口之前所有事件完成(1833-1850),因为先前事件可能转移焦点,按键要送到新焦点。

### 3.3 ready 检查:ANR 的第一现场

目标窗口逐个经过 `checkWindowReadyForMoreInputLocked`(1800 行起)体检,任一不通过即产生一条 `reason` 字符串,随后 `goto Unresponsive` 汇聚到 `handleTargetsNotReadyLocked`。每条 reason 都会原样出现在 logcat 与 dumpsys 中:

| 检查项 | 行号 | 说明 |
|---|---|---|
| 窗口 paused | 1804 | 窗口处于暂停态 |
| input channel 未注册 | 1809 | 窗口可能正在被移除 |
| connection 状态非 NORMAL | 1819 | 如已被判定 broken |
| **输入管道满** | 1826 | `inputPublisherBlocked`,最常见的 ANR 现场:应用不消费导致共享内存环写满 |
| key 遇残留队列 | 1845 | outbound/waitQueue 非空则必须等 |
| **非 key 事件滞留** | 1867-1869 | waitQueue 头部事件超过 `STREAM_AHEAD_EVENT_TIMEOUT`(0.5s)未 finish |

最后一条是关键设计:触摸平时允许"超前流送",一旦最早未完成事件滞留超过 0.5s 就停止投递,目的是**让 ANR 能被可靠、及时地测出来**,而不是让队列无限堆积。

### 3.4 投递动作与回收回环

目标就绪后:`dispatchEventLocked` 对每个 target 调 `enqueueDispatchEntryLocked`(2030 行),构造 `DispatchEntry`,做 `inputState.trackKey/trackMotion` 一致性记账(跳过不一致事件、补齐 HOVER、改写 OUTSIDE/HOVER_EXIT 等,2054-2120),随后入 `connection->outboundQueue`。

`startDispatchCycleLocked`(2160 行)每轮尽量 flush outbound 队列:

- publish 前打上 `deliveryTime = currentTime`(2175,即 ANR 判龄起点);
- 经 `inputPublisher.publishKeyEvent` / `publishMotionEvent` 写入共享内存并触发 socket 通知;
- 环满返回 `WOULD_BLOCK` 则置 `inputPublisherBlocked = true` 等应用消化(2248-2266);
- 成功后 DispatchEntry 从 outbound 移入 `waitQueue`(2276-2279)——即 dumpsys 中可见的"已收未答"清单。

应用处理完写回 finished 信号,dispatcher 侧 fd 变可读触发 `handleReceiveCallback`(2340 行):

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
                if (status == WOULD_BLOCK) {
                    return 1;
                }
            }
```

`finishDispatchCycleLocked`(2283 行)按 seq 摘掉 waitQueue 条目、清 `inputPublisherBlocked`,再经 `onDispatchCycleFinishedLocked` 决定是否需要唤醒后续派发。至此一轮完整分发周期闭合。

---

## 四、ANR 计时:handleTargetsNotReadyLocked

```1044:1048:frameworks/native/services/inputflinger/InputDispatcher.cpp
int32_t InputDispatcher::handleTargetsNotReadyLocked(nsecs_t currentTime,
        const EventEntry* entry,
        const sp<InputApplicationHandle>& applicationHandle,
        const sp<InputWindowHandle>& windowHandle,
        nsecs_t* nextWakeupTime, const char* reason) {
```

它为"等待"建档并设闹钟,分两种语义:

```1049:1089:frameworks/native/services/inputflinger/InputDispatcher.cpp
    if (applicationHandle == nullptr && windowHandle == nullptr) {
        if (mInputTargetWaitCause != INPUT_TARGET_WAIT_CAUSE_SYSTEM_NOT_READY) {
            mInputTargetWaitCause = INPUT_TARGET_WAIT_CAUSE_SYSTEM_NOT_READY;
            mInputTargetWaitStartTime = currentTime;
            mInputTargetWaitTimeoutTime = LONG_LONG_MAX;
            mInputTargetWaitTimeoutExpired = false;
            mInputTargetWaitApplicationToken.clear();
        }
    } else {
        if (mInputTargetWaitCause != INPUT_TARGET_WAIT_CAUSE_APPLICATION_NOT_READY) {
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
            mInputTargetWaitTimeoutExpired = false;
            mInputTargetWaitApplicationToken.clear();
            ...
        }
    }
```

- **SYSTEM_NOT_READY**:窗口与应用都不存在(如开机早期窗口树未建),`LONG_LONG_MAX` 无限等待,**永不 ANR**。
- **APPLICATION_NOT_READY**:窗口或应用存在但未就绪,取该窗口的 dispatchingTimeout(可被 WMS 定制,缺省 5s),并把 deadline 同步给主循环的 `nextWakeupTime`(1107-1108)。

典型调用现场:

- 应用启动中无窗口但有 focused application(1210-1214):等待应用起窗口,同样不加超时;
- 焦点窗口不 ready(1234-1235);
- 触摸目标分支同理(1538-1540)。

> **与新版的结构差异**:Q 版没有"每轮扫描谁超时"的机制,而是"谁在等待,谁就把自己的 deadline 塞进下一轮的 `nextWakeupTime`"。

---

## 五、超时爆发与策略裁决

### 5.1 触发点

到点后窗口仍不 ready,在同一入口处触发:

```1096:1111:frameworks/native/services/inputflinger/InputDispatcher.cpp
    if (currentTime >= mInputTargetWaitTimeoutTime) {
        // input target wait timeout expired ,input anr
        onANRLocked(currentTime, applicationHandle, windowHandle,
                entry->eventTime, mInputTargetWaitStartTime, reason);

        // Force poll loop to wake up immediately on next iteration once we get the
        // ANR response back from the policy.
        *nextWakeupTime = LONG_LONG_MIN;
        return INPUT_EVENT_INJECTION_PENDING;
    } else {
        // Force poll loop to wake up when timeout is due.
        if (mInputTargetWaitTimeoutTime < *nextWakeupTime) {
            *nextWakeupTime = mInputTargetWaitTimeoutTime;
        }
        return INPUT_EVENT_INJECTION_PENDING;
    }
```

若策略已裁决放弃(`mInputTargetWaitTimeoutExpired`),后续循环直接返回 `INPUT_EVENT_INJECTION_TIMED_OUT`(1092-1094)。

### 5.2 onANRLocked:上报 + 留档

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

三件事:

1. 打 logcat(`Application is not responding`,含距事件时长、距开始等待时长、原因);
2. **把全量 dispatcher 状态快照进 `mLastANRState`**——之后 `dumpsys input` 首屏即可见,是排查卡死的黄金数据;
3. 仅入队一个命令即返回,**事件仍保持 PENDING 不丢**。

### 5.3 doNotifyANRLockedInterruptible:解锁回调策略层

真正的外部动作在下一轮 `runCommandsLockedInterruptible()` 中执行,**先解锁再回调**,避免持锁进行 binder 调用:

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

调用链:`NativeInputManager` → `InputManagerService` → `AMS`。由 **AMS 决定**弹 ANR 对话框、继续等待还是杀进程;裁决结果以纳秒 `newTimeout` 返回,大于 0 表示续期。

### 5.4 resumeAfterTargetsNotReadyTimeoutLocked:续期或放弃

```1121:1148:frameworks/native/services/inputflinger/InputDispatcher.cpp
void InputDispatcher::resumeAfterTargetsNotReadyTimeoutLocked(nsecs_t newTimeout,
        const sp<InputChannel>& inputChannel) {
    if (newTimeout > 0) {
        // Extend the timeout.
        mInputTargetWaitTimeoutTime = now() + newTimeout;
    } else {
        // Give up.
        mInputTargetWaitTimeoutExpired = true;

        // Input state will not be realistic.  Mark it out of sync.
        if (inputChannel.get()) {
            ssize_t connectionIndex = getConnectionIndexLocked(inputChannel);
            if (connectionIndex >= 0) {
                sp<Connection> connection = mConnectionsByFd.valueAt(connectionIndex);
                sp<IBinder> token = connection->inputChannel->getToken();

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
    }
}
```

- **续期**:把 deadline 顺延到 `now + newTimeout` 继续等(如 ANR 对话框用户点 "Wait");
- **放弃**三连:置 `mInputTargetWaitTimeoutExpired` → 按 token 移除该窗口(触摸状态一并清)→ 给该 connection 合成 `"application not responding"` 的 CANCEL 事件。

此后该事件以 `INPUT_EVENT_INJECTION_TIMED_OUT` 收场,后续新事件因窗口已移除而重新寻路,系统得以绕过卡死的 app。

### 5.5 配套机制:复位与"用户点走"

- `resetANRTimeoutsLocked`(1159 行):把 `mInputTargetWaitCause` 复位为 `NONE` 并清空 `mInputTargetWaitApplicationToken`;窗口树变化、焦点变化等路径触发。
- **app switch 逃生通道**:处于 `APPLICATION_NOT_READY` 等待时,若用户触摸了**另一个**应用的窗口,dispatcher 将该触摸事件记为 `mNextUnblockedEvent`,丢弃它之前排队的全部旧事件,输入流立即切到新应用,无需等 5s。

---

## 六、超时调度模型:单闹钟 + 惰性判定

`nextWakeupTime` 是**本文件唯一的"软定时器"时钟源**:每轮以 `LONG_LONG_MAX`(无事可做)初始化,派发路径中任何关心"某时刻必须被叫醒"的逻辑都把自己的 deadline 与之取小写回;命令被执行过则赋 `LONG_LONG_MIN` 强迫 0 毫秒超时立即转下一轮。

`toMillisecondTimeoutDelay` 换算存在三个特例:

| 输入 | 结果 | 语义 |
|---|---|---|
| `LONG_LONG_MAX` | `-1` | `pollOnce` 无限阻塞 |
| `diff <= 0` | `0` | 立即返回 |
| 正常 | 向上取整毫秒 | 闹钟到点 |

### 6.1 一轮派发内塞进闹钟的时刻

| 写点 | 塞入什么 | 语义 |
|---|---|---|
| 279-280 | `LONG_LONG_MIN` | 有命令刚执行完,不睡,立即再转一圈 |
| 312-313 | `mAppSwitchDueTime` | **App switch 缓冲到期**:允许等待中的事件队列被"用户点走"抢占 |
| 332-333 | `mKeyRepeatState.nextRepeatTime` | **长按 repeat 闹钟**:到点仍按住则 synthesize 一条 repeat |
| 438-439 | `LONG_LONG_MIN` | 事件被丢弃/处理完释放 pending 后,立即取下一个 |
| 832-833 | `entry->interceptKeyWakeupTime` | **policy 拦截重试**:`TRY_AGAIN_LATER` 到点重新进 `dispatchKeyLocked` |
| 1102-1103 | `LONG_LONG_MIN` | ANR 已上报,立刻醒来等策略层回话(命令队列) |
| 1107-1108 | `mInputTargetWaitTimeoutTime` | **ANR deadline**(默认 5s):窗口不 ready 时注册的唤醒点 |

其中 1107 是 ANR 核心——`handleTargetsNotReadyLocked` 把 `now + dispatchingTimeout` 塞进闹钟,主线程睡到那一刻复查目标是否恢复。

另有一类**不依赖闹钟、每轮即时判断**的时限:入队处判 stale 超过 10s 丢弃、motion 超前流送超 0.5s 停发、事件处理超 2s 仅打慢处理警告。

### 6.2 惰性求值,没有回调线程

全文件没有"定时器到期 → 自动回调"机制。超时触发一律是**到点后下一次重新进入同一段代码时用 `now() >= deadline` 就地判断**:

- keyRepeat:`dispatchOnceInnerLocked` 取事件前(327-336)判断到点即 synthesize;
- intercept 重试:`dispatchKeyLocked` 开头(830-839);
- ANR:`handleTargetsNotReadyLocked` 开头(1096)。

到点后不会重复爆发:上报完策略立即把唤醒点改成 `LONG_LONG_MIN` 去等命令结果;命令回来后由 `resumeAfterTargetsNotReadyTimeoutLocked` 决定续期或放弃,再把新 deadline(或不再等)写回闹钟。**每个超时只爆发一次,处理完必须自己重新注册下一个闹钟**。

### 6.3 超时不删除事件

ANR/拦截超时到点,**事件不会因超时被自动删除**:被卡住的事件一直占着 `mPendingEvent`,期间新事件排队在 `mInboundQueue`。事件退出派发只有三种情况:

1. 策略裁决放弃(窗口被移除、CANCEL 已合成);
2. 事件最终以 `INPUT_EVENT_INJECTION_TIMED_OUT` 结束;
3. dispatch 被冻结(`mDispatchFrozen`)时直接丢弃。

真正清理动作发生在 438 行附近:事件走完生命周期 → `releasePendingEventLocked()` → `LONG_LONG_MIN` 立刻取下一个。等待注入结果的调用方(如 `adb shell input`)阻塞在自己的条件变量上,由 `setInjectionResult` 唤醒;若 dispatcher 迟迟不结束该事件,注入方按等待超时自行返回。

---

## 七、关键常量表

```72:92:frameworks/native/services/inputflinger/InputDispatcher.cpp
// Default input dispatching timeout if there is no focused application or paused window
// from which to determine an appropriate dispatching timeout.
constexpr nsecs_t DEFAULT_INPUT_DISPATCHING_TIMEOUT = 5000 * 1000000LL; // 5 sec

// Amount of time to allow for all pending events to be processed when an app switch
// key is on the way. ...
constexpr nsecs_t APP_SWITCH_TIMEOUT = 500 * 1000000LL; // 0.5sec

// Amount of time to allow for an event to be dispatched (measured since its eventTime)
// before considering it stale and dropping it.
constexpr nsecs_t STALE_EVENT_TIMEOUT = 10000 * 1000000LL; // 10sec

// Amount of time to allow touch events to be streamed out to a connection before requiring
// that the first event be finished. ...
constexpr nsecs_t STREAM_AHEAD_EVENT_TIMEOUT = 500 * 1000000LL; // 0.5sec

// Log a warning when an event takes longer than this to process, even if an ANR does not occur.
constexpr nsecs_t SLOW_EVENT_PROCESSING_WARNING_TIMEOUT = 2000 * 1000000LL; // 2sec
```

另有长按 repeat 相关参数由 policy 启动时经 `policy->getDispatcherConfiguration(&mConfig)` 下发(248 行):`mConfig.keyRepeatTimeout`(首次 repeat 延迟)与 `mConfig.keyRepeatDelay`(repeat 间隔,用于 `synthesizeKeyRepeatLocked` 的 756 行)。

---

## 八、AOSP 10(Q)与 Android 12+ 架构对照

### 8.1 新版(A12+)调用链

新版把 ANR 检测独立为扫描器,并按"窗口/应用"与"无焦点窗口"分路上报:

```text
[Native] InputDispatcher::dispatchOnce()
  └─ processAnrsLocked()
       ├─ 无聚焦窗口分支:
       │    mNoFocusedWindowTimeoutTime 到期
       │    └─ processNoFocusedWindowAnrLocked()          // 二次校验 focused app 未变
       │         └─ onAnrLocked(mAwaitedFocusedApplication)
       │              └─ postCommand → mPolicy->notifyNoFocusedWindowAnr(app)
       └─ connection 分发超时分支:
            mAnrTracker.firstTimeout() 到期
            └─ onAnrLocked(connection)
                 ├─ if (connection->waitQueue.empty()) return   // 已恢复则放弃报 ANR
                 ├─ reason = "<channel> is not responding. Waited Nms for <event>"
                 ├─ processConnectionUnresponsiveLocked()
                 │    └─ sendWindowUnresponsiveCommandLocked()
                 │         └─ mPolicy->notifyWindowUnresponsive(token, pid, reason)
                 └─ cancelEventsForAnrLocked(connection)         // 丢弃该连接后续事件

[JNI 回调 Java] InputManagerService.notifyWindowUnresponsive()
  └─ mWindowManagerCallbacks.notifyWindowUnresponsive()          // InputManagerCallback (wm/)
       └─ WindowManagerService.mAnrController.notifyWindowUnresponsive(token, pid, reason)
            ├─ preDumpIfLockTooSlow()
            ├─ dumpAnrStateLocked()      // saveANRStateLocked + mAtmService.saveANRState
            ├─ [有 ActivityRecord] activity.inputDispatchingTimedOut(reason, pid)
            └─ [无 Activity/仅 pid] mService.mAmInternal.inputDispatchingTimedOut(pid, aboveSystem, reason)
                 └─ AMS.inputDispatchingTimedOut(proc, ...)
                      ├─ proc.isDebugging() → return false       // 断点调试豁免
                      ├─ proc.getActiveInstrumentation() != null → finishInstrumentationLocked
                      └─ [Android 13] mAnrHelper.appNotResponding(proc, ...)
                           └─ [Android 9]  mHandler.post(() -> mAppErrors.appNotResponding(...))
```

### 8.2 Q(AOSP 10)等价链

**SF 侧**:无集中扫描器,走"惰性等待 + 命令队列上报",即第四、五节所述 `handleTargetsNotReadyLocked` → `onANRLocked` → `doNotifyANRLockedInterruptible`。

**"无焦点窗口"在 Q 中不是 ANR**,而是直接丢弃事件:

```1218:1221:frameworks/native/services/inputflinger/InputDispatcher.cpp
        ALOGI("Dropping event because there is no focused window or focused application in display "
                "%" PRId32 ".", displayId);
        injectionResult = INPUT_EVENT_INJECTION_FAILED;
        goto Failed;
```

**JNI 回调层**:新版 `notifyWindowUnresponsive(token, pid, reason)` 对应 Q 的 `notifyANR`,且 Q 的 `IMS.notifyANR` 已是两参版(分流逻辑下沉到 WMS):

```1839:1842:frameworks/base/services/core/java/com/android/server/input/InputManagerService.java
    private long notifyANR(IBinder token, String reason) {
        return mWindowManagerCallbacks.notifyANR(
                token, reason);
    }
```

> 注:Q 的 JNI 桥 `NativeInputManager.cpp` 未收录于本仓库树,其职责是把 SF 的 `notifyANR(app, token, reason)` 转成对该两参方法的回调,并把返回的纳秒转回 `nsecs_t newTimeout`。

**WMS 侧**:新版 `AnrController` 在 Q 中是 `InputManagerCallback`(仍位于 `wm/` 下,尚未并入 IMS 类内部),且没有 `preDumpIfLockTooSlow` 概念:

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
            // Notify the activity manager about the timeout and let it decide whether
            // to abort dispatching or keep waiting.
            final boolean abort = appWindowToken.keyDispatchingTimedOut(reason,
                    (windowState != null) ? windowState.mSession.mPid : -1);
            if (!abort) {
                // The activity manager declined to abort dispatching.
                // Wait a bit longer and timeout again later.
                return appWindowToken.mInputDispatchingTimeoutNanos;
            }
        } else if (windowState != null) {
            long timeout = mService.mAmInternal.inputDispatchingTimedOut(
                    windowState.mSession.mPid, aboveSystem, reason);
            if (timeout >= 0) {
                // The activity manager declined to abort dispatching.
                // Wait a bit longer and timeout again later.
                return timeout * 1000000L; // nanoseconds
            }
        }
        return 0; // abort dispatching
    }
```

- 状态快照对应 102 行 `saveANRStateLocked` 与 106 行 `mAtmInternal.saveANRState`;
- `[有 Activity]` 分支 → `AppWindowToken.keyDispatchingTimedOut`;
- `[无 Activity/仅 pid]` 分支 → `mAmInternal.inputDispatchingTimedOut(pid, aboveSystem, reason)`;
- 返回纳秒即给 SF 的续期值,返回 0 表示放弃。

**Activity/AMS 侧**:有 Activity 时经 `AppWindowToken.keyDispatchingTimedOut(reason, pid)`(1939,abort 语义)→ `ActivityRecord.keyDispatchingTimedOut`(2463):先用 `getWaitingHistoryRecordLocked` 找"真凶"(stopped 的 activity 不算),再判断是否与超时窗口同进程(`windowFromSameProcessAsActivity`,2470-2471);同进程走 6 参,否则走 pid-only 3 参并以 `< 0` 为 abort(2481-2482)。

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

与相邻版本的差异:Android 9 用 `mHandler.post` 异步投递、Android 13 用 `mAnrHelper`,而 **Q 是同步直调**,且 ANR 主体已从 AMS 迁至 `ProcessRecord`(`AppErrors` 类在 Q 尚未出现):

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
            // PowerManager.reboot() can block for a long time, so ignore ANRs while shutting down.
            if (mService.mAtmInternal.isShuttingDown()) { ... return; }
            else if (isNotResponding()) { ... return; }   // 重复 ANR 去重
            else if (isCrashing()) { ... return; }
            else if (killedByAm) { ... return; }
            else if (killed) { ... return; }

            // In case we come through here for the same app before completing
            // this one, mark as anring now so we will bail out.
            setNotResponding(true);

            // Log the ANR to the event log.
            EventLog.writeEvent(EventLogTags.AM_ANR, userId, pid, processName, info.flags,
                    annotation);

            // Dump thread traces as quickly as we can, starting with "interesting" processes.
            firstPids.add(pid);

            // Don't dump other PIDs if it's a background ANR
            if (!isSilentAnr()) { ... 组 firstPids: 父进程 / MY_PID / persistent / IME / lastPids ... }
```

其后(1476 行起)收集各进程 traces:后台/不可见 silent ANR 直接 kill,前台才弹 ANR 对话框;返回值沿 `ActivityRecord` → `AppWindowToken` 回传,决定 WMS 是否给 SF 续期。

### 8.3 对照总表

| 新版(A12+) | AOSP 10(Q)/ 本仓库 |
|---|---|
| `dispatchOnce → processAnrsLocked` 集中扫描 | `dispatchOnce → dispatchKey/MotionLocked` 遇不 ready 才登记,无扫描器 |
| `mAnrTracker.firstTimeout()` 每连接独立超时 | `mInputTargetWaitTimeoutTime` 单点 deadline(1107) |
| 无聚焦窗口分支 `processNoFocusedWindowAnrLocked` | 无该概念:直接 drop 事件(1218-1221) |
| `onAnrLocked` + `notifyNoFocusedWindowAnr` | `onANRLocked`(4068)→ `mPolicy->notifyANR`(4134) |
| `sendWindowUnresponsiveCommandLocked` | `postCommandLocked(doNotifyANRLockedInterruptible)` |
| `notifyWindowUnresponsive(token, pid, reason)` | `IMS.notifyANR(IBinder token, String reason)`(1839) |
| `AnrController`(wm/) | `InputManagerCallback`(wm/,71),存现场于 `saveANRStateLocked` / `mAtmInternal.saveANRState` |
| `mAtmService.inputDispatchingTimedOut(reason, pid)` | `AppWindowToken/ActivityRecord.keyDispatchingTimedOut`(1939 / 2463)→ `mAmInternal.inputDispatchingTimedOut`(18385 / 18393) |
| `mAnrHelper.appNotResponding`(13)/ `mHandler.post`(9) | **同步** `proc.appNotResponding(...)`(AMS 18640),主体在 `ProcessRecord`(1407) |
| `cancelEventsForAnrLocked` 丢弃该连接事件 | `resumeAfterTargetsNotReadyTimeoutLocked`:`removeWindowByToken` + CANCEL("application not responding") |

**结论**:Q 把"谁卡住、等多久、报不报、续不续期"全部揉进**事件分发循环 + 一条策略回调**,AMS 端同步直调 `ProcessRecord.appNotResponding`;新版将其拆为独立的 ANR 扫描器、每连接超时跟踪器与 `AnrController`/AMS 异步编排。两端语义一致:**探测 → 上报 → AMS 裁决续期或中止 → 中止则移除窗口并 CANCEL**。

---

## 九、排查建议与调试入口

按优先级:

1. **logcat 定性**:`logcat -s InputDispatcher`,看 `Application is not responding ... Reason:` 行(含距事件时长、等待时长、原因)。
2. **dumpsys 定位**:立即 `dumpsys input`,先看首屏 `Input Dispatcher State at time of last ANR` 快照(ANR 瞬间全量状态),再核对当前段:
   - 当前 `mPendingEvent`;
   - 各 connection 的 `OutboundQueue` / `WaitQueue` 长度与滞留时间;
   - `inputPublisherBlocked` 标志。
3. **窗口侧**:`dumpsys window windows` 确认该窗口的 dispatchingTimeout 是否被改过。
4. **应用侧闭环**:`dumpsys activity` 与 ANR trace,确认主线程(UI 线程消息队列)是否被卡住。

其它特征日志:`Channel is unrecoverably broken`(连接判死)、`Stale event`(10s 陈旧丢弃)。若 `pollOnce` 长期被设为接近 0(反复空转),通常是命令风暴或 `LONG_LONG_MIN` 路径过多。

---

## 十、关键函数行号速查

| 函数 | 行号 | 职责 |
|---|---|---|
| `InputDispatcherThread::threadLoop` | 5239 | 驱动 `dispatchOnce` 死循环 |
| `dispatchOnce` | 265 | 主循环:派发 → 跑命令 → `pollOnce` |
| `dispatchOnceInnerLocked` | 290 | 一轮派发(去冻/取事件/判丢弃/分派) |
| `dispatchKeyLocked` | 790 | 按键派发(repeat 预处理、policy 拦截、目标查找) |
| `checkWindowReadyForMoreInputLocked` | 1800 | 窗口 ready 体检,产出 ANR reason |
| `handleTargetsNotReadyLocked` | 1044 | 登记等待与 ANR deadline,到点触发上报 |
| `findFocusedWindowTargetsLocked` | 1194 | 按键目标查找 |
| `findTouchedWindowTargetsLocked` | 1258 | 触摸目标查找 |
| `resetANRTimeoutsLocked` | 1159 | 复位 ANR 等待状态 |
| `resumeAfterTargetsNotReadyTimeoutLocked` | 1121 | 策略裁决后:续期或放弃(移除窗口 + CANCEL) |
| `onANRLocked` | 4068 | ANR 上报与状态快照 |
| `doNotifyANRLockedInterruptible` | 4134 | 解锁回调 `mPolicy->notifyANR` |
| `resetKeyRepeatLocked` / `synthesizeKeyRepeatLocked` | 721 / 728 | 长按 repeat 状态清理与合成 |
| `enqueueDispatchEntryLocked` | 2030 | 构造 DispatchEntry 入 outboundQueue |
| `startDispatchCycleLocked` | 2160 | 发布事件并移入 waitQueue |
| `handleReceiveCallback` | 2340 | 应用回 finished 信号后的回收入口 |
| `finishDispatchCycleLocked` | 2283 | 按 seq 摘除 waitQueue 条目 |
