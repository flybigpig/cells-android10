# AOSP 设计特性总览（Transaction 模式 + Zygote Socket）

> 分析基于本仓库 `cells-android10`（Android 10）源码。
> 本文把前面分散在 `AOSP_Transaction_分析.md`、`Zygote_Socket_分析.md` 中的结论，按**原子性 / 鲁棒性 / 其他特性**三个维度归一，形成一份总览。

---

## 0. 背景：两类被反复复用的“机制”

AOSP 里 “Transaction” 与 “Zygote Socket” 都不是单一类，而是被多子系统复用的**设计范式**：

- **Transaction 模式**：命令集合 + 批量收集 + 原子提交 + 跨进程执行。
  - 图形侧：`SurfaceControl.Transaction`（图层原子更新）。
  - 组件侧：`ClientTransaction`（Activity 生命周期命令批次）。
- **Zygote Socket**：init 创建的 Unix 域 socket（`/dev/socket/zygote`），AMS 经它请求 fork 新 app。

两者共享大量工程化特性，下面分维度展开。

---

## 一、原子性（Atomicity）

| 机制 | 原子性体现 | 源码 |
| --- | --- | --- |
| `SurfaceControl.Transaction` | `apply()` 把**整批**图层变更一次性提交给 SurfaceFlinger（`nativeApplyTransaction`），要么整帧全生效、要么全不生效，客户端看不到中间态 | `frameworks/base/core/java/android/view/SurfaceControl.java:2099-2102` |
| `ClientTransaction` | 整容器（callbacks + 最终 lifecycle 态）作为一个单元跨进程投递，由 `TransactionExecutor` 完整执行到目标态 | `frameworks/base/core/java/android/app/servertransaction/ClientTransaction.java:134-136` |
| Zygote fork | `fork()` 在内核里是**瞬时快照**：子进程得到父进程地址空间的完整一致副本，不存在“复制到一半” | `Zygote.forkAndSpecialize`（native `fork()`） |
| Zygote socket 协议 | 一次请求 = `argc\narg0\n...\n`，随后阻塞读**一个** pid；请求/响应在单连接上原子；父进程 fork 后才回写 pid | `frameworks/base/core/java/android/os/ZygoteProcess.java:406,435` |

核心思想：**“收集→一次性提交”** 是这几种机制共同的原子性来源——把易错的多步操作折叠成一次不可分割的动作。

---

## 二、鲁棒性（Robustness）

### Transaction 侧
- **资源不泄漏**：`SurfaceControl.Transaction` 实现 `Closeable`，`close()` 释放 native 对象（`SurfaceControl.java:2090`），并用 `NativeAllocationRegistry` 管理 native 内存；`ClientTransaction` 用 `ObjectPool.obtain/recycle` 复用，避免频繁分配/GC 抖动（`ClientTransaction.java:144-171`）。
- **可恢复 / 可合并**：`apply()` 后事务可清空复用；`merge()` 合并多个事务（`SurfaceControl.java:2632`）。

### Zygote socket 侧（作为 root 特权进程，鲁棒性做得尤其重）
- **防注入**：`zygoteSendArgsAndGetResult` 先校验参数里不允许嵌入 `\n`/`\r`，否则直接抛 `ZygoteStartFailedEx`（`ZygoteProcess.java:389-393`），防止换行破坏“逐行”线协议。
- **鉴权**：连上即 `getPeerCredentials()` 取内核级 `SO_PEERCRED`，再 `applyUidSecurityPolicy` 只允许特权方 fork（`ZygoteConnection.java:103`、`Zygote.java:558`）；文件权限 `660 root system` 双重隔离。
- **容错回退**：启用 USAP 池时先走 USAP socket，若 `IOException` 则 **catch 后降级回普通 Zygote 路径**（`ZygoteProcess.java:408-417`）。
- **重试**：`ZYGOTE_RETRY_MILLIS = 500` 周期重试连 socket（`:346`），应对 zygote 尚未就绪的启动竞态。
- **读干净**：读结果时“总是读完整个响应”，避免残留字节污染后续启动（`ZygoteProcess.java:431-433`）。
- **fork 安全**：单线程 `poll` 循环 + `preFork/postFork` 停/恢复线程，避免多线程 `fork` 死锁。

---

## 三、其他特性

### 1. 解耦与跨进程传输（Decoupling / IPC）
- `ClientTransaction` 实现 `Parcelable`，经 `IApplicationThread` Binder 从 system_server 投到 app 进程执行。
- `SurfaceControl.Transaction` 在 native 层经 `ISurfaceComposer` 提交给 SurfaceFlinger（跨进程、跨语言）。
- `Zygote` 用 `LocalSocket`（`/dev/socket/zygote`）跨进程收命令。
- 共同点：**“系统侧组装、另一侧原子执行”**，收发双方不直接耦合。

### 2. 批量收集与延迟执行（Batching / Lazy）
- `SurfaceControl.Transaction` 先把 `setXxx` 攒进一个对象，不立即生效；WMS 每帧共用一个 `mTransaction`，帧末统一 `apply()` 一次（`WindowManagerService.java:889,1086`）。
- `ClientTransaction` 先把多个 `ClientTransactionItem` 收集，再一次性 `schedule()`。
- 好处：减少 IPC 次数、避免中间态。

### 3. 顺序性与最终一致性（Ordering / Consistency）
- `ClientTransaction`：callbacks 按 `addCallback` 顺序执行，最终落到 `setLifecycleStateRequest` 指定的生命周期态（`ClientTransaction.java:71-105`）。
- `SurfaceControl`：`apply()` 时按添加顺序生效。

### 4. 流畅接口 / Builder 风格（Fluent API）
- `SurfaceControl.Transaction` 的 `setVisibility/show/setLayer/setPosition/...` 全部返回 `this`，可链式编排（`SurfaceControl.java:2124` 等）。

### 5. 可复用、可合并、对象池（Reuse / Merge / Pool）
- `apply()` 后事务可清空复用；`merge(Transaction)` 合并多个事务（`SurfaceControl.java:2632`）。
- `ClientTransaction` 用 `ObjectPool.obtain/recycle` 复用，避免频繁分配（`ClientTransaction.java:144-171`）。

### 6. 资源与生命周期管理（Resource Management）
- `SurfaceControl.Transaction` 是 `Closeable`，`close()` 释放 native 对象（`SurfaceControl.java:2090`），并用 `NativeAllocationRegistry` 管理 native 内存。
- Zygote 侧 `ZygoteState.close()` 关闭会话 socket（`ZygoteProcess.java:218`）。

### 7. 命令模式 + 组合模式（Command + Composite）
- 每个 `ClientTransactionItem` / `setXxx` 是“命令”，`Transaction` 是“命令集合”，统一调度执行。

### 8. 安全：鉴权、隔离、防注入（Security）
- `SO_PEERCRED` 内核级凭据 + `660 root system` 文件隔离（Zygote）。
- `applyUidSecurityPolicy` 按凭据限制可 fork 的 uid（`Zygote.java:558`）。
- 参数防注入：禁止嵌入 `\n`/`\r`（`ZygoteProcess.java:389-393`）。
- Zygote/USAP 以 root 运行但代码精简，最小化攻击面。

### 9. 可测试性 / 依赖注入（Testability）
- WMS 用 `TransactionFactory` 把事务创建抽象出来，单测可注入假事务。
- `ZygoteProcess` 的 socket 地址由构造参数传入（`mZygoteSocketAddress` 等，`ZygoteProcess.java:89-119`），便于替换/测试。

### 10. 性能优化：批处理、USAP 池（Performance）
- WMS 单事务按帧提交：把全窗口图层变更合并成一次原子更新，降低 IPC 开销、杜绝撕裂。
- Android 10 的 **USAP 池**：预 fork 一批 `usap` 进程，启动 app 时直接 specialize，省去每次 fork 开销（`Zygote.java:506-558`）。

### 11. 可扩展性（Extensibility）
- 同一 socket 模型平滑扩展出 USAP 池（复用 `accept()` + specialize）。
- `ClientTransactionItem` 可新增类型（Launch/Resume/Pause/Destroy/Configuration 等）而不改调度框架。

### 12. 容错与回退（Fault Tolerance）
- USAP 路径 `IOException` 时 **catch 后降级回普通 Zygote 路径**（`ZygoteProcess.java:408-417`）。
- `ZYGOTE_RETRY_MILLIS=500` 重试连 socket，应对启动竞态（`:346`）。
- 读结果“总是读完整个响应”，防止残留字节污染后续启动（`:431-433`）。
- `pid<0` → 抛 `ZygoteStartFailedEx`，明确失败（`ZygoteProcess.java:438`）。

---

## 四、一句话总结

AOSP 的这两类机制都围绕 **“收集多步操作 → 原子一次性提交 → 跨进程/跨层执行”** 展开，并辅以 **解耦、批处理、流畅 API、资源回收、命令组合、安全鉴权、可测、性能池化、容错回退** 等一整套工程化特性；其底层支撑的原子性与鲁棒性，正是它们能在图形合成、组件生命周期、进程孵化等高频关键路径上长期稳定工作的原因。

---

## 五、关键源码索引

| 主题 | 文件 | 行号 |
| --- | --- | --- |
| Transaction.apply 原子提交 | `frameworks/base/core/java/android/view/SurfaceControl.java` | 2099-2102 |
| Transaction.merge 合并 | `SurfaceControl.java` | 2632 |
| Transaction.Closeable 资源释放 | `SurfaceControl.java` | 2090 |
| ClientTransaction Parcelable 跨进程 | `frameworks/base/core/java/android/app/servertransaction/ClientTransaction.java` | 134-136 |
| ClientTransaction ObjectPool 复用 | `ClientTransaction.java` | 144-171 |
| WMS 单事务按帧提交 | `frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java` | 889 / 1086 |
| init 创建本地 socket（660 root system） | `system/core/rootdir/init.zygote64_32.rc` | 6 / 22 |
| Zygote 单线程 poll 循环 | `frameworks/base/core/java/com/android/internal/os/ZygoteServer.java` | 373 / 432 |
| SO_PEERCRED 取凭据 | `frameworks/base/core/java/com/android/internal/os/ZygoteConnection.java` | 103 |
| 按凭据做 uid 安全策略 | `frameworks/base/core/java/com/android/internal/os/Zygote.java` | 558 |
| AMS 发起 Process.start | `frameworks/base/services/core/java/com/android/server/am/ProcessList.java` | 1823 |
| 组装 ZygoteArguments | `frameworks/base/core/java/android/os/ZygoteProcess.java` | 541 / 561-630 |
| LocalSocket 连 /dev/socket/zygote | `ZygoteProcess.java` | 175 / 181-189 |
| 线协议 argc + 逐行参数 | `ZygoteProcess.java` | 406 / 396-405 |
| 写参数并读回 pid | `ZygoteProcess.java` | 422 / 428-436 |
| 参数防注入校验 | `ZygoteProcess.java` | 389-393 |
| USAP 池 fallback / 重试 | `ZygoteProcess.java` | 408-417 / 346 |
| USAP 池复用同一 socket | `frameworks/base/core/java/com/android/internal/os/Zygote.java` | 506-558 |
