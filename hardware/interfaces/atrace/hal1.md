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