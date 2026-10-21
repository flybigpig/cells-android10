# ActivityManagerService 代码分析（Android 10 / cells-android10）

> 源文件：`frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java`（约 845KB，近 19000 行）
> 本文只分析 `ActivityManagerService` 本体；进程优先级 / lmkd 淘汰 / trimming / ANR 的细节见 `doc/AMS_lmkd_流程分析_Android10.md`。

---

## 一、总览：它到底"管"什么

AMS 是 system_server 里最核心的服务之一，形式上是一个 Binder 服务（`ActivityManagerNative` / 内部 `LocalService` 双面），实质上是一个**持有大量协作子系统的主控类**。Android 10 的 AMS 已经不是"全知全能的上帝类"，而是把各类职责下沉到同包的合作类里，自己只保留"入口 + 状态 + 锁 + 调度"。

它的四个真实身份：

1. **Binder 门面**：`startActivity` / `startService` / `sendBroadcast` / `getContentProvider` / `attachApplication` 等对外入口。
2. **进程状态中心**：谁在跑、什么优先级、何时该杀（委托 `ProcessList` + `OomAdjutor`）。
3. **主锁持有者**：`synchronized (this)` 即 AMS 自身，是整条 am 包的事实临界区。
4. **消息分发中枢**：`MainHandler` 处理 ANR 对话框、Service 超时、进程启动超时、后台 GC 等异步事件。

---

## 二、类结构与核心子系统

AMS 把职责分派给下列协作对象（均在 am 包内）：

| 字段 | 行号 | 职责 |
|---|---|---|
| `ProcessList mProcessList` | :663 | 进程记录表 `mLruProcesses`、zygote 交互、`writeLmkd` 下发 oom_adj |
| `ActiveServices mServices` | :1009 | Service 生命周期、超时、前台服务 |
| `ProviderMap mProviderMap` | :1055 | ContentProvider 的发布与跨进程获取 |
| `mFgBroadcastQueue` / `mBgBroadcastQueue` / `mOffloadBroadcastQueue` | :575-580 | 前台/后台/offload 三条广播队列 |
| `UserController mUserController` | :606 | 多用户切换与隔离 |
| `AppErrors mAppErrors` / `PackageWatchdog mPackageWatchdog` | :610-611 | 崩溃、ANR、卡死进程判定 |
| `ProcessCpuTracker mProcessCpuTracker` | :1457 | CPU 负载与进程统计 |
| `mProcessObservers` | :1425 | 进程生死回调观察者 |
| `MainHandler mHandler` / `mHandlerThread` | :1553 / :1552 | 异步消息处理线程 |
| `ActivityManagerConstants mConstants` | :1558 | 可调参数（后台进程数、GC 间隔等） |

注意 Activity / 窗口侧不在 AMS：Android 10 起 Activity 栈归 `ActivityTaskManagerService`（wm 包），AMS 通过 `mAtmInternal`（`ActivityTaskManagerInternal`）调用它，例如 `mAtmInternal.preBindApplication(...)`、`mAtmInternal.attachApplication(...)`（见第五节）。

### MainHandler 消息表（:1509-1539）

AMS 主循环的事件源就是这张表，节选关键项：

```1509:1539:frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
static final int SHOW_ERROR_UI_MSG = 1;
static final int SHOW_NOT_RESPONDING_UI_MSG = 2;
static final int GC_BACKGROUND_PROCESSES_MSG = 5;
static final int SERVICE_TIMEOUT_MSG = 12;
static final int PROC_START_TIMEOUT_MSG = 20;
static final int KILL_APPLICATION_MSG = 22;
static final int SERVICE_FOREGROUND_TIMEOUT_MSG = 66;
static final int DISPATCH_OOM_ADJ_OBSERVER_MSG = 70;
static final int KILL_APP_ZYGOTE_MSG = 71;
static final int FIRST_BROADCAST_QUEUE_MSG = 200;   // 之后 200+ 归广播队列
```

---

## 三、生命周期与启动流程

### 3.1 构造

AMS 有两个构造函数：测试注入版（`:2478` 起，各子系统置 null）与真实版（`:2526` 起）。真实版顺序：建 `ServiceThread`（`THREAD_PRIORITY_FOREGROUND`）→ `MainHandler` → `mUiHandler` → `ActivityManagerConstants` → `ActiveUids` → 三条 `BroadcastQueue` → `ActiveServices` / `ProviderMap` / `UserController` / `AppErrors` / `PendingIntentController`；最后起 `mProcessCpuThread` 初始化 `ProcessCpuTracker`，并用 latch 阻塞到初始化完成（`:2613-2688`）。

### 3.2 systemReady（:9017）

分两阶段。第一阶段持锁：取 `DeviceIdleController.LocalService` → `mActivityTaskManager.onSystemReady()` → `mUserController.onSystemReady()` → `mAppOpsService.systemReady()` → `mSystemReady = true`。第二阶段锁外：清理 `isAllowedWhileBooting` 之外的系统更新进程 → `mProcessesReady = true` → 写 `BOOT_PROGRESS_AMS_READY` 事件 → 注册 `mActivityLaunchObserver`、启动各类 observer（`startBroadcastObservers`，:9240）→ 回调 `goingCallback.run()` 推进后续 boot phase。这就是"AMS ready 之后才允许启动真实应用进程"的分界点。

### 3.3 其它启动钩子

- `installSystemProviders()`（:7618）：安装系统 Provider（先于应用进程）。
- `installEncryptionUnawareProviders()`（:7676）：加密未感知的 Provider。
- `finishBooting()`（:5273）：收尾启动。

---

## 四、应用进程附着：attachApplication 全链路

这是 AMS 与每个应用进程握手的关键路径，也是理解"进程创建后组件如何被拉起"的入口。

`attachApplication(IApplicationThread, startSeq)`（:5255，Binder 入口）→ `attachApplicationLocked`（:4842，持 `this` 锁）：

1. 用 `startSeq` 从 pending 队列反查 `ProcessRecord`（处理"进程比 AMS 先到"的竞态，:4879-4881）。
2. `mAtmInternal.preBindApplication(...)`（:5119）→ `bindApplication(...)` → `app.makeActive(thread, mProcessStats)`（:5154）→ `mProcessList.updateLruProcessLocked(app, ...)`（:5156）把进程挂入 LRU。
3. `mAtmInternal.attachApplication(...)`（:5183）拉起该进程的 Activity。
4. `mServices.attachApplicationLocked(...)`（:5193）拉起 Service。
5. `sendPendingBroadcastsLocked(...)`（:5204）补投挂起的广播。
6. 若三者都没做事：`updateOomAdjLocked(OOM_ADJ_REASON_PROCESS_BEGIN)`（:5236）立即重算优先级。

配套的进程创建侧是 `ProcessList.startProcess` + `addAppLocked`（:7845/:7854）创建 `ProcessRecord`，再经 zygote fork 出进程，进程起来后回打 `attachApplication`。

---

## 五、职责域分布（方法按域归类）

1. **进程管理**：`addAppLocked`（:7845）、`attachApplicationLocked`（:4842）、`handleAppDiedLocked`、`trimApplicationsLocked`（:17320）、`killBackgroundProcesses`（:4210）、`killAllBackgroundProcesses`（:4253）、`updateOomAdjLocked`（贯穿全文件，委托 `OomAdjutor`，结果经 `ProcessList.setOomAdj` 下发 lmkd）。
2. **Service**：全部委托 `ActiveServices`（`mServices`，:1009）。
3. **Broadcast**：委托三条 `BroadcastQueue`（:575-580），AMS 侧负责 `broadcastQueueForIntent`（:15414）分流、锁内转发与 pending 管理（:14449-14466）。
4. **Provider**：委托 `ProviderMap`（:1055）。
5. **Activity / 窗口**：委托 `ActivityTaskManagerInternal`（`mAtmInternal`）。
6. **多用户**：委托 `UserController`（:606）。
7. **崩溃 / ANR / 卡死**：委托 `AppErrors`（:610）与 `ProcessRecord.appNotResponding`（详见 lmkd 篇）。
8. **统计**：`mProcessCpuTracker`（:1457）+ `updateCpuStatsNow`（:2849）+ `mProcessObservers`（:1425）。
9. **dump / proto**：`dump()`（约 :11483 起）分 activity/service/broadcast/provider/processes 五大块，对应 `ActivityManagerServiceDump*Proto`。

---

## 六、锁与并发模型

- **主锁 `this`**：AMS 实例自身即锁，`@GuardedBy("this")` 遍布全类。所有状态变更（进程列表、广播队列、Service 记录、oom_adj 重算）都在此临界区内。
- **`mPidsSelfLocked`**：保护 pid→ProcessRecord 映射，独立于主锁，便于无锁快速查找。
- **`mProcessCpuTracker` 独立锁**：CPU 统计在独立线程（`mProcessCpuThread`）初始化与更新，避免阻塞主锁。
- **`OomAdjutor.adjusterThread`**：adj 计算重、且 `setProcessGroup` 较重，AMS 把它放到独立线程，并通过 `mProcessGroupHandler` 异步执行（详见 lmkd 篇）。
- **`MainHandler` / `mUiHandler`**：主线程负责 ANR 对话框、Service 超时等 UI 或定时语义事件。

这个设计的代价是：AMS 主锁是 system_server 里最热的锁之一，任何耗时操作都不能放在持锁路径上——这也是 `attachApplicationLocked` 里大量 `checkTime(...)` 埋点的原因（:4961-5206）。

---

## 七、方法索引表（核心项）

| 方法 | 行号 | 说明 |
|---|---|---|
| `ActivityManagerService(...)` | :2478 / :2526 | 测试注入版 / 真实版构造 |
| `MainHandler` 消息常量 | :1509-1539 | 异步事件定义 |
| `systemReady` | :9017 | 系统就绪，允许启动真实进程 |
| `startBroadcastObservers` | :9240 | 启动广播 observer |
| `installSystemProviders` | :7618 | 系统 Provider 安装 |
| `finishBooting` | :5273 | 启动收尾 |
| `attachApplication` | :5255 | 应用进程附着 Binder 入口 |
| `attachApplicationLocked` | :4842 | 附着核心：bind → activity/service/broadcast → adj |
| `addAppLocked` | :7845 / :7854 | 创建 ProcessRecord |
| `trimApplicationsLocked` | :17320 | 空进程回收 + 重算 adj |
| `killBackgroundProcesses` | :4210 | 按包杀后台 |
| `killAllBackgroundProcesses` | :4253 | 批量杀后台 |
| `doLowMemReportIfNeededLocked` | :3726 | 低内存上报 |
| `performAppGcsLocked` | :16449 | 空闲 GC |
| `appNotResponding`（ProcessRecord） | ProcessRecord.java:1407 | ANR 处理 |
| `setOomAdj`（ProcessList） | ProcessList.java:1176 | 下发 oom_adj 到 lmkd |
| `dump` | 约 :11483 | dumpsys 输出 |

---

## 八、小结

读 AMS 的关键是"**认门面、追委托、守主锁**"：AMS 本体只做入口校验、状态登记、持锁编排，真正干活的都在 `ProcessList` / `OomAdjutor` / `ActiveServices` / `ProviderMap` / `BroadcastQueue` / `UserController` / `ActivityTaskManagerInternal` 这些合作类里。抓住 `attachApplicationLocked`（应用进来）与 `systemReady`（系统起来）两条主链路，再配合 `mAtmInternal` / `mServices` / 广播队列的委托边界，就能把这张近 19000 行的网拆开。本构建的 cells 多容器改造主要落在输入分发与图层归属（见 SurfaceFlinger 篇），AMS 侧的 freeze 能力已裁剪，只剩 PMS 的 `PackageFreezer`。

---

## 参考源码

- `frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java`
- 同包 `ProcessList.java` / `OomAdjutor.java` / `ProcessRecord.java` / `AppErrors.java` / `ActiveServices.java` / `ProviderMap.java` / `BroadcastQueue.java` / `UserController.java`
