# EventHub 分析（Android 10 / cells-android10）

> 源文件：`frameworks/native/services/inputflinger/EventHub.cpp`（1979 行）+ `EventHub.h`
> 相关调用方：`InputReaderFactory.cpp`、`InputReader.cpp`、`InputManager.cpp`

---

## 一、定位与职责

`EventHub` 是 Android 输入子系统最底层的"事件汇聚中心"，头文件注释称之为 *Grand Central Station for events*。它向下直接对接 Linux evdev 字符设备，向上只服务一个消费者：`InputReader`。实例化发生在 `InputReaderFactory.cpp` 的 `createInputReader()`：

```cpp
InputReader* createInputReader(sp<EventHubInterface> eventHub, ...)
// 实际是 new InputReader(new EventHub(), policy, listener)
```

随后 `InputReader::loopOnce()` 每轮调用一次 `EventHub::getEvents()` 取一批原始事件。

整条链路是：

```
内核 evdev 驱动
   → EventHub::getEvents()          （epoll + read，转成 RawEvent）
   → InputReader::processEventsLocked()
   → 各 InputMapper（KeyboardInputMapper / TouchInputMapper / ...）
   → QueuedListener → InputClassifier → InputDispatcher
   → InputChannel（socketpair）→ ViewRootImpl → App
```

职责可归为四类：

1. **设备发现与生命周期管理**：扫描 `/dev/input`、inotify 监听热插拔、`enableDevice/disableDevice` 开关设备节点。
2. **原始事件读取与转换**：把内核 `struct input_event` 转成 `RawEvent` 并回填单调时钟时间戳。
3. **设备能力/映射查询**：classes、key/rel/abs/sw/led/ff/prop 各 bitmask、绝对轴信息、KeyLayout（`*.kl`）、KeyCharacterMap（`*.kcm`）、虚拟键、振动、LED。
4. **设备状态查询**：通过 `EVIOCGKEY`、`EVIOCGSW`、`EVIOCGABS` 读取当前按键/开关/轴的实时状态。

关键常量：

```74:77:frameworks/native/services/inputflinger/EventHub.cpp
static const char *WAKE_LOCK_ID = "KeyEvents";
static const char *DEVICE_PATH = "/dev/input";
// v4l2 devices go directly into /dev
static const char *VIDEO_DEVICE_PATH = "/dev";
```

---

## 二、核心数据结构：`EventHub::Device`

每个打开的 evdev 节点对应一个 `Device`（193 行构造）：持有 `fd`、`id`、`path`、`identifier`，七个 bitmask（`keyBitmask`、`absBitmask`、`relBitmask`、`swBitmask`、`ledBitmask`、`ffBitmask`、`propBitmask`），`classes` 能力位、配置 `configuration`（PropertyMap）、`virtualKeyMap`、`keyMap`（KeyLayout + KCM）、`controllerNumber`、`enabled`，以及可选的 `videoDevice`。

构造时用 `memset` 清空全部 bitmask，`isVirtual(fd < 0)` 标记虚拟设备（虚拟键盘 fd 为 -1）。

几个小方法需要留意：`hasValidFd()` 返回 `!isVirtual && enabled`，是所有 ioctl 类 API 的前置判断；`enable()` 用 `open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK)` 重新打开；`disable()` 关闭 fd 并置 `enabled = false`。

### 设备标识符与 descriptor

`openDeviceLocked()` 通过一串 ioctl 填充 `InputDeviceIdentifier`：

| ioctl | 用途 | 行号 |
|---|---|---|
| `EVIOCGNAME` | 设备名 | 1203 |
| `EVIOCGVERSION` | 驱动版本（失败直接放弃该设备） | 1222 |
| `EVIOCGID` | bus / vendor / product / version | 1230 |
| `EVIOCGPHYS` | 物理位置 | 1241 |
| `EVIOCGUNIQ` | 唯一 ID | 1249 |

`assignDescriptorLocked()`（684）生成稳定描述符：先 `generateDescriptor()` 拼出 `":vendor:product:"` + `uniqueId` 或 `nonce`；若 vendor/product 都是 0（多半是内置设备），退而使用 name 或 location；最后 `sha1(rawDescriptor)` 得到 descriptor。若与已连接设备冲突且无 uniqueId，则递增 `nonce` 重试，保证同一时刻唯一。

---

## 三、构造：EventHub()

```245:300:frameworks/native/services/inputflinger/EventHub.cpp
EventHub::EventHub(void) :
        mBuiltInKeyboardId(NO_BUILT_IN_KEYBOARD), mNextDeviceId(1), ...
    acquire_wake_lock(PARTIAL_WAKE_LOCK, WAKE_LOCK_ID);
    mEpollFd = epoll_create1(EPOLL_CLOEXEC);
    ...
    mINotifyFd = inotify_init();
    mInputWd = inotify_add_watch(mINotifyFd, DEVICE_PATH, IN_DELETE | IN_CREATE);
    ...
```

初始化顺序：申请 `PARTIAL_WAKE_LOCK`（名为 "KeyEvents"）→ 创建 epoll 实例 → `inotify_init()` 并对 `/dev/input` 注册 `IN_DELETE | IN_CREATE`；若 `ro.input.video_enabled` 为真（默认真），再对 `/dev` 注册第二个 watch（mVideoWd）→ 把 inotify fd 加入 epoll → 创建 wake pipe（一对 `pipe()` fd，两端都设 `O_NONBLOCK`），把读端加入 epoll → `getLinuxRelease()` 判定内核版本，`mUsingEpollWakeup = major > 3 || (major == 3 && minor >= 5)`（EPOLLWAKEUP 自 3.5 引入）。

epoll 监听三类 fd：inotify fd、wake pipe 读端、所有输入设备 fd（以及视频设备 fd）。

---

## 四、设备发现：`scanDevicesLocked()` → `openDeviceLocked()`

`scanDevicesLocked()`（1082）做三件事：扫描 `/dev/input`、可选扫描 `/dev` 下的 `v4l-touch*`、若不存在虚拟键盘则 `createVirtualKeyboardLocked()`。

`scanDirLocked()`（1860）用 `opendir/readdir` 遍历目录，跳过 `.` 和 `..`，对每个节点调 `openDeviceLocked()`。

### openDeviceLocked() 的完整流程（1189-1477）

1. `open(devicePath, O_RDWR | O_CLOEXEC | O_NONBLOCK)`，失败返回 -1。
2. 依次 ioctl 拿 name / version / id / phys / uniq（见上表）。
3. **排除名单**：若设备名命中 `mExcludedDevices`（由 `setExcludedDevices()` 设置），直接关闭返回。
4. `assignDescriptorLocked()` 生成 descriptor。
5. `new Device(fd, deviceId, devicePath, identifier)`，`deviceId = mNextDeviceId++`。
6. `loadConfigurationLocked()`：按 identifier 找 `.idc` 配置文件并用 `PropertyMap::load()` 加载。
7. **读取能力 bitmask**：`EVIOCGBIT(EV_KEY/ABS/REL/SW/LED/FF)` 与 `EVIOCGPROP`。
8. **判定 classes**（这是最关键的一段）：

| classes | 判定条件 | 行号 |
|---|---|---|
| `KEYBOARD` | keyBitmask 在 `[0, BTN_MISC)` 或 `[KEY_OK, KEY_MAX]` 有非零字节，或含 gamepad 按键 | 1290-1299 |
| `CURSOR` | `BTN_MOUSE` + `REL_X` + `REL_Y` | 1302 |
| `ROTARY_ENCODER` | idc 中 `device.type == rotaryEncoder` | 1310 |
| `TOUCH` + `TOUCH_MT` | 有 `ABS_MT_POSITION_X/Y`，且有 `BTN_TOUCH` 或没有 gamepad 按键 | 1319-1326 |
| `TOUCH`（旧式单点） | `BTN_TOUCH` + `ABS_X` + `ABS_Y` | 1328 |
| `EXTERNAL_STYLUS` | 有 `ABS_PRESSURE`/`BTN_TOUCH` 但无 `ABS_X/Y`，并**反悔**清掉 KEYBOARD | 1333-1342 |
| `JOYSTICK` | 有 gamepad 按键且至少一个绝对轴归属 joystick（`getAbsAxisUsage`） | 1347-1356 |
| `SWITCH` | swBitmask 任意位 | 1359 |
| `VIBRATOR` | `FF_RUMBLE` | 1367 |
| `MIC` | idc 中 `audio.mic` 为真 | 1431 |
| `EXTERNAL` | idc `device.internal` 取反，或 bus 为 USB / 蓝牙 | 1436 |

9. 若是触摸屏，先 `loadVirtualKeyMapLocked()`（读 `/sys/board_properties/virtualkeys.<name>`），成功则加 `KEYBOARD`。
10. 若是键盘或游戏杆，`loadKeyMapLocked()` 加载 `.kl` / `.kcm`。
11. 键盘进一步细分：判定内置键盘（`isEligibleBuiltInKeyboard`）、`ALPHAKEY`（有 `AKEYCODE_Q`）、`DPAD`（有五个方向键）、`GAMEPAD`（`GAMEPAD_KEYCODES` 表任一命中）。
12. **`classes == 0` 直接丢弃**（"If the device isn't recognized as something we handle, don't monitor it"），`delete device` 返回 -1。
13. 手柄分配 `controllerNumber` 并点亮对应 LED。
14. 按名字匹配 video device（必须在 epoll 注册前完成，保证两个 fd 都入 epoll）。
15. `registerDeviceForEpollLocked()` → `configureFd()` → `addDeviceLocked()`。

`configureFd()`（1479）有三个细节：键盘设备用 `EVIOCSREP {0,0}` **关掉内核按键重复**（由 Android 自己实现重复逻辑）；若内核不支持 EPOLLWAKEUP 则退而尝试 `EVIOCSSUSPENDBLOCK`；最后用 `EVIOCSCLOCKID` 把时钟设为 `CLOCK_MONOTONIC`——这保证了事件时间戳与 Android 其余部分同一时基。

`registerFdForEpoll()` 使用 `EPOLLIN | EPOLLWAKEUP`，这是驱动有未读事件时能唤醒系统的关键。

虚拟键盘 `createVirtualKeyboardLocked()`（1586）用 fd = -1、`VIRTUAL_KEYBOARD_ID`，classes 为 `KEYBOARD | ALPHAKEY | DPAD | VIRTUAL`。

---

## 五、热插拔：inotify → `readNotifyLocked()`

`readNotifyLocked()`（1810）从 inotify fd 读事件缓冲（512 字节），逐个解析 `struct inotify_event`：

- `wd == mInputWd`：`IN_CREATE` 则 `openDeviceLocked("/dev/input/<name>")`，否则 `closeDeviceByPathLocked()`。
- `wd == mVideoWd` 且名字匹配 `v4l-touch*`（`isV4lTouchNode`）：`openVideoDeviceLocked()` 或 `closeVideoDeviceByPathLocked()`。
- 其它 wd：`LOG_ALWAYS_FATAL`，不应发生。

`closeDeviceLocked()`（1758）顺序很讲究：先 `unregisterDeviceFromEpollLocked()`；若有 video device，**必须在移出 epoll 之后**才把它搬回 `mUnattachedVideoDevices`（否则 fd 可能已被关闭而仍在 epoll 中）；释放 controller number；从 `mDevices` 移除并 `close()`。然后分两种情况：若设备还在 `mOpeningDevices` 链表里（刚开就被关），直接从链表摘掉并 `delete`——因为客户端还不知道它存在过，无需上报；否则挂到 `mClosingDevices` 链表，等下一轮 `getEvents()` 上报 `DEVICE_REMOVED` 后再删除。

`requestReopenDevices()`（1908）只置 `mNeedToReopenDevices = true`，由 `getEvents()` 在下一轮执行 `closeAllDevicesLocked()` 并重新扫描——这是配置变更（如 `.idc` 变更）后的重建路径。

---

## 六、主循环：`getEvents()`

```815:1057:frameworks/native/services/inputflinger/EventHub.cpp
size_t EventHub::getEvents(int timeoutMillis, RawEvent* buffer, size_t bufferSize)
```

这是一个 `for (;;)` 循环，每轮按固定优先级检查"虚拟事件"、真实事件，最后才真正阻塞。执行顺序：

1. **配置变更重开**：`mNeedToReopenDevices` → `closeAllDevicesLocked()` + 置 `mNeedToScanDevices` + `break`（先返回调用方，下一轮才真正重扫）。
2. **上报关闭的设备**：遍历 `mClosingDevices`，产出 `DEVICE_REMOVED` 事件，`delete device`，置 `mNeedToSendFinishedDeviceScan`。
3. **首次扫描**：`mNeedToScanDevices` → `scanDevicesLocked()`。
4. **上报新设备**：遍历 `mOpeningDevices`，产出 `DEVICE_ADDED`。
5. **扫描结束标记**：`mNeedToSendFinishedDeviceScan` → 产出 `FINISHED_DEVICE_SCAN`。
6. **消费 epoll 结果**：遍历 `mPendingEventItems[mPendingEventIndex..mPendingEventCount)`：
   - fd == `mINotifyFd`：置 `mPendingINotify = true`。
   - fd == `mWakeReadPipeFd`：把管道读空，置 `awoken = true`。
   - 是 video device 的 fd：`readAndQueueFrames()`；`EPOLLHUP` 时从 epoll 摘除并置空。
   - 是输入设备 fd：`EPOLLIN` 时 `read()` 最多 `capacity` 个 `input_event`，逐个转成 `RawEvent`（`when = processEventTimestamp(iev)`，deviceId 为内置键盘时映射为 0）；`readSize == 0` 或 `ENODEV` 视为设备已拔（早于 inotify 通知），`closeDeviceLocked()`；`EPOLLHUP` 同样关闭。
   - 若结果缓冲被填满，会 `mPendingEventIndex -= 1` **回退一个索引**，保证下一次迭代还能继续读这个设备。
7. **处理 inotify**：`mPendingINotify && mPendingEventIndex >= mPendingEventCount` 时才调 `readNotifyLocked()`。注释解释了原因：readNotify 会修改设备链表，必须放在处理完所有其它事件之后，以确保关设备前先把剩余事件读完。
8. `deviceChanged` 则 `continue` 重新走一轮（立即上报设备增删）。
9. 已经收集到事件或被 `wake()` 唤醒则 `break` 返回。
10. **阻塞等待**（"Mind the wake lock dance!"）：

```1023:1031:frameworks/native/services/inputflinger/EventHub.cpp
        mPendingEventIndex = 0;
        mLock.unlock(); // release lock before poll, must be before release_wake_lock
        release_wake_lock(WAKE_LOCK_ID);
        int pollResult = epoll_wait(mEpollFd, mPendingEventItems, EPOLL_MAX_EVENTS, timeoutMillis);
        acquire_wake_lock(PARTIAL_WAKE_LOCK, WAKE_LOCK_ID);
        mLock.lock(); // reacquire lock after poll, must be after acquire_wake_lock
```

wake lock 的编排：设备驱动有未读事件时持有内核 wake lock，但读空后会释放；为防止此时系统睡去，EventHub 在客户端处理事件期间始终持有自己的用户态 wake lock。**只有"无事件待处理且无事件在处理中"时系统才可能睡眠**。`epoll_wait` 出错且非 `EINTR` 时 `usleep(100000)` 避免因瞬态错误把 CPU 打满。

返回值是 `event - buffer`，即本批事件数。

### 时间戳

`processEventTimestamp()`（132）刻意使用**事件自带的时间**而非读取时刻的时间，让下游能更准确估算"事件入队到分发"的延迟。注释指出：evdev 驱动用 `ktime_get_ts()` 打时间戳，Android 的 `systemTime(SYSTEM_TIME_MONOTONIC)` 走 `clock_gettime(CLOCK_MONOTONIC)`，两者同源，因此可以直接用。

### 唤醒

`wake()`（1069）向 wake pipe 写一个 "W"，把阻塞在 `epoll_wait` 的 `getEvents()` 唤醒。这是 `InputReader` 要求在超时前返回时的手段。

### RawEvent 类型

除真实的 `EV_KEY/EV_ABS/EV_REL/EV_SYN/EV_SW/...` 外，EventHub 自己合成三类：`DEVICE_ADDED`、`DEVICE_REMOVED`、`FINISHED_DEVICE_SCAN`。

---

## 七、查询与控制 API

全部方法都先 `AutoMutex _l(mLock)` 再 `getDeviceLocked(deviceId)`，设备不存在则返回空值：

| 方法 | 作用 | 行号 | 关键 ioctl |
|---|---|---|---|
| `getDeviceIdentifier` | 标识符 | 319 | — |
| `getDeviceClasses` | 能力位 | 326 | — |
| `getDeviceControllerNumber` | 手柄编号 | 333 | — |
| `getConfiguration` | `.idc` 属性 | 340 | — |
| `getAbsoluteAxisInfo` | 轴的 min/max/flat/fuzz/resolution | 350 | `EVIOCGABS` |
| `hasRelativeAxis` | 相对轴能力 | 380 | bitmask |
| `hasInputProperty` | INPUT_PROP_* | 392 | bitmask |
| `getScanCodeState` | 扫描码当前按下状态 | 404 | `EVIOCGKEY` |
| `getKeyCodeState` | keycode 当前按下状态（反查扫描码） | 420 | `EVIOCGKEY` |
| `getSwitchState` | 开关状态 | 444 | `EVIOCGSW` |
| `getAbsoluteAxisValue` | 轴当前值 | 460 | `EVIOCGABS` |
| `markSupportedKeyCodes` | 批量标记设备是否支持某些 keycode | 482 | — |
| `hasScanCode` / `hasLed` | 能力查询 | 572 / 583 | — |
| `setLedState` | 写 `EV_LED` 事件控制 LED | 595 | `write()` |

`getAbsoluteAxisInfo` 有一个细节：`info.minimum != info.maximum` 才算 `valid`，否则视为无效轴（驱动没上报量程）。

---

## 八、按键映射

`mapKey()`（510）分三步：先查 **KCM**（`KeyCharacterMap::mapKey`，优先），失败再查 **KeyLayout**（`keyLayoutMap->mapKey`），最后用 `kcm->tryRemapKey()` 处理 meta 状态重映射。任一失败则 `outKeycode = 0` 返回 `NAME_NOT_FOUND`。

`mapAxis()`（552）走 KeyLayout 的 `mapAxis`，用于游戏杆轴映射。

`markSupportedKeyCodes()`（482）先用 `findScanCodesForKey` 把 keycode 反查成一组扫描码，再逐一与设备 `keyBitmask` 求交——即"布局声明支持"与"驱动实际会发"取交集。

`setKeyboardLayoutOverlay()`（640）在运行时替换 KCM：`combinedKeyMap = KeyCharacterMap::combine(原 KCM, overlay)`，供 IME / 设置切换键盘布局使用。

`hasKeycodeLocked()`（1687）与 `mapLed()`（1705）是 classes 判定与 LED 控制的基础工具函数。

---

## 九、振动、LED 与手柄编号

`vibrate()`（706）：构造 `ff_effect`（`FF_RUMBLE`，强弱幅度均 0xc000，时长按毫秒换算），`EVIOCSFF` 上传效果拿到 `effect.id`，再写一个 `EV_FF` 事件（value=1）启动，`ffEffectPlaying = true`。`cancelVibrate()`（740）写 value=0 停止。

`getNextControllerNumberLocked()`（1661）：`mControllerNumbers` 位图 `markFirstUnmarkedBit() + 1`（0 保留给非控制器）；满了则返回 0。`setLedForControllerLocked()` 点亮对应编号的 LED。`releaseControllerNumberLocked()` 在设备关闭时归还位。

---

## 十、视频设备（TouchVideoDevice）

这是 Android 10 新增的 `v4l-touch*` 支持，用于给触摸设备配套的热成像/视频流。开关由系统属性 `ro.input.video_enabled` 控制（`isV4lScanningEnabled()`，默认 true）。注释说明：V4L 不支持多客户端，EventHub 一旦打开就独占，因此提供该属性在调试时让其它客户端能直接读 `/dev`。

生命周期：`scanVideoDirLocked()` 扫 `/dev/v4l-touch*` → `openVideoDeviceLocked()` 按**设备名匹配**输入设备，匹配上就挂到 `device->videoDevice` 并注册 epoll，匹配不上先进 `mUnattachedVideoDevices` 暂存队列（后续输入设备可能后到）。`getVideoFrames()`（1059）从 `videoDevice->consumeFrames()` 取出已排队帧供 InputReader 使用。

---

## 十一、调试与排障

`dump()`（1915）输出 `dumpsys input` 中的 "Event Hub State" 段：内置键盘 id、每个设备的 classes / path / enabled / descriptor / location / controllerNumber / uniqueId / bus-vendor-product-version / KeyLayoutFile / KeyCharacterMapFile / ConfigurationFile / 是否有 KCM overlay / VideoDevice，以及未挂载的视频设备列表。

`monitor()`（1971）只是 `mLock.lock(); mLock.unlock();`，供 watchdog 检测 EventHub 是否死锁。

排障要点：

- 设备没被发现：先看 `mExcludedDevices`、再看 `classes == 0` 被丢弃（`ALOGV("Dropping device")`）、再看 `/dev/input` 权限与 SELinux。
- 事件时间戳异常：确认 `EVIOCSCLOCKID` 是否成功（`configureFd` 里 `usingClockIoctl` 日志）。
- 系统因输入无法休眠/输入唤醒失败：检查 `wakeMechanism` 日志（EPOLLWAKEUP / EVIOCSSUSPENDBLOCK / `<none>`）与内核版本。
- 反复重开设备：看 "Reopening all input devices due to a configuration change."（`.idc` 或 keylayout 变更触发 `requestReopenDevices()`）。

---

## 十二、一图流：典型时序

```
[启动]  EventHub()            → epoll_create1 + inotify + wake pipe + PARTIAL_WAKE_LOCK
        InputReader::loopOnce()
            → getEvents(timeout)
                mNeedToScanDevices → scanDevicesLocked()
                    → scanDirLocked("/dev/input") → openDeviceLocked() ×N
                        open → EVIOCGNAME/VERSION/ID/PHYS/UNIQ
                        → assignDescriptorLocked() → EVIOCGBIT×6 + EVIOCGPROP
                        → loadConfigurationLocked() → 判定 classes
                        → loadVirtualKeyMapLocked() / loadKeyMapLocked()
                        → registerDeviceForEpollLocked(EPOLLIN|EPOLLWAKEUP)
                        → configureFd(EVIOCSREP / EVIOCSCLOCKID)
                        → addDeviceLocked() → 挂入 mOpeningDevices
                → 上报 DEVICE_ADDED ×N + FINISHED_DEVICE_SCAN
                → epoll_wait() 阻塞（释放 wake lock）

[有事件] 用户按键 → 内核 evdev → epoll_wait 返回
                → read() 得到 input_event[]
                → processEventTimestamp() 回填 when
                → 转成 RawEvent[] 返回 InputReader
                → InputReader::processEventsLocked() → KeyboardInputMapper
                → ... → InputDispatcher → App

[热插拔] 插入 USB 鼠标 → inotify IN_CREATE → mPendingINotify
                → readNotifyLocked() → openDeviceLocked()
                → 下一轮 getEvents 上报 DEVICE_ADDED
```

---

## 十三、小结

`EventHub` 的设计要点可以概括为五条：单消费者 + 批量拉取（`getEvents` 一次填满调用方缓冲）；epoll 统一监听设备 fd、inotify fd 与 wake pipe；wake lock 与 `epoll_wait` 严格配对以兼顾功耗与唤醒；设备能力完全由 ioctl bitmask 推导，任何 classes 为 0 的设备直接忽略；时间戳与时钟统一到 `CLOCK_MONOTONIC` 以便下游计算延迟。
