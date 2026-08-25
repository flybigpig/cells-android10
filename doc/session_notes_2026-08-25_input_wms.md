# Android 10 源码研读会话记录（Input / WMS / SurfaceFlinger）

> 工作区：`c:/D/android_project/cells-android10`（AOSP Android 10）
> 日期：2026-08-25
> 主题：WindowManagerService 构造、InputMethodManagerService 构造、SurfaceFlinger 启动与 init、IMS 初始化流程、输入事件窗口选择机制

---

## 一、本次会话已写入源文件的注释

### 1. WindowManagerService 构造函数（1046–1225 行）

文件：`frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java`

按逻辑分 13 个区段注释：

1. 加锁与全局锁（复用 ATMS 锁 `mGlobalLock = atm.getGlobalLock()`）
2. 资源配置读取（合成、Dpad、触摸模式、绘制超时等）
3. 核心子系统装配（mInputManager 必须最先，依赖 `createDisplayContentLocked`）
4. 图形事务工厂（TransactionFactory / mTransaction）
5. 窗口策略/动画器/根窗口容器（mPolicy / mAnimator / mRoot）
6. 布局摆放器、快照、追踪（mWindowPlacerLocked / mTaskSnapshotController / mWindowTracing）
7. Binder 代理与 Local 内部服务（mActivityManager / mAtmInternal / mAmInternal 等）
8. 应用挂起广播监听
9. 动画缩放设置读取
10. LatencyTracker / SettingsObserver / WakeLock / SurfaceAnimationRunner
11. TaskPositioning 与 DragDrop 两个控制器（含两者职责对比）
12. 高刷新率黑名单与系统手势排除参数
13. 收尾注册 WindowManagerInternal 及两阶段初始化说明

关键代码行注释示例（节选）：

```java
// 3. 核心子系统装配：InputManager 必须最先持有，因为后续创建 DisplayContent 时会
//    注册输入通道（见 createDisplayContentLocked 的依赖顺序说明）。
mInputManager = inputManager; // Must be before createDisplayContentLocked.
```

### 2. InputMethodManagerService 构造函数（1443–1509 行）

文件：`frameworks/base/services/core/java/com/android/server/inputmethod/InputManagerService.java`

10 个区段注释：

1. 跨进程与本地服务句柄
2. 主线程 Handler 与 SettingsObserver
3. WMS 远程代理与内部接口（shouldShowIme 判断）
4. IME 显示校验器 lambda
5. HandlerCaller 异步串行化跨进程调用
6. 设备能力开关
7. 常驻输入法切换通知
8. 当前用户 id 获取
9. InputMethodSettings 创建时机
10. profile 刷新与子类型切换控制器

### 3. SurfaceFlinger main 方法（84–137 行）

文件：`frameworks/native/services/surfaceflinger/main_surfaceflinger.cpp`

14 个区段注释：拉起伴随服务、忽略 SIGPIPE、配置 HIDL RPC 线程池、启动图形分配器 HAL、Binder 线程池上限 4、启动 Binder 池、工厂创建 SurfaceFlinger、主线程优先级 URGENT_DISPLAY、cpusets、flinger->init()、注册 ServiceManager、注册 DisplayService、SCHED_FIFO、flinger->run()。

### 4. SurfaceFlinger::init 方法（621–760 行）

文件：`frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp`

16 个区段注释：

0. 启动日志与相位偏移
1. 持有 mStateLock 进入临界区
2. 创建 Scheduler + 重同步回调
3. 建立 app / sf 两条事件连接
4. SF 事件连接挂到 MessageQueue
5. VsyncModulator 接管连接句柄
6. 启动 RegionSamplingThread
7. 计算 RenderEngine 特性位
8. 创建 RenderEngine 并注入合成引擎
9. 创建/注册 HWComposer，处理热插拔，确认内屏
10. 可选 VrFlinger（postMessageAsync 避免死锁）
11. 初始化绘制状态 + initializeDisplays()
12. 预热 RenderEngine 着色器缓存
13. 创建并启动开机属性设置线程
14. 注册刷新率变更/查询/VSYNC 周期回调
15. 填充刷新率配置表

### 5. InputManagerService 构造函数（313–329 行，单独追加）

文件：`frameworks/base/services/core/java/com/android/server/input/InputManagerService.java`

5 个区段注释：

1. 保存上下文 + 创建绑定 DisplayThread 的 InputManagerHandler
2. 读取 config_useDevInputEventForAudioJack
3. nativeInit 核心调用：JNI 创建 NativeInputManager / 底层 InputManager，传入 MessageQueue，返回 mPtr
4. 读取双击手势使能文件路径
5. 注册 InputManagerInternal 到 LocalServices

---

## 二、IMS 初始化流程分析

### 总览

跨越 **SystemServer → Java IMS → JNI NativeInputManager → Native InputManager（InputReader/InputClassifier/InputDispatcher）** 四层，依赖 WMS 提供窗口/策略回调。三阶段模型：**先建对象、再起线程、最后系统就绪**。

### 阶段一：构造（SystemServer early 阶段）

- `SystemServer.java:1024` `inputManager = new InputManagerService(context);`
- IMS 构造（`InputManagerService.java:313`）：
  1. 创建 `InputManagerHandler`（绑定 `DisplayThread` Looper）
  2. 读取资源配置
  3. `mPtr = nativeInit(...)` —— JNI 传 Java 指针/上下文/MessageQueue，返回 native 长期指针
  4. `LocalServices.addService(InputManagerInternal.class, new LocalService())`
- `nativeInit`（`com_android_server_input_InputManagerService.cpp:1317`）→ new `NativeInputManager(contextObj, serviceObj, looper)`
- `NativeInputManager` 构造（cpp:333）：`mInputManager = new InputManager(this, this)` —— 同时充当 readerPolicy 与 dispatcherPolicy
- Native `InputManager::InputManager`（InputManager.cpp:33）：
  - `mDispatcher = new InputDispatcher(dispatcherPolicy)`
  - `mClassifier = new InputClassifier(mDispatcher)`
  - `mReader = createInputReader(readerPolicy, mClassifier)`
  - `initialize()`（cpp:46）创建 `InputReaderThread` 与 `InputDispatcherThread`，**但此时未 run**

### 阶段二：start（SystemServer 调 inputManager.start()）

- `InputManagerService.java:339` `start()`：
  1. `nativeStart(mPtr)` → `InputManager::start()`（InputManager.cpp:51）：先后 run `InputDispatcherThread`（`PRIORITY_URGENT_DISPLAY`）与 `InputReaderThread`
  2. 加入 Watchdog 监控
  3. 注册指针速度/显示触摸/大指针三个设置观察者

### 阶段三：与 WMS 联动 + systemRunning

- WMS 构造缓存 `mInputManager`；`onInitReady()`（WindowManagerService.java:1254）调 `initPolicy()`，把 `InputManagerCallback`（含 `interceptKeyBeforeDispatching`、`filterInputEvent`）注册回 IMS（`setWindowManagerCallbacks`）
- `inputManager.systemRunning()`（InputManagerService.java:365）：
  1. 置 `mSystemReady = true`，获取 NotificationManager
  2. 注册包增删改、蓝牙别名广播，触发 `MSG_RELOAD_DEVICE_ALIASES`、`MSG_UPDATE_KEYBOARD_LAYOUTS`
  3. `mWiredAccessoryCallbacks.systemReady()`

### 数据流与依赖顺序

```
/dev/input 事件 → InputReader(readerPolicy=NativeInputManager)
  → InputClassifier → InputDispatcher(dispatcherPolicy=NativeInputManager)
  → JNI 回调 PhoneWindowManager 策略(经 WMS 注入的回调)
  → InputChannel 投递到目标窗口
```

关键时序：**WMS 必须先拿到 IMS 引用（构造期），但 IMS 策略回调要等 WMS 的 onInitReady 才注册**，故 dispatcher 在 WMS 就绪前不做窗口级策略拦截；`nativeStart` 启动的两大线程确保输入管线在 start() 后即具备吞吐能力。

---

## 三、输入管线主要类分析

### 1. EventHub（前驱，被 InputReader 持有）

用 `inotify` + `epoll` 监听 `/dev/input` 节点增删，`getEvents` 阻塞读取并封装为 `RawEvent`。是整条管线源头。

### 2. InputReader —— 事件读取与设备归一化

- `InputReader.h:115`，实现 `InputReaderInterface`，运行在 `InputReaderThread`
- 关键成员：
  - `mEventHub`：事件源
  - `mPolicy`（`InputReaderPolicyInterface`）：即 NativeInputManager
  - `mQueuedListener`（`QueuedInputListener`）：批量转交事件的桥梁
- `loopOnce()`（InputReader.cpp:286）：`getEvents` 阻塞取 → `processEventsLocked` 解码为 NotifyKeyArgs/NotifyMotionArgs → 设备变化则 `mPolicy->notifyInputDevicesChanged` → 锁外 flush 推给 InputClassifier/Dispatcher（防死锁）

### 3. InputClassifier —— 运动事件分类（HAL 加速）

- `InputClassifier.h:233`，位于 InputReader 与 InputDispatcher 之间
- 运动事件交 HAL 端 `IInputClassifier` 分类/预测，按键事件透传
- 多数调用在 reader 的 flush 路径同步完成

### 4. InputDispatcher —— 事件分发枢纽

- `InputDispatcher.h:411`，运行在 `InputDispatcherThread`
- 关键成员：
  - `mPolicy`（`InputDispatcherPolicyInterface`）：NativeInputManager
  - `mLooper`：自有 Looper，pollOnce 唤醒
  - `mInboundQueue`：事件入队
  - `mCommandQueue`：延迟命令（锁内入队、锁外执行）
  - `mConnectionsByFd`：每目标窗口一条 Connection（背后是 InputChannel）
- `dispatchOnce()`（InputDispatcher.cpp:265）：`dispatchOnceInnerLocked` 取事件找目标 → `runCommandsLockedInterruptible` 跑命令 → `mLooper->pollOnce`

### 5. NativeInputManager —— 策略桥（双重身份）

- `com_android_server_input_InputManagerService.cpp:193`，同时实现 readerPolicy 与 dispatcherPolicy
- 持有 mServiceObj（Java IMS 引用）与 DisplayThread Looper
- 是 native 输入子系统与 Java 框架（IMS → PhoneWindowManager/WMS）唯一 JNI 桥

### 6. 线程与并发边界

- `InputReaderThread`：只管收 + 解码
- `InputClassifier`：reader 推送路径同步分类
- `InputDispatcherThread`：只管路由 + 策略咨询 + 投递，持 mLock
- 两者经 `QueuedInputListener` 解耦，约定 flush/回调在锁外防互锁

---

## 四、InputDispatcher 怎么选窗口

### 数据来源（WMS 推送，Dispatcher 只读）

- `mWindowHandlesByDisplay[displayId]`：窗口列表 + 几何（frame / touchableRegion）
- `mFocusedWindowHandlesByDisplay`：焦点窗口
- 刷新入口：`setInputWindows`（InputDispatcher.cpp:3156）、`setFocusedWindow`（cpp:3265）

### 路径一：Key 事件 → 焦点窗口

`findFocusedWindowTargetsLocked`（InputDispatcher.cpp:1194）：
1. 取 `mFocusedWindowHandlesByDisplay[displayId]`
2. 焦点为空：有 focused app 则等待启动，否则丢弃
3. `checkInjectionPermission` 权限校验
4. `checkWindowReadyForMoreInputLocked` 窗口就绪检查（队列/ANR）
5. 成功 `addWindowTargetLocked`（FLAG_FOREGROUND | DISPATCH_AS_IS）

Key 事件**不看坐标**，纯靠 WMS 维护的焦点窗口。

### 路径二：Motion 事件 → 坐标命中

`findTouchedWindowTargetsLocked`（InputDispatcher.cpp:1257）→ `findTouchedWindowAtLocked`（cpp:511）：
1. 取 `mWindowHandlesByDisplay[displayId]`，按 Z 序从前往后遍历
2. 跳过 displayId 不匹配 / 不可见 / FLAG_NOT_TOUCHABLE
3. 命中判定：
   - `isTouchModal`（未同时带 NOT_FOCUSABLE | NOT_TOUCH_MODAL）→ 整窗命中
   - 否则 `touchableRegionContainsPoint(x, y)`
   - `portalToDisplayId` → 递归到别的屏
4. `FLAG_WATCH_OUTSIDE_TOUCH` → +OUTSIDE 目标；gesture monitor → +监听目标

### 选中之后：Connection 投递

`InputWindowHandle` → `addWindowTargetLocked` → `dispatchEventLocked`（cpp:1018）→ 按 inputChannel fd 查 `mConnectionsByFd` → `InputPublisher::publishKey/MotionEvent` → 写入 socket pair → 应用进程。

---

## 五、窗口选择一图总结

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          事件进入 InputDispatcher                            │
│   mInboundQueue (EventEntry: KEY / MOTION)                                  │
└───────────────┬───────────────────────────────────┬─────────────────────────┘
                │ 按键 Key                           │ 触控/指针 Motion
                ▼                                    ▼
┌───────────────────────────────┐   ┌─────────────────────────────────────────┐
│ findFocusedWindowTargetsLocked│   │ findTouchedWindowTargetsLocked           │
│ (InputDispatcher.cpp:1194)    │   │ (InputDispatcher.cpp:1257)               │
└───────────────┬───────────────┘   └───────────────────┬─────────────────────┘
                │                                        │
                ▼                                        ▼
   查 mFocusedWindowHandlesByDisplay  ──  查 mWindowHandlesByDisplay[displayId]
   [displayId] = 焦点窗口(由WMS推送)     按 Z 序从前往后遍历(findTouchedWindowAtLocked:511)
                │                                        │
                │ 不看坐标，纯靠焦点             命中判定：
                │                                        │ • visible 且 !FLAG_NOT_TOUCHABLE
   ① 焦点窗口为空?                            │ • isTouchModal → 整窗命中
     有 focused app → 等待启动                  │ • 否则 touchableRegionContainsPoint(x,y)
     都无 → 丢弃事件                            │ • portalToDisplayId → 递归到别的屏
   ② checkInjectionPermission 权限校验          │ • FLAG_WATCH_OUTSIDE_TOUCH → +OUTSIDE目标
   ③ checkWindowReadyForMoreInputLocked         │ • gesture monitor → +监听目标
     窗口就绪?(队列/ANR 检查)                   ▼
   ④ addWindowTargetLocked                    命中 windowHandle(可能多个 target)
      FLAG_FOREGROUND|DISPATCH_AS_IS           │
                │                       addWindowTargetLocked(逐个)
                └───────────────┬───────────────┘
                                ▼
                  dispatchEventLocked (InputDispatcher.cpp:1018)
                                │
                                ▼
          按 target.inputChannel fd 查 mConnectionsByFd
                                │
                                ▼
              Connection → InputPublisher::publishKey/MotionEvent
                                │
                                ▼
                  写入 socket pair (InputChannel)
                                │
                                ▼
                  应用进程窗口收到事件

┌─────────────────────────────────────────────────────────────────────────────┐
│  数据来源（WMS 推送，Dispatcher 只读快照）                                    │
│  • mWindowHandlesByDisplay[displayId]  → 窗口列表+几何(frame/touchableRegion)│
│  • mFocusedWindowHandlesByDisplay      → 焦点窗口                            │
│  setInputWindows(:3156) / setFocusedWindow(:3265)                            │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 六、涉及的关键文件清单

| 文件 | 操作 |
|------|------|
| `frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java` | 已注释构造（13 区段） |
| `frameworks/base/services/core/java/com/android/server/inputmethod/InputMethodManagerService.java` | 已注释构造（10 区段） |
| `frameworks/base/services/core/java/com/android/server/input/InputManagerService.java` | 已注释构造（5 区段，二次确认写入） |
| `frameworks/native/services/surfaceflinger/main_surfaceflinger.cpp` | 已注释 main（14 区段） |
| `frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp` | 已注释 init（16 区段） |
| `frameworks/native/services/inputflinger/InputManager.cpp` | 分析（未改） |
| `frameworks/native/services/inputflinger/InputReader.cpp` | 分析（未改） |
| `frameworks/native/services/inputflinger/InputClassifier.cpp` | 分析（未改） |
| `frameworks/native/services/inputflinger/InputDispatcher.cpp` | 分析（未改） |
| `frameworks/base/services/core/jni/com_android_server_input_InputManagerService.cpp` | 分析（未改） |
| `frameworks/base/services/java/com/android/server/SystemServer.java` | 分析（未改） |

---

## 七、待深入方向（会话中提出的后续分析点）

1. InputReader 的 `processEventsLocked` 设备解码细节
2. InputDispatcher 焦点窗口解析与 ANR 超时机制（`checkWindowReadyForMoreInputLocked`）
3. `interceptKeyBeforeDispatching` 的完整 JNI 回调链
4. `setInputWindows` 中窗口 Z 序如何由 WMS 排好
5. `initializeDisplays`、`SurfaceFlinger::run` 合成主循环
