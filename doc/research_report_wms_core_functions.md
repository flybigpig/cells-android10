# WindowManagerService (WMS) 核心功能总结

## 定位

`WindowManagerService`（位于 `frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java`）是运行在 `system_server` 的核心系统服务，实现 `IWindowManager.Stub`（应用层 `WindowManagerGlobal`/`ViewRootImpl` 的对端）、`Watchdog.Monitor` 与 `WindowManagerPolicy.WindowManagerFuncs`。它和 `ActivityTaskManagerService`（同包，管理 Activity/任务栈）紧密协作：WMS 专注于“窗口”本身——其生命周期、层级、布局、动画、输入焦点、显示与旋转、壁纸/系统栏/锁屏、拖拽与快照。

核心协作字段（`WindowManagerService.java`）：

| 字段 | 类型 | 职责 |
|---|---|---|
| `mPolicy` | `WindowManagerPolicy` | 窗口策略（实际为 `PhoneWindowManager`） |
| `mRoot` | `RootWindowContainer` | 所有显示/窗口的层级树根 |
| `mWindowMap` | `WindowHashMap` | 全局所有 `WindowState` |
| `mAnimator` | `WindowAnimator` | 窗口动画驱动 |
| `mWindowPlacerLocked` | `WindowSurfacePlacer` | 布局与 Surface 落盘 |
| `mInputManager` | `InputManagerService` | 输入子系统 |
| `mDisplayManagerInternal` | `DisplayManagerInternal` | 显示管理 |
| `mAtmService` / `mAtmInternal` | `ActivityTaskManagerService` / `Internal` | 与 ATMS 协作 |
| `mDragDropController` | `DragDropController` | 跨窗口拖拽 |
| `mTaskSnapshotController` | `TaskSnapshotController` | 任务快照 |
| `mTransactionFactory` | `TransactionFactory` | 向 SurfaceFlinger 提交 `SurfaceControl.Transaction` |
| `mH` | `H` | 主线程 Handler |

## 核心功能

### 1. 窗口对象与层级树管理

每个应用窗口在 WMS 中对应一个 `WindowState`，归属一个 `WindowToken`（同类型窗口的令牌），Activity 窗口再挂到 `AppWindowToken`。整棵容器树为：`RootWindowContainer` → `DisplayContent`（每显示器一个）→ `TaskStack`/`Task` → `ActivityStack`/`AppWindowToken` → `WindowState`。`Session`（每个 Client 进程一个）承接 `addWindow`/`removeWindow`/`relayoutWindow`：

- `addWindow()`（`WindowManagerService.java:1311` 起）校验权限、调用 `DisplayPolicy.prepareAddWindowLw()`、构造 `WindowState` 并加入 `mWindowMap` 与对应 `DisplayContent`/`WindowToken`。
- 系统窗口类型由 `TYPE_LAYER_MULTIPLIER=10000`、`TYPE_LAYER_OFFSET=1000`、`WINDOW_LAYER_MULTIPLIER=5` 等常量决定 Z 序（`WindowManagerService.java:309-330`）。

### 2. 布局与 Surface 落盘（Z 序 / 位置 / 尺寸）

`WindowSurfacePlacer`（`mWindowPlacerLocked`）驱动 `performSurfacePlacement()`，遍历容器树为每个 `WindowState` 依据 `WindowFrames` / `DisplayFrames` 计算位置、尺寸、`mLayer`（Z 序）与可见性，最终通过 `SurfaceControl.Transaction`（`mTransactionFactory`）把图层交给 SurfaceFlinger。`WindowContainer` 作为容器基类统一承载布局/动画/转场逻辑。

### 3. 窗口策略（WindowManagerPolicy）

`mPolicy`（即 `PhoneWindowManager`）决定：允许哪些窗口类型、系统栏/导航栏行为、Keyguard（锁屏）、Dream（屏保）、截屏、旋转策略、刘海/挖孔（cutout）与系统手势排除区等。`addWindow` 与 `relayoutWindow` 都会调用 `DisplayPolicy.adjustWindowParamsLw()`、`prepareAddWindowLw()`、`getLayoutHintLw()`（`WindowManagerService.java:1507/1511/1644`）。

### 4. 焦点与输入分发

WMS 维护焦点窗口（`mCurrentFocus`）与输入法目标（`computeImeTarget`、`setInputMethodWindowLocked`，`WindowManagerService.java:1588/1591`）。`InputMonitor` / `InputManagerCallback` 桥接 `InputManagerService`，把输入事件分发给当前焦点窗口，并在窗口增删/焦点变化时更新 InputDispatcher 的窗口列表与焦点。

### 5. 动画与转场（App Transition）

`AppTransition`（每个 `DisplayContent` 持有 `mAppTransition`）处理 Activity 的开合与切换转场（如 clip reveal、multi-thumb 等，`WindowManagerService.java:1782/2602/2622`）；`WindowAnimator`（`mAnimator`）逐帧驱动窗口进出/缩放/淡入淡出等动画；`SurfaceAnimator`/`SurfaceAnimationRunner` 负责底层 Surface 动画执行。还支持 `RemoteAnimationController` 与 `RecentsAnimationController`（近期任务动画）。

### 6. 显示与旋转（多显示器）

`DisplayContent` + `DisplayPolicy` + `DisplayRotation` + `DisplayFrames` 管理单/多显示器下的旋转、冻结、屏幕旋转动画（`ScreenRotationAnimation`）。WMS 监听显示热插拔，动态增删 `DisplayContent` 并重建对应策略（`configureDisplayPolicy()`，`WindowManagerService.java:5290`）。

### 7. 壁纸 / 系统栏 / 锁屏

`WallpaperController`（每个 `DisplayContent` 的 `mWallpaperController`）管理壁纸窗口与壁纸目标层级；`StatusBarController` / `BarController` 控制状态栏/导航栏；`KeyguardController` 管理锁屏可见性与“锁屏遮挡焦点”逻辑。WMS 把这些系统装饰窗口和活动窗口统一纳入层级计算。

### 8. 拖拽（Drag & Drop）

`DragDropController`（`mDragDropController`）配合 `DragState` 实现跨窗口拖拽，处理拖拽源/目标、拖动阴影、权限与跨用户约束。

### 9. 任务快照 / 截屏

`TaskSnapshotController`（`mTaskSnapshotController`）+ `TaskSnapshotPersister` 在任务切到后台时抓取并持久化缩略图，供 Recents（最近任务）使用；`WindowTracing` 提供窗口层级/状态追踪用于调试。

### 10. 启动窗口（Starting Window）

`SplashScreenStartingData` / `SnapshotStartingData` 在 Activity 首帧绘制前插入占位窗口（启动闪屏/快照），避免白屏；`AppTransition` 完成后移除。

### 11. Inset / 手势 / Letterbox

`InsetsStateController` + `InsetsSourceProvider` 统一管理状态栏、导航栏、IME 等 inset 区域；`DisplayPolicy` 处理刘海/挖孔（cutout）与系统手势排除区（`updateSystemGestureExclusionLimit`，`WindowManagerService.java:1235`）；分屏/非全屏比例用 `Letterbox` 绘制黑边。

### 12. 与周边系统的协作

- **ATMS**：通过 `mAtmService`/`mAtmInternal` 协作——应用转场、ANR 清除（`mAtmInternal.clearSavedANRState()`，`WindowManagerService.java:4945`）、Keyguard 状态变化通知（`mAtmInternal.notifyKeyguardFlagsChanged()`）等。
- **SurfaceFlinger**：所有图层的最终提交都经 `mTransactionFactory.make()` 得到的 `SurfaceControl.Transaction`。
- **InputManagerService**：输入事件与焦点同步。
- 通过 `LocalService`（继承 `WindowManagerInternal`）向 framework 其它模块暴露内部能力。

## 启动与装配

WMS 由 `main()` 创建并注册到 `ServiceManager`。构造函数内装配各子系统，注释明确强调 **InputManager 必须最先持有**，因为后续 `createDisplayContentLocked` 创建 `DisplayContent` 时会注册输入通道（`WindowManagerService.java:1078-1080`）。`systemReady()` 阶段让各 `DisplayPolicy.systemReady()` 完成策略就绪（`WindowManagerService.java:4543`）。同时 WMS 实现 `Watchdog.Monitor`，在系统卡死时参与看门狗重启判定。

## 一句话总结

WMS 是 Android 的“窗口系统中枢”：管理所有窗口（`WindowState`/`WindowToken`/容器树）、布局与 Z 序、动画与 Activity 转场、输入焦点与 IME、显示与旋转、壁纸/状态栏/锁屏、拖拽、任务快照与启动窗口，并通过 `WindowManagerPolicy` 施加系统策略、通过 `SurfaceControl.Transaction` 把最终图层交给 SurfaceFlinger，与 `ActivityTaskManagerService`、`InputManagerService` 紧密协作。
