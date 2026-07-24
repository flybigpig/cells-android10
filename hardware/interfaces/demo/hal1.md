先核实 `atrace` 的 `default/Android.bp` 与顶层 `hidl_defaults` 定义的真实写法，确保示例风格一致、可编译。
模板风格已确认（`hidl_defaults` 在顶层定义，`default/Android.bp` 与 atrace 完全对应）。现在把整套 `android.hardware.demo@1.0` HAL 落盘。
全部 10 个文件已落盘到 `hardware/interfaces/demo/1.0/`。结构如下：

```
hardware/interfaces/demo/1.0/
├── Android.bp                              # hidl_interface 定义
├── types.hal                               # Result 枚举 + DemoStatus struct
├── IDemo.hal                               # 主接口(4 个方法)
├── IDemoCallback.hal                       # 异步回调接口
└── default/
    ├── Android.bp                          # cc_binary 服务(继承 hidl_defaults)
    ├── Demo.h / Demo.cpp                    # IDemo 实现
    ├── service.cpp                          # main() + registerAsService
    ├── android.hardware.demo@1.0-service.rc
    └── android.hardware.demo@1.0-service.xml  # VINTF fragment
```

接口覆盖 HIDL 四种典型能力：`setValue` 同步单返回值、`getValue` 同步多返回值（callback 回传）、`setCallback` 注册异步回调、`getStatus` 返回复合 struct。

编译与验证（在 AOSP 根目录，需先 `source build/envsetup.sh && lunch` 选好 target）：

```
m android.hardware.demo@1.0            # 生成接口库 + 头文件(hidl-gen 等价)
m android.hardware.demo@1.0-service    # 编译服务二进制 + rc/xml
```

由于 `default/Android.bp` 带了 `vintf_fragments`，xml 会被自动合并进 device manifest 并随 vendor 镜像打包；开机由 `init` 依据 `.rc` 拉起 `/vendor/bin/hw/android.hardware.demo@1.0-service`，并注册 `default` 实例到 `hwservicemanager`。顶层 `hardware/interfaces/Android.bp` 用的是 `hidl_package_root { use_current: true }`，自动发现子包，**无需手动登记**。

开机后检查：

```
adb shell lshal | grep demo          # 应看到 android.hardware.demo@1.0::IDemo/default
adb shell ps -A | grep demo-service  # 服务进程应运行
```

客户端（Native）调用骨架：

```cpp
sp<IDemo> demo = IDemo::getService("default");   // 或 waitForHwService
demo->setValue(42);
demo->getValue([&](Result r, uint32_t v){ /* v == 42 */ });
demo->setCallback(sp<IDemoCallback>(new MyCallback));  // 之后 setValue 触发 onValueChanged
```

需要我再加一个 `default/DemoClient.cpp`（可执行测试，验证完整调用链）或 `1.0/vts/functional/` 的 VTS 测试吗？




-------

先读取 demo HAL 当前各文件，确认回调机制现状，再决定如何"添加回调"。
demo HAL 已有 `onValueChanged` 回调。你说的"添加回调"我理解为给 `IDemoCallback.hal` 再加一个回调方法。我新增 `onStatusChanged(DemoStatus status)`，并配套加一个 `notifyStatus()` 触发方法，使整条链路可编译、可测试。先读取需同步改动的客户端与 VTS 文件。
已读取全部相关文件。demo HAL 当前已有 `onValueChanged` 回调，我新增第二个回调 `onStatusChanged(DemoStatus status)`，并加 `notifyStatus()` 作为触发入口，保证整条链路（HAL 定义→实现→客户端→VTS）可编译。6 个文件分别改动：
已给 demo HAL 的 `IDemoCallback.hal` 新增第二个回调 `onStatusChanged(DemoStatus status)`，并打通整条链路。改动如下：

`IDemoCallback.hal`：接口新增 `onStatusChanged(DemoStatus status);`（`DemoStatus` 来自同包 `types.hal`，自动可见）。

`IDemo.hal`：主接口新增触发入口 `notifyStatus() generates (Result result);`——客户端调用它主动让服务端推送一次状态。

`default/Demo.h` + `Demo.cpp`：`Demo` 实现 `notifyStatus()`，若已注册回调则构造当前 `DemoStatus`（含 `mCounter`）并调用 `mCallback->onStatusChanged(status)`，否则返回 `NOT_SUPPORTED`。与现有 `setValue` 触发 `onValueChanged` 形成两条独立回调路径。

`client/DemoClient.cpp`：`DemoCallback` 补上 `onStatusChanged` 实现（用 `gStatusFired` 标记），`main` 里在 `setCallback` 后新增步骤 4.5 调用 `notifyStatus()` 并等待 `onStatusChanged` 在 binder 线程上触发；最终返回码改为 `gCallbackFired && gStatusFired` 才算全链路通过。

`vts/functional/VtsHalDemoV1_0TargetTest.cpp`：`NoopCallback` 必须补 `onStatusChanged` 实现，否则因 `IDemoCallback` 变抽象类而无法编译（这是新增回调后最易踩的坑——所有实现方都要补方法）。

关键设计点：HIDL 回调是"服务端反向调用客户端"的单向异步通道。客户端实现 `IDemoCallback` 并 `setCallback` 注册，服务端持有 `sp<IDemoCallback>` 在合适时机调用；回调事务经 `hwbinder` 回到客户端进程，因此客户端必须 `configureRpcThreadpool` 启动 binder 线程池才能收到。

编译与验证：

```
m android.hardware.demo@1.0            # 重新生成接口（含新回调）
m android.hardware.demo@1.0-service
m android.hardware.demo@1.0-client
m VtsHalDemoV1_0TargetTest

adb shell /vendor/bin/hw/android.hardware.demo@1.0-client   # 应看到 onValueChanged 与 onStatusChanged 均触发，返回 0
```

如果你希望 `onStatusChanged` 改为在 `setValue` 时被自动连带触发（而不是单独 `notifyStatus()`），或想给 atrace HAL 也照此加回调，告诉我即可。