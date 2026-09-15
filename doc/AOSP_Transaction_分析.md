# AOSP 中 “Transaction” 多处运用的特点分析

> 分析基于本仓库 `cells-android10`（Android 10）源码。
> 结论：AOSP 里的 “Transaction” 不是单一类，而是一种被多子系统复用的**设计模式**（命令集合 + 批量收集 + 原子提交 + 跨进程执行）。

---

## 一、两处典型实现

### 1. `SurfaceControl.Transaction` —— 图层变更事务（图形 / WMS 侧）

定义见 `frameworks/base/core/java/android/view/SurfaceControl.java:2050`：

```2050:2071:SurfaceControl.java
/**
 * An atomic set of changes to a set of SurfaceControl.
 */
public static class Transaction implements Closeable {
    ...
    // Open a new transaction object. The transaction may be filed with commands to
    // manipulate SurfaceControl instances, and then applied atomically with apply().
    // ... re-using a transaction after a call to apply is allowed as a convenience.
```

- 所有 `setXxx` 方法（`setVisibility`/`show`/`hide`/`setPosition`/`setLayer`/`setAlpha`/`setGeometry`/`reparent`/`setBuffer` …）均返回 `this`（`SurfaceControl.java:2124` 等），可链式调用。
- `apply()` 一次性把整批变更提交给 SurfaceFlinger（`nativeApplyTransaction`，`SurfaceControl.java:2099-2102`）。
- `close()` 释放 native 资源而不提交（`:2090`）。
- `merge(Transaction)` 可把多个事务合并为一次提交（`:2632`）。

### 2. `ClientTransaction` —— Activity 生命周期事务（AMS / ATMS 侧）

定义见 `frameworks/base/core/java/android/app/servertransaction/ClientTransaction.java:35`：

```35:44:ClientTransaction.java
/**
 * A container that holds a sequence of messages, which may be sent to a client.
 * This includes a list of callbacks and a final lifecycle state.
 */
public class ClientTransaction implements Parcelable, ObjectPoolItem {
```

- 持有 `mActivityCallbacks`（一串 `ClientTransactionItem`）与 `mLifecycleStateRequest`（最终生命周期态）。
- 通过 `schedule()` → `mClient.scheduleTransaction(this)`（`ClientTransaction.java:134-136`）经 `IApplicationThread` Binder 跨进程发到 app 进程。
- 由 `TransactionExecutor` 顺序执行：先 `preExecute`（系统侧做前置工作），再在 app 侧逐个回调并补到目标生命周期态。
- 实现 `Parcelable`，可跨进程序列化（`writeToParcel` / `CREATOR`）。
- 用 `ObjectPool.obtain/recycle`（`ClientTransaction.java:144-171`）复用实例，避免频繁分配。

---

## 二、它们共享的核心特点

1. **批量收集，延迟执行** —— 先把多个分散操作塞进一个对象（`setXxx` / `addCallback`），不立即生效。
2. **原子提交** —— `SurfaceControl.Transaction.apply()` 把整批图层变更“要么全生效、要么全不生效”地一次性下发；客户端不会看到中间态。
3. **跨进程 / Parcelable 传输** —— `ClientTransaction` 实现 `Parcelable` 经 Binder 传到 app；`SurfaceControl.Transaction` 在 native 层经 `ISurfaceComposer` 提交给 SurfaceFlinger。都是“系统侧组装、另一侧原子执行”。
4. **顺序与最终一致性** —— `ClientTransaction` 保证 callbacks 顺序执行并最终落到 `setLifecycleStateRequest` 指定的状态；`SurfaceControl` 操作在 `apply` 时按添加顺序生效。
5. **流畅接口（Fluent / Builder）** —— `SurfaceControl.Transaction` 的 `setXxx` 返回自身，可链式编排。
6. **可复用 / 可合并** —— `apply()` 后事务可被清空复用（注释明确允许）；`merge()` 能合并多个事务。
7. **资源与回收管理** —— `SurfaceControl.Transaction` 是 `Closeable`，需 `close()` 释放 native 对象；`ClientTransaction` 用 `ObjectPool` 回收。
8. **本质是命令模式 + 组合模式** —— 每个 item 是命令，Transaction 是命令集合，统一调度执行。

---

## 三、WMS 里的关键实践：单事务、按帧提交

WMS 并不是每改一个窗口就发一次 Binder，而是**每帧共用一个事务对象**：

- 装配：`mTransactionFactory = SurfaceControl.Transaction::new`，`mTransaction = mTransactionFactory.make()`（`WindowManagerService.java:889`、`1086`）。
- 在 `WindowSurfacePlacer.performSurfacePlacement()` 的帧遍历中，`WindowStateAnimator`、`Dimmer`、`AppWindowToken`、`DisplayContent`、`InsetsSourceProvider` 等把各自的 `setPosition`/`setLayer`/`setAlpha`/`reparent` 都加进同一个 `mTransaction`，帧末统一 `apply()` 一次。

收益：

- 把所有窗口的图层变更**合并成一次原子更新**，避免多次 IPC 开销；
- 杜绝“部分窗口已更新、部分还没更新”导致的**闪烁 / 撕裂**。

此外 `TransactionFactory` 把事务创建抽象出来，便于在单测中注入假事务（依赖注入 / 可测性）。

---

## 四、一句话总结

AOSP 的 “Transaction” 是一种 **“收集多步操作 → 原子一次性提交 → 跨进程 / 跨层执行”** 的通用范式：

- 图形侧：`SurfaceControl.Transaction`（图层原子更新）；
- 组件侧：`ClientTransaction`（生命周期命令批次）。

二者都体现了 **批量、原子、解耦、可复用、命令组合** 五大特征。
