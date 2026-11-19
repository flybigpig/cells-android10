# AMS → lmkd 整套流程代码分析（Android 10 / cells-android10）

> 涉及源码：
> - Java：`frameworks/base/services/core/java/com/android/server/am/` 下 `ActivityManagerService.java`、`OomAdjutor.java`、`ProcessList.java`、`ProcessRecord.java`、`AppErrors.java`
> - Native：`system/core/lmkd/lmkd.c`、`system/core/lmkd/include/lmkd.h`
> - Kernel：`kernel/drivers/staging/android/lowmemorykiller.c`

---

## 一、总览：三条相关但独立的线

"应用卡死 / 后台清理 / 低内存杀进程"在 AMS 里其实由三条线协作完成，理解它们的边界是看懂整套流程的关键：

1. **OOM 优先级计算 + 下发（AMS 决策）**：AMS 算出每个进程该活的"优先级"（oom_adj / procstate / sched group），通过 socket 下发给 lmkd，并用 `LMK_TARGET` 下发内存水位阈值。AMS 只"排座次、画线"，不亲自杀。
2. **lmkd 实际淘汰（Native 执行）**：`lmkd` 守护进程基于 PSI / vmpressure 感知内存压力，按 oom_score_adj 从大到小挑最不重要的进程 `SIGKILL`。内核态 `lowmemorykiller` 作为 shrinker 兜底保留。
3. **ANR（应用卡死，独立超时线）**：由 InputDispatcher / Service / Broadcast 的超时检测驱动，与内存无关，写 `/data/anr/traces.txt` 并决定杀进程或弹窗。

`trimming` 是 AMS 在 1 之外的"主动清理"补充：清掉空进程、让后台进程 GC、显式杀后台。

```
[组件状态变化 / activity切换 / 进程死亡 / 低内存]
   → AMS.updateOomAdjLocked(reason)
       → OomAdjutor.computeOomAdjLocked  (算 adj/procstate/schedGroup)
       → OomAdjutor.applyOomAdjLocked     (落地)
           → ProcessList.setOomAdj → writeLmkd(LMK_PROCPRIO) → lmkd socket
       → ProcessList.updateOomMinFree → writeLmkd(LMK_TARGET) (水位表)
[系统内存跌破阈值]
   → lmkd 收 PSI/vmpressure 事件 → mp_event_common
       → find_and_kill_process → kill(pid, SIGKILL)
   （兜底）内核 lowmemorykiller shrinker → send_sig(SIGKILL)
```

---

## 二、Java 层：优先级计算（谁该活）

所有触发点最终都汇聚到 `updateOomAdjLocked(reason)`，`reason` 是 `OomAdjuster.OOM_ADJ_REASON_*` 常量（activity 切换、service 启动、广播、进程死亡、trim 等）。

实际计算在 `OomAdjutor.computeOomAdjLocked`（约 `OomAdjutor.java:849` 起）：按"重要度从高到低"遍历每个 `ProcessRecord`，算出三件套 `adj`（oom_adj 数值）、`procState`、`schedGroup`，并打 `cached`/`empty` 标志。重要度分层（节选自计算顺序）：

- `TOP_APP` → `FOREGROUND_APP_ADJ = 0`，`SCHED_GROUP_TOP_APP`（`:858-864`）
- `VISIBLE_APP_ADJ`、`PERCEPTIBLE`、`FOREGROUND_SERVICE`、`BOUND_FG_SERVICE`
- `BACKGROUND_APP_ADJ`
- `CACHED_APP_MIN_ADJ = 900` ~ `CACHED_APP_MAX_ADJ = 999`（`ProcessList.java:145-146`），LRU 越靠后越接近 999

随后 `applyOomAdjLocked`（`OomAdjutor.java:1676`，注释 "Applies the computed oomadj... and freezes them in set*"）把算好的值写下去。当 `curAdj != setAdj` 时真正下发：

```1719:1721:frameworks/base/services/core/java/com/android/server/am/OomAdjutor.java
if (app.curAdj != app.setAdj) {
    ProcessList.setOomAdj(app.pid, app.uid, app.curAdj);
    if (DEBUG_SWITCH || DEBUG_OOM_ADJ || mService.mCurOomAdjUid == app.info.uid) {
```

同一函数里还设置调度组与线程优先级：`setThreadPriority(app.pid, TOP_APP_PRIORITY_BOOST)`（`:1785` 前台 UI/render 线程提优先级）、`setThreadScheduler`/`setThreadPriority` 复位（`:1802-1818`），以及经 `mProcessGroupHandler` 把进程放进对应 cpuset 线程组（前台 `THREAD_GROUP_TOP_APP`、后台 `THREAD_GROUP_RESTRICTED`，`OomAdjutor.java:180-186`）。注意，若进程被判入后台且带有 `waitingToKill`，会在此直接 `app.kill(app.waitingToKill, true)`（`:1740-1742`）。

---

## 三、Java 层：下发到 lmkd（writeLmkd）

`ProcessList.setOomAdj`（`ProcessList.java:1176`）向 lmkd socket 发 `LMK_PROCPRIO <pid> <uid> <prio>`，并写 `/proc/<pid>/oom_score_adj`：

```1176:1191:frameworks/base/services/core/java/com/android/server/am/ProcessList.java
public static void setOomAdj(int pid, int uid, int amt) {
    // This indicates that the process is not started yet and so no need to proceed further.
    if (pid <= 0) {
        return;
    }
    ...
    buf.putInt(pid);
    buf.putInt(uid);
    buf.putInt(amt);
    writeLmkd(buf, null);
```

命令码与 `lmkd.c` 对齐（`ProcessList.java:265-267`）：

```265:268:frameworks/base/services/core/java/com/android/server/am/ProcessList.java
static final byte LMK_TARGET = 0;
static final byte LMK_PROCPRIO = 1;
static final byte LMK_PROCREMOVE = 2;
```

- **水位表下发**：`updateOomMinFree` 发 `LMK_TARGET <minfree> <minkillprio> ...`（最多 6 对，`ProcessList.java:672-682`），`minfree` 单位为页、按设备总内存缩放（`mOomMinFree`）。
- **进程注销**：进程死亡时 `LMK_PROCREMOVE`（`ProcessList.java:1206`）；socket 重连时 `LMK_PROCPURGE` 清掉旧 pid（`ProcessList.java:1296`）。
- **传输**：`writeLmkd`（`ProcessList.java:1282`）带 3 次重试，写失败会 purge 后重连。

---

## 四、Native 层：lmkd（真正执行 SIGKILL）

### 4.1 socket 与命令解析主循环

`init()` 阶段 `epoll_create` 建 epoll（`lmkd.c:1936`），经 `android_get_control_socket("lmkd")` 拿到 init 预创建的 "lmkd" socket 并 `listen`（`lmkd.c:1947-1953`），监听 socket 回调用 `ctrl_connect_handler` 注册进 epoll（`lmkd.c:1960-1962`）。AMS `connect` 时 `accept` 并把数据 socket 回调设为 `ctrl_data_handler`（`lmkd.c:1019-1031`）。主循环 `mainloop()` 经 `epoll_wait` 等待（`lmkd.c:2002`/`2023`），事件到来调 `handler_info->handler`（`lmkd.c:2068`）；数据可读时 `ctrl_data_handler` → `ctrl_command_handler`（`lmkd.c:986-988`）→ 按命令码 `switch` 分发：

```940:972:system/core/lmkd/lmkd.c
switch (cmd) {
    case LMK_TARGET:      cmd_target(...);      break;  // :950
    case LMK_PROCPRIO:    cmd_procprio(...);    break;  // :955
    case LMK_PROCREMOVE:  cmd_procremove(...);  break;  // :960
    case LMK_PROCPURGE:   cmd_procpurge(...);   break;  // :965
    case LMK_GETKILLCNT:  cmd_getkillcnt(...);  break;  // :970-972
}
```

### 4.2 每进程 oomadj 存储

进程用 `struct proc` 表示（`lmkd.c:324`，含 `pid/uid/oomadj` 与 `pidhash_next`），按 pid 哈希入 `pidhash[1024]`（`lmkd.c:343-344`），同时按 `oom_score_adj` 经 `ADJTOSLOT` 分桶挂入 `procadjslot_list[]` 双向链表（`lmkd.c:346-348`）。`cmd_procprio` 先校验范围、确认是线程组 leader、写 `/proc/<pid>/oom_score_adj`（`lmkd.c:610-634`），在 per-app memcg 模式还设 `memory.soft_limit_in_bytes`（`lmkd.c:640-682`），再 `pid_lookup` 查重后 `proc_insert`（入哈希 + 分桶，`lmkd.c:491-696`），存在则更新 `oomadj` 并重新分桶（`lmkd.c:697-698`）。删除走 `pid_remove`（`lmkd.c:499`）。

### 4.3 内存压力检测

优先用 **PSI**：`init_psi_monitors`（`lmkd.c:1843`）→ `init_mp_psi`（`:1811`），阈值 `psi_thresholds`（`:163-167`）经 `init_psi_monitor` 读 `/proc/pressure/memory` 再 `register_psi_monitor` 注入 epoll（`:1822`）。若内核无 PSI 则回退 **vmpressure**：`init_mp_common`（`:1859`）打开 `memory.pressure_level` 与 `cgroup.event_control`（`:1870`/`1876`），写 eventfd 并 `epoll_ctl`（`:1906`）。两类回调均指向 `mp_event_common`（`lmkd.c:1593`），即回收入口。

### 4.4 淘汰决策与 SIGKILL

`mp_event_common` 算出 `min_score_adj` 后调 `find_and_kill_process`（`lmkd.c:1475`），自 `OOM_SCORE_ADJ_MAX` 向下遍历到 `min_score_adj`（`:1483`），每层取受害者：开 `kill_heaviest_task` 用 `proc_get_heaviest`（按 RSS 最大），否则用 `proc_adj_lru`（链表尾最早，`lmkd.c:1320-1324`），再 `kill_one_process`（`:1493`）。最终：

```1431:1431:system/core/lmkd/lmkd.c
    kill(pid, SIGKILL);
```

### 4.5 LMK_TARGET 阈值映射

`cmd_target`（`lmkd.c:804`）把 AMS 传来的 `minfree`（页）与 `oom_adj_score` 写入 `lowmem_minfree[]` / `lowmem_adj[]`（`:835-836`）。启用 `use_minfree_levels` 时，`mp_event_common` 从 `i=0` 起逐个比较 `other_free` 与 `other_file` 是否同时低于 `lowmem_minfree[i]`，取对应 `lowmem_adj[i]` 为 `min_score_adj` 并 break（`lmkd.c:1675-1694`）。PSI 路径则用 `level_oomadj[]`（由 `ro.lmk.low/medium/critical` 设置，`lmkd.c:2091-2096`）直接按压力等级映射到 oomadj（`:1749`/`1774`）。

> 经全目录检索 `lmkd.c` / `lmkd.h` 均**无 "cells/cell" 相关注释或逻辑**，本文件为标准 AOSP Android 10 lmkd 实现，无 cells 项目定制。

---

## 五、Kernel 层：lowmemorykiller（兜底）

本驱动挂到内存回收的 **shrinker** 路径（非 vmpressure）：定义 `struct shrinker lowmem_shrinker`（`.scan_objects=lowmem_scan`、`.count_objects=lowmem_count`，`lowmemorykiller.c:342-346`），`lowmem_init()` 在 `:348` 调 `register_shrinker`（`:350`），由 `device_initcall`（`lowmemorykiller.c:354`）启动阶段注册。扫描回调 `lowmem_scan` 入口 `:222`。

参数源自 `/sys/module/lowmemorykiller/parameters/`：`lowmem_minfree[6]`（`:62`）、`lowmem_adj[6]`（`:54`），经 `module_param_array_named`（`:444`/`:446`）暴露。每个 task 的 oom_score_adj 通过 `p->signal->oom_score_adj` 读取（`:282`）。

受害者选择：`lowmem_scan` 先据水位定 `min_score_adj`（初始 `OOM_SCORE_ADJ_MAX+1`，`:229`），遍历 `lowmem_minfree/lowmem_adj`（`:242-247`），当 `other_free < minfree && other_file < minfree` 时取 `lowmem_adj[i]`；未触发即 `return 0`（`:256`）。随后 `rcu_read_lock()`（`:264`）下 `for_each_process`（`:265`）：跳过 `oom_score_adj < min_score_adj` 的进程（`:283`）；挑选 **oom_score_adj 最高（最不重要）** 者（`:292-300`）。发送信号：

```309:314:kernel/drivers/staging/android/lowmemorykiller.c
    task_lock(selected);
    send_sig(SIGKILL, selected, 0);
    task_set_lmk_waiting(selected);
    task_unlock(selected);
    trace_lowmemory_kill(...);   // lowmemorykiller.h 中 TRACE_EVENT(lowmemory_kill)
```

**与 userspace lmkd 的关系**：`Kconfig` 中 `ANDROID_LOW_MEMORY_KILLER`（`Kconfig:17`）默认开启，`ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES`（`Kconfig:27`，`default y`）。源码本身无 "deprecated/fallback/disabled" 注释，仍经 shrinker 参与回收，并可经 `/proc/lowmemorykiller` 向用户态上报 kill 事件。实际回收在 Android 10 多由**用户态 lmkd（PSI/vmpressure）**完成，本内核驱动作为 **kernel-side 兜底**保留。

---

## 六、Trimming：AMS 主动清理（不依赖内存水位）

### 6.1 空进程回收

`trimApplications(String reason)` → `trimApplicationsLocked`（`ActivityManagerService.java:17320`）遍历 `mRemovedProcesses`（包卸载/替换后、无 activity/receiver/service 的进程）直接 `app.kill("empty", false)` 或 `thread.scheduleExit()`，再 `cleanUpApplicationRecordLocked`，末尾 `updateOomAdjLocked(reason)` 重算全表（`:17352`）：

```17323:17341:frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
for (int i = mProcessList.mRemovedProcesses.size() - 1; i >= 0; i--) {
    final ProcessRecord app = mProcessList.mRemovedProcesses.get(i);
    if (!app.hasActivitiesOrRecentTasks()
            && app.curReceivers.isEmpty() && app.services.size() == 0) {
        ...
        if (app.pid > 0 && app.pid != MY_PID) {
            app.kill("empty", false);
        } else if (app.thread != null) {
            app.thread.scheduleExit();
        }
        cleanUpApplicationRecordLocked(app, false, true, -1, false);
```

### 6.2 空闲 GC（回收内存不杀进程）

`performAppGcsIfAppropriateLocked` → `performAppGcsLocked`（`ActivityManagerService.java:16449`）→ `performAppGcLocked`（`:16417`，调 `app.thread.scheduleGc`），由 `GC_BACKGROUND_PROCESSES_MSG`（`:1677`）驱动，系统空闲时逐个让后台进程 GC（`scheduleAppGcsLocked`，`:16493`）。

### 6.3 显式杀后台

`killBackgroundProcesses`（`:4210`，按包名）、`killAllBackgroundProcesses`（`:4253`，批量，以 `CACHED_APP_MIN_ADJ` 为界）、`killAllBackgroundProcessesExcept`（`:4288`）。结束时调 `doLowMemReportIfNeededLocked`（`:3726`）汇报低内存并可能触发一轮 GC。

### 6.4 进程死亡连锁

`handleAppDiedLocked` → `trimApplications`（`ActivityManagerService.java:18092`，reason = `OOM_ADJ_REASON_ACTIVITY`）在应用死后回收 LRU。

---

## 七、ANR 线（应用卡死，独立于 lmkd）

ANR 是"主线程超时无响应"，与内存无关。检测源：InputDispatcher 输入事件 5s 无响应（经 `InputManagerService` 回调 `notifyANR`）；前台 Service 20s / 后台 200s（`ActiveServices`）；前台 Broadcast 10s / 后台 60s（`BroadcastQueue`）。它们直接调 `ProcessRecord.appNotResponding(...)`（`ProcessRecord.java:1407`）：

```1407:1410:frameworks/base/services/core/java/com/android/server/am/ProcessRecord.java
void appNotResponding(String activityShortComponentName, ApplicationInfo aInfo,
        String parentShortComponentName, WindowProcessController parentProcess,
        boolean aboveSystem, String annotation) {
```

核心逻辑：
- `isSilentAnr()`（`:1391`，后台且非"感兴趣"进程时为真）→ 后台 ANR **静默 kill** 不弹窗：`kill("bg anr", true)`（`:1581`）。
- 前台 ANR：`makeAppNotRespondingLocked` 置状态（`:1586`），并 `ActivityManagerService.dumpStackTraces(firstPids, ...)`（`:1525`）收集各进程栈写入 `/data/anr/traces.txt`；后台 ANR 不 dump 其他 pid、不收集 CPU 统计（`:1450-1527`）。
- UI：`AppErrors.handleShowAnrUi`（`AppErrors.java:850`）构造 `AppNotRespondingDialog`；若不允许弹窗（`canShowErrorDialogs()` 为假）则 `killAppAtUsersRequest(proc, null)`（`:879`）。

---

## 八、三条线关系小结

| 线 | 触发 | 决策方 | 执行方 | 是否杀进程 |
|---|---|---|---|---|
| OOM 优先级（oom_adj） | 组件/状态变化 | AMS `OomAdjutor` | 写入 `/proc/.../oom_score_adj` + 通知 lmkd | 否（只排座次） |
| lmkd 淘汰 | 内存跌破 `LMK_TARGET` 水位 | lmkd（PSI/vmpressure） | `kill(pid, SIGKILL)` | 是 |
| 内核 lowmemorykiller | shrinker 内存回收 | 内核驱动 | `send_sig(SIGKILL)` | 是（兜底） |
| trimming | 空进程/空闲/显式请求 | AMS | `app.kill` / `scheduleExit` / GC | 部分 |
| ANR | 输入/Service/Broadcast 超时 | AMS `ProcessRecord.appNotResponding` | `kill("anr")` 或弹窗 | 是（后台静默） |

一句话：**AMS 算优先级并"画线 + 排座次"，lmkd（及内核兜底）按内存水位"执行枪决"，trimming 是 AMS 在内存还没崩时就主动腾位，ANR 则是另一条按超时判定应用卡死的独立线**。本构建中 AMS 的进程"冻结（freeze）"能力已被裁剪，相关语义只剩 PMS 的 `PackageFreezer`（见另文）。

---

## 参考源码

- `frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java`
- `frameworks/base/services/core/java/com/android/server/am/OomAdjutor.java`
- `frameworks/base/services/core/java/com/android/server/am/ProcessList.java`
- `frameworks/base/services/core/java/com/android/server/am/ProcessRecord.java`
- `frameworks/base/services/core/java/com/android/server/am/AppErrors.java`
- `system/core/lmkd/lmkd.c`、`system/core/lmkd/include/lmkd.h`
- `kernel/drivers/staging/android/lowmemorykiller.c`、`kernel/drivers/staging/android/trace/lowmemorykiller.h`
