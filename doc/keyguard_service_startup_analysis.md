# Android 10 SystemUI 锁屏（Keyguard）服务启动流程解读

本文整合对 AOSP Android 10（cells-android10）中 SystemUI 锁屏服务启动流程的分析，
涵盖两条主线：其一是 SystemUI 进程内 `KeyguardService` 的创建与 `KeyguardViewMediator`
等核心组件的初始化；其二是 `system_server` 侧 `PhoneWindowManager` 通过 `IKeyguardService`
AIDL 接口跨进程绑定该服务，并在绑定完成后触发系统 UI 初始化的完整代码链路。

---

## 一、锁屏启动总体时序

锁屏服务的启动可以分为四个阶段：

1. **system_server 绑定 KeyguardService**：系统启动过程中，`PhoneWindowManager`
   在 `onSystemUiStarted()` 时机通过 `KeyguardServiceDelegate.bindService()` 以
   `bindServiceAsUser(..., UserHandle.SYSTEM)` 的方式绑定 SystemUI 的 `KeyguardService`。
   该绑定使用 `config_keyguardComponent` 资源拼出目标 `ComponentName`。

2. **onCreate 触发 SystemUI 组件初始化**：`KeyguardService.onCreate()` 被调用后，
   首先通过 `SystemUIApplication.startServicesIfNeeded()` 启动 SystemUI 内部的全部
   组件（含 `KeyguardViewMediator`、`StatusBar` 等），随后取出 `KeyguardViewMediator`
   实例，并构造 `KeyguardLifecyclesDispatcher`。

3. **StatusBar 挂接锁屏视图**：SystemUI 初始化完成后，`StatusBar.startKeyguard()`
   调用 `KeyguardViewMediator.registerStatusBar(...)`，将 Bouncer 容器、状态栏等视图
   依赖注入到 `StatusBarKeyguardViewManager`，完成锁屏视图管理层与状态栏的挂接。

4. **onSystemReady 回调首次展示锁屏**：当系统就绪，`system_server` 通过
   `IKeyguardService.onSystemReady()` 回调进入 `KeyguardViewMediator` 的
   `handleSystemReady()`，进而调用 `doKeyguardLocked(null)` 首次展示锁屏。

---

## 二、SystemUI 侧关键实现

### 1. KeyguardService 声明

`frameworks/base/packages/SystemUI/AndroidManifest.xml`（546-549 行）：

```xml
<service
    android:name=".keyguard.KeyguardService"
    android:exported="true"
    android:enabled="@bool/config_enableKeyguardService" />
```

`exported="true"` 使其可被 `system_server` 跨进程绑定；`enabled` 受
`@bool/config_enableKeyguardService` 资源控制，可在不同设备配置中开关锁屏服务。

### 2. onCreate 触发组件初始化

`frameworks/base/packages/SystemUI/src/com/android/systemui/keyguard/KeyguardService.java`
（46-55 行）：

```java
@Override
public void onCreate() {
    ((SystemUIApplication) getApplication()).startServicesIfNeeded();
    mKeyguardViewMediator =
            ((SystemUIApplication) getApplication()).getComponent(KeyguardViewMediator.class);
    mKeyguardLifecyclesDispatcher = new KeyguardLifecyclesDispatcher(
            Dependency.get(ScreenLifecycle.class),
            Dependency.get(WakefulnessLifecycle.class));
}
```

`onCreate()` 是 SystemUI 锁屏初始化的真正入口：先 `startServicesIfNeeded()` 拉起所有
SystemUI 组件，再从 `SystemUIApplication` 中取回 `KeyguardViewMediator` 引用，并基于
`ScreenLifecycle` 与 `WakefulnessLifecycle` 构造生命周期分发器。

### 3. KeyguardViewMediator 核心职责

`frameworks/base/packages/SystemUI/src/com/android/systemui/keyguard/KeyguardViewMediator.java`：

- `setupLocked()`（约 726-728 行）中构造视图管理者：
  ```java
  mStatusBarKeyguardViewManager = SystemUIFactory.getInstance()
          .createStatusBarKeyguardViewManager(...);
  ```
- `registerStatusBar(...)`（约 2071-2080 行）由 `StatusBar.startKeyguard()` 调用，
  将状态栏相关依赖注入进来。
- `handleSystemReady()`（约 788-794 行）中调用 `doKeyguardLocked(null)` 首次展示锁屏。

### 4. StatusBar 与视图管理者

`frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/StatusBar.java`
（约 1285-1304 行）的 `startKeyguard()` 调用 `registerStatusBar`，注入 Bouncer 容器等。

`frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/StatusBarKeyguardViewManager.java`
（约 301-328 行）的 `show(Bundle)` 与 `showBouncerOrKeyguard()` 决定是直接显示锁屏壁纸
还是弹出密码盘（Bouncer）。

---

## 三、system_server 侧跨进程绑定详解

### 1. PhoneWindowManager 创建代理

`frameworks/base/services/core/java/com/android/server/policy/PhoneWindowManager.java`：

- 构造阶段（约 1959-1970 行）创建委托对象：
  ```java
  mKeyguardDelegate = new KeyguardServiceDelegate(mContext);
  ```
- `onSystemUiStarted()`（约 4827-4840 行）中调用 `bindKeyguard()`，而非在
  `systemReady()` 中绑定——注释明确说明（约 4844-4848 行）绑定不应在 `systemReady()` 进行。

### 2. KeyguardServiceDelegate 绑定服务

`frameworks/base/services/core/java/com/android/server/policy/keyguard/KeyguardServiceDelegate.java`：

- `bindService()`（约 137-161 行）使用 `config_keyguardComponent` 资源拼出
  `ComponentName`，并以 `bindServiceAsUser(..., UserHandle.SYSTEM)` + `BIND_AUTO_CREATE`
  方式绑定。
- `mKeyguardConnection.onServiceConnected`（约 163-168 行）中创建 `KeyguardServiceWrapper`，
  将 Binder 代理 `IBinder` 包装为 `IKeyguardService` 接口对象。
- 状态补发（约 169-203 行）：在连接建立后，将此前缓存的 `onSystemReady`、
  `setCurrentUser`、`onStartedWakingUp` 等调用重放给新连接的 Keyguard 服务，
  避免绑定前的系统事件丢失。

### 3. Wrapper 与状态监控

`frameworks/base/services/core/java/com/android/server/policy/keyguard/KeyguardServiceWrapper.java`
（约 43-47 行）构造器持有 `mService`（即 `IKeyguardService` 代理）与 `mKeyguardStateMonitor`。

`frameworks/base/services/core/java/com/android/server/policy/keyguard/KeyguardStateMonitor.java`
（约 58-71 行）构造时通过 `service.addStateMonitorCallback(this)` 注册回调，
使 `system_server` 能反向接收锁屏状态（如是否安全锁定、是否已Dismiss）的变化通知。

---

## 四、关键设计要点

- **双层面结构**：`KeyguardService` 是对 `system_server` 暴露的 Binder 服务边界，
  `KeyguardViewMediator` 是 SystemUI 内部真正管理锁屏状态与视图的核心组件，二者通过
  `onCreate()` 中 `getComponent()` 衔接。
- **绑定时机**：选择 `onSystemUiStarted()` 而非 `systemReady()`，确保系统 UI 框架先就绪。
- **UserHandle.SYSTEM 绑定**：锁屏属于系统级组件，以 SYSTEM 用户身份绑定，保证跨用户安全。
- **状态重放机制**：`KeyguardServiceDelegate` 缓存连接前的事件并在 `onServiceConnected`
  后重放，保证启动竞态下不丢状态。
- **资源开关**：`config_enableKeyguardService` 与 `config_keyguardComponent` 让 OEM 能
  灵活开关或替换锁屏实现。

---

## 五、关键类深度解析（细粒度）

### 1. KeyguardServiceDelegate 类

`frameworks/base/services/core/java/com/android/server/policy/keyguard/KeyguardServiceDelegate.java`（39 行）是 `system_server` 中 `PhoneWindowManager` 与 SystemUI `KeyguardService` 之间的本地代理/委托类。核心职责：维护一份 keyguard 状态缓存用于在锁屏进程崩溃后恢复状态，并支持本地/远程实例切换。

- **状态缓存 `mKeyguardState`**：`KeyguardState`（56 行）保存 `showing`、`occluded`、`secure`、`dreaming`、`systemIsReady`、`deviceHasKeyguard`、`enabled`、`currentUser`、`bootCompleted`、`screenState`、`interactiveState` 等字段。其 `reset()`（80-90 行）刻意将 `showing`/`secure`/`deviceHasKeyguard`/`enabled` 默认置 `true`，注释解释"在 keyguard 服务真正启动前若有人查询状态，应保守假设锁屏处于显示且安全状态"，避免竞态暴露未锁屏窗口。
- **绑定机制**：`bindService()`（137-161 行）从 `config_keyguardComponent` 解析 `ComponentName`，经 `bindServiceAsUser(intent, mKeyguardConnection, BIND_AUTO_CREATE, mHandler, UserHandle.SYSTEM)` 绑定。失败时将 `deviceHasKeyguard` 置 `false`。
- **`mKeyguardConnection`（163-219 行）**：`onServiceConnected`（165-203 行）中构造 `KeyguardServiceWrapper`，并按 `systemIsReady` 标志重放缓存的 `onSystemReady`/`setCurrentUser`/`onStartedWakingUp`/`onFinishedWakingUp`/`onScreenTurningOn`/`onScreenTurnedOn`/`onBootCompleted`/`setOccluded`/`setKeyguardEnabled`。`onServiceDisconnected`（206-218 行）置空 `mKeyguardService`、`reset()` 状态，并经 `ActivityTaskManager` 重新显示锁屏 scrim。
- **双写方法**：所有公开方法（221-414 行）若 `mKeyguardService` 非空则转发远程，同时回写 `mKeyguardState`。`onSystemReady`/`onScreenTurningOn` 在连接前仅缓存（`systemIsReady=true`、暂存 `DrawnListener`），连接后补发。
- **内部回调包装**：`KeyguardShowDelegate`（98-112 行）、`KeyguardExitDelegate`（115-129 行）为 Binder Stub，映射远端 `onDrawn`/`onKeyguardExitResult` 回本地 `DrawnListener`/`OnKeyguardExitResult`。
- **诊断**：`writeToProto`（416-424 行）、`dump`（426-448 行）输出全部本地状态。

### 2. PhoneWindowManager 类（锁屏视角）

`frameworks/base/services/core/java/com/android/server/policy/PhoneWindowManager.java`（245 行）`implements WindowManagerPolicy`，是窗口/输入/电源策略中枢，作为"策略编排者"协调锁屏，自身不实现锁屏逻辑。

- **关键成员**：`KeyguardServiceDelegate mKeyguardDelegate`（421 行）为唯一锁屏桥；`mKeyguardDrawnOnce`（324 行）标记首帧绘制；`mKeyguardOccluded` 记录锁屏是否被覆盖。
- **创建与绑定**：`init()`（约 1959 行）仅 `new KeyguardServiceDelegate(mContext)`，不立即绑定；`onSystemUiStarted()`（4837-4840 行）调用 `bindKeyguard()`。
- **协同调用面**：`isShowing()`/`isInputRestricted()`/`verifyUnlock()` 决定按键路由；`startedGoingToSleep`/`finishedGoingToSleep`/`startedWakingUp`/`finishedWakingUp`/`screenTurningOn`/`screenTurnedOn`/`screenTurningOff`/`screenTurnedOff`（4458-4636 行）等透传事件；`setOccluded(...)`（3545-3563 行）处理全屏 Activity 覆盖；`onDreamingStarted/Stopped`（4421-4426 行）转发屏保状态。

### 3. WindowManagerPolicy.onSystemUiStarted()

- **接口定义**（`WindowManagerPolicy.java` 1437-1440 行）：
  ```java
  /** Called when System UI has been started. */
  void onSystemUiStarted();
  ```
  语义为"System UI 进程已启动完成"的事件钩子，由 WMS/SystemServer 在 SystemUI 就绪后调用。
- **实现**（`PhoneWindowManager.java` 4837-4840 行）：`bindKeyguard()`，即锁屏跨进程绑定的真正入口。
- **设计要点**：把"系统就绪"与"SystemUI 就绪"两个时机解耦；锁屏服务进程需在系统就绪之后才启动，过早绑定会因目标组件未就绪而失败。接口只声明事件，具体动作由策略实现决定，保持对 WMS 的抽象隔离。

### 4. Binder 代理封装代码

`KeyguardServiceDelegate.mKeyguardConnection.onServiceConnected`（163-168 行）：

```java
mKeyguardService = new KeyguardServiceWrapper(mContext,
        IKeyguardService.Stub.asInterface(service), mCallback);
```

- `service` 是 SystemUI 侧 `KeyguardService.mBinder` 的跨进程 `IBinder` 引用。
- `IKeyguardService.Stub.asInterface(service)`：跨进程时返回 `Proxy`，后续调用经 Binder 驱动 IPC。
- `KeyguardServiceWrapper` 构造（43-47 行）持有 `mService`（远程代理）并 `new KeyguardStateMonitor(context, service, callback)`；`KeyguardStateMonitor` 构造时 `service.addStateMonitorCallback(this)`，使锁屏状态变化反向回调到 `system_server`。
- **三层价值**：异常隔离（`RemoteException` 被 `try/catch` 降级为 `Slog.w`，配合缓存与重放使崩溃无感）；查询走本地缓存（`isShowing`/`isSecure` 等返回 `KeyguardStateMonitor` 本地值）；状态反向同步（双层缓存为崩溃恢复与启动竞态的基石）。该行代码使 `system_server` 真正获得锁屏控制与状态感知能力，并重放可正确执行。

### 5. KeyguardService 两个成员变量

`KeyguardService`（43-44 行）：

```java
private KeyguardViewMediator mKeyguardViewMediator;
private KeyguardLifecyclesDispatcher mKeyguardLifecyclesDispatcher;
```

- **`mKeyguardViewMediator`**：`KeyguardViewMediator`（142 行）`extends SystemUI`，是锁屏业务核心协调者，方法多 `synchronized` 且 UI 动作 post 到 UI 线程。`KeyguardService.mBinder` 的每个 AIDL 方法（`onSystemReady`/`setOccluded`/`verifyUnlock`/`dismiss`/`setCurrentUser`/`onStartedWakingUp` 等）在 `checkPermission()` 后几乎都转发给它。掌管锁屏显示/隐藏决策、`StatusBarKeyguardViewManager` 协作、监听 `KeyguardUpdateMonitor`、校验 `LockPatternUtils`。在 `onCreate()` 经 `getComponent(KeyguardViewMediator.class)` 取出。
- **`mKeyguardLifecyclesDispatcher`**：`KeyguardLifecyclesDispatcher`（25 行）是将 WindowManager 生命周期事件分发到主线程的轻量适配器。构造（40-44 行）接收 `ScreenLifecycle` 与 `WakefulnessLifecycle`，定义 8 种事件常量，`dispatch(int what)`（46-48 行）经 Handler 广播给 SystemUI 内所有监听者。`onCreate()` 中传入 `Dependency.get(...)` 全局单例，使 `system_server` 推送的唤醒/屏幕事件统一纳入 SystemUI 生命周期总线。
- **协作**：`mKeyguardViewMediator` 处理"锁屏该做什么"（业务语义），`mKeyguardLifecyclesDispatcher` 处理"把系统生命周期事件广播给内部模块"（机制/通知），二者在 `mBinder` 各方法中并列调用，体现 `KeyguardService` 作为"薄 Binder 边界"的哲学。

---

## 六、interactiveState 状态变化流程

`interactiveState` 是 `KeyguardState`（78 行）中的整型字段，用于 `system_server` 侧镜像锁屏感知的"交互（唤醒）状态"。四个取值（48-51 行）：`INTERACTIVE_STATE_SLEEP=0`、`WAKING=1`、`AWAKE=2`、`GOING_TO_SLEEP=3`。

### 1. 状态变更四个入口（双写）

- `onStartedWakingUp()`（294-300 行）：转发后置 `WAKING`。
- `onFinishedWakingUp()`（302-308 行）：转发后置 `AWAKE`。
- `onStartedGoingToSleep(int why)`（348-354 行）：转发后写 `offReason=why` 与 `GOING_TO_SLEEP`。
- `onFinishedGoingToSleep(int why, boolean cameraGestureTriggered)`（356-361 行）：转发后置 `SLEEP`。

### 2. 上游触发源

上述入口均由 `PhoneWindowManager` 在窗口/电源生命周期回调中调用（先判 `mKeyguardDelegate != null`）：`startedWakingUp`→`onStartedWakingUp`（4516-4518）、`finishedWakingUp`→`onFinishedWakingUp`（4529-4531）、`startedGoingToSleep(why)`→`onStartedGoingToSleep`（4458-4460）、`finishedGoingToSleep`→`onFinishedGoingToSleep`（4485-4488）。回调源头是 PowerManager 的 wake/sleep 流程经 WMS 转发。

### 3. 状态流转图

```
设备开始唤醒 → startedWakingUp → onStartedWakingUp → interactiveState = WAKING(1)
设备完成唤醒 → finishedWakingUp → onFinishedWakingUp → interactiveState = AWAKE(2)
设备开始休眠 → startedGoingToSleep(why) → onStartedGoingToSleep → interactiveState = GOING_TO_SLEEP(3), offReason=why
设备完成休眠 → finishedGoingToSleep → onFinishedGoingToSleep → interactiveState = SLEEP(0)
```

### 4. 状态消费（重放与诊断）

- **连接重放**：`onServiceConnected`（177-183 行）中若 `interactiveState` 为 `AWAKE`/`WAKING` 补发 `onStartedWakingUp()`；若为 `AWAKE` 再补发 `onFinishedWakingUp()`，使锁屏崩溃重启后恢复到正确唤醒态。
- **诊断**：`writeToProto`（422 行）写 `INTERACTIVE_STATE`；`dump`（443-444 行）经 `interactiveStateToString` 打印可读字符串。

### 5. 设计要义

`interactiveState` 体现"状态缓存 + 崩溃容错"设计：把瞬时电源事件落为持久本地镜像，使未连接时也能返回最近交互态，并通过重放在锁屏重建后无缝衔接，避免丢失唤醒/休眠上下文。

---

## 七、涉及文件索引

| 角色 | 路径 |
| --- | --- |
| 服务声明 | `frameworks/base/packages/SystemUI/AndroidManifest.xml` |
| 锁屏服务 | `frameworks/base/packages/SystemUI/src/com/android/systemui/keyguard/KeyguardService.java` |
| 核心协调者 | `frameworks/base/packages/SystemUI/src/com/android/systemui/keyguard/KeyguardViewMediator.java` |
| 状态栏挂接 | `frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/StatusBar.java` |
| 视图管理 | `frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/StatusBarKeyguardViewManager.java` |
| 策略入口 | `frameworks/base/services/core/java/com/android/server/policy/PhoneWindowManager.java` |
| 绑定委托 | `frameworks/base/services/core/java/com/android/server/policy/keyguard/KeyguardServiceDelegate.java` |
| 代理封装 | `frameworks/base/services/core/java/com/android/server/policy/keyguard/KeyguardServiceWrapper.java` |
| 状态监控 | `frameworks/base/services/core/java/com/android/server/policy/keyguard/KeyguardStateMonitor.java` |
