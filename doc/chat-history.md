# Android 10 Source Code Analysis Chat Export

> Exported: 2026-08-18 09:00:50
> Project: cells-android10 (Pixel 3a XL, Android 10)

---

## 1. ims 构造方法 以及初始化 详解

让我查找 `InputManagerService` 的构造方法和初始化逻辑。



现在让我查看 native 层初始化以及 SystemServer 中的调用流程。



现在让我查看 IMS 的 `InputManagerHandler` 和 `onInitReady` 阶段的调用。



## InputManagerService (IMS) 构造方法与初始化详解

---

### 一、IMS 在 SystemServer 中的创建时序

在 SystemServer.java 中，IMS 的创建**先于 WMS**：

```
① new InputManagerService(context)        ← L1024 创建 IMS
② WindowManagerService.main(context, inputManager, ...)  ← L1033 创建 WMS（传入 IMS）
③ ServiceManager.addService(INPUT_SERVICE, inputManager) ← L1037 注册到 ServiceManager
④ wm.onInitReady()                        ← L1046 WMS 初始化就绪
⑤ inputManager.start()                    ← （在 startOtherServices 内部调用）
⑥ inputManagerF.systemRunning()           ← L2238 系统就绪回调
```

---

### 二、Java 层构造方法

InputManagerService(context)

```java
public InputManagerService(Context context) {
    this.mContext = context;
    this.mHandler = new InputManagerHandler(DisplayThread.get().getLooper());

    mUseDevInputEventForAudioJack =
            context.getResources().getBoolean(R.bool.config_useDevInputEventForAudioJack);

    mPtr = nativeInit(this, mContext, mHandler.getLooper().getQueue());

    String doubleTouchGestureEnablePath = context.getResources().getString(
            R.string.config_doubleTouchGestureEnableFile);
    mDoubleTouchGestureEnableFile = TextUtils.isEmpty(doubleTouchGestureEnablePath) ? null :
        new File(doubleTouchGestureEnablePath);

    LocalServices.addService(InputManagerInternal.class, new LocalService());
}
```

**构造方法逐步解析：**

| 步骤 | 代码 | 说明 |
|------|------|------|
| 1 | `mContext = context` | 保存系统上下文 |
| 2 | `new InputManagerHandler(DisplayThread.get().getLooper())` | 创建消息处理器，绑定到 **DisplayThread**（与 WMS 共享同一个前台线程，保证低延迟显示操作） |
| 3 | `mUseDevInputEventForAudioJack` | 读取配置：是否使用 `/dev/input/event` 子系统检测音频插孔 |
| 4 | `nativeInit(...)` | **关键步骤** — 通过 JNI 创建 Native 层 InputManager，返回 native 指针 `mPtr` |
| 5 | `mDoubleTouchGestureEnableFile` | 读取双击手势使能文件路径（用于折叠屏等设备） |
| 6 | `LocalServices.addService(...)` | 注册 `InputManagerInternal` 本地服务，供系统进程内其他服务直接调用 |

---

### 三、JNI 层 — nativeInit

com_android_server_input_InputManagerService.cpp

```cpp
static jlong nativeInit(JNIEnv* env, jclass, jobject serviceObj, 
                        jobject contextObj, jobject messageQueueObj) {
    sp<MessageQueue> messageQueue = android_os_MessageQueue_getMessageQueue(env, messageQueueObj);
    NativeInputManager* im = new NativeInputManager(contextObj, serviceObj,
            messageQueue->getLooper());
    im->incStrong(0);
    return reinterpret_cast<jlong>(im);
}
```

从 Java 层传入 `MessageQueue`，获取其绑定的 `Looper`，然后创建 `NativeInputManager`。

---

### 四、Native 层 — NativeInputManager 构造

NativeInputManager::NativeInputManager

```cpp
NativeInputManager::NativeInputManager(jobject contextObj,
        jobject serviceObj, const sp<Looper>& looper) :
        mLooper(looper), mInteractive(true) {
    JNIEnv* env = jniEnv();
    mServiceObj = env->NewGlobalRef(serviceObj);

    {
        AutoMutex _l(mLock);
        mLocked.systemUiVisibility = ASYSTEM_UI_VISIBILITY_STATUS_BAR_VISIBLE;
        mLocked.pointerSpeed = 0;
        mLocked.pointerGesturesEnabled = true;
        mLocked.showTouches = false;
        mLocked.pointerCapture = false;
        mLocked.pointerDisplayId = ADISPLAY_ID_DEFAULT;
    }
    mInteractive = true;

    mInputManager = new InputManager(this, this);
    defaultServiceManager()->addService(String16("inputflinger"), mInputManager, false);
}
```

**初始化内容：**

| 步骤 | 说明 |
|------|------|
| 保存 Looper | 将 DisplayThread 的 Looper 保存到 `mLooper`，用于后续事件循环 |
| JNI 全局引用 | 创建 Java 层 `InputManagerService` 对象的全局引用，防止被 GC 回收 |
| 锁定状态初始化 | `pointerSpeed=0`、`showTouches=false`、`pointerGesturesEnabled=true` 等默认值 |
| **创建 InputManager** | `new InputManager(this, this)` — NativeInputManager 同时充当 ReaderPolicy 和 DispatcherPolicy |
| 注册 Binder 服务 | 将 InputManager 注册为 `"inputflinger"` Binder 服务 |

---

### 五、核心 — InputManager 构造方法

InputManager::InputManager

```cpp
InputManager::InputManager(
        const sp<InputReaderPolicyInterface>& readerPolicy,
        const sp<InputDispatcherPolicyInterface>& dispatcherPolicy) {
    mDispatcher = new InputDispatcher(dispatcherPolicy);
    mClassifier = new InputClassifier(mDispatcher);
    mReader = createInputReader(readerPolicy, mClassifier);
    initialize();
}
```

**这是整个输入系统的核心架构，创建了三大组件：**

```
InputManager
    │
    ├── InputReader（读取原始输入事件）
    │       ↓ 传递给
    ├── InputClassifier（分类输入事件：触摸/按键/鼠标等）
    │       ↓ 传递给
    └── InputDispatcher（分发事件到目标窗口）
```

| 组件 | 创建方式 | 职责 |
|------|----------|------|
| **InputDispatcher** | `new InputDispatcher(dispatcherPolicy)` | 负责将输入事件分发到正确的窗口，管理焦点、触摸目标查找 |
| **InputClassifier** | `new InputClassifier(mDispatcher)` | 对输入事件进行分类和预处理（如手势识别），作为 Reader 和 Dispatcher 之间的桥梁 |
| **InputReader** | `createInputReader(readerPolicy, mClassifier)` | 从内核 `/dev/input/` 设备节点读取原始输入事件，进行坐标转换、设备管理等 |

随后调用 initialize()：

```cpp
void InputManager::initialize() {
    mReaderThread = new InputReaderThread(mReader);
    mDispatcherThread = new InputDispatcherThread(mDispatcher);
}
```

创建两个独立的工作线程：
- **InputReaderThread** — 持续轮询 `/dev/input/` 读取原始事件
- **InputDispatcherThread** — 持续将分类后的事件分发到应用窗口

---

### 六、IMS 的 start() 方法

InputManagerService.start()

```java
public void start() {
    nativeStart(mPtr);                     // 启动 native 层的 Reader/Dispatcher 线程

    Watchdog.getInstance().addMonitor(this); // 注册 Watchdog 监控

    registerPointerSpeedSettingObserver();   // 注册指针速度设置观察者
    registerShowTouchesSettingObserver();    // 注册"显示触摸"设置观察者
    registerAccessibilityLargePointerSettingObserver(); // 注册无障碍大指针观察者

    // 注册广播接收器：用户切换时更新设置
    mContext.registerReceiver(new BroadcastReceiver() { ... },
            new IntentFilter(Intent.ACTION_USER_SWITCHED), null, mHandler);

    // 初始加载设置
    updatePointerSpeedFromSettings();
    updateShowTouchesFromSettings();
    updateAccessibilityLargePointerFromSettings();
}
```

其中 `nativeStart` 调用到 native 层 InputManager::start()：

```cpp
status_t InputManager::start() {
    // 先启动 Dispatcher 线程（优先级 URGENT_DISPLAY）
    status_t result = mDispatcherThread->run("InputDispatcher", PRIORITY_URGENT_DISPLAY);
    // 再启动 Reader 线程
    result = mReaderThread->run("InputReader", PRIORITY_URGENT_DISPLAY);
    return OK;
}
```

---

### 七、systemRunning() — 系统就绪阶段

InputManagerService.systemRunning()

在 `ActivityManagerService.systemReady()` 回调中被调用：

| 步骤 | 说明 |
|------|------|
| 获取 NotificationManager | 用于后续发送键盘布局切换通知 |
| 设置 `mSystemReady = true` | 标记系统已就绪 |
| 注册包管理广播 | 监听 `PACKAGE_ADDED/REMOVED/CHANGED/REPLACED`，触发键盘布局更新 |
| 注册蓝牙别名广播 | 监听 `ACTION_ALIAS_CHANGED`，重新加载设备别名 |
| 发送初始消息 | `MSG_RELOAD_DEVICE_ALIASES` + `MSG_UPDATE_KEYBOARD_LAYOUTS` |
| 通知 WiredAccessory | 调用 `mWiredAccessoryCallbacks.systemReady()` 初始化有线配件检测 |

---

### 八、完整初始化流程图

```
SystemServer.startOtherServices()
    │
    ├── ① new InputManagerService(context)          [Java 层]
    │       │
    │       ├── new InputManagerHandler(DisplayThread.Looper)
    │       │
    │       ├── nativeInit()                         [JNI 层]
    │       │       │
    │       │       └── new NativeInputManager(context, service, looper)  [Native 层]
    │       │               │
    │       │               ├── 初始化锁定状态（指针速度、触摸显示等）
    │       │               │
    │       │               ├── new InputManager(readerPolicy, dispatcherPolicy)
    │       │               │       │
    │       │               │       ├── new InputDispatcher()     ← 事件分发器
    │       │               │       ├── new InputClassifier()     ← 事件分类器
    │       │               │       ├── createInputReader()       ← 事件读取器
    │       │               │       └── initialize()
    │       │               │               ├── new InputReaderThread
    │       │               │               └── new InputDispatcherThread
    │       │               │
    │       │               └── 注册 "inputflinger" Binder 服务
    │       │
    │       └── LocalServices.addService(InputManagerInternal)
    │
    ├── ② WindowManagerService.main(context, inputManager, ...)
    │
    ├── ③ ServiceManager.addService(INPUT_SERVICE, inputManager)
    │
    └── ④ inputManager.start()
            │
            ├── nativeStart() → InputManager::start()
            │       ├── InputDispatcherThread.run()   ← 分发线程启动
            │       └── InputReaderThread.run()       ← 读取线程启动
            │
            ├── 注册 Watchdog 监控
            ├── 注册设置观察者（指针速度、显示触摸等）
            └── 初始加载设置值

SystemServer → AMS.systemReady() callback
    │
    └── ⑤ inputManagerF.systemRunning()
            ├── 获取 NotificationManager
            ├── 注册包管理/蓝牙广播
            └── 通知 WiredAccessory 就绪
```

**总结**：IMS 的初始化是一个**跨三层**（Java → JNI → Native）的过程。Java 层负责生命周期管理和系统服务注册；JNI 层做对象桥接；Native 层构建输入系统的核心三大组件（Reader → Classifier → Dispatcher）并创建独立工作线程。整个输入管道从内核设备读取原始事件，经过分类处理后，最终分发到正确的应用窗口。

## 2. wms 构造方法 以及初始化 详解

让我全面读取 WMS 构造方法、初始化流程以及相关调用链。



让我再查看一些关键组件的初始化细节，以便给出更完整的分析。



## WindowManagerService (WMS) 构造方法与初始化详解

---

### 一、WMS 在 SystemServer 中的创建时序

在 SystemServer.java 中，WMS 的创建位于 IMS 之后，并经历多个初始化阶段：

```
① new InputManagerService(context)                  ← L1024 先创建 IMS
② WindowManagerService.main(context, inputManager, ...) ← L1033 创建 WMS
③ ServiceManager.addService(WINDOW_SERVICE, wm)     ← L1035 注册到 ServiceManager
④ ServiceManager.addService(INPUT_SERVICE, inputManager) ← L1037 注册 IMS
⑤ mActivityManagerService.setWindowManager(wm)      ← L1042 AMS 关联 WMS
⑥ wm.onInitReady()                                  ← L1046 WMS 初始化就绪
   ...
⑦ wm.displayReady()                                 ← L1150 显示就绪
```

---

### 二、静态工厂方法 main()

WindowManagerService.main()

```java
public static WindowManagerService main(final Context context, final InputManagerService im,
        final boolean showBootMsgs, final boolean onlyCore, WindowManagerPolicy policy,
        ActivityTaskManagerService atm, TransactionFactory transactionFactory) {
    // WMS 的创建运行在 "android.display" 线程中
    DisplayThread.getHandler().runWithScissors(() ->
            sInstance = new WindowManagerService(context, im, showBootMsgs, onlyCore,
                    policy, atm, transactionFactory), 0);
    return sInstance;
}
```

**关键设计：**
- 通过 `DisplayThread.getHandler().runWithScissors()` 确保 WMS 在 **DisplayThread**（`android.display` 线程）中创建
- DisplayThread 是一个单例前台线程，专供 WMS、DisplayManager、InputManager 共享使用
- `runWithScissors` 会同步等待构造完成后才返回，确保 `sInstance` 已就绪

**参数说明：**

| 参数 | 类型 | 来源 | 说明 |
|------|------|------|------|
| `context` | Context | SystemServer | 系统上下文 |
| `im` | InputManagerService | 步骤①创建 | 输入管理服务（必须在 WMS 之前创建） |
| `showBootMsgs` | boolean | `!mFirstBoot` | 是否显示启动消息（非首次启动时显示） |
| `onlyCore` | boolean | SystemServer | 是否仅运行核心模式 |
| `policy` | WindowManagerPolicy | `new PhoneWindowManager()` | 窗口策略实现 |
| `atm` | ActivityTaskManagerService | AMS 内部 | Activity 任务管理服务 |
| `transactionFactory` | TransactionFactory | `SurfaceControl.Transaction::new` | Surface 事务工厂 |

---

### 三、构造方法详解

WindowManagerService() 共约 180 行，按职责分为 **8 个阶段**：

#### 阶段 1：锁与基础引用（L1049-L1054）

```java
installLock(this, INDEX_WINDOW);          // 安装 Binder 调用锁（防止死锁）
mGlobalLock = atm.getGlobalLock();        // 共享 ATM 的全局锁
mAtmService = atm;                        // ActivityTaskManager 引用
mContext = context;                       // 系统上下文
mAllowBootMessages = showBootMsgs;        // 是否显示启动消息
mOnlyCore = onlyCore;                     // 核心模式标志
```

> **设计要点**：WMS 与 ATM 共享同一把 `mGlobalLock`，保证窗口操作与 Activity 操作的原子性，避免跨锁死锁。

#### 阶段 2：资源配置读取（L1055-L1072）

从系统资源（`config.xml`）中读取窗口管理相关的配置：

| 成员变量 | 资源 ID | 说明 |
|----------|---------|------|
| `mLimitedAlphaCompositing` | `config_sf_limitedAlpha` | 是否限制 Alpha 合成 |
| `mHasPermanentDpad` | `config_hasPermanentDpad` | 是否有永久方向键 |
| `mInTouchMode` | `config_defaultInTouchMode` | 默认是否触摸模式 |
| `mDrawLockTimeoutMillis` | `config_drawLockTimeoutMillis` | 绘制锁超时 |
| `mAllowAnimationsInLowPowerMode` | `config_allowAnimationsInLowPowerMode` | 省电模式允许动画 |
| `mMaxUiWidth` | `config_maxUiWidth` | UI 最大宽度限制 |
| `mDisableTransitionAnimation` | `config_disableTransitionAnimation` | 禁用过渡动画 |
| `mPerDisplayFocusEnabled` | `config_perDisplayFocusEnabled` | 每屏独立焦点 |
| `mLowRamTaskSnapshotsAndRecents` | `config_lowRamTaskSnapshotsAndRecents` | 低内存任务快照 |

#### 阶段 3：核心组件创建（L1073-L1087）

```java
mInputManager = inputManager;              // IMS 引用（必须在 createDisplayContent 之前）
mDisplayManagerInternal = LocalServices.getService(DisplayManagerInternal.class);
mDisplayWindowSettings = new DisplayWindowSettings(this);   // 显示窗口设置持久化

mTransactionFactory = transactionFactory;
mTransaction = mTransactionFactory.make(); // SurfaceFlinger 事务对象
mPolicy = policy;                          // PhoneWindowManager 策略
mAnimator = new WindowAnimator(this);      // 窗口动画管理器
mRoot = new RootWindowContainer(this);     // ★ 窗口容器树根节点

mWindowPlacerLocked = new WindowSurfacePlacer(this);       // 窗口 Surface 布局器
mTaskSnapshotController = new TaskSnapshotController(this); // 任务快照控制器
mWindowTracing = WindowTracing.createDefaultAndStartLooper(...); // 窗口追踪调试
```

**核心数据结构 — 窗口容器树：**

```
RootWindowContainer (mRoot)
    └── DisplayContent (默认屏幕)
            └── TaskStack
                    └── Task
                            └── AppWindowToken
                                    └── WindowState (实际窗口)
```

- RootWindowContainer：容器树根节点，管理所有 DisplayContent，负责焦点更新和遍历操作
- WindowAnimator：在 AnimationThread 中运行，通过 Choreographer 驱动窗口动画帧回调
- WindowSurfacePlacer：负责窗口 Surface 的布局计算和放置（`performSurfacePlacement`）
- TaskSnapshotController：管理任务快照（最近任务缩略图的截图、持久化、缓存）

#### 阶段 4：系统服务获取（L1089-L1128）

```java
LocalServices.addService(WindowManagerPolicy.class, mPolicy);  // 注册窗口策略

mDisplayManager = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
mKeyguardDisableHandler = KeyguardDisableHandler.create(...);  // 键盘锁禁用处理

mPowerManager = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
mPowerManagerInternal = LocalServices.getService(PowerManagerInternal.class);

mActivityManager = ActivityManager.getService();               // AMS Binder 代理
mActivityTaskManager = ActivityTaskManager.getService();       // ATM Binder 代理
mAmInternal = LocalServices.getService(ActivityManagerInternal.class);
mAtmInternal = LocalServices.getService(ActivityTaskManagerInternal.class);
mAppOps = (AppOpsManager) context.getSystemService(Context.APP_OPS_SERVICE);
mPmInternal = LocalServices.getService(PackageManagerInternal.class);
```

#### 阶段 5：省电模式监听（L1098-L1119）

```java
mPowerManagerInternal.registerLowPowerModeObserver(result -> {
    synchronized (mGlobalLock) {
        if (mAnimationsDisabled != enabled && !mAllowAnimationsInLowPowerMode) {
            mAnimationsDisabled = enabled;
            dispatchNewAnimatorScaleLocked(null);  // 动态调整全局动画缩放
        }
    }
});
```

当设备进入省电模式时，自动将动画缩放设为 0（除非配置允许省电模式下播放动画）。

#### 阶段 6：WakeLock 与权限监控（L1120-L1152）

**WakeLock 管理：**
- `mScreenFrozenLock`（PARTIAL_WAKE_LOCK）— 屏幕冻结期间保持 CPU 唤醒
- `mHoldingScreenWakeLock`（SCREEN_BRIGHT_WAKE_LOCK）— 保持屏幕亮起

**AppOps 权限监控：**
```java
mAppOps.startWatchingMode(OP_SYSTEM_ALERT_WINDOW, null, opListener);  // 悬浮窗权限
mAppOps.startWatchingMode(AppOpsManager.OP_TOAST_WINDOW, null, opListener); // Toast 窗口权限
```

当应用的悬浮窗或 Toast 权限发生变化时，触发 `updateAppOpsState()` 重新评估窗口可见性。

**包挂起广播监听：**
```java
context.registerReceiverAsUser(new BroadcastReceiver() {
    public void onReceive(Context context, Intent intent) {
        updateHiddenWhileSuspendedState(...);  // 更新被挂起包的窗口隐藏状态
    }
}, UserHandle.ALL, suspendPackagesFilter, null, null);
```

#### 阶段 7：设置读取与广播注册（L1154-L1191）

**动画设置读取：**
```java
mWindowAnimationScaleSetting = Settings.Global.getFloat(resolver,
        Settings.Global.WINDOW_ANIMATION_SCALE, ...);        // 窗口动画缩放
mTransitionAnimationScaleSetting = Settings.Global.getFloat(resolver,
        Settings.Global.TRANSITION_ANIMATION_SCALE, ...);    // 过渡动画缩放
setAnimatorDurationScale(Settings.Global.getFloat(resolver,
        Settings.Global.ANIMATOR_DURATION_SCALE, ...));      // 动画时长缩放
```

**其他设置：**
- `mForceDesktopModeOnExternalDisplays` — 外接屏幕强制桌面模式（开发者选项）

**广播注册：**
- `ACTION_DEVICE_POLICY_MANAGER_STATE_CHANGED` — 设备策略管理器状态变化（启用/禁用键盘锁）

**交互控制器：**
- `mTaskPositioningController` — 任务窗口拖拽定位控制
- `mDragDropController` — 拖放操作控制
- `mHighRefreshRateBlacklist` — 高刷新率黑名单

#### 阶段 8：系统手势与本地服务（L1193-L1224）

**系统手势排除区域：**
```java
mSystemGestureExclusionLimitDp = Math.max(MIN_GESTURE_EXCLUSION_LIMIT_DP,
        DeviceConfig.getInt(..., KEY_SYSTEM_GESTURE_EXCLUSION_LIMIT_DP, 0));
```

从 `DeviceConfig` 读取手势排除区域限制（dp），并注册动态配置变更监听器，当限制值变化时遍历所有屏幕更新排除区域。

**注册本地服务：**
```java
LocalServices.addService(WindowManagerInternal.class, new LocalService());
```

---

### 四、onInitReady() — 就绪初始化

onInitReady()

在 SystemServer 中 `AMS.setWindowManager(wm)` 之后调用：

```java
public void onInitReady() {
    initPolicy();                              // ① 初始化窗口策略

    Watchdog.getInstance().addMonitor(this);   // ② 注册 Watchdog 监控

    openSurfaceTransaction();
    try {
        createWatermarkInTransaction();        // ③ 创建水印（调试版本标识）
    } finally {
        closeSurfaceTransaction("createWatermarkInTransaction");
    }

    showEmulatorDisplayOverlayIfNeeded();      // ④ 模拟器叠加层
}
```

| 步骤 | 方法 | 说明 |
|------|------|------|
| ① | initPolicy() | 在 **UiThread** 中调用 `PhoneWindowManager.init()`，初始化窗口策略（拦截器、布局规则等） |
| ② | `Watchdog.addMonitor` | 将 WMS 注册为 Watchdog 监控对象，定期检测是否死锁 |
| ③ | `createWatermarkInTransaction` | 在屏幕上创建水印 Surface（显示构建信息，仅 userdebug/eng 版本） |
| ④ | `showEmulatorDisplayOverlayIfNeeded` | 模拟器环境下显示叠加层 |

---

### 五、displayReady() — 显示就绪

displayReady()

在 SystemServer 中所有显示配置完成后调用：

```java
public void displayReady() {
    synchronized (mGlobalLock) {
        if (mMaxUiWidth > 0) {
            mRoot.forAllDisplays(dc -> dc.setMaxUiWidth(mMaxUiWidth));  // 设置最大 UI 宽度
        }
        final boolean changed = applyForcedPropertiesForDefaultDisplay(); // 应用默认显示属性
        mAnimator.ready();                    // 动画管理器就绪
        mDisplayReady = true;                 // ★ 标记显示就绪（此后才允许 addWindow）
        if (changed) {
            reconfigureDisplayLocked(getDefaultDisplayContentLocked()); // 重新配置默认显示
        }
        mIsTouchDevice = mContext.getPackageManager().hasSystemFeature(
                PackageManager.FEATURE_TOUCHSCREEN);
    }
    mActivityTaskManager.updateConfiguration(null); // 通知 ATM 更新配置
    updateCircularDisplayMaskIfNeeded();            // 更新圆角显示遮罩
}
```

---

### 六、完整初始化流程图

```
SystemServer.startOtherServices()
    │
    ├── ① new InputManagerService(context)
    │
    ├── ② WindowManagerService.main(context, im, ...)
    │       │
    │       └── DisplayThread.runWithScissors(() -> new WindowManagerService(...))
    │               │
    │               ├── 锁与基础引用 ── installLock, mGlobalLock(共享ATM)
    │               │
    │               ├── 资源配置读取 ── Alpha合成/方向键/触摸模式/动画/最大宽度等
    │               │
    │               ├── 核心组件创建
    │               │       ├── InputManager 引用
    │               │       ├── SurfaceControl.Transaction
    │               │       ├── PhoneWindowManager (策略)
    │               │       ├── WindowAnimator (动画管理器)
    │               │       ├── RootWindowContainer (容器树根) ★
    │               │       ├── WindowSurfacePlacer (Surface布局)
    │               │       ├── TaskSnapshotController (任务快照)
    │               │       └── WindowTracing (追踪调试)
    │               │
    │               ├── 系统服务获取 ── DisplayManager/PowerManager/AMS/ATM/AppOps/PMS
    │               │
    │               ├── 省电模式监听 ── 注册 LowPowerModeObserver
    │               │
    │               ├── WakeLock 管理 ── ScreenFrozenLock + HoldingScreenWakeLock
    │               │
    │               ├── 权限监控 ── AppOps(悬浮窗/Toast) + 包挂起广播
    │               │
    │               ├── 设置读取 ── 动画缩放/桌面模式/设备策略广播
    │               │
    │               ├── 交互控制 ── 拖拽/定位/高刷黑名单/手势排除
    │               │
    │               └── 本地服务注册 ── WindowManagerInternal
    │
    ├── ③ ServiceManager.addService(WINDOW_SERVICE, wm)
    │
    ├── ④ ServiceManager.addService(INPUT_SERVICE, im)
    │
    ├── ⑤ AMS.setWindowManager(wm)
    │       └── mWindowManager = wm; ATM.setWindowManager(wm)
    │
    ├── ⑥ wm.onInitReady()
    │       ├── initPolicy() → PhoneWindowManager.init() [UiThread]
    │       ├── Watchdog.addMonitor(this)
    │       ├── createWatermarkInTransaction()
    │       └── showEmulatorDisplayOverlayIfNeeded()
    │
    │   ... (SystemServer 继续其他初始化) ...
    │
    └── ⑦ wm.displayReady()
            ├── 设置最大 UI 宽度
            ├── 应用默认显示属性
            ├── mAnimator.ready()
            ├── mDisplayReady = true  ← 此后允许 addWindow
            ├── 重新配置默认显示
            └── 通知 ATM 更新配置
```

---

### 七、WMS 与 IMS 的协作关系

```
                    ┌─────────────────────────────────────┐
                    │           SystemServer              │
                    │                                     │
                    │  IMS ──────────→ WMS               │
                    │  (InputManager)   (WindowManager)   │
                    │       │               │             │
                    │       │  共享 DisplayThread          │
                    │       │  共享 mGlobalLock (via ATM)  │
                    │       │               │             │
                    │       ▼               ▼             │
                    │  ┌─────────────────────────┐       │
                    │  │   PhoneWindowManager     │       │
                    │  │   (窗口策略实现)          │       │
                    │  └─────────────────────────┘       │
                    └─────────────────────────────────────┘
```

- **IMS 先于 WMS 创建**：WMS 构造方法中需要 IMS 引用来处理输入事件分发
- **共享 DisplayThread**：两者都在 DisplayThread 中创建和运行，保证低延迟
- **通过 PhoneWindowManager 协作**：IMS 通过 `WindowManagerCallbacks` 回调 WMS，WMS 通过 `PhoneWindowManager` 处理拦截逻辑（如电源键、Home 键）
- **共享全局锁**：WMS 通过 `atm.getGlobalLock()` 与 ATM 共享同一把锁，保证窗口操作与 Activity 操作的原子性

**总结**：WMS 的初始化是一个**分阶段、跨线程**的过程。构造方法在 DisplayThread 中完成核心组件创建和系统服务关联；`onInitReady()` 在 UiThread 中完成策略初始化；`displayReady()` 最终标记显示系统就绪。整个过程中，WMS 建立了以 `RootWindowContainer` 为核心的窗口容器树，并与 IMS、AMS、PowerManager 等系统服务建立了紧密的协作关系。

## 3. wms 事件获取和分发  ims 事件读取和分发

让我全面搜索 IMS 和 WMS 的事件读取与分发相关代码。



## IMS 事件读取与分发 + WMS 事件获取与分发 详解

---

### 一、整体事件流架构图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          内核空间                                        │
│   /dev/input/event0  /dev/input/event1  /dev/input/event2 ...          │
│   (触摸屏)            (按键)             (鼠标/轨迹球)                    │
└────────────┬────────────────────────────────────────────────────────────┘
             │ read()
             ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  Native 层 — InputReaderThread (优先级: PRIORITY_URGENT_DISPLAY)         │
│                                                                         │
│  EventHub::getEvents()        ← 从 /dev/input/ 读取原始内核事件           │
│       ↓                                                                 │
│  InputReader::loopOnce()      ← 主循环                                   │
│       ↓                                                                 │
│  InputReader::processEventsLocked() ← 按设备分批处理                      │
│       ↓                                                                 │
│  InputDevice::process()       ← 各 Mapper 转换坐标/按键映射               │
│       ↓                                                                 │
│  InputClassifier::notifyMotion/Key() ← 事件分类（触摸手势识别）            │
│       ↓                                                                 │
│  InputDispatcher::notifyMotion/Key() ← 加入 InboundQueue               │
└────────────┬────────────────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  Native 层 — InputDispatcherThread (优先级: PRIORITY_URGENT_DISPLAY)     │
│                                                                         │
│  InputDispatcher::dispatchOnce()          ← 分发主循环                   │
│       ↓                                                                 │
│  InputDispatcher::dispatchOnceInnerLocked() ← 从 InboundQueue 取事件     │
│       ↓                                                                 │
│  ┌─ Policy 拦截 ─────────────────────────────────────────────────────┐  │
│  │ interceptKeyBeforeQueueing() → JNI → Java PhoneWindowManager     │  │
│  │ interceptKeyBeforeDispatching() → JNI → Java PhoneWindowManager  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│       ↓                                                                 │
│  dispatchKey/Motion Locked()   ← 查找目标窗口 (InputTarget)              │
│       ↓                                                                 │
│  dispatchEventLocked()         ← 遍历 InputTarget 列表                   │
│       ↓                                                                 │
│  prepareDispatchCycleLocked()  ← 通过 Connection 序列化事件               │
│       ↓                                                                 │
│  InputChannel (Socket)         ← 通过 Unix Socket 发送到应用进程          │
└────────────┬────────────────────────────────────────────────────────────┘
             │ Unix Domain Socket
             ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  Java 层 — 应用进程                                                      │
│                                                                         │
│  InputEventReceiver::onInputEvent()  ← Looper 监听到 Socket 可读         │
│       ↓                                                                 │
│  WindowInputEventReceiver::onInputEvent() ← ViewRootImpl 内部类          │
│       ↓                                                                 │
│  ViewRootImpl::enqueueInputEvent()   ← 加入事件队列                      │
│       ↓                                                                 │
│  ViewRootImpl::doProcessInputEvents() ← 遍历队列处理                      │
│       ↓                                                                 │
│  View::dispatchKeyEvent/MotionEvent() ← 最终到达 View 层级               │
└─────────────────────────────────────────────────────────────────────────┘
```

---

### 二、IMS 事件读取（InputReader 层）

#### 2.1 InputReaderThread 主循环

InputReaderThread::threadLoop()

```cpp
bool InputReaderThread::threadLoop() {
    mReader->loopOnce();  // 每次循环读取并处理一批事件
    return true;          // 持续运行
}
```

#### 2.2 EventHub — 从内核读取原始事件

EventHub::getEvents()

```cpp
size_t EventHub::getEvents(int timeoutMillis, RawEvent* buffer, size_t bufferSize) {
    struct input_event readBuffer[bufferSize];  // Linux 内核输入事件缓冲区

    for (;;) {
        // 1. 检查是否需要重新打开设备
        if (mNeedToReopenDevices) { closeAllDevicesLocked(); ... }

        // 2. 报告已关闭/已添加的设备
        while (mClosingDevices) { ... generate DEVICE_REMOVED ... }
        while (mOpeningDevices != nullptr) { ... generate DEVICE_ADDED ... }

        // 3. 使用 epoll 等待内核事件
        //    监听所有 /dev/input/eventX 文件描述符
        struct epoll_event eventBuffer[EPOLL_MAX_EVENTS];
        int eventCount = epoll_wait(mEpollFd, eventBuffer, EPOLL_MAX_EVENTS, timeoutMillis);

        // 4. 从设备文件读取原始事件
        for (int i = 0; i < eventCount; i++) {
            int fd = eventBuffer[i].data.fd;
            if (fd == mWakeReadFd) { /* 唤醒事件 */ }
            else {
                // 从 /dev/input/eventX 读取 Linux input_event 结构
                size_t size = read(fd, readBuffer, sizeof(struct input_event) * capacity);
                // 转换为 RawEvent 格式
                for (size_t j = 0; j < size / sizeof(struct input_event); j++) {
                    event->deviceId = deviceId;
                    event->type = readBuffer[j].type;    // EV_KEY / EV_ABS / EV_REL ...
                    event->code = readBuffer[j].code;    // 具体键码/轴码
                    event->value = readBuffer[j].value;  // 按键状态/坐标值
                }
            }
        }
    }
}
```

**核心机制**：通过 `epoll` 同时监听所有输入设备文件描述符，当任何设备有数据可读时立即唤醒并批量读取。

#### 2.3 InputReader::loopOnce() — 事件处理主循环

InputReader::loopOnce()

```cpp
void InputReader::loopOnce() {
    // 1. 从 EventHub 获取原始事件（阻塞等待）
    size_t count = mEventHub->getEvents(timeoutMillis, mEventBuffer, EVENT_BUFFER_SIZE);

    {
        AutoMutex _l(mLock);

        // 2. 按设备分批处理事件
        if (count) {
            processEventsLocked(mEventBuffer, count);
        }

        // 3. 处理超时（如手势超时）
        if (mNextTimeout != LLONG_MAX) {
            if (now >= mNextTimeout) {
                timeoutExpiredLocked(now);
            }
        }

        // 4. 如果有设备变化，通知 listener
        if (inputDevicesChanged) {
            mListener->onInputDevicesChanged(inputDevices);
        }
    }
}
```

#### 2.4 processEventsLocked — 按设备分类处理

InputReader::processEventsLocked()

```cpp
void InputReader::processEventsLocked(const RawEvent* rawEvents, size_t count) {
    for (const RawEvent* rawEvent = rawEvents; count;) {
        int32_t type = rawEvent->type;

        if (type < FIRST_SYNTHETIC_EVENT) {
            // 普通硬件事件（按键/触摸/鼠标移动等）
            // 按相同 deviceId 批量处理
            processEventsForDeviceLocked(deviceId, rawEvent, batchSize);
        } else {
            // 合成事件（设备添加/移除/扫描完成）
            switch (type) {
                case DEVICE_ADDED:   addDeviceLocked(...); break;
                case DEVICE_REMOVED: removeDeviceLocked(...); break;
                case FINISHED_DEVICE_SCAN: handleConfigurationChangedLocked(...); break;
            }
        }
    }
}
```

#### 2.5 InputDevice::process() — 各 Mapper 处理

InputReader::processEventsForDeviceLocked()

```cpp
void InputReader::processEventsForDeviceLocked(int32_t deviceId,
        const RawEvent* rawEvents, size_t count) {
    InputDevice* device = mDevices.valueAt(deviceIndex);
    device->process(rawEvents, count);
    // 内部遍历所有 Mapper：
    //   SwitchMapper    — 翻盖/滑块等开关事件
    //   KeyboardMapper  — 键码映射（扫描码 → 键码）
    //   TouchMapper     — 触摸坐标旋转/缩放
    //   CursorMapper    — 鼠标/轨迹球坐标转换
    //   VibratorMapper  — 振动反馈
    // 最终调用 notifyKey() / notifyMotion() 传递给 Classifier
}
```

---

### 三、IMS 事件分类（InputClassifier 层）

InputClassifier::notifyMotion()

```cpp
void InputClassifier::notifyMotion(const NotifyMotionArgs* args) {
    // 仅触摸事件需要分类
    const bool sendToMotionClassifier = mMotionClassifier && isTouchEvent(*args);
    if (!sendToMotionClassifier) {
        mListener->notifyMotion(args);  // 非触摸事件直接传递给 Dispatcher
        return;
    }

    // 触摸事件经过 MotionClassifier 分类
    NotifyMotionArgs newArgs(*args);
    newArgs.classification = mMotionClassifier->classify(newArgs);
    // 分类结果：AMBIGUOUS_GESTURE / CERTAIN_GESTURE / NO_GESTURE 等
    mListener->notifyMotion(&newArgs);  // 传递给 InputDispatcher
}
```

---

### 四、IMS 事件分发（InputDispatcher 层）

#### 4.1 InputDispatcherThread 主循环

InputDispatcherThread::threadLoop()

```cpp
bool InputDispatcherThread::threadLoop() {
    mDispatcher->dispatchOnce();  // 每次循环执行一次分发
    return true;
}
```

#### 4.2 InputDispatcher::dispatchOnce()

InputDispatcher::dispatchOnce()

```cpp
void InputDispatcher::dispatchOnce() {
    nsecs_t nextWakeupTime = LONG_LONG_MAX;
    {
        std::scoped_lock _l(mLock);

        // 1. 如果没有待处理的命令，执行分发循环
        if (!haveCommandsLocked()) {
            dispatchOnceInnerLocked(&nextWakeupTime);
        }

        // 2. 执行所有待处理的命令
        if (runCommandsLockedInterruptible()) {
            nextWakeupTime = LONG_LONG_MIN;  // 立即唤醒
        }
    }

    // 3. 等待下一次唤醒（通过 Looper epoll）
    int timeoutMillis = toMillisecondTimeoutDelay(currentTime, nextWakeupTime);
    mLooper->pollOnce(timeoutMillis);
}
```

#### 4.3 dispatchOnceInnerLocked — 核心分发逻辑

InputDispatcher::dispatchOnceInnerLocked()

```cpp
void InputDispatcher::dispatchOnceInnerLocked(nsecs_t* nextWakeupTime) {
    // 1. 分发冻结时不处理任何事件
    if (mDispatchFrozen) return;

    // 2. 如果没有待分发事件，从 InboundQueue 取出一个
    if (!mPendingEvent) {
        if (mInboundQueue.isEmpty()) {
            // 检查是否需要合成按键重复事件
            if (mKeyRepeatState.lastKeyEntry && currentTime >= nextRepeatTime) {
                mPendingEvent = synthesizeKeyRepeatLocked(currentTime);
            }
            if (!mPendingEvent) return;  // 无事可做
        } else {
            mPendingEvent = mInboundQueue.dequeueAtHead();  // 从队列头部取出
        }
        pokeUserActivityLocked(mPendingEvent);  // 用户活动检测
        resetANRTimeoutsLocked();                // 重置 ANR 计时
    }

    // 3. 根据事件类型分发
    switch (mPendingEvent->type) {
        case TYPE_CONFIGURATION_CHANGED:
            dispatchConfigurationChangedLocked(...); break;
        case TYPE_DEVICE_RESET:
            dispatchDeviceResetLocked(...); break;
        case TYPE_KEY:
            dispatchKeyLocked(...); break;       // ★ 按键事件分发
        case TYPE_MOTION:
            dispatchMotionLocked(...); break;    // ★ 触摸/鼠标事件分发
    }
}
```

#### 4.4 Policy 拦截 — 系统按键拦截

在分发到应用之前，InputDispatcher 会通过 JNI 回调 Java 层的 `PhoneWindowManager` 进行策略拦截：

**Native → JNI → Java 调用链：**

```
InputDispatcher (Native)
    │
    ├── interceptKeyBeforeQueueing()     ← 按键入队前拦截
    │       ↓ JNI
    │   NativeInputManager::interceptKeyBeforeQueueing()
    │       ↓ JNI CallIntMethod
    │   InputManagerService.interceptKeyBeforeQueueing()
    │       ↓
    │   WindowManagerCallbacks.interceptKeyBeforeQueueing()
    │       ↓
    │   PhoneWindowManager.interceptKeyBeforeQueueing()  ← 处理电源键/Home键/音量键等
    │
    └── interceptKeyBeforeDispatching()  ← 按键分发前拦截
            ↓ (同上调用链)
        PhoneWindowManager.interceptKeyBeforeDispatching()  ← 处理菜单键等
```

PhoneWindowManager.interceptKeyBeforeQueueing() 负责拦截：
- **电源键** → 亮屏/锁屏/关机对话框
- **Home 键** → 回到桌面
- **音量键** → 音量调节
- **ENDCALL 键** → 挂断电话
- **MENU 键** → 菜单显示

#### 4.5 dispatchEventLocked — 发送到目标窗口

InputDispatcher::dispatchEventLocked()

```cpp
void InputDispatcher::dispatchEventLocked(nsecs_t currentTime,
        EventEntry* eventEntry, const std::vector<InputTarget>& inputTargets) {
    pokeUserActivityLocked(eventEntry);

    for (const InputTarget& inputTarget : inputTargets) {
        ssize_t connectionIndex = getConnectionIndexLocked(inputTarget.inputChannel);
        if (connectionIndex >= 0) {
            sp<Connection> connection = mConnectionsByFd.valueAt(connectionIndex);
            // 通过 Connection 将事件序列化并通过 InputChannel 发送
            prepareDispatchCycleLocked(currentTime, connection, eventEntry, &inputTarget);
        }
    }
}
```

**InputTarget 查找过程**（以触摸事件为例）：
1. 根据触摸坐标查找命中的窗口（`findTouchedWindowLocked`）
2. 考虑窗口层级（Z-order）、可见性、可触摸性
3. 处理多窗口同时触摸（split touch）
4. 返回 `InputTarget` 列表（包含 InputChannel 和坐标偏移）

---

### 五、WMS 事件接收（Java 应用层）

#### 5.1 InputChannel — 进程间通信管道

```
InputDispatcher (system_server 进程)
    │
    │  Unix Domain Socket（双向管道）
    │  ┌──────────────────────────┐
    │  │  InputChannel (server端)  │ ←→  InputChannel (client端)
    │  └──────────────────────────┘
    │
    ▼
ViewRootImpl (应用进程)
```

当 WMS 调用 `addWindow()` 时，会创建一对 `InputChannel`：
- 服务端注册到 InputDispatcher 的 Connection
- 客户端传递给应用进程的 ViewRootImpl

#### 5.2 InputEventReceiver — 接收输入事件

InputEventReceiver

```java
public abstract class InputEventReceiver {
    private InputChannel mInputChannel;
    private MessageQueue mMessageQueue;

    public InputEventReceiver(InputChannel inputChannel, Looper looper) {
        mInputChannel = inputChannel;
        mMessageQueue = looper.getQueue();
        // JNI 初始化：将 InputChannel 注册到 Looper 的 epoll 中
        mReceiverPtr = nativeInit(new WeakReference<>(this), inputChannel, looper.getQueue());
    }

    // 当 epoll 检测到 InputChannel 可读时回调
    public void onInputEvent(InputEvent event) { ... }
}
```

#### 5.3 WindowInputEventReceiver — ViewRootImpl 内部类

WindowInputEventReceiver

```java
final class WindowInputEventReceiver extends InputEventReceiver {
    @Override
    public void onInputEvent(InputEvent event) {
        // 兼容性处理
        List<InputEvent> processedEvents =
                mInputCompatProcessor.processInputEventForCompatibility(event);
        // 入队到 ViewRootImpl 的事件队列
        enqueueInputEvent(event, this, 0, true);  // processImmediately = true
    }
}
```

#### 5.4 ViewRootImpl 事件队列处理

ViewRootImpl.enqueueInputEvent()

```java
void enqueueInputEvent(InputEvent event, InputEventReceiver receiver,
        int flags, boolean processImmediately) {
    QueuedInputEvent q = obtainQueuedInputEvent(event, receiver, flags);

    // 加入链表队列尾部（保证顺序）
    QueuedInputEvent last = mPendingInputEventTail;
    if (last == null) {
        mPendingInputEventHead = q;
        mPendingInputEventTail = q;
    } else {
        last.mNext = q;
        mPendingInputEventTail = q;
    }
    mPendingInputEventCount++;

    if (processImmediately) {
        doProcessInputEvents();    // 立即处理
    } else {
        scheduleProcessInputEvents();  // 延迟到下一帧处理
    }
}
```

最终调用链：
```
doProcessInputEvents()
    → deliverInputEvent()
        → processInputEvent()
            → View.dispatchKeyEvent() / View.dispatchTouchEvent()
                → Activity.onKeyDown() / Activity.onTouchEvent()
```

---

### 六、完整事件生命周期总结

```
用户触摸屏幕
    │
    ▼ [内核]
    /dev/input/event0 产生 EV_ABS 事件
    │
    ▼ [InputReaderThread]
    EventHub::getEvents() ← epoll_wait 唤醒 + read() 读取
    │
    ▼
    InputReader::loopOnce()
    │
    ├── processEventsLocked() ← 按设备分批
    │       ↓
    │   InputDevice::process() ← TouchMapper 坐标转换
    │       ↓
    │   notifyMotion() ← 传递给 Classifier
    │
    ▼ [InputClassifier]
    MotionClassifier::classify() ← 手势分类（模糊/确定/无）
    │
    ▼
    InputDispatcher::notifyMotion() ← 加入 InboundQueue
    │
    ▼ [InputDispatcherThread]
    InputDispatcher::dispatchOnce()
    │
    ├── dispatchOnceInnerLocked()
    │       ↓
    │   mInboundQueue.dequeueAtHead() ← 取出事件
    │       ↓
    │   dispatchMotionLocked()
    │       ↓
    │   findTouchedWindowLocked() ← 查找触摸命中的窗口
    │       ↓
    │   dispatchEventLocked()
    │       ↓
    │   prepareDispatchCycleLocked() ← 序列化事件
    │       ↓
    │   Connection::send → InputChannel (Socket写入)
    │
    ▼ [Unix Domain Socket]
    跨进程传输到应用进程
    │
    ▼ [应用进程 - 主线程]
    InputEventReceiver (epoll 监听 Socket 可读)
    │
    ▼
    WindowInputEventReceiver::onInputEvent()
    │
    ▼
    ViewRootImpl::enqueueInputEvent()
    │
    ▼
    ViewRootImpl::doProcessInputEvents()
    │
    ▼
    DecorView → ViewGroup → View
    Activity.dispatchTouchEvent() / View.onTouchEvent()
```

**关键设计要点：**

| 设计 | 说明 |
|------|------|
| **双线程架构** | Reader 和 Dispatcher 各自独立线程，互不阻塞 |
| **epoll 驱动** | EventHub 和 InputEventReceiver 都使用 epoll 实现零延迟事件感知 |
| **策略拦截** | 通过 `PhoneWindowManager` 在入队前和分发前两次拦截系统按键 |
| **Socket 通信** | InputChannel 基于 Unix Domain Socket，支持跨进程零拷贝传输 |
| **事件队列** | 应用层使用链表队列保证事件顺序，支持立即/延迟处理 |
| **ANR 监控** | Dispatcher 对每个事件设置超时计时器，应用未及时处理则触发 ANR |

## 4. ams 负责进程间通信

让我查找 AMS 中与进程间通信相关的核心代码。



## AMS (ActivityManagerService) 进程间通信详解

---

### 一、AMS 在 IPC 体系中的核心地位

AMS 是 Android 系统中**最重要的 IPC 枢纽**，它本身通过 Binder 机制暴露服务接口，同时协调所有应用进程之间的通信。

```
┌─────────────────────────────────────────────────────────────────┐
│                     system_server 进程                           │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  ActivityManagerService (AMS)                            │   │
│  │  extends IActivityManager.Stub                           │   │
│  │  ─────────────────────────────────────                   │   │
│  │  • 四大组件生命周期管理                                     │   │
│  │  • 进程创建/销毁/调度                                      │   │
│  │  • ContentProvider 连接管理                                │   │
│  │  • Service 绑定管理                                        │   │
│  │  • 广播分发                                                │   │
│  └──────────────────────────────────────────────────────────┘   │
│           ↑ Binder IPC                    ↑ Binder IPC           │
│           │                               │                      │
└───────────┼───────────────────────────────┼──────────────────────┘
            │                               │
    ┌───────┴────────┐              ┌───────┴────────┐
    │  应用进程 A      │              │  应用进程 B      │
    │                │              │                │
    │ ActivityThread │              │ ActivityThread │
    │ ┌────────────┐ │              │ ┌────────────┐ │
    │ │Application │ │              │ │Application │ │
    │ │Thread(Stub)│←┼── Binder ───┼→│Thread(Stub)│ │
    │ └────────────┘ │              │ └────────────┘ │
    └────────────────┘              └────────────────┘
```

---

### 二、AMS 的 Binder 服务注册

AMS 继承自 IActivityManager.Stub，是一个标准的 Binder 服务端：

```java
public class ActivityManagerService extends IActivityManager.Stub
        implements Watchdog.Monitor, BatteryStatsImpl.BatteryCallback {
```

在 start() 中注册到 ServiceManager：

```java
ServiceManager.addService(Context.ACTIVITY_SERVICE, this, /* allowIsolated= */ true,
        DUMP_FLAG_PRIORITY_CRITICAL | DUMP_FLAG_PRIORITY_NORMAL | DUMP_FLAG_PROTO);
ServiceManager.addService(ProcessStats.SERVICE_NAME, mProcessStats);
ServiceManager.addService("meminfo", new MemBinder(this), ...);
ServiceManager.addService("gfxinfo", new GraphicsBinder(this));
ServiceManager.addService("dbinfo", new DbBinder(this));
ServiceManager.addService("cpuinfo", new CpuBinder(this), ...);
ServiceManager.addService("permission", new PermissionController(this));
ServiceManager.addService("processinfo", new ProcessInfoService(this));
```

AMS 不仅注册自身，还注册了多个辅助 Binder 服务（meminfo、gfxinfo、cpuinfo 等），供其他进程查询系统状态。

---

### 三、AMS IPC 通信的五大场景

#### 3.1 Activity 跨进程启动

IActivityManager.aidl 定义了 AMS 对外暴露的 Binder 接口：

```java
interface IActivityManager {
    ParcelFileDescriptor openContentUri(in String uriString);
    void registerUidObserver(...);
    // ... 数十个跨进程接口方法
}
```

AMS.startActivity() — 应用 A 通过 Binder 调用 AMS 启动应用 B 的 Activity：

```java
public int startActivity(IApplicationThread caller, String callingPackage,
        Intent intent, String resolvedType, IBinder resultTo, ...) {
    return mActivityTaskManager.startActivity(caller, callingPackage, intent, ...);
}
```

**流程：**
```
应用A: startActivity(intent)
    │ Binder 调用
    ▼
AMS: startActivity() → ATM.startActivity()
    │ 解析 Intent，找到目标 Activity
    │ 如果目标进程不存在 → 创建新进程
    │ 如果目标进程已存在 → 通过 Binder 回调目标进程
    ▼
应用B: ApplicationThread.scheduleLaunchActivity()
    │ 通过 Handler 投递到主线程
    ▼
应用B: Activity.onCreate() 执行
```

#### 3.2 进程创建与应用绑定

AMS.attachApplication() — 新进程启动后向 AMS 注册：

```java
public final void attachApplication(IApplicationThread thread, long startSeq) {
    synchronized (this) {
        int callingPid = Binder.getCallingPid();
        final int callingUid = Binder.getCallingUid();
        final long origId = Binder.clearCallingIdentity();
        attachApplicationLocked(thread, callingPid, callingUid, startSeq);
        Binder.restoreCallingIdentity(origId);
    }
}
```

**进程启动与绑定流程：**
```
AMS: startProcess() → Zygote fork 新进程
    │
    ▼ (新进程)
ActivityThread.main()
    │ 创建 ActivityThread 实例
    │ 创建 ApplicationThread (IApplicationThread.Stub)
    │
    │ Binder 回调 AMS
    ▼
AMS: attachApplication(thread)
    │ 保存 ApplicationThread 的 Binder 引用
    │ 存入 ProcessRecord.app (IApplicationThread)
    │
    │ 通过 ApplicationThread Binder 回调新进程
    ▼
应用进程: ApplicationThread.bindApplication()
    │ 安装 ContentProvider、初始化 Application
```

#### 3.3 Service 绑定（跨进程）

AMS.startService() — 启动跨进程服务：

```java
public ComponentName startService(IApplicationThread caller, Intent service,
        String resolvedType, boolean requireForeground, String callingPackage, int userId) {
    enforceNotIsolatedCaller("startService");
    synchronized(this) {
        final int callingPid = Binder.getCallingPid();
        final int callingUid = Binder.getCallingUid();
        final long origId = Binder.clearCallingIdentity();
        // ... 查找/创建 ServiceRecord，调度到目标进程
    }
}
```

AMS.publishService() — 服务进程将 Binder 对象发布给绑定方：

```java
public void publishService(IBinder token, Intent intent, IBinder service) {
    synchronized(this) {
        if (!(token instanceof ServiceRecord)) {
            throw new IllegalArgumentException("Invalid service token");
        }
        mServices.publishServiceLocked((ServiceRecord)token, intent, service);
    }
}
```

**Service 绑定流程：**
```
应用A: bindService(intent, conn)
    │ Binder
    ▼
AMS: bindService() → 查找/创建目标 Service 进程
    │
    ▼ (服务进程)
ApplicationThread.scheduleCreateService()
    │ 创建 Service 实例
    │ 调用 Service.onBind() → 返回 IBinder
    │
    │ Binder 回调 AMS
    ▼
AMS: publishService(token, intent, service_binder)
    │ 将 service_binder 传递给应用A
    │
    ▼ (应用A)
ApplicationThread.scheduleBindService()
    │ ServiceConnection.onServiceConnected(service_binder)
    │ 应用A 获得服务端的 Binder 引用，可直接跨进程调用
```

#### 3.4 ContentProvider 跨进程数据共享

AMS.getContentProviderExternal() — 获取 ContentProvider 的跨进程引用：

```java
public ContentProviderHolder getContentProviderExternal(
        String name, int userId, IBinder token, String tag) {
    enforceCallingPermission(...);
    userId = mUserController.handleIncomingUser(...);
    return getContentProviderExternalUnchecked(name, token, ...);
}
```

AMS.publishContentProviders() — 进程发布其 ContentProvider：

```java
public final void publishContentProviders(IApplicationThread caller,
        List<ContentProviderHolder> providers) {
    synchronized (this) {
        final ProcessRecord r = getRecordForAppLocked(caller);
        // 将 Provider 的 Binder 引用注册到 AMS 的映射表中
        // 其他进程可通过 AMS 获取此 Provider 的代理
    }
}
```

**ContentProvider 跨进程流程：**
```
应用A: ContentResolver.query(uri)
    │
    ▼
AMS: getContentProvider("authority")
    │ 查找 Provider 所在进程
    │ 如果进程未启动 → 先启动进程
    │ 返回 ContentProviderHolder (含 IContentProvider Binder代理)
    │
    ▼ (Provider进程)
ApplicationThread.bindApplication(providers)
    │ 创建 ContentProvider 实例
    │
    │ Binder 回调
    ▼
AMS: publishContentProviders(providers)
    │ 将 Provider Binder 注册到全局映射
    │
    ▼ (应用A 获得 IContentProvider 代理)
应用A: IContentProvider.query() → 跨进程读取数据
```

#### 3.5 广播跨进程分发

```
应用A: sendBroadcast(intent)
    │ Binder
    ▼
AMS: broadcastIntent()
    │ 查找匹配的 BroadcastReceiver
    │ 可能涉及多个进程
    │
    ▼ (各目标进程)
ApplicationThread.scheduleReceiver(intent, info, ...)
    │ 通过 Handler 投递到主线程
    ▼
应用B/C/D: BroadcastReceiver.onReceive()
```

---

### 四、AMS IPC 通信的核心架构

#### 4.1 双向 Binder 通信模型

```
┌──────────────────────────────────────────────────────┐
│  system_server 进程                                   │
│                                                      │
│  AMS (IActivityManager.Stub)                         │
│  ↑ 持有所有进程的 IApplicationThread Binder 引用       │
│  │                                                   │
│  │ ProcessRecord {                                   │
│  │   app: IApplicationThread  ← 应用进程的 Binder 代理│
│  │   pid, uid, processName                           │
│  │ }                                                 │
└──┼───────────────────────────────────────────────────┘
   │
   │  ← AMS 通过 IApplicationThread 向应用进程发指令
   │  → 应用通过 IActivityManager 向 AMS 发请求
   │
┌──┼───────────────────────────────────────────────────┐
│  │  应用进程                                          │
│  │                                                    │
│  │  ActivityThread                                    │
│  │  ├── ApplicationThread (IApplicationThread.Stub)   │
│  │  │   ← AMS 通过此 Binder 回调应用进程               │
│  │  │                                                 │
│  │  ├── ActivityManager.getService()                  │
│  │  │   → 获取 AMS 的 Binder 代理 (IActivityManager)  │
│  │  │   ← 应用通过此代理调用 AMS                       │
│  │  │                                                 │
│  │  └── H (Handler)                                   │
│  │      ← Binder 线程收到请求后投递到主线程处理          │
└──────────────────────────────────────────────────────┘
```

**关键点**：AMS 与应用进程之间是**双向 Binder 通信**：
- **应用 → AMS**：通过 `IActivityManager` 代理（`ActivityManager.getService()`）
- **AMS → 应用**：通过 `IApplicationThread` 代理（保存在 `ProcessRecord.app` 中）

#### 4.2 IApplicationThread — AMS 向应用发指令的通道

IApplicationThread.aidl 定义了 AMS 可以向应用进程发送的所有指令：

```java
oneway interface IApplicationThread {
    void scheduleReceiver(in Intent intent, in ActivityInfo info, ...);  // 广播
    void scheduleCreateService(IBinder token, in ServiceInfo info, ...); // 创建服务
    void scheduleStopService(IBinder token);                             // 停止服务
    void bindApplication(in String packageName, in ApplicationInfo info,
            in List<ProviderInfo> providers, ...);                       // 绑定应用
    // ... 还有 scheduleLaunchActivity, scheduleDestroyActivity 等
}
```

所有方法都是 `oneway`（异步单向调用），AMS 发出指令后立即返回，不等待应用进程处理完成。

#### 4.3 ProcessRecord — 进程信息记录

ProcessRecord 保存了 AMS 对每个进程的管理信息：

```java
class ProcessRecord implements WindowProcessListener {
    private final ActivityManagerService mService;
    final ApplicationInfo info;    // 应用信息
    final boolean isolated;        // 是否隔离进程
    final int uid;                 // 进程 UID
    final int userId;              // 用户 ID
    final String processName;      // 进程名
    final PackageList pkgList;     // 运行的包列表
    // ...
    IApplicationThread thread;     // ★ 应用进程的 Binder 代理
    // ...
}
```

---

### 五、AMS IPC 通信总结

| 通信场景 | 应用→AMS 调用 | AMS→应用 回调 | 传输的 Binder 对象 |
|----------|--------------|--------------|-------------------|
| **Activity 启动** | `startActivity()` | `scheduleLaunchActivity()` | Intent + IBinder token |
| **Service 绑定** | `bindService()` | `scheduleCreateService()` → `publishService()` | Service 的 IBinder |
| **ContentProvider** | `getContentProvider()` | `bindApplication(providers)` → `publishContentProviders()` | IContentProvider |
| **广播发送** | `broadcastIntent()` | `scheduleReceiver()` | Intent |
| **进程注册** | `attachApplication()` | `bindApplication()` | IApplicationThread |

**总结**：AMS 是 Android 进程间通信的**中央调度器**。它通过 `IActivityManager` Binder 接口接收所有应用的请求，通过 `IApplicationThread` Binder 接口向各应用进程下发指令。所有四大组件的跨进程交互（Activity 启动、Service 绑定、ContentProvider 数据共享、广播分发）都必须经过 AMS 中转协调。AMS 内部通过 `ProcessRecord` 维护每个进程的 Binder 引用，实现了完整的进程生命周期管理和 IPC 通信调度。

## 5. IApplicationThread 实例

## IApplicationThread 接口与实例详解

---

### 一、接口定义

IApplicationThread.aidl 是 AMS 与应用进程之间的 **单向(oneway) Binder 接口**，AMS 通过它向应用进程发送指令：

```java
oneway interface IApplicationThread {
    // ── 四大组件相关 ──
    void scheduleReceiver(...);           // 分发广播给 BroadcastReceiver
    void scheduleCreateService(...);      // 创建 Service
    void scheduleBindService(...);        // 绑定 Service
    void scheduleUnbindService(...);      // 解绑 Service
    void scheduleServiceArgs(...);        // 传递 Service 启动参数
    void scheduleStopService(...);        // 停止 Service
    void scheduleTransaction(...);        // 事务化执行 Activity 生命周期（现代方式）

    // ── 应用绑定 ──
    void bindApplication(...);            // 绑定应用（初始化 Application + ContentProvider）

    // ── 系统控制 ──
    void scheduleExit();                  // 退出应用
    void scheduleSuicide();               // 自杀
    void scheduleLowMemory();             // 低内存通知
    void scheduleTrimMemory(int level);   // 内存修剪
    void setSchedulingGroup(int group);   // 设置调度组
    void processInBackground();           // 后台处理

    // ── 调试/Dump ──
    void dumpHeap(...);                   // 堆转储
    void dumpService(...);                // 服务信息转储
    void dumpActivity(...);               // Activity 信息转储
    void dumpMemInfo(...);                // 内存信息转储
    void profilerControl(...);            // 性能分析控制

    // ── 其他 ──
    void updateTimeZone();                // 更新时区
    void clearDnsCache();                 // 清除 DNS 缓存
    void updateHttpProxy();               // 更新 HTTP 代理
    void dispatchPackageBroadcast(...);   // 分发包广播
    void scheduleCrash(String msg);       // 强制崩溃（调试用）
}
```

**关键特性**：`oneway` 关键字表示所有方法都是**异步单向调用** — AMS 发出指令后立即返回，不等待应用进程处理完成。

---

### 二、唯一实现：ApplicationThread

ApplicationThread 是 `IApplicationThread.Stub` 的唯一实现，定义在 ActivityThread 内部：

```java
public class ActivityThread extends ClientTransactionHandler {
    // 在 ActivityThread 构造时立即创建
    final ApplicationThread mAppThread = new ApplicationThread();
    final Looper mLooper = Looper.myLooper();   // 主线程 Looper
    final H mH = new H();                       // 主线程 Handler

    private class ApplicationThread extends IApplicationThread.Stub {
        // ★ 所有方法都运行在 Binder 线程池
        // ★ 通过 sendMessage() 将任务转发到主线程
    }
}
```

**创建时机**：`ActivityThread` 构造时立即实例化 `mAppThread`，然后将其 Binder 引用通过 `attachApplication()` 传递给 AMS。

---

### 三、核心设计模式：Binder 线程 → 主线程转发

ApplicationThread 的所有方法都运行在 **Binder 线程池**中，但 Android UI 操作必须在**主线程**执行。因此采用统一的转发模式：

```
AMS (system_server 进程)
    │ Binder IPC (跨进程)
    ▼
ApplicationThread 方法 (Binder 线程)
    │ sendMessage(H.XXX, data)
    ▼
H (Handler) 消息队列 (主线程)
    │ mLooper.dispatchMessage()
    ▼
handleXxx() 实际执行 (主线程)
```

sendMessage() 的实现：

```java
private void sendMessage(int what, Object obj, int arg1, int arg2, boolean async) {
    Message msg = Message.obtain();
    msg.what = what;
    msg.obj = obj;
    msg.arg1 = arg1;
    msg.arg2 = arg2;
    if (async) {
        msg.setAsynchronous(true);  // 异步消息，可跳过同步屏障
    }
    mH.sendMessage(msg);            // 投递到主线程 Handler
}
```

---

### 四、关键方法实现详解

#### 4.1 bindApplication — 应用绑定（最复杂的方法）

bindApplication() 在新进程启动时被 AMS 调用，传递应用初始化所需的全部信息：

```java
public final void bindApplication(String processName, ApplicationInfo appInfo,
        List<ProviderInfo> providers, ComponentName instrumentationName,
        ProfilerInfo profilerInfo, Bundle instrumentationArgs,
        IInstrumentationWatcher instrumentationWatcher,
        IUiAutomationConnection instrumentationUiConnection, int debugMode,
        boolean enableBinderTracking, boolean trackAllocation,
        boolean isRestrictedBackupMode, boolean persistent, Configuration config,
        CompatibilityInfo compatInfo, Map services, Bundle coreSettings,
        String buildSerial, AutofillOptions autofillOptions,
        ContentCaptureOptions contentCaptureOptions) {
    // 初始化系统服务缓存
    if (services != null) {
        ServiceManager.initServiceCache(services);
    }
    setCoreSettings(coreSettings);

    // 打包所有参数到 AppBindData
    AppBindData data = new AppBindData();
    data.processName = processName;
    data.appInfo = appInfo;
    data.providers = providers;
    data.instrumentationName = instrumentationName;
    data.debugMode = debugMode;
    data.config = config;
    data.compatInfo = compatInfo;
    // ... 其他字段
    sendMessage(H.BIND_APPLICATION, data);  // 转发到主线程
}
```

主线程处理 `H.BIND_APPLICATION` 时执行：
- 创建 Application 实例
- 安装 ContentProvider
- 调用 `Application.onCreate()`

#### 4.2 scheduleCreateService — 创建服务

scheduleCreateService()：

```java
public final void scheduleCreateService(IBinder token,
        ServiceInfo info, CompatibilityInfo compatInfo, int processState) {
    updateProcessState(processState, false);
    CreateServiceData s = new CreateServiceData();
    s.token = token;        // Service 的唯一标识（AMS 分配的 Binder token）
    s.info = info;          // Service 元信息
    s.compatInfo = compatInfo;
    sendMessage(H.CREATE_SERVICE, s);  // 转发到主线程
}
```

#### 4.3 scheduleBindService / scheduleUnbindService — 绑定/解绑服务

scheduleBindService()：

```java
public final void scheduleBindService(IBinder token, Intent intent,
        boolean rebind, int processState) {
    updateProcessState(processState, false);
    BindServiceData s = new BindServiceData();
    s.token = token;
    s.intent = intent;
    s.rebind = rebind;
    sendMessage(H.BIND_SERVICE, s);
}
```

#### 4.4 scheduleReceiver — 分发广播

scheduleReceiver()：

```java
public final void scheduleReceiver(Intent intent, ActivityInfo info,
        CompatibilityInfo compatInfo, int resultCode, String data, Bundle extras,
        boolean sync, int sendingUser, int processState) {
    updateProcessState(processState, false);
    ReceiverData r = new ReceiverData(intent, resultCode, data, extras,
            sync, false, mAppThread.asBinder(), sendingUser);
    r.info = info;
    r.compatInfo = compatInfo;
    sendMessage(H.RECEIVER, r);
}
```

#### 4.5 scheduleTransaction — 现代 Activity 生命周期入口

scheduleTransaction() 是 Android 9+ 引入的统一事务机制：

```java
public void scheduleTransaction(ClientTransaction transaction) throws RemoteException {
    ActivityThread.this.scheduleTransaction(transaction);
}
```

所有 Activity 生命周期回调（onCreate、onStart、onResume、onPause、onStop、onDestroy）都通过 `ClientTransaction` 统一调度，取代了旧的分散方法（如 `scheduleLaunchActivity`、`scheduleResumeActivity` 等）。

#### 4.6 scheduleTrimMemory — 内存修剪

scheduleTrimMemory()：

```java
public void scheduleTrimMemory(int level) {
    final Runnable r = PooledLambda.obtainRunnable(ActivityThread::handleTrimMemory,
            ActivityThread.this, level).recycleOnUse();
    // 在帧绘制完成后执行，减少卡顿风险
    Choreographer choreographer = Choreographer.getMainThreadInstance();
    if (choreographer != null) {
        choreographer.postCallback(Choreographer.CALLBACK_COMMIT, r, null);
    } else {
        mH.post(r);
    }
}
```

#### 4.7 立即执行的方法（不经过 Handler）

部分方法需要**立即执行**，不转发到主线程：

```java
// 时区更新 — 立即生效
public void updateTimeZone() {
    TimeZone.setDefault(null);
}

// DNS 缓存清除 — 立即生效
public void clearDnsCache() {
    InetAddress.clearDnsCache();
    NetworkEventDispatcher.getInstance().onNetworkConfigurationChanged();
}

// 调度组设置 — 立即生效（影响进程优先级）
public void setSchedulingGroup(int group) {
    Process.setProcessGroup(Process.myPid(), group);
}
```

---

### 五、H (Handler) 消息码表

H 是主线程的 Handler，定义了所有消息码：

```java
class H extends Handler {
    public static final int BIND_APPLICATION        = 110;  // 绑定应用
    public static final int EXIT_APPLICATION        = 111;  // 退出应用
    public static final int RECEIVER                = 113;  // 广播接收
    public static final int CREATE_SERVICE          = 114;  // 创建服务
    public static final int SERVICE_ARGS            = 115;  // 服务启动参数
    public static final int STOP_SERVICE            = 116;  // 停止服务
    public static final int CONFIGURATION_CHANGED   = 118;  // 配置变更
    public static final int GC_WHEN_IDLE            = 120;  // 空闲时 GC
    public static final int BIND_SERVICE            = 121;  // 绑定服务
    public static final int UNBIND_SERVICE          = 122;  // 解绑服务
    public static final int DUMP_SERVICE            = 123;  // 转储服务
    public static final int LOW_MEMORY              = 124;  // 低内存
    public static final int PROFILER_CONTROL        = 127;  // 性能分析
    public static final int CREATE_BACKUP_AGENT     = 128;  // 创建备份代理
    public static final int DESTROY_BACKUP_AGENT    = 129;  // 销毁备份代理
    public static final int SUICIDE                 = 130;  // 自杀
    public static final int REMOVE_PROVIDER         = 131;  // 移除 Provider
    public static final int DISPATCH_PACKAGE_BROADCAST = 133; // 包广播
    // ... 还有更多
}
```

---

### 六、完整通信链路图

```
┌──────────────────────────────────────────────────────────────────────┐
│  system_server 进程                                                   │
│                                                                      │
│  AMS                                                                 │
│  │                                                                   │
│  │ ProcessRecord.app = IApplicationThread (Binder Proxy)             │
│  │                     ↑ 持有应用进程的 Binder 代理                     │
│  │                                                                   │
│  │ 调用示例：                                                         │
│  │ appThread.scheduleCreateService(token, serviceInfo, ...)          │
│  │     │ Binder.transact() — 跨进程                                   │
│  └─────┼──────────────────────────────────────────────────────────────┘
│        │
│        │ Binder Driver
│        ▼
│  ┌─────────────────────────────────────────────────────────────────┐
│  │  应用进程                                                        │
│  │                                                                  │
│  │  Binder 线程池                                                    │
│  │  ┌──────────────────────────────────────────────────────────┐   │
│  │  │ ApplicationThread (IApplicationThread.Stub)              │   │
│  │  │                                                          │   │
│  │  │ onTransact(code, data, reply, flags) {                  │   │
│  │  │     switch(code) {                                       │   │
│  │  │         case CREATE_SERVICE:                             │   │
│  │  │             scheduleCreateService(token, info, ...);     │   │
│  │  │             break;                                       │   │
│  │  │         // ...                                           │   │
│  │  │     }                                                    │   │
│  │  │ }                                                        │   │
│  │  │                                                          │   │
│  │  │ scheduleCreateService() {                                │   │
│  │  │     CreateServiceData s = new CreateServiceData();       │   │
│  │  │     s.token = token;                                     │   │
│  │  │     sendMessage(H.CREATE_SERVICE, s);  ──────────────┐  │   │
│  │  │ }                                                    │  │   │
│  │  └──────────────────────────────────────────────────────┼──┘   │
│  │                                                          │       │
│  │  主线程                                                    │       │
│  │  ┌──────────────────────────────────────────────────────▼──┐   │
│  │  │ H (Handler) + Looper                                   │   │
│  │  │                                                         │   │
│  │  │ handleMessage(Message msg) {                            │   │
│  │  │     switch(msg.what) {                                  │   │
│  │  │         case H.CREATE_SERVICE:                          │   │
│  │  │             handleCreateService((CreateServiceData)     │   │
│  │  │                                  msg.obj);              │   │
│  │  │             break;                                      │   │
│  │  │         // ...                                          │   │
│  │  │     }                                                   │   │
│  │  │ }                                                       │   │
│  │  │                                                         │   │
│  │  │ handleCreateService(CreateServiceData data) {           │   │
│  │  │     // 1. 反射创建 Service 实例                          │   │
│  │  │     // 2. 调用 service.onCreate()                       │   │
│  │  │     // 3. 存入 mServices 映射表                          │   │
│  │  │ }                                                       │   │
│  │  └─────────────────────────────────────────────────────────┘   │
│  └─────────────────────────────────────────────────────────────────┘
```

---

### 七、总结

| 维度 | 说明 |
|------|------|
| **接口类型** | `oneway interface` — 所有方法异步单向 |
| **唯一实现** | `ActivityThread.ApplicationThread` (内部类) |
| **创建时机** | `ActivityThread` 构造时立即 `new ApplicationThread()` |
| **注册时机** | `attachApplication()` 时将 `mAppThread.asBinder()` 传给 AMS |
| **运行线程** | 所有方法运行在 **Binder 线程池**（非主线程） |
| **核心模式** | 通过 `sendMessage()` 将任务转发到主线程 `H` Handler |
| **立即执行** | `updateTimeZone()`、`clearDnsCache()`、`setSchedulingGroup()` 直接在 Binder 线程执行 |
| **现代方式** | `scheduleTransaction(ClientTransaction)` 统一调度 Activity 生命周期 |
| **AMS 持有** | `ProcessRecord.app` 保存 `IApplicationThread` 的 Binder 代理引用 |

`IApplicationThread` 是 AMS 控制应用进程的**唯一通道**。AMS 通过它实现了对应用进程的完全远程操控：启动/停止四大组件、内存管理、性能分析、配置变更通知等所有系统级指令都通过这个接口传递到应用进程。

## 6. 'c:/D/android_project/cells-android10/frameworks/base/core/java/android/app/ActivityThread.java' ​ActivityThread.java​ handleMessage 处理启动activity

## ActivityThread.handleMessage 处理启动 Activity 全流程

---

### 一、整体调用链概览

```
AMS (system_server)
    │ Binder IPC
    ▼
ApplicationThread.scheduleTransaction(ClientTransaction)    [Binder 线程]
    │
    ▼
ClientTransactionHandler.scheduleTransaction(transaction)
    │ transaction.preExecute(this)
    │ sendMessage(H.EXECUTE_TRANSACTION, transaction)
    ▼
H.handleMessage(msg)                                        [主线程]
    │ case EXECUTE_TRANSACTION:
    ▼
TransactionExecutor.execute(transaction)
    │
    ├── executeCallbacks(transaction)
    │       │ 遍历 callbacks 列表
    │       ▼
    │   LaunchActivityItem.execute()     → handleLaunchActivity()  → performLaunchActivity()  → Activity.onCreate()
    │   ResumeActivityItem.execute()     → handleResumeActivity()  → performResumeActivity()  → Activity.onResume()
    │
    └── executeLifecycleState(transaction)
            │ 转换到最终生命周期状态
            ▼
        cycleToPath() → 各生命周期回调
```

---

### 二、第一环：ApplicationThread.scheduleTransaction

ApplicationThread.scheduleTransaction() — 运行在 **Binder 线程**：

```java
public void scheduleTransaction(ClientTransaction transaction) throws RemoteException {
    ActivityThread.this.scheduleTransaction(transaction);
}
```

调用父类 ClientTransactionHandler.scheduleTransaction()：

```java
void scheduleTransaction(ClientTransaction transaction) {
    transaction.preExecute(this);                                    // 预处理
    sendMessage(ActivityThread.H.EXECUTE_TRANSACTION, transaction);  // 投递到主线程
}
```

通过 sendMessage() 将事务封装为 `Message` 投递到主线程 `H` Handler：

```java
private void sendMessage(int what, Object obj, int arg1, int arg2, boolean async) {
    Message msg = Message.obtain();
    msg.what = what;       // EXECUTE_TRANSACTION = 159
    msg.obj = obj;         // ClientTransaction 对象
    mH.sendMessage(msg);   // 投递到主线程消息队列
}
```

---

### 三、第二环：H.handleMessage 消息分发

H.handleMessage() 运行在**主线程**，是消息分发的核心：

```java
public void handleMessage(Message msg) {
    switch (msg.what) {
        case BIND_APPLICATION:
            AppBindData data = (AppBindData)msg.obj;
            handleBindApplication(data);
            break;

        case CREATE_SERVICE:
            handleCreateService((CreateServiceData)msg.obj);
            break;

        // ... 其他消息 ...

        case EXECUTE_TRANSACTION:                                    // ★ Activity 启动入口
            final ClientTransaction transaction = (ClientTransaction) msg.obj;
            mTransactionExecutor.execute(transaction);               // 交给事务执行器
            if (isSystem()) {
                transaction.recycle();
            }
            break;

        case RELAUNCH_ACTIVITY:
            handleRelaunchActivityLocally((IBinder) msg.obj);
            break;
    }
}
```

**关键点**：`EXECUTE_TRANSACTION` (消息码 159) 是 Android 9+ 统一处理 Activity 生命周期的入口，取代了旧版本中分散的消息码（如 `LAUNCH_ACTIVITY`、`RESUME_ACTIVITY` 等）。

---

### 四、第三环：TransactionExecutor.execute 事务执行

TransactionExecutor.execute() 负责按序执行事务中的所有操作：

```java
public void execute(ClientTransaction transaction) {
    final IBinder token = transaction.getActivityToken();

    // 检查是否有待销毁的 Activity
    if (token != null) {
        final Map<IBinder, ClientTransactionItem> activitiesToBeDestroyed =
                mTransactionHandler.getActivitiesToBeDestroyed();
        final ClientTransactionItem destroyItem = activitiesToBeDestroyed.get(token);
        if (destroyItem != null) {
            if (transaction.getLifecycleStateRequest() == destroyItem) {
                activitiesToBeDestroyed.remove(token);
            }
            if (mTransactionHandler.getActivityClient(token) == null) {
                return;  // Activity 尚未创建就已被请求销毁，跳过
            }
        }
    }

    executeCallbacks(transaction);      // ★ 第一步：执行回调列表（Launch/Resume等）
    executeLifecycleState(transaction); // ★ 第二步：转换到最终生命周期状态
    mPendingActions.clear();
}
```

---

### 五、第四环：executeCallbacks 执行回调列表

executeCallbacks() 遍历事务中的所有回调项：

```java
public void executeCallbacks(ClientTransaction transaction) {
    final List<ClientTransactionItem> callbacks = transaction.getCallbacks();
    if (callbacks == null || callbacks.isEmpty()) {
        return;
    }

    final IBinder token = transaction.getActivityToken();
    ActivityClientRecord r = mTransactionHandler.getActivityClient(token);

    final ActivityLifecycleItem finalStateRequest = transaction.getLifecycleStateRequest();
    final int finalState = finalStateRequest != null ? finalStateRequest.getTargetState() : UNDEFINED;
    final int lastCallbackRequestingState = lastCallbackRequestingState(transaction);

    final int size = callbacks.size();
    for (int i = 0; i < size; ++i) {
        final ClientTransactionItem item = callbacks.get(i);

        // 如果需要先转换到一个中间状态
        final int closestPreExecutionState = mHelper.getClosestPreExecutionState(r,
                item.getPostExecutionState());
        if (closestPreExecutionState != UNDEFINED) {
            cycleToPath(r, closestPreExecutionState, transaction);
        }

        // ★ 执行回调项（如 LaunchActivityItem.execute()）
        item.execute(mTransactionHandler, token, mPendingActions);
        item.postExecute(mTransactionHandler, token, mPendingActions);

        if (r == null) {
            // LaunchActivityItem 执行后会创建 ActivityClientRecord
            r = mTransactionHandler.getActivityClient(token);
        }

        // 转换到回调项请求的目标状态
        final int postExecutionState = item.getPostExecutionState();
        if (postExecutionState != UNDEFINED && r != null) {
            final boolean shouldExcludeLastTransition =
                    i == lastCallbackRequestingState && finalState == postExecutionState;
            cycleToPath(r, postExecutionState, shouldExcludeLastTransition, transaction);
        }
    }
}
```

---

### 六、第五环：LaunchActivityItem.execute — 启动 Activity

LaunchActivityItem.execute() 是实际触发 Activity 创建的入口：

```java
public void execute(ClientTransactionHandler client, IBinder token,
        PendingTransactionActions pendingActions) {
    Trace.traceBegin(TRACE_TAG_ACTIVITY_MANAGER, "activityStart");

    // 创建 ActivityClientRecord，封装启动信息
    ActivityClientRecord r = new ActivityClientRecord(token, mIntent, mIdent, mInfo,
            mOverrideConfig, mCompatInfo, mReferrer, mVoiceInteractor, mState,
            mPersistentState, mPendingResults, mPendingNewIntents, mIsForward,
            mProfilerInfo, client, mAssistToken);

    // ★ 调用客户端处理器启动 Activity
    client.handleLaunchActivity(r, pendingActions, null /* customIntent */);

    Trace.traceEnd(TRACE_TAG_ACTIVITY_MANAGER);
}
```

---

### 七、第六环：handleLaunchActivity — 处理启动

handleLaunchActivity() 是主线程执行的核心方法：

```java
public Activity handleLaunchActivity(ActivityClientRecord r,
        PendingTransactionActions pendingActions, Intent customIntent) {
    // 1. 取消后台 GC
    unscheduleGcIdler();
    mSomeActivitiesChanged = true;

    // 2. 性能分析器设置
    if (r.profilerInfo != null) {
        mProfiler.setProfiler(r.profilerInfo);
        mProfiler.startProfiling();
    }

    // 3. 更新配置
    handleConfigurationChanged(null, null);

    // 4. 预加载硬件加速渲染器
    if (!ThreadedRenderer.sRendererDisabled
            && (r.activityInfo.flags & ActivityInfo.FLAG_HARDWARE_ACCELERATED) != 0) {
        HardwareRenderer.preload();
    }
    WindowManagerGlobal.initialize();

    // 5. ★ 执行 Activity 创建
    final Activity a = performLaunchActivity(r, customIntent);

    // 6. 创建成功后的后续处理
    if (a != null) {
        r.createdConfig = new Configuration(mConfiguration);
        reportSizeConfigurations(r);
        if (!r.activity.mFinished && pendingActions != null) {
            pendingActions.setOldState(r.state);
            pendingActions.setRestoreInstanceState(true);
            pendingActions.setCallOnPostCreate(true);  // 标记需要调用 onPostCreate
        }
    } else {
        // 创建失败，通知 AMS 结束 Activity
        ActivityTaskManager.getService()
                .finishActivity(r.token, Activity.RESULT_CANCELED, null,
                        Activity.DONT_FINISH_TASK_WITH_ACTIVITY);
    }

    return a;
}
```

---

### 八、第七环：performLaunchActivity — Activity 实例化与 onCreate

performLaunchActivity() 是 Activity 实际被创建的地方：

```java
private Activity performLaunchActivity(ActivityClientRecord r, Intent customIntent) {
    ActivityInfo aInfo = r.activityInfo;

    // ── 阶段1：准备 ──
    // 获取 PackageInfo（APK 信息）
    if (r.packageInfo == null) {
        r.packageInfo = getPackageInfo(aInfo.applicationInfo, r.compatInfo,
                Context.CONTEXT_INCLUDE_CODE);
    }

    // 解析目标组件
    ComponentName component = r.intent.getComponent();
    if (component == null) {
        component = r.intent.resolveActivity(mInitialApplication.getPackageManager());
        r.intent.setComponent(component);
    }

    // ── 阶段2：创建 Context ──
    ContextImpl appContext = createBaseContextForActivity(r);

    // ── 阶段3：反射创建 Activity 实例 ──
    Activity activity = null;
    try {
        java.lang.ClassLoader cl = appContext.getClassLoader();
        activity = mInstrumentation.newActivity(
                cl, component.getClassName(), r.intent);
        // ↑ 内部调用 Class.forName().newInstance()
        r.intent.setExtrasClassLoader(cl);
        if (r.state != null) {
            r.state.setClassLoader(cl);
        }
    } catch (Exception e) {
        if (!mInstrumentation.onException(activity, e)) {
            throw new RuntimeException("Unable to instantiate activity " + component, e);
        }
    }

    try {
        // ── 阶段4：获取/创建 Application ──
        Application app = r.packageInfo.makeApplication(false, mInstrumentation);

        if (activity != null) {
            // ── 阶段5：配置 Activity ──
            CharSequence title = r.activityInfo.loadLabel(appContext.getPackageManager());
            Configuration config = new Configuration(mCompatConfiguration);
            if (r.overrideConfig != null) {
                config.updateFrom(r.overrideConfig);
            }

            // ── 阶段6：attach — 绑定所有核心组件 ──
            appContext.setOuterContext(activity);
            activity.attach(appContext, this, getInstrumentation(), r.token,
                    r.ident, app, r.intent, r.activityInfo, title, r.parent,
                    r.embeddedID, r.lastNonConfigurationInstances, config,
                    r.referrer, r.voiceInteractor, window, r.configCallback,
                    r.assistToken);
            // ↑ attach() 内部创建 PhoneWindow、WindowManager、设置回调等

            // ── 阶段7：设置主题 ──
            int theme = r.activityInfo.getThemeResource();
            if (theme != 0) {
                activity.setTheme(theme);
            }

            // ── 阶段8：★ 调用 onCreate() ──
            activity.mCalled = false;
            if (r.isPersistable()) {
                mInstrumentation.callActivityOnCreate(activity, r.state, r.persistentState);
            } else {
                mInstrumentation.callActivityOnCreate(activity, r.state);
            }
            // ↑ 内部调用 activity.performCreate() → activity.onCreate()

            // 验证 super.onCreate() 被调用
            if (!activity.mCalled) {
                throw new SuperNotCalledException(
                    "Activity did not call through to super.onCreate()");
            }
            r.activity = activity;
        }
        r.setState(ON_CREATE);

        // 将 Activity 记录注册到全局映射表
        synchronized (mResourcesManager) {
            mActivities.put(r.token, r);
        }

    } catch (Exception e) {
        if (!mInstrumentation.onException(activity, e)) {
            throw new RuntimeException("Unable to start activity " + component, e);
        }
    }

    return activity;
}
```

---

### 九、第八环：executeLifecycleState — 转换到最终状态

executeLifecycleState() 将 Activity 转换到事务请求的最终状态：

```java
private void executeLifecycleState(ClientTransaction transaction) {
    final ActivityLifecycleItem lifecycleItem = transaction.getLifecycleStateRequest();
    if (lifecycleItem == null) {
        return;  // 没有状态请求，跳过
    }

    final IBinder token = transaction.getActivityToken();
    final ActivityClientRecord r = mTransactionHandler.getActivityClient(token);
    if (r == null) {
        return;
    }

    // 先转换到目标状态的前一个状态
    cycleToPath(r, lifecycleItem.getTargetState(), true /* excludeLastState */, transaction);

    // 执行最终状态转换
    lifecycleItem.execute(mTransactionHandler, token, mPendingActions);
    lifecycleItem.postExecute(mTransactionHandler, token, mPendingActions);
}
```

如果最终状态是 `RESUMED`，则 ResumeActivityItem.execute() 被调用：

```java
public void execute(ClientTransactionHandler client, IBinder token,
        PendingTransactionActions pendingActions) {
    client.handleResumeActivity(token, true, mIsForward, "RESUME_ACTIVITY");
}
```

→ handleResumeActivity() → performResumeActivity() → `activity.performResume()` → `Activity.onResume()`

---

### 十、完整生命周期执行时序图

```
AMS → ApplicationThread.scheduleTransaction(transaction)
    │  [Binder 线程]
    │
    │  transaction 包含:
    │    callbacks = [LaunchActivityItem, ResumeActivityItem]
    │    lifecycleStateRequest = ResumeActivityItem(RESUMED)
    │
    ▼  sendMessage(EXECUTE_TRANSACTION)
H.handleMessage()  [主线程]
    │
    ▼  case EXECUTE_TRANSACTION
TransactionExecutor.execute(transaction)
    │
    ├── executeCallbacks(transaction)
    │   │
    │   ├── i=0: LaunchActivityItem.execute()
    │   │   │
    │   │   ▼
    │   │   handleLaunchActivity(r, pendingActions, null)
    │   │   │
    │   │   ├── unscheduleGcIdler()         // 取消 GC
    │   │   ├── handleConfigurationChanged() // 更新配置
    │   │   ├── HardwareRenderer.preload()   // 预加载渲染器
    │   │   ├── WindowManagerGlobal.initialize() // 初始化 WMS 全局对象
    │   │   │
    │   │   └── performLaunchActivity(r, customIntent)
    │   │       │
    │   │       ├── getPackageInfo()          // 获取 APK 信息
    │   │       ├── createBaseContextForActivity() // 创建 Context
    │   │       ├── mInstrumentation.newActivity() // ★ 反射创建 Activity
    │   │       ├── r.packageInfo.makeApplication() // 获取 Application
    │   │       ├── activity.attach(...)      // ★ 绑定 PhoneWindow/WMS
    │   │       ├── activity.setTheme()       // 设置主题
    │   │       ├── mInstrumentation.callActivityOnCreate() // ★ Activity.onCreate()
    │   │       └── mActivities.put(token, r) // 注册到映射表
    │   │
    │   │   状态: INITIALIZED → ON_CREATE
    │   │
    │   ├── i=1: ResumeActivityItem.execute()
    │   │   │
    │   │   ▼
    │   │   handleResumeActivity(token, ...)
    │   │   │
    │   │   └── performResumeActivity(token, ...)
    │   │       │
    │   │       ├── activity.onStateNotSaved()
    │   │       ├── deliverNewIntents()       // 传递新 Intent
    │   │       ├── deliverResults()          // 传递结果
    │   │       └── activity.performResume()  // ★ Activity.onResume()
    │   │
    │   │   状态: ON_CREATE → ON_RESUME
    │   │
    │   └── postExecute: ActivityTaskManager.getService().activityResumed(token)
    │       // 通知 AMS Activity 已 Resume
    │
    └── executeLifecycleState(transaction)
        │
        └── 已经是 RESUMED 状态，cycleToPath 无需额外转换
```

---

### 十一、关键设计总结

| 组件 | 职责 | 运行线程 |
|------|------|---------|
| **ApplicationThread** | 接收 AMS 的 Binder 调用 | Binder 线程池 |
| **ClientTransactionHandler.scheduleTransaction()** | 预处理 + 投递消息到主线程 | Binder → 主线程 |
| **H (Handler)** | 主线程消息分发 | 主线程 |
| **TransactionExecutor** | 按序执行事务中的所有回调和状态转换 | 主线程 |
| **LaunchActivityItem** | 触发 Activity 创建 → onCreate | 主线程 |
| **ResumeActivityItem** | 触发 Activity 恢复 → onResume | 主线程 |
| **performLaunchActivity()** | 反射创建 Activity + attach + onCreate | 主线程 |
| **performResumeActivity()** | 执行 onResume | 主线程 |

**核心设计思想**：Android 9+ 引入了 `ClientTransaction` 事务机制，将 Activity 的所有生命周期操作（Launch、Resume、Pause、Stop、Destroy、ReLaunch 等）统一封装为一个事务，通过 `EXECUTE_TRANSACTION` 消息投递到主线程，由 `TransactionExecutor` 按序执行。这取代了旧版本中每种生命周期操作都有独立消息码的分散设计，使生命周期管理更加一致和可控。

