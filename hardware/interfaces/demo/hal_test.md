加这两项前，我先找一个现成的 VTS functional 测试作模板，确保 `Android.bp` 和测试写法符合本仓库 Android 10 的真实风格。
用 `light/2.0` 的 VTS 测试作模板最贴近（接口简单、风格标准）。读它的 `Android.bp` 和测试 cpp。
模板已确认（light 用 `cc_test` + `VtsHalTargetTestDefaults`，`static_libs` 链 HAL 库）。现在并行创建 `DemoClient.cpp`、`vts/functional/Android.bp`、`VtsHalDemoV1_0TargetTest.cpp`，并读取 `default/Android.bp` 准备追加 client 模块。
`default/Android.bp` 已读。现在追加 client 模块；同时修正 VTS 的 `SetCallbackOk`——`IDemoCallback` 是抽象接口，不能 `new` 加 lambda，需子类化。两处独立，并行处理。
全部落盘完成。新增/修改文件如下：

```
hardware/interfaces/demo/1.0/
├── default/
│   ├── DemoClient.cpp                       # 新增：独立可执行客户端
│   └── Android.bp                           # 修改：追加 client 模块
└── vts/functional/
    ├── Android.bp                           # 新增：VTS 测试构建
    └── VtsHalDemoV1_0TargetTest.cpp         # 新增：VTS 测试用例
```

`DemoClient.cpp` 演示完整调用链：取服务 → `setValue(42)` 同步返回 → `getValue` 同步回传 42 → `setCallback` 注册异步回调 → `setValue(100)` 触发 `onValueChanged` 回调（通过 condition_variable 等待 2s 确认触发）→ `getStatus` 打印复合 struct。关键是 `main` 里 `configureRpcThreadpool(1, false)` 启动 binder 线程池以接收服务端发起的回调事务，否则回调无法送达客户端进程。

`VtsHalDemoV1_0TargetTest.cpp` 沿用本仓库 `light/2.0` 的真实 VTS 骨架（`VtsHalHidlTargetTestEnvBase` + `VtsHalHidlTargetTestBase`），覆盖：SetGetValue（setValue/getValue 往返）、GetStatus（struct 字段校验）、SetCallbackInvalidArg（`nullptr` 返回 `INVALID_ARG`）、SetCallbackOk（有效回调返回 `OK`）。

编译与运行：

```
# 客户端
m android.hardware.demo@1.0-client
adb shell /vendor/bin/hw/android.hardware.demo@1.0-client   # 需先刷入/同步 vendor 镜像

# VTS 测试
m VtsHalDemoV1_0TargetTest
adb shell /data/local/tmp/VtsHalDemoV1_0TargetTest
# 或用 vts 框架：vts-tradefed run vts -m VtsHalDemoV1_0TargetTest
```

注意 VTS 用例未断言"回调实际被触发"，因为测试进程默认 RPC 线程池行为在 VTS 框架下不确定；回调端到端验证已交由 `DemoClient` 完成。若希望 VTS 也覆盖回调触发，可在测试 `SetUp` 中加 `configureRpcThreadpool` + 等待，需要的话我再加。