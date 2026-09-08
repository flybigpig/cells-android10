# ActivityManagerService 方法分类速查（Android 10）

> 源文件：`frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java`（约 19000 行，844 KB）
> 工作区：`c:/D/android_project/cells-android10`（AOSP Android 10）
> 说明：本文所有方法**均取自本仓库源码逐条核实**，行号可直接跳转。

---

## 目录

0. [总体架构：AMS 已"空心化"](#〇总体架构ams-已空心化)
1. [启动与生命周期](#一启动与生命周期)
2. [Activity / Task](#二activity--task全部转发-atms)
3. [Service](#三service)
4. [Broadcast](#四broadcast)
5. [ContentProvider](#五contentprovider)
6. [进程管理](#六进程管理)
7. [权限](#七权限)
8. [错误 / ANR / Crash](#八错误--anr--crash)
9. [多用户](#九多用户)
10. [系统控制 / 调试](#十系统控制--调试)
11. [内存 / 统计 / 查询](#十一内存--统计--查询)
12. [Handler 消息常量](#十二handler-消息常量)
13. [内部类清单](#十三内部类清单)
14. [阅码建议](#十四阅码建议)

---

## 〇、总体架构：AMS 已"空心化"

Android 10 上 AMS 大量 public 方法只是**一行转调**，真正实现在拆出去的子系统里：

| 委托目标 | 持有字段 | 负责域 |
|---|---|---|
| `ActivityTaskManagerService` | `mActivityTaskManager:1476` / `mAtmInternal:1478` | Activity / Task / 栈 / 窗口 |
| `ActiveServices` | `mServices` | Service |
| `BroadcastQueue[]` | `mBroadcastQueues:580` | Broadcast |
| `ProcessList` | `mProcessList` | 进程创建 / 杀 / 内存 |
| `OomAdjuster` | `mOomAdjuster:551` | OOM adj |
| `UserController` | `mUserController` | 多用户 |
| `UriGrantsManagerInternal` | `mUgmInternal:1480` | URI 授权 |
| `AppErrors` | `mAppErrors` | crash / ANR 弹窗 |

典型形态（`:3572-3575`）：

```java
public int startActivity(IApplicationThread caller, String callingPackage, ...) {
    return mActivityTaskManager.startActivity(caller, callingPackage, intent, resolvedType, ...);
}
```

> **读 AMS 方法的第一原则：先看它是"真实现"还是"转发壳"。**

---

## 一、启动与生命周期

| 方法 / 类 | 行号 | 说明 |
|---|---|---|
| `Lifecycle extends SystemService` | 2209 | AMS 的 SystemService 包装 |
| `Lifecycle.startService(ssm, atm)` | 2218 | SystemServer 启动入口 |
| `Lifecycle.onStart()` | 2225 | → `mService.start()` |
| `Lifecycle.onBootPhase(int)` | 2230 | `PHASE_SYSTEM_SERVICES_READY` / `PHASE_ACTIVITY_MANAGER_READY` / `PHASE_THIRD_PARTY_APPS_CAN_START` |
| `Lifecycle.onCleanupUser(int)` | 2243 | 用户清理 |
| `Lifecycles` | 2256 | 变体（本仓库已带中文注释 2252-2255） |
| `ActivityManagerService(Injector)` | 2467 / 2476 | 测试注入构造 |
| `ActivityManagerService(Context, ActivityTaskManagerService)` | 2515 | 正式构造 |
| `setSystemProcess()` | 2040 | 把自己注册进 ServiceManager |
| `setWindowManager(WindowManagerService)` | 2093 | 与 WMS 双向绑定 |
| `setSystemServiceManager` / `setInstaller` | 2666 / 2670 | 依赖注入 |
| `initPowerManagement()` | 2697 | 电源联动 |
| `systemReady(Runnable, TimingsTraceLog)` | 9017 | 系统就绪，之后才允许三方 App |
| `installSystemProviders()` | 7618 | 安装 SettingsProvider |
| `startObservingNativeCrashes()` | 2115 | native crash 监听 |
| `onTransact` | 2789 | Binder 事务拦截（权限 / 隐藏 API） |

---

## 二、Activity / Task（全部转发 ATMS）

| 方法 | 行号 | 备注 |
|---|---|---|
| `startActivity` | 3572 | → `mActivityTaskManager` |
| `startActivityAsUser` | 3580 | → ATMS |
| `startActivityAndWait` | 3589 | 返回 `WaitResult` |
| `finishActivity` | 3625 | → ATMS |
| `removeTask` | 6360 | → ATMS |
| `moveTaskToFront` | 6365 | → ATMS |
| `getRecentTasks` | 6407 | → ATMS |
| `getIntentSender` | 5429 | 分流：`mAtmInternal` 或 `mPendingIntentController` |

> `activityIdle` / `activityResumed` / `activityPaused` 这类回调在 Q 上已落入 ATMS / `ActivityTaskManagerInternal`，AMS 侧无独立入口。

---

## 三、Service

| 方法 | 行号 |
|---|---|
| `startService` | 13970 |
| `stopService` | 14002 |
| `stopServiceToken` | 14033 |
| `setServiceForeground` | 14041 |
| `bindService` | 14110（内部转 `bindIsolatedService`） |
| `bindIsolatedService` | 14117 |
| `unbindService` | 14155 |
| `publishService` | 14161（服务回调 onBind 结果） |
| `unbindFinished` | 14175 |

实际调度在 `ActiveServices`（`mServices`），超时 20s / 200s 见 `SERVICE_TIMEOUT_MSG`。

---

## 四、Broadcast

| 方法 | 行号 |
|---|---|
| `registerReceiver` | 14471 |
| `unregisterReceiver` | 14646 |
| `broadcastIntent` | 15632 |
| `unbroadcastIntent` | 15681（撤粘性广播） |
| `finishReceiver` | 15729 |
| `skipCurrentReceiverLocked` | 9269（ANR 时跳过当前接收者） |
| `batterySendBroadcast` | 2968 |

队列分发在 `BroadcastQueue`；`scheduleBroadcastsLocked` 调用点见 14638 / 15426 / 15540。

---

## 五、ContentProvider

| 方法 | 行号 | 说明 |
|---|---|---|
| `getContentProvider` | 7305 | 必要时触发进程启动 |
| `getContentProviderExternal` | 7327 | 需 `ACCESS_CONTENT_PROVIDERS_EXTERNALLY` |
| `removeContentProvider` | 7346 | |
| `publishContentProviders` | 7418 | `attachApplication` 后发布 |
| `refContentProvider` | 7489 | 引用计数 |
| `unstableProviderDied` | 7530 | |
| `appNotRespondingViaProvider` | 7594 | 客户端主动上报 Provider ANR |
| `getProviderMimeType` | 7733 | |

---

## 六、进程管理

| 方法 | 行号 | 说明 |
|---|---|---|
| `attachApplication` | 5255 | 新进程 fork 后报到，绑 `ProcessRecord` |
| `startProcessLocked` | 3102 | → `mProcessList.startProcessLocked` |
| `startIsolatedProcess` | 3073 | 隔离进程 |
| `handleAppDiedLocked` | 3673 | 死亡清理 + 重启调度 |
| `cleanUpApplicationRecordLocked` | 13730 | 核心清理 |
| `killBackgroundProcesses` | 4210 | |
| `killAllBackgroundProcesses` | 4253 | |
| `forceStopPackage` | 4309 | |
| `killPackageProcessesLocked` | 4243 / 4726 / 8767（调用点） | 实现在 `ProcessList` |
| `killProcessesBelowAdj` | 8786 | 仅系统调用 |
| `setProcessLimit` | 5695 | |
| `getRunningAppProcesses` | 9770 | |
| `setProcessMemoryTrimLevel` | 3255 | |

---

## 七、权限

| 方法 | 行号 | 说明 |
|---|---|---|
| `checkComponentPermission` | 5927（static） | 底层判定 |
| `checkPermission` | 5946 | → `checkComponentPermission` |
| `enforceCallingPermission` | 5986 | 不通过抛 SecurityException |
| `checkUriPermission` | 6202 | → `mUgmInternal` |
| `grantUriPermission` | 6227 | → `mUgmInternal` |
| `revokeUriPermission` | 6260 | → `mUgmInternal` |
| `PermissionController` | 5865 | `IPermissionController.Stub` |
| `ShellDelegate` | 18968 | adb shell 权限代理 |

---

## 八、错误 / ANR / Crash

| 方法 | 行号 | 说明 |
|---|---|---|
| `handleApplicationCrash` | 9281 | Java crash 上报 |
| `handleApplicationStrictModeViolation` | 9337 | |
| `handleApplicationWtf` | 9460 | |
| `crashApplication` | 3650 | 主动杀指定 App |
| `addErrorToDropBox` | 9596 | 落 dropbox |
| `inputDispatchingTimedOut` | 18588（int pid）/ 18612（ProcessRecord） | **输入 ANR 入口** |
| `killAppAtUsersRequest` | 9263 | 用户点"关闭应用" |
| `dumpStackTraces` | 3860 / 3995 | trace 收集 |
| `createAnrDumpFile` / `maybePruneOldTraces` | 3914 / 3943 | `/data/anr` 文件管理 |

> **注意**：Q 上**没有** `handleApplicationANR`（全仓 0 命中），也没有 `appNotResponding` —— 后者实现在 `ProcessRecord.java:1407`。

---

## 九、多用户

| 方法 | 行号 |
|---|---|
| `startUserInBackground` | 17638 |
| `unlockUser` | 17656 |
| `switchUser` | 17661 |
| `stopUser` | 17666 |
| `getCurrentUser` | 17671 |
| `restartUserInBackground` | 18778 |

全部委托 `mUserController`。

---

## 十、系统控制 / 调试

| 方法 | 行号 |
|---|---|
| `shutdown` | 7974 |
| `prepareForPossibleShutdown` | 18864 |
| `setDebugApp` | 8015 |
| `setAlwaysFinish` | 8147（"不保留活动"） |
| `setActivityController` | 8166（→ ATMS，Monkey / CTS 用） |
| `updateConfiguration` | 16049（→ ATMS） |
| `updatePersistentConfiguration` | 16017 |
| `isUserAMonkey` | 8190 |

---

## 十一、内存 / 统计 / 查询

| 方法 | 行号 |
|---|---|
| `getMemoryInfo` | 6309 |
| `getProcessMemoryInfo` | 4422 |
| `getMemoryTrimLevel` | 9839 |
| `getMyMemoryState` | 9818 |
| `getProcessesInErrorState` | 9723 |
| `updateCpuStatsNow` | 2849（ANR 时采样 CPU） |
| `MemBinder` / `GraphicsBinder` / `DbBinder` / `CpuBinder` | 2124 / 2153 / 2167 / 2181（`dumpsys meminfo` / `gfxinfo` / `dbinfo` / `cpuinfo` 入口） |

---

## 十二、Handler 消息常量

常量定义区：`1509-1539`；`MainHandler` 类在 `1668`；`UiHandler` 类在 `1580`。

| MSG | 值 | 用途 |
|---|---|---|
| `SHOW_ERROR_UI_MSG` | 1 | crash 弹窗 |
| `SHOW_NOT_RESPONDING_UI_MSG` | 2 | **ANR 弹窗** |
| `GC_BACKGROUND_PROCESSES_MSG` | 5 | |
| `WAIT_FOR_DEBUGGER_UI_MSG` | 6 | |
| `SERVICE_TIMEOUT_MSG` | 12 | **服务 ANR（20s / 200s）** |
| `PROC_START_TIMEOUT_MSG` | 20 | 进程启动超时 |
| `KILL_APPLICATION_MSG` | 22 | |
| `SHOW_STRICT_MODE_VIOLATION_UI_MSG` | 26 | |
| `CHECK_EXCESSIVE_POWER_USE_MSG` | 27 | |
| `CLEAR_DNS_CACHE_MSG` | 28 | |
| `UPDATE_HTTP_PROXY_MSG` | 29 | |
| `DISPATCH_PROCESSES_CHANGED_UI_MSG` | 31 | |
| `DISPATCH_PROCESS_DIED_UI_MSG` | 32 | |
| `REPORT_MEM_USAGE_MSG` | 33 | |
| `UPDATE_TIME_PREFERENCE_MSG` | 41 | |
| `NOTIFY_CLEARTEXT_NETWORK_MSG` | 49 | |
| `POST_DUMP_HEAP_NOTIFICATION_MSG` | 50 | |
| `DELETE_DUMPHEAP_MSG` | 51 | |
| `DISPATCH_UIDS_CHANGED_UI_MSG` | 53 | |
| `SHUTDOWN_UI_AUTOMATION_CONNECTION_MSG` | 56 | |
| `CONTENT_PROVIDER_PUBLISH_TIMEOUT_MSG` | 57 | **Provider 发布 10s** |
| `IDLE_UIDS_MSG` | 58 | |
| `HANDLE_TRUST_STORAGE_UPDATE_MSG` | 63 | |
| `SERVICE_FOREGROUND_TIMEOUT_MSG` | 66 | startForegroundService 未调 startForeground |
| `PUSH_TEMP_WHITELIST_UI_MSG` | 68 | |
| `SERVICE_FOREGROUND_CRASH_MSG` | 69 | |
| `DISPATCH_OOM_ADJ_OBSERVER_MSG` | 70 | |
| `KILL_APP_ZYGOTE_MSG` | 71 | |
| `FIRST_BROADCAST_QUEUE_MSG` | 200 | 广播队列消息起点 |
| `COLLECT_PSS_BG_MSG` | 1902 | PSS 统计线程 |
| `DEFER_PSS_MSG` | 1903 | |
| `STOP_DEFERRING_PSS_MSG` | 1904 | |

**线程模型**：

- `MainHandler`（1668）跑在 `mHandlerThread`（1552，`ServiceThread`）——承载上表绝大部分消息。
- `UiHandler`（1580）跑在 system_server **主线程**——专用于弹 crash / ANR 对话框。

---

## 十三、内部类清单

| 内部类 | 行号 | 说明 |
|---|---|---|
| `PackageAssociationInfo` | 694 | |
| `PidMap` | 734 | |
| `ImportanceToken` | 815 | |
| `Association` | 1011 | |
| `DevelopmentSettingsObserver` | 1070 | |
| `Identity` | 1106 | |
| `PendingTempWhitelist` | 1151 | |
| `ProfileData` | 1290 | |
| `ProcessChangeItem` | 1350 | |
| `UidObserverRegistration` | 1361 | |
| `AppDeathRecipient` | 1483 | |
| `UiHandler` | 1580 | system_server 主线程 Handler |
| `MainHandler` | 1668 | 服务线程 Handler |
| `MemBinder` | 2124 | `dumpsys meminfo` |
| `GraphicsBinder` | 2153 | `dumpsys gfxinfo` |
| `DbBinder` | 2167 | |
| `CpuBinder` | 2181 | `dumpsys cpuinfo` |
| `Lifecycle` | 2209 | SystemService 包装 |
| `Lifecycles` | 2256 | SystemService 变体 |
| `HiddenApiSettings` | 2336 | |
| `ProcessInfoService` | 5806 | |
| `PermissionController` | 5865 | `IPermissionController.Stub` |
| `IntentFirewallInterface` | 5913 | |
| `StartActivityRunnable` | 7235 | |
| `ItemMatcher` | 11395 | |
| `MemItem` | 12107 | |
| `MemoryUsageDumpOptions` | 12332 | |
| `RecordPssRunnable` | 16189 | |
| `ProcStatsRunnable` | 16833 | |
| **`LocalService`** | 17839 | `ActivityManagerInternal` 实现，system_server 内部 API |
| `Injector` | 18873 | 测试注入 |
| `ShellDelegate` | 18968 | adb shell 权限代理 |

---

## 十四、阅码建议

1. **先判"转发壳"**：Activity / Task 类方法在 Q 上基本都转 ATMS，想看算法直接去 `wm/` 包。
2. **ANR 链路**：`inputDispatchingTimedOut(18588/18612)` → `ProcessRecord.appNotResponding(1407)` → `dumpStackTraces(3860)` → `SHOW_NOT_RESPONDING_UI_MSG` → `AppErrors.handleShowAnrUi(850)`。
3. **超时都在 MSG 常量里**：Service 20s / 200s、Provider 10s、广播 10s / 60s，全部由 `MainHandler` 的延迟消息驱动，而非独立定时器线程。
4. **进程生死**：`attachApplication(5255)` ↔ `handleAppDiedLocked(3673)` 是一对，中间 `cleanUpApplicationRecordLocked(13730)` 是清理主体。
5. **AMS 与 ATMS 的边界**：凡涉 Activity / Task / 栈 / 窗口，一律在 ATMS；AMS 只保留进程、服务、广播、Provider、权限、多用户。

---

*基于 AOSP Android 10 源码整理 · 2026-09-08*
