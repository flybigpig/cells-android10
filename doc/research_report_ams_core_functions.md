# ActivityManagerService (AMS) 核心功能总结

## 定位

`ActivityManagerService`（位于 `frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java`）是 Android framework 中**最核心的系统服务**之一，运行在 `system_server` 进程。它对外实现 `IActivityManager.Stub`（应用层 `ActivityManager` 通过 `IActivityManager` 与之通信），同时实现 `Watchdog.Monitor` 与 `BatteryStatsImpl.BatteryCallback`。

AMS 本身不直接持有所有逻辑，而是通过一组“子系统助手类”分工协作。关键协作字段（`ActivityManagerService.java`）：

| 字段 | 类型 | 职责 |
|---|---|---|
| `mProcessList` | `ProcessList` | 进程创建/回收 |
| `mOomAdjuster` | `OomAdjuster` | OOM 评分与内存压力管理 |
| `mServices` | `ActiveServices` | 四大组件之 Service |
| `mBroadcastQueues[3]` | `BroadcastQueue` | 广播队列（fg/bg/offload） |
| `mBroadcastDispatcher` | `BroadcastDispatcher` | 广播分发与慢接收者缓解 |
| `mUserController` | `UserController` | 多用户切换 |
| `mAppErrors` | `AppErrors` | 崩溃 / ANR 处理 |
| `mBatteryStatsService` | `BatteryStatsService` | 电量/资源统计 |
| `mPendingIntentController` | `PendingIntentController` | PendingIntent 管理 |
| `mActivityTaskManager` | `ActivityTaskManagerService` | Activity/任务栈（wm 包） |

## 核心功能

### 1. 进程管理（ProcessList）

AMS 负责所有应用进程的生与死。`startProcessLocked()` 实际委托给 `mProcessList.startProcessLocked()`，最终通过 `Process.ZYGOTE_PROCESS` 向 zygote 发请求 fork 出子进程（如 `ActivityManagerService.java:3091`、`4927`、`7888` 等大量调用点）。每个进程对应一个 `ProcessRecord`（保存 uid、包名、状态、已加载组件等），AMS 维护 `ProcessRecord` 表、LRU 进程列表、进程死亡监听（`AppDeathRecipient`），并在进程 attach（`attachApplicationLocked`）时完成 Application 初始化与组件调度。

### 2. 内存与 OOM 管理（OomAdjuster）

`OomAdjuster`（`OomAdjuster.java`）根据进程承载的组件（前台 Activity、Service、Provider、可见性等）计算 `oom_score_adj` 与 `ProcessState`，写入 `/proc/<pid>/oom_score_adj`，并施加调度组（cgroup / thread group：`THREAD_GROUP_TOP_APP`、`THREAD_GROUP_BG_NONINTERACTIVE` 等）、CPU 调度策略。这是 Android 在低内存时决定“杀谁”的核心机制。

### 3. Activity 管理（委托给 ATMS）

在 Android 10 中，Activity/任务栈/窗口层级管理已从 AMS 拆分到独立服务 `ActivityTaskManagerService`（包 `com.android.server.wm`）。AMS 仍提供 `IActivityManager` 接口（`startActivity`、`startActivityAsUser` 等），但**直接转发**给 ATMS：

```3572:3577:ActivityManagerService.java
public int startActivity(IApplicationThread caller, String callingPackage,
        Intent intent, String resolvedType, IBinder resultTo, String resultWho, int requestCode,
        int startFlags, ProfilerInfo profilerInfo, Bundle bOptions) {
    return mActivityTaskManager.startActivity(caller, callingPackage, intent, resolvedType,
            resultTo, resultWho, requestCode, startFlags, profilerInfo, bOptions);
}
```

因此 AMS 在 Activity 维度主要承担“入口/权限校验/跨用户转发”，真正的栈、生命周期、焦点切换由 ATMS 完成（`mAtmInternal` 为 `ActivityTaskManagerInternal`，`ActivityManagerService.java:1478`）。

### 4. Service 管理（ActiveServices）

四大组件中的 Service 由 `ActiveServices`（`ActiveServices.java`）负责：`startServiceLocked`、`bindServiceLocked`、服务生命周期、`ServiceRecord`、`ConnectionRecord`（绑定关系）、`IntentBindRecord` 等。AMS 在 `broadcastIntentLocked` 之外，对 `startService`/`bindService` 进行权限、后台启动限制（后台启动服务的限制）、绑定超时等管控。

### 5. 广播管理（BroadcastQueue + BroadcastDispatcher）

广播发送 `broadcastIntentLocked()` 入队到 `mBroadcastQueues[3]`：`mFgBroadcastQueue`（前台）、`mBgBroadcastQueue`（后台）、`mOffloadBroadcastQueue`（卸载/低优先级），分别有不同超时与调度（`ActivityManagerService.java:2567-2569`）。`BroadcastDispatcher`（`BroadcastDispatcher.java` 类注释：“Manages ordered broadcast delivery, applying policy to mitigate the effects of slow receivers.”）负责按策略对慢接收者做延迟/退避，避免单个劣质 Receiver 拖垮整体广播分发，并维护 `BroadcastRecord`、`BroadcastFilter`、`ReceiverList`。

### 6. Content Provider 管理

通过 `getContentProviderImpl()`（`ActivityManagerService.java:6789` 起）解析、缓存并发布 `ContentProvider`：维护 `ContentProviderRecord`、`ProviderMap`、`ContentProviderConnection`，按需启动承载 provider 的进程，并做权限校验与跨用户约束。

### 7. 应用错误与 ANR 处理（AppErrors）

`AppErrors`（`AppErrors.java`）集中处理进程崩溃、Native 崩溃、ANR：弹出 `AppErrorDialog` / `AppNotRespondingDialog` / `AppWaitingForDebuggerDialog`，记录 `ApplicationErrorReport`，并配合 `Watchdog`（AMS 实现 `Watchdog.Monitor`）在系统卡死时触发重启。

### 8. 多用户管理（UserController）

`UserController`（`UserController.java`）管理 Android 的多用户：切换用户（`switchUser`）、启动/停止用户、解锁状态机（`UserState`：`STATE_BOOTING`/`RUNNING_UNLOCKING`/`RUNNING_UNLOCKED`…）、用户切换观察者与对话框。几乎所有 AMS 操作都带 `userId` 维度做隔离。

### 9. 统计与辅助能力

- `BatteryStatsService`：电量、CPU、唤醒锁等统计（AMS 实现其 `BatteryCallback`）。
- `PendingIntentController`：统一管理 `PendingIntent` 的注册/发送/取消。
- `AppCompactor`、`LowMemDetector`、`OomAdjProfiler`、`ProcessStatsService`：内存压缩、低内存探测、进程状态统计。
- Instrumentation、权限控制器（`PermissionController`）、`IntentFirewall` 等。

## 生命周期与启动

AMS 作为 `SystemService` 由 `Lifecycle`（`ActivityManagerService.java:2209`）托管：`onStart()` 调 `start()` 发布 binder 服务；`onBootPhase()` 在不同启动阶段（如 `PHASE_SYSTEM_SERVICES_READY`、`PHASE_ACTIVITY_MANAGER_READY`）逐级唤醒 `mBatteryStatsService`、`mServices`、`mBroadcastDispatcher` 等子系统（`ActivityManagerService.java:2230-2239`）。它还通过 `LocalService`（继承 `ActivityManagerInternal`，`ActivityManagerService.java:17839`）向 framework 其它模块暴露内部能力。

## 一句话总结

AMS 是 Android 的“进程与组件调度中枢”：它管进程的创建与回收、OOM 评分与内存策略、Service 与 Broadcast 与 ContentProvider 三大组件的调度、应用错误/ANR 处置、多用户隔离，并把 Activity 的管理在 Android 10 起委托给 `ActivityTaskManagerService`；其内部由 `ProcessList`、`OomAdjuster`、`ActiveServices`、`BroadcastQueue`/`BroadcastDispatcher`、`UserController`、`AppErrors`、`BatteryStatsService` 等子系统分工实现。
