# InputClassifier 分析解读（Android 10 / cells-android10）

> 适用工程：`cells-android10` / AOSP `android-10.0.0_r33`（Columbia Cells 树，Pixel 3a XL / coral）
> 源码根：`frameworks/native/services/inputflinger/`（`InputClassifier.{h,cpp}`、`InputClassifierConverter.cpp`、`InputManager.cpp`）
> 配套：本文是 `输入事件处理流程_Android10.md` 里「InputReader → InputDispatcher」链路上**被略过的那一级**的专文解读。

---

## 0. 一图总览（在输入管线中的位置 + 两个类的关系）

```
输入事件管线（system_server 的 inputflinger 内）：

   InputReader ──(mQueuedListener 缓冲)──► InputClassifier ──► InputDispatcher ──(InputChannel)──► App
        │                                     │  mListener = mDispatcher (InputManager.cpp:37)
        │                                     │
        │                       ┌─────────────┴───────────────────┐
        │                       │  InputClassifier（舞台类, .cpp:389）│  InputListenerInterface 一级
        │                       │   - 仅 notifyMotion 非直通 (:416)   │
        │                       │   - 其余事件全部 passthrough        │
        │                       └─────────────┬───────────────────┘
        │                                     │ mMotionClassifier（按需创建, :403）
        │                       ┌─────────────┴───────────────────┐
        │                       │  MotionClassifier（.cpp:144）      │  封装 InputClassifier HAL
        │                       │   mHalThread("InputClassifier")    │  独立线程，异步调 HAL
        │                       │   mEvents(BlockingQueue, MAX=5)    │  + mClassifications 每设备
        │                       └─────────────┬───────────────────┘
        │                                     │ HIDL 跨进程
        │                       ┌─────────────┴───────────────────┐
        │                       │  IInputClassifier HAL (1.0)        │  hardware/interfaces/.../1.0
        │                       │   classify(MotionEvent)→Classification│ reset() / resetDevice(id)
        │                       └────────────────────────────────────┘
```

**一句话**：`InputClassifier` 是 inputflinger 里夹在 `InputReader` 与 `InputDispatcher` 之间的一个**事件舞台（listener stage）**。它对绝大多数事件直接透传，唯独对 touch 类 `NotifyMotionArgs` 追加一个 `MotionClassification`（如 `DEEP_PRESS` 深压），分类结果由一块**可选的、硬件相关的 `InputClassifier HAL`** 在独立线程异步算出——**宁可让分类结果晚 1~2 帧，也绝不阻塞主线程的触摸派发**。

---

## 1. 结论前置

1. **InputClassifier 是「可选增强层」，不是必须路径**：当 deep-press 功能关闭、或设备没有实现 `InputClassifier HAL` 时，`mMotionClassifier` 为 `nullptr`，它就是一个纯透传舞台，对延迟/行为**零影响**。这是它最重要的设计基调——增强项绝不能拖累基线。
2. **核心机制是「异步分类 + 结果滞后复用」**：`notifyMotion`（主线程）只把事件塞进 `BlockingQueue`（`MAX_EVENTS=5`，`.cpp:57`）就立刻返回，读回的是**上一个手势片段已确定的分类**（`getClassification` `.cpp:313`）。真正的 HAL 调用在 `mHalThread`（线程名 `"InputClassifier"`，`.cpp:149`）里做。**分类可容忍 1~2 帧延迟**——因为 HAL 调用没有超时机制，一旦同步调用就会直接拖慢触摸延迟。
3. **它只处理 touch 事件**：`isTouchEvent`（`.cpp:78`）限定 `SOURCE_TOUCHSCREEN` / `SOURCE_TOUCHPAD`；键盘、开关、配置变更、非触摸 motion 全部原样透传（`notifyKey`/`notifySwitch`/`notifyConfigurationChanged` 都是 passthrough）。
4. **分类结果最终透到 App 的 `MotionEvent.getClassification()`**：枚举 `MotionClassification`（`Input.h:228`）只有三种：`NONE`/`AMBIGUOUS_GESTURE`/`DEEP_PRESS`，与 HAL 的 `common::V1_0::Classification` **1:1 对应**（`.cpp:69-74` 的 `static_assert` 锁死）。`DEEP_PRESS` 即 Android 的「深压 / 压力感知」特性。
5. **硬件相关性 + 优雅降级**：HAL 死亡（`serviceDied` `.cpp:444`）→ `mMotionClassifier` 置空 → 自动退回透传；设备重置（`notifyDeviceReset` `.cpp:435`）→ 清掉该设备挂起的事件与分类，防止跨设备/跨手势污染。

---

## 2. 总览

### 2.1 在管线中的装配点（`InputManager.cpp:37-38`）

```cpp
mClassifier = new InputClassifier(mDispatcher);                  // InputManager.cpp:37  listener=下一级 dispatcher
mReader = createInputReader(readerPolicy, mClassifier);          // InputManager.cpp:38  reader 的 listener=classifier
```

`InputReader` 内部用 `QueuedInputListener` 缓冲后，把事件 flush 给 `mClassifier`，`InputClassifier` 再把事件 forward 给 `mListener`（`mDispatcher`）。所以完整链路是 **`InputReader → QueuedInputListener → InputClassifier → InputDispatcher`**——本文专讲中间这级。

### 2.2 两个类各司其职

| 类 | 文件:行 | 角色 |
|---|---|---|
| `InputClassifier` | `InputClassifier.h:233` / `.cpp:389` | **舞台类**，实现 `InputListenerInterface`。决定「哪些事件要分类、怎么透传」，并持有 `MotionClassifier`。 |
| `MotionClassifier` | `InputClassifier.h:114` / `.cpp:144` | **HAL 封装类**，在独立线程里调用 `IInputClassifier` HAL，维护每设备的分类状态。 |
| `IInputClassifier` | `hardware/interfaces/input/classifier/1.0/IInputClassifier.hal` | **HIDL HAL 接口**，硬件厂商可选实现，做手势/深压判定。 |

### 2.3 它解决什么问题

Android 9 起引入 `MotionClassification`，典型用途是「深压（deep press）」：用户比普通点按更用力地按压屏幕时，HAL 结合压力、时序甚至 `videoFrames`（高帧率画面）判断这是「有意的重压」还是「普通触摸」，从而让 App 触发深压菜单等。这类判定需要**跨多个事件、看整段手势**，且可能依赖厂商私有算法/传感器——所以被抽成可选的 HAL，而非写死在 framework。

---

## 3. 分层拆解

### 3.1 `InputClassifier` 舞台类：透传为主，touch 才分类

**构造 + `onFirstRef`（特征开关门控）**
```cpp
InputClassifier::InputClassifier(const sp<InputListenerInterface>& listener)   // .cpp:389
        : mListener(listener) { /* 其余初始化推迟到 onFirstRef：需要 sp<this> 注册 HAL 死亡 */ }

void InputClassifier::onFirstRef() {                                          // .cpp:395
    if (!deepPressEnabled()) {                                                // .cpp:396  读 feature flag
        return;                          // 功能关 → mMotionClassifier 保持 null → 纯透传
    }
    std::scoped_lock lock(mLock);
    mMotionClassifier = std::make_unique<MotionClassifier>(this);              // .cpp:403  传入 this 作死亡回调
}
```

> `deepPressEnabled()`（`.cpp:83`）读服务端可配置标志 `input_native_boot.deep_press_enabled`（默认 `"true"`，`.cpp:52-54`）。标志关闭时**根本不创建** `MotionClassifier`，彻底零开销。

**`notifyMotion`——唯一的非透传路径（`.cpp:416`）**
```cpp
void InputClassifier::notifyMotion(const NotifyMotionArgs* args) {             // .cpp:416
    std::scoped_lock lock(mLock);
    const bool sendToMotionClassifier = mMotionClassifier && isTouchEvent(*args); // .cpp:419  仅 touch
    if (!sendToMotionClassifier) {
        mListener->notifyMotion(args);                                        // .cpp:421  非 touch / 未启用：直接透传
        return;
    }
    NotifyMotionArgs newArgs(*args);
    newArgs.classification = mMotionClassifier->classify(newArgs);            // .cpp:426  取「当前已知」分类
    mListener->notifyMotion(&newArgs);                                        // .cpp:427  带 classification 继续下行
}
```

其余 `notifyKey`/ `notifySwitch`/ `notifyConfigurationChanged` 全是 `mListener->...`（passthrough，`.cpp:406/411/430`）。

### 3.2 `MotionClassifier` + HAL 线程：异步分类的核心（`.cpp:144` 起）

这是整个组件最值得理解的设计——**为什么用独立线程 + 队列**：

```cpp
MotionClassifier::MotionClassifier(...) : mDeathRecipient(deathRecipient), mEvents(MAX_EVENTS) {  // .cpp:144
    mHalThread = std::thread(&MotionClassifier::callInputClassifierHal, this);   // .cpp:146  专用线程
    pthread_setname_np(mHalThread.native_handle(), "InputClassifier");          // .cpp:149  线程名便于调试
}
```

**主线程侧 `classify`（非阻塞，`.cpp:324`）**
```cpp
MotionClassification MotionClassifier::classify(const NotifyMotionArgs& args) {  // .cpp:324
    if ((args.action & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_DOWN)
        updateLastDownTime(args.deviceId, args.downTime);                       // .cpp:325  记录本次手势起点
    ClassifierEvent event(std::make_unique<NotifyMotionArgs>(args));
    enqueueEvent(std::move(event));                                             // .cpp:330  入队（不等待 HAL）
    return getClassification(args.deviceId);                                    // .cpp:331  返回「上一次」算出的分类
}
```

**HAL 线程侧 `callInputClassifierHal`（`.cpp:216`）**
```cpp
void MotionClassifier::callInputClassifierHal() {                               // .cpp:216  仅在 HAL 线程
    const bool initialized = init();                                            // .cpp:218  getService + linkToDeath
    ...
    while (true) {
        ClassifierEvent event = mEvents.pop();                                  // .cpp:234  阻塞取事件
        switch (event.type) {
            case MOTION: {
                common::V1_0::MotionEvent motionEvent =
                        notifyMotionArgsToHalMotionEvent(*motionArgs);          // .cpp:240  转 HIDL 结构
                Return<common::V1_0::Classification> response =
                        mService->classify(motionEvent);                        // .cpp:241  跨进程调 HAL
                updateClassification(deviceId, eventTime,                       // .cpp:245  存回“本次”分类
                        getMotionClassification(halClassification));
                break; }
            case DEVICE_RESET: mService->resetDevice(deviceId); ...             // .cpp:252
            case HAL_RESET:    mService->reset(); clearClassifications(); ...    // .cpp:257
            case EXIT:        clearClassifications(); return;                   // .cpp:261
        }
    }
}
```

**设计要点解读**
- **延迟可接受的代价换「零阻塞」**：HAL 调用没有超时（注释 `.cpp:211-214` 明说）。若在主线程同步调，HAL 卡顿会直接拖慢全系统触摸。把它挪到 `mHalThread`，主线程永远不等——代价是分类结果**天然滞后 1~2 帧**（本帧拿到的是上一帧算完的值）。对深压判定这种「迟一点也没关系」的场景完全可接受。
- **每设备独立状态**：`mClassifications`、`mLastDownTimes` 都以 `deviceId` 为 key（`.cpp:189/211`），多指/多设备互不干扰。
- **队列满即降级**：`enqueueEvent`（`.cpp:275`）若 `MAX_EVENTS=5` 装不下（说明 HAL 处理太慢），丢弃该事件并 `reset()`（`.cpp:280`），宁可不要分类也不能积压拖死链路。

### 3.3 HAL 接口 `IInputClassifier`（`IInputClassifier.hal`）

```
interface IInputClassifier {
  classify(MotionEvent event) generates (Classification classification);  // 给当前手势序列一个分类
  reset();                                                              // 重置 HAL 全部内部状态
  resetDevice(int32_t deviceId);                                        // 重置某设备状态
};
```

- HAL 可能**缓存整段手势**来判定（而非只看单帧），并可能利用 `NotifyMotionArgs::videoFrames`（高帧率画面）辅助决策（头文件 `.h:106-113` 注释）。
- native → HIDL 转换由 `notifyMotionArgsToHalMotionEvent`（`InputClassifierConverter.cpp:354`）完成。

### 3.4 `MotionClassification` 枚举与到 App 的传递（`Input.h:228`）

```cpp
enum class MotionClassification : uint8_t {       // frameworks/native/include/input/Input.h:228
    NONE = 0,                  // 无分类
    AMBIGUOUS_GESTURE = 1,      // 尚早，无法判定（需更多事件）
    DEEP_PRESS = 2,             // 用户有意重压屏幕
};
```

`InputClassifier.cpp:68-76` 用 `static_assert` 把 native 枚举与 HAL `common::V1_0::Classification` **锁成同值**，保证 1:1 转换无映射表。该字段最终写入 `NotifyMotionArgs::classification`，经 `InputDispatcher` → `InputChannel` → 到 App 的 `MotionEvent.getClassification()`。

### 3.5 健壮性机制（`.cpp` 多处）

| 机制 | 位置 | 作用 |
|---|---|---|
| 特征开关门控 | `deepPressEnabled` `.cpp:83` / `onFirstRef` `.cpp:396` | 关闭则 `MotionClassifier` 不创建，零开销透传 |
| HAL 死亡 → 降级 | `serviceDied` `.cpp:444` | `mMotionClassifier = nullptr`，后续变纯透传 |
| 设备重置 | `notifyDeviceReset` `.cpp:435` → `reset(args)` `.cpp:343` | 擦掉该设备挂起事件 + 通知 HAL `resetDevice`，防残留 |
| 迟到分类丢弃 | `updateClassification` `.cpp:289` | 若 `eventTime < lastDownTime`（新手势已开始），丢旧分类，防污染当前手势 |
| 队列满降级 | `enqueueEvent` `.cpp:275` | HAL 太慢则丢事件 + `reset()`，保护延迟 |
| 初始化失败降级 | `callInputClassifierHal` `.cpp:219` | 拿不到 HAL 就通知死亡回调并退出线程，不留半成品 |

---

## 4. 速查表（函数 / 字段 → 文件 → 行）

| 项 | 文件 | 行 | 说明 |
|---|---|---|---|
| 管线装配 `mClassifier` / `mReader` | `InputManager.cpp` | `:37` / `:38` | Reader→Classifier→Dispatcher |
| `InputClassifier` 类 | `InputClassifier.h` | `:233` | 舞台类，实现 `InputListenerInterface` |
| `MotionClassifier` 类 | `InputClassifier.h` | `:114` | HAL 封装，独立线程 |
| `IInputClassifier` HAL | `IInputClassifier.hal` | `:27` / `:39` | `classify` / `resetDevice` / `reset` |
| `MAX_EVENTS=5` | `InputClassifier.cpp` | `:57` | 事件队列上限 |
| `getMotionClassification` + static_assert | `InputClassifier.cpp` | `:68` / `:69-74` | 枚举 1:1 映射 |
| `isTouchEvent` | `InputClassifier.cpp` | `:78` | 仅 TOUCHSCREEN/TOUCHPAD |
| `deepPressEnabled`（feature flag） | `InputClassifier.cpp` | `:83` | `input_native_boot.deep_press_enabled` |
| `MotionClassifier` 构造 + 线程 | `InputClassifier.cpp` | `:144` / `:146` / `:149` | 线程名 `"InputClassifier"` |
| `init()`（getService/linkToDeath） | `InputClassifier.cpp` | `:157` / `:160` / `:170` | HAL 绑定 |
| `callInputClassifierHal`（HAL 循环） | `InputClassifier.cpp` | `:216` / `:241` | 真正调 `classify` |
| `enqueueEvent`（队列满降级） | `InputClassifier.cpp` | `:275` / `:280` | |
| `updateClassification`（迟到丢弃） | `InputClassifier.cpp` | `:289` / `:293` | |
| `classify`（主线程非阻塞） | `InputClassifier.cpp` | `:324` / `:330` / `:331` | 入队 + 返回已知分类 |
| `InputClassifier::onFirstRef` | `InputClassifier.cpp` | `:395` / `:403` | 按需建 `MotionClassifier` |
| `InputClassifier::notifyMotion` | `InputClassifier.cpp` | `:416` / `:426` | 唯一非透传 |
| `notifyDeviceReset` | `InputClassifier.cpp` | `:435` | 设备重置透传 + 清状态 |
| `serviceDied`（HAL 死亡降级） | `InputClassifier.cpp` | `:444` / `:448` | 置空 → 透传 |
| `notifyMotionArgsToHalMotionEvent` | `InputClassifierConverter.cpp` | `:354` | native→HIDL 转换 |
| `MotionClassification` 枚举 | `Input.h` | `:228` | NONE/AMBIGUOUS_GESTURE/DEEP_PRESS |

---

## 5. 调试验证

| 目的 | 手段 | 关联行 / 关键字 |
|---|---|---|
| 是否启用分类 | `adb shell dumpsys input` | 看 `Input Classifier State:`（`.cpp:455`）下 `Motion Classifier:` 是否为 `<nullptr>`（未启用时是） |
| HAL 是否在线 | `dumpsys input` | `mService status: running/null/not responding`（`.cpp:353-360`） |
| 各设备当前分类 | `dumpsys input` | `mClassifications, mLastDownTimes` 表（`.cpp:369`） |
| 设备无 HAL | `logcat -s InputClassifier` | `"Could not obtain InputClassifier HAL"`（`.cpp:164`）——正常（无 HAL 时静默退化） |
| HAL 死亡 | `logcat -s InputClassifier` | `"InputClassifier HAL has died. Setting mMotionClassifier to null"`（`.cpp:447`） |
| 队列满/慢 HAL | `logcat -s InputClassifier` | `"Dropped event with eventTime"`（`.cpp:279`） |
| 开关 deep-press | `adb shell device_config put input_native_boot deep_press_enabled false` | 重启 SF 后 `mMotionClassifier` 变 null |
| App 侧验证分类 | 代码读 `MotionEvent.getClassification()` | `MotionClassification` 三类（`Input.h:228`） |

---

## 6. 踩坑清单（InputClassifier 专属）

1. **它不是「输入加速/拦截」组件，别和 InputFilter/Blocker 混淆**：它只给 touch 事件**贴一个分类标签**，不改坐标、不丢事件（除非队列满降级）。排查「触摸被吞/坐标错」不要来翻这里。
2. **分类结果天然滞后 1~2 帧**：`classify()` 返回的是前几帧由 HAL 算出的分类，不是当前帧。App 端若发现「深压判定比实际按压晚一点点」是设计预期，不是 bug。
3. **仅 touch 源生效**：`isTouchEvent` 限定 `TOUCHSCREEN`/`TOUCHPAD`（`.cpp:78`）。鼠标、笔（stylus 若 source 不是 touch）、轨迹球都不会被分类——确认 `MotionEvent.getSource()` 再排查「为什么没分类」。
4. **HAL 缺失/死亡 = 静默退化**：无 HAL 时只打 `ALOGI` 不报错（`.cpp:164`），`mMotionClassifier` 后续置空；功能「没了」但不崩。要确认 deep-press 是否生效，先看 `dumpsys input` 里 `Motion Classifier` 是不是 `<nullptr>`。
5. **特征标志决定一切**：`deepPressEnabled` 默认 true，但可被 `device_config` 关掉。改了 flag 必须**重启 surfaceflinger/inputflinger 进程**（`onFirstRef` 在构造期只跑一次）才生效——这是最容易被忽略的「为什么改了没反应」。
6. **`onFirstRef` 才建 `MotionClassifier`**：构造器（`.cpp:389`）故意留空，因为 `serviceDied` 回调需要 `sp<this>`（只有 `onFirstRef` 时才有）。任何想在构造器里访问 `mMotionClassifier` 的逻辑都拿不到正确状态。
7. **它不是 framework 内算法**：判定逻辑在厂商 HAL（HIDL `IInputClassifier`），framework 只负责「采集事件 → 转结构 → 异步送 HAL → 收回标签」。要改「怎么判定深压」，改的是 HAL 实现，不是 `InputClassifier.cpp`。
8. **HAL 调用无超时**：注释（`.cpp:211-214`）明确无超时机制，所以才必须独立线程。若你自定义 HAL 且 `classify` 耗时，会直接撑爆 `MAX_EVENTS=5` 队列触发降级——HAL 必须快。

---

## 7. 与既有文档衔接

- 本文补全 `输入事件处理流程_Android10.md` 中「InputReader → InputDispatcher」之间被略过的 **InputClassifier 一级**（完整链路：`InputReader → QueuedInputListener → InputClassifier → InputDispatcher`）。
- 更上游的「事件怎么从 Reader 加工、经 Dispatcher 找焦点窗口」见该流程文档；本文聚焦这级「分类贴标」舞台自身。
- 下游到 App 后，`MotionEvent.getClassification()` 的值即本文 `MotionClassification` 枚举（`Input.h:228`）。
