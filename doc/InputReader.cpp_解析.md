# InputReader.cpp 代码解析报告

## 摘要

`InputReader.cpp` 是 Android 10 输入系统 InputFlinger 中 **InputReader 线程的核心实现**，位于 `frameworks/native/services/inputflinger/`，全文约 7534 行。它的职责是把 EventHub 从内核 evdev 读出的原始输入事件（`RawEvent`）加工成 Android 标准的按键/触摸事件（`NotifyKeyArgs` / `NotifyMotionArgs`），通过 `QueuedInputListener` 交给下游的 `InputClassifier` → `InputDispatcher`。本报告基于对该文件及关联头文件 `InputReader.h`、`InputListener.h/cpp`、`InputManager.cpp` 的逐行分析，梳理其架构、类结构、核心流程与关键设计细节。

## 背景：InputReader 在输入链路中的位置

Android 输入事件流水线为：内核 evdev 驱动 → EventHub → **InputReader** → InputClassifier → InputDispatcher → 应用窗口。`InputManager`（InputManager.cpp:36-49）在 `initialize()` 中创建两条线程：优先级为 `PRIORITY_URGENT_DISPLAY` 的 `InputDispatcher` 线程和 `InputReader` 线程。本文件即 InputReader 线程 `loopOnce()` 循环体的全部逻辑。InputReader 的所有对外状态由单一互斥锁 `mLock` 保护，持锁期间允许调用 EventHub 与 Policy，但绝不持锁调用 Listener（因为 Listener 会经 Dispatcher → WMS 回调 InputReader，可能死锁，见 341-346 行注释）。

## 一、文件内的类型全景

按代码顺序，文件定义了以下主要类型：

| 分类 | 类型 | 行号 | 职责 |
|---|---|---|---|
| 读者主体 | `InputReader` | 263-911 | 设备管理、主循环、配置刷新、状态查询 |
| | `InputReader::ContextImpl` | 916-979 | 向 Mapper 暴露全局状态的上下文 |
| 设备抽象 | `InputDevice` | 983-1286 | 单设备，持有一组 Mapper |
| 累积器 | `CursorButtonAccumulator` | 1288-1365 | 鼠标/触摸板按键 |
| | `CursorMotionAccumulator` | 1368-1398 | REL_X/REL_Y 相对位移 |
| | `CursorScrollAccumulator` | 1401-1437 | 滚轮 REL_WHEEL/REL_HWHEEL |
| | `TouchButtonAccumulator` | 1440-1582 | 触摸/触控笔按键与工具类型 |
| 数据结构 | `RawPointerAxes` | 1585-1605 | 原始轴信息集合 |
| | `RawPointerData` | 1608-1647 | 原始指针数据（含 idToIndex、hovering/touching IdBits） |
| | `CookedPointerData` | 1650-1674 | 加工后指针数据（PointerProperties/PointerCoords） |
| 运动累积器 | `SingleTouchMotionAccumulator` | 1677-1729 | 单点协议 A |
| | `MultiTouchMotionAccumulator`（含嵌套 `Slot`） | 1732-1921 | 多点协议 B slot 机制 |
| Mapper 基类 | `InputMapper` | 1924-2014 | 纯虚 `process()` + 默认实现 |
| 具体 Mapper | `SwitchInputMapper` | 2016-2071 | EV_SW 开关事件 |
| | `VibratorInputMapper` | 2073-2183 | FF 振动波形播放（状态机） |
| | `KeyboardInputMapper` | 2186-2521 | 按键、meta 状态、LED 灯 |
| | `CursorInputMapper` | 2524-2942 | 鼠标/轨迹球，含 Pointer Capture |
| | `RotaryEncoderInputMapper` | 2944-3054 | 旋钮（如手表数码表冠） |
| | `TouchInputMapper` | 3056-6852 | 触摸总管线（最大、最复杂） |
| | `SingleTouchInputMapper` | 6854-6921 | 单点触摸 |
| | `MultiTouchInputMapper` | 6924-7069 | 多点触摸 |
| | `ExternalStylusInputMapper` | 7071-7140 | 外接触控笔数据融合 |
| | `JoystickInputMapper` | 7143-7532 | 游戏手柄 |

## 二、主循环与事件分发主干

### 2.1 `InputReader::loopOnce()`（286-348）

每个循环周期做六件事：其一，持锁判断是否有待处理的配置变更（`mConfigurationChangesToRefresh`）或计算距 `mNextTimeout` 的超时毫秒数；其二，调用 `mEventHub->getEvents()` 阻塞拉取原始事件到 `mEventBuffer`（容量 256）；其三，在锁内 `processEventsLocked()`；其四，检查超时到期则 `timeoutExpiredLocked()`；其五，若设备代次 `mGeneration` 变化则向 Policy 上报设备列表；其六，**在锁之外**调用 `mQueuedListener->flush()` 把本周期累积的事件统一发给下游。最后一步必须在锁外，原因是 Listener 链路（Dispatcher → WMS → 可能回调 InputReader 的 `getScanCodeState` 等）会造成死锁，代码注释明确记录了这一点。

### 2.2 `processEventsLocked()`（350-386）

把同一设备的连续原始事件聚成一个 batch 一次性交给该设备；遇到 `FIRST_SYNTHETIC_EVENT` 以上的合成事件则分派给 `DEVICE_ADDED` / `DEVICE_REMOVED` / `FINISHED_DEVICE_SCAN` 三个处理器。

### 2.3 设备创建与 Mapper 挂载（`createDeviceLocked`，447-520）

这是理解整个文件的钥匙：EventHub 按设备能力给出 `INPUT_DEVICE_CLASS_*` 位掩码，InputReader 据此为每个设备挂载对应 Mapper——如 `TOUCH_MT` 挂 `MultiTouchInputMapper`、`TOUCH` 挂 `SingleTouchInputMapper`、`KEYBOARD`/`DPAD`/`GAMEPAD` 组合挂 `KeyboardInputMapper`、`CURSOR` 挂 `CursorInputMapper`、`JOYSTICK` 挂 `JoystickInputMapper` 等。**一个设备可挂多个 Mapper**（例如带按键的触摸屏同时有 Touch 与 Keyboard Mapper），InputDevice 的 `process()`（1132-1167）把每个原始事件依次分发给所有 Mapper。

### 2.4 事件丢失处理（1145-1164）

`InputDevice::process()` 检测到 `EV_SYN/SYN_DROPPED` 时置位 `mDropUntilNextSync` 并 `reset()`，之后丢弃所有事件直到下一个 `SYN_REPORT`——这是内核事件缓冲区溢出（buffer overrun）后的标准恢复手段。

## 三、核心设计模式

### 3.1 SYN_REPORT 同步点模式

全文所有 Mapper 遵循同一节律：`process()` 只把数据写进各自 Accumulator，遇到 `EV_SYN/SYN_REPORT` 才调用 `sync()` 真正产出并上报事件。这样保证了"一帧驱动数据的原子性"——应用收到的坐标、按键状态、按钮状态永远自洽。MultiTouch 协议 B 中 `SYN_MT_REPORT` 表示"一个指针的数据汇报完毕"，用于驱动 `mCurrentSlot += 1`（1868-1870）。

### 3.2 三级数据流：Raw → Cook → Dispatch

`TouchInputMapper` 是全书最复杂的部分，其内部管线为：

1. 原始事件经 Accumulator 累积，在 `sync()`（4334-4374）中生成 `RawState`（含 `RawPointerData`），压入 `mRawStatesPending` 队列；
2. `processRawTouches()`（4376-4425）逐帧消费（等待外接触控笔数据时会延迟消费并注册超时）；
3. `cookPointerData()`（4960-5222）把原始坐标做校准、缩放、旋转，换算成显示坐标，生成 `CookedState`；
4. `cookAndDispatch()`（4427-4551）决定派发路径：`DEVICE_MODE_DIRECT` 直接派发触摸事件序列（DOWN/MOVE/POINTER_DOWN/POINTER_UP/UP/HOVER_*），`DEVICE_MODE_POINTER` 则走指针手势识别，把触摸板手势翻译成鼠标指针事件。

`dispatchTouches()`（4780-4869）严格按协议顺序发事件：先 POINTER_UP、再 MOVE、后 POINTER_DOWN，并用 `updateMovedPointers()` 保证抬起手指时其他手指的新位置能同时送达。`dispatchMotion()`（6497-6545）统一处理 action 的 index 编码，并在单指针首末事件时把 POINTER_DOWN/UP 改写为 DOWN/UP。

### 3.3 指针 ID 分配（`assignPointerIds`，6615-6800）

多点触控协议 B 直接用驱动给的 `ABS_MT_TRACKING_ID` 做稳定 id（MultiTouchInputMapper::syncTouch，6995-7021）。非 slot 协议则用**最小堆 + 贪心匹配**：计算当前帧与上一帧所有同工具类型指针两两之间的平方欧氏距离建堆，按距离从小到大配对，剩余的新指针分配未使用的 id。这是保证手指跨帧身份稳定的经典算法。

### 3.4 校准体系（`parseCalibration`/`resolveCalibration`，4005-4143）

`TouchInputMapper` 支持五组校准，全部通过设备配置属性（`touch.size.calibration`、`touch.pressure.calibration`、`touch.orientation.calibration`、`touch.distance.calibration`、`touch.coverage.calibration`）控制，先在 `parseCalibration()` 解析、再在 `resolveCalibration()` 依据设备实际拥有的轴做默认值归并（如设备有压力轴则默认 `physical` 校准，否则 `none`）。位置校准还支持厂商下发的仿射变换矩阵 `TouchAffineTransformation`（4259-4262），通过 Policy 获取。

### 3.5 旋转与视口适配（`configureSurface`，3503-3927）

`findViewport()`（3458-3501）确定设备关联的 DisplayViewport（可按显示端口、uniqueId、内外屏类型查找，找不到时退化为非显示视口）；随后按四种屏幕旋转方向把 viewport 尺寸换算成自然方向的 surface 尺寸，计算 `mXScale/mYScale/mXTranslate/mYTranslate` 等变换因子，并据此推导各轴的 `MotionRange`。触摸屏默认 `orientationAware`，`cookPointerData` 中再按 `mSurfaceOrientation` 把坐标旋转回逻辑显示方向（5142-5187）。

### 3.6 虚拟按键（3942-4003、4648-4761）

老式带虚拟键的触摸屏（如早期 Android 手机的四大电容键）由 EventHub 提供 `VirtualKeyDefinition`，`configureVirtualKeys()` 把显示坐标换算成触摸坐标的命中框；`consumeRawTouches()` 在手指按下时判断是否落在屏幕外并命中虚拟键，是则派发按键事件并"吃掉"这路触摸；并有 `virtualKeyQuietTime` 静默期机制（4742-4745）过滤误触。

### 3.7 外接触控笔数据融合（4553-4646）

`ExternalStylusInputMapper` 单独成设备（`INPUT_DEVICE_CLASS_EXTERNAL_STYLUS`），其 `sync()`（7118-7140）把压力/按钮/工具类型汇成 `StylusState` 广播给所有设备。`TouchInputMapper` 在 DIRECT 模式下等待触控笔数据与触摸数据配对（`EXTERNAL_STYLUS_DATA_TIMEOUT` = 72ms，第 79 行），超时则按纯触摸处理，以此实现"触摸屏 + 外接蓝牙笔"的压力融合。

### 3.8 触摸板手势状态机（`preparePointerGestures`，5505-6265）

`PointerGesture` 定义了九种模式：`NEUTRAL`/`TAP`/`TAP_DRAG`/`BUTTON_CLICK_OR_DRAG`/`HOVER`/`PRESS`/`SWIPE`/`FREEFORM`/`QUIET`。核心判别逻辑：两指按下未动时先假设 `PRESS`；两指同向移动超过阈值且位移向量点积余弦大于 `pointerGestureSwipeTransitionAngleCosine` 时判定为 `SWIPE`，否则 `FREEFORM`；超过两指直接 `FREEFORM`。`QUIET` 态用于手势切换后的静默，防止误触。该状态机还集成了 `VelocityTracker`（拖动时自动切换到速度最快的手指）与 `VelocityControl`（指针加速度）。

### 3.9 其他值得注意的机制

- **策略标志**：`POLICY_FLAG_WAKE`（外部设备按键/移动才唤醒，内部设备默认不唤醒以防口袋里误触，2431-2433、2821-2823）、`POLICY_FLAG_VIRTUAL`、`POLICY_FLAG_GESTURE`、`POLICY_FLAG_DISABLE_KEY_REPEAT`。
- **旋转感知按键**：`rotateKeyCode()`（181-186）把 DPAD/导航键随屏幕旋转做映射（`keyCodeRotationMap`，138-153）；`rotateDelta()`（188-208）旋转鼠标位移。
- **meta 状态与 LED**：`KeyboardInputMapper` 维护 Caps/Num/Scroll Lock 的 meta 状态并驱动键盘 LED 灯（2490-2521）；Reader 级 `mGlobalMetaState` 聚合所有设备（576-583）。
- **触摸延迟统计**：`reportEventForStatistics()`（4310-4321）统计事件从内核到 Reader 的延迟，每 5 分钟经 statsd 上报（`TOUCH_EVENT_REPORTED`）。
- **Android 10 新特性**：`TouchVideoFrame`（6536-6538）——`dispatchMotion` 时从 EventHub 取触摸视频帧并按表面方向旋转后随事件下发；`InputClassifier` 链路（InputManager.cpp:37-38，Reader 的 Listener 是 Classifier 而非 Dispatcher 本身）；代码广泛使用 `std::optional`/`std::vector`。

## 四、潜在风险点

分析中发现几处值得注意的代码脆弱点。`VibratorInputMapper::vibrate()`（2111）直接 `memcpy` 调用方传入的 pattern 到定长数组成员 `mPattern[MAX_VIBRATE_PATTERN_SIZE]`，代码本身未做长度钳制，安全性依赖调用方契约。`assignPointerIds()`（6654）在栈上分配 `MAX_POINTERS * MAX_POINTERS` 大小的堆数组，在指针数达到上限时栈占用可观。`mRawStatesPending` 在等待外接触控笔数据期间可能积压多帧（4411-4424 有超时兜底）。此外 `InputDevice::process()` 对 `SYN_DROPPED` 的恢复依赖驱动行为，异常固件下存在反复复位的可能。

## 五、结论

`InputReader.cpp` 是 Android 输入系统的"翻译层"：向下通过 EventHub 消费 Linux 输入子系统事件，向上产出标准化的 Android 输入事件。其架构可概括为"InputReader（设备管理 + 主循环）→ InputDevice（设备抽象）→ InputMapper（协议翻译）→ Accumulator（按 SYN_REPORT 归并）→ Listener（事件出口）"五层，辅以 QueuedInputListener 实现锁外投递避免死锁。触摸部分（TouchInputMapper 及其子类）占了全文约一半篇幅，集中体现了校准、坐标变换、多指跟踪、手势识别、虚拟键、触控笔融合等复杂工程折中，是阅读和理解本文件的重心。对于在本项目（cells-android10）上做输入相关定制（如触摸调试、手势调整、唤醒策略、虚拟键问题排查）的开发者，建议重点阅读 `loopOnce`、`createDeviceLocked`、`TouchInputMapper::configureSurface`、`cookPointerData`、`dispatchTouches` 与 `assignPointerIds` 六处。

## 局限

本解析基于 Android 10 版本的静态代码阅读，未结合运行时日志或动态调试验证；部分行为（如 InputClassifier 的触摸分类策略、EventHub 的 evdev 细节）在关联文件中展开，本报告仅描述其接口边界。报告中的行号对应当前工作区快照，若文件被修改可能偏移。

## 参考

1. [InputReader.cpp（本仓库，frameworks/native/services/inputflinger/InputReader.cpp）](frameworks/native/services/inputflinger/InputReader.cpp)
2. [InputReader.h（frameworks/native/services/inputflinger/InputReader.h）](frameworks/native/services/inputflinger/InputReader.h)
3. [include/InputListener.h（frameworks/native/services/inputflinger/include/InputListener.h）](frameworks/native/services/inputflinger/include/InputListener.h)
4. [InputListener.cpp（frameworks/native/services/inputflinger/InputListener.cpp）](frameworks/native/services/inputflinger/InputListener.cpp)
5. [InputManager.cpp（frameworks/native/services/inputflinger/InputManager.cpp）](frameworks/native/services/inputflinger/InputManager.cpp)
6. [EventHub.h（frameworks/native/services/inputflinger/EventHub.h）](frameworks/native/services/inputflinger/EventHub.h)
