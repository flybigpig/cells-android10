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

## 五、涉及文件索引

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
