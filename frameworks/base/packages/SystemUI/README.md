# SystemUI

“Everything you see in Android that's not an app”

> 译：你在 Android 上看到的、不属于某个应用的那些界面。

SystemUI is a persistent process that provides UI for the system but outside
of the system_server process.

> 译：SystemUI 是一个常驻进程，为系统提供 UI，但运行在 system_server 进程之外。

The starting point for most of sysui code is a list of services that extend
SystemUI that are started up by SystemUIApplication. These services then depend
on some custom dependency injection provided by Dependency.

> 译：大多数 sysui 代码的起点是一份继承自 SystemUI、由 SystemUIApplication 启动的服务列表。这些服务随后依赖 Dependency 提供的一些自定义依赖注入。

Inputs directed at sysui (as opposed to general listeners) generally come in
through IStatusBar. Outputs from sysui are through a variety of private APIs to
the android platform all over.

> 译：指向 sysui 的输入（区别于通用监听器）一般经由 IStatusBar 进入。sysui 的输出则通过散布在 Android 平台各处的多种私有 API 发出。

## SystemUIApplication

When SystemUIApplication starts up, it will start up the services listed in
config_systemUIServiceComponents or config_systemUIServiceComponentsPerUser.

> 译：SystemUIApplication 启动时会拉起 config_systemUIServiceComponents 或 config_systemUIServiceComponentsPerUser 中列出的服务。

Each of these services extend SystemUI. SystemUI provides them with a Context
and gives them callbacks for onConfigurationChanged (this historically was
the main path for onConfigurationChanged, now also happens through
ConfigurationController). They also receive a callback for onBootCompleted
since these objects may be started before the device has finished booting.

> 译：这些服务都继承自 SystemUI。SystemUI 为它们提供 Context，并回调 onConfigurationChanged（历史上这是 onConfigurationChanged 的主路径，现在也会经由 ConfigurationController 触发）。由于这些对象可能在设备完成开机前就已被启动，它们还会收到 onBootCompleted 回调。

SystemUI and SystemUIApplication also have methods for putComponent and
getComponent which were existing systems to get a hold of other parts of
sysui before Dependency existed. Generally new things should not be added
to putComponent, instead Dependency and other refactoring is preferred to
make sysui structure cleaner.

> 译：SystemUI 与 SystemUIApplication 还提供 putComponent/getComponent 方法，这是在 Dependency 出现之前用于获取 sysui 其他部分的机制。通常不应再向 putComponent 添加新内容，而应优先使用 Dependency 及其他重构手段，让 sysui 的结构更清晰。

Each SystemUI service is expected to be a major part of system ui and the
goal is to minimize communication between them. So in general they should be
relatively silo'd.

> 译：每个 SystemUI 服务都应是系统 UI 的一个重要组成部分，目标是尽量减少它们之间的相互通信。因此一般来说它们应当相对独立、互不干扰。

## Dependencies

The first SystemUI service that is started should always be Dependency.
Dependency provides a static method for getting a hold of dependencies that
have a lifecycle that spans sysui. Dependency has code for how to create all
dependencies manually added. SystemUIFactory is also capable of
adding/replacing these dependencies.

> 译：最先启动的 SystemUI 服务应当始终是 Dependency。Dependency 提供一个静态方法，用于获取那些生命周期横跨整个 sysui 的依赖。Dependency 中包含了如何手动创建所有已注册依赖的代码。SystemUIFactory 也能添加/替换这些依赖。

Dependencies are lazily initialized, so if a Dependency is never referenced at
runtime, it will never be created.

> 译：依赖是惰性初始化的，因此如果一个 Dependency 在运行时从未被引用，它就永远不会被创建。

If an instantiated dependency implements Dumpable it will be included in dumps
of sysui (and bug reports), allowing it to include current state information.
This is how \*Controllers dump state to bug reports.

> 译：如果一个已实例化的依赖实现了 Dumpable，它会被纳入 sysui 的 dump（以及 bug report）中，从而可以输出自身当前状态。\*Controllers 正是通过这种方式将状态 dump 到 bug report。

If an instantiated dependency implements ConfigurationChangeReceiver it will
receive onConfigurationChange callbacks when the configuration changes.

> 译：如果一个已实例化的依赖实现了 ConfigurationChangeReceiver，它会在配置发生变化时收到 onConfigurationChange 回调。

## IStatusBar

CommandQueue is the object that receives all of the incoming events from the
system_server. It extends IStatusBar and dispatches those callbacks back any
number of listeners. The system_server gets a hold of the IStatusBar when
StatusBar calls IStatusBarService#registerStatusBar, so if StatusBar is not
included in the XML service list, it will not be registered with the OS.

> 译：CommandQueue 是接收来自 system_server 所有入站事件的对象。它继承自 IStatusBar，并将这些回调分派给任意数量的监听器。system_server 在 StatusBar 调用 IStatusBarService#registerStatusBar 时获得 IStatusBar 的句柄，因此如果 StatusBar 未被包含在 XML 服务列表中，就不会向系统注册。

CommandQueue posts all incoming callbacks to a handler and then dispatches
those messages to each callback that is currently registered. CommandQueue
also tracks the current value of disable flags and will call #disable
immediately for any callbacks added.

> 译：CommandQueue 将所有入站回调投递到一个 handler，随后把这些消息分派给当前已注册的每个回调。CommandQueue 还会跟踪 disable 标志的当前值，并对任何新增的回调立即调用 #disable。

There are a few places where CommandQueue is used as a bus to communicate
across sysui. Such as when StatusBar calls CommandQueue#recomputeDisableFlags.
This is generally used a shortcut to directly trigger CommandQueue rather than
calling StatusManager and waiting for the call to come back to IStatusBar.

> 译：有少数地方把 CommandQueue 当作总线在 sysui 内部通信，例如 StatusBar 调用 CommandQueue#recomputeDisableFlags。这通常作为捷径直接触发 CommandQueue，而不是调用 StatusManager 然后等待调用绕回 IStatusBar。

## Default SystemUI services list

> 译：默认的 SystemUI 服务列表

### [com.android.systemui.Dependency](/packages/SystemUI/src/com/android/systemui/Dependency.java)

Provides custom dependency injection.

> 译：提供自定义依赖注入。

### [com.android.systemui.util.NotificationChannels](/packages/SystemUI/src/com/android/systemui/util/NotificationChannels.java)

Creates/initializes the channels sysui uses when posting notifications.

> 译：创建/初始化 sysui 在发送通知时使用的通知渠道。

### [com.android.systemui.statusbar.CommandQueue$CommandQueueStart](/packages/SystemUI/src/com/android/systemui/statusbar/CommandQueue.java)

Creates CommandQueue and calls putComponent because its always been there
and sysui expects it to be there :/

> 译：创建 CommandQueue 并调用 putComponent，因为它一直都在那里，sysui 期望它存在 :/

### [com.android.systemui.keyguard.KeyguardViewMediator](/packages/SystemUI/src/com/android/systemui/keyguard/KeyguardViewMediator.java)

Manages keyguard view state.

> 译：管理锁屏（keyguard）视图状态。

### [com.android.systemui.recents.Recents](/packages/SystemUI/src/com/android/systemui/recents/Recents.java)

Recents tracks all the data needed for recents and starts/stops the recents
activity. It provides this cached data to RecentsActivity when it is started.

> 译：Recents 跟踪近期任务所需的所有数据，并启动/停止近期任务 Activity。当 RecentsActivity 启动时，它会把这些缓存数据提供给它。

### [com.android.systemui.volume.VolumeUI](/packages/SystemUI/src/com/android/systemui/volume/VolumeUI.java)

Registers all the callbacks/listeners required to show the Volume dialog when
it should be shown.

> 译：注册所有在需要显示音量对话框时所需的回调/监听器。

### [com.android.systemui.stackdivider.Divider](/packages/SystemUI/src/com/android/systemui/stackdivider/Divider.java)

Shows the drag handle for the divider between two apps when in split screen
mode.

> 译：在分屏模式下，显示两个应用之间分隔条的可拖动手柄。

### [com.android.systemui.SystemBars](/packages/SystemUI/src/com/android/systemui/SystemBars.java)

This is a proxy to the actual SystemUI for the status bar. This loads from
config_statusBarComponent which defaults to StatusBar. (maybe this should be
removed and copy how config_systemUiVendorServiceComponent works)

> 译：这是状态栏实际 SystemUI 的代理。它从 config_statusBarComponent 加载，默认值指向 StatusBar。（也许应当移除它，并仿照 config_systemUiVendorServiceComponent 的方式实现）

### [com.android.systemui.status.phone.StatusBar](/packages/SystemUI/src/com/android/systemui/status/phone/StatusBar.java)

This shows the UI for the status bar and the notification shade it contains.
It also contains a significant amount of other UI that interacts with these
surfaces (keyguard, AOD, etc.). StatusBar also contains a notification listener
to receive notification callbacks.

> 译：展示状态栏及其包含的通知面板的 UI。它还包含大量与这些界面交互的其他 UI（锁屏、AOD 等）。StatusBar 还包含一个通知监听器，用于接收通知回调。

### [com.android.systemui.usb.StorageNotification](/packages/SystemUI/src/com/android/systemui/usb/StorageNotification.java)

Tracks USB status and sends notifications for it.

> 译：跟踪 USB 状态，并为其发送通知。

### [com.android.systemui.power.PowerUI](/packages/SystemUI/src/com/android/systemui/power/PowerUI.java)

Tracks power status and sends notifications for low battery/power saver.

> 译：跟踪电源状态，并在低电量/省电模式时发送通知。

### [com.android.systemui.media.RingtonePlayer](/packages/SystemUI/src/com/android/systemui/media/RingtonePlayer.java)

Plays ringtones.

> 译：播放铃声。

### [com.android.systemui.keyboard.KeyboardUI](/packages/SystemUI/src/com/android/systemui/keyboard/KeyboardUI.java)

Shows UI for keyboard shortcuts (triggered by keyboard shortcut).

> 译：显示键盘快捷键的 UI（由键盘快捷键触发）。

### [com.android.systemui.pip.PipUI](/packages/SystemUI/src/com/android/systemui/pip/PipUI.java)

Shows the overlay controls when Pip is showing.

> 译：在画中画（Pip）显示时展示浮层控制。

### [com.android.systemui.shortcut.ShortcutKeyDispatcher](/packages/SystemUI/src/com/android/systemui/shortcut/ShortcutKeyDispatcher.java)

Dispatches shortcut to System UI components.

> 译：将快捷键分派给 System UI 各组件。

### @string/config_systemUIVendorServiceComponent

Component allowing the vendor/OEM to inject a custom component.

> 译：允许厂商/OEM 注入自定义组件的组件。

### [com.android.systemui.util.leak.GarbageMonitor$Service](/packages/SystemUI/src/com/android/systemui/util/leak/GarbageMonitor.java)

Tracks large objects in sysui to see if there are leaks.

> 译：跟踪 sysui 中的大对象，以发现是否存在内存泄漏。

### [com.android.systemui.LatencyTester](/packages/SystemUI/src/com/android/systemui/LatencyTester.java)

Class that only runs on debuggable builds that listens to broadcasts that
simulate actions in the system that are used for testing the latency.

> 译：仅在可调试版本中运行的类，监听用于模拟系统动作的广播，以测试延迟。

### [com.android.systemui.globalactions.GlobalActionsComponent](/packages/SystemUI/src/com/android/systemui/globalactions/GlobalActionsComponent.java)

Shows the global actions dialog (long-press power).

> 译：显示全局操作对话框（长按电源键）。

### [com.android.systemui.ScreenDecorations](/packages/SystemUI/src/com/android/systemui/ScreenDecorations.java)

Draws decorations about the screen in software (e.g. rounded corners, cutouts).

> 译：以软件方式绘制屏幕周边的装饰（例如圆角、挖孔）。

### [com.android.systemui.biometrics.BiometricDialogImpl](/packages/SystemUI/src/com/android/systemui/biometrics/BiometricDialogImpl.java)

Biometric UI.

> 译：生物识别 UI。

---

 * [Plugins](/packages/SystemUI/docs/plugins.md)
 * [Demo Mode](/packages/SystemUI/docs/demo_mode.md)
