# 集成 customservice（Native Binder 服务）到系统

## 1. 添加到产品构建

在你的产品 .mk 文件中（例如 `device/google/coral/` 下相关 .mk 或 cells 构建配置）添加：

    PRODUCT_PACKAGES += customservice customservice_test

- `customservice`：守护进程二进制（含 init_rc，自动安装到 `/system/bin` 与 `/system/etc/init`）。
- `customservice_test`：可选的命令行客户端，用于验证服务是否正常工作。

## 2. 目录结构

frameworks/native/services/customservice/
├── Android.bp              # 构建配置（两个 cc_binary）
├── IYourService.h          # Binder 接口声明（含 Bp / Bn）
├── IYourService.cpp        # Bn 端 onTransact 分发 + IMPLEMENT_META_INTERFACE
├── YourService.h           # 服务实现头文件
├── YourService.cpp         # 服务具体实现（doSomething / getStatus / setConfig）
├── main_customservice.cpp  # 进程入口（参考 mediaserver）
├── test_client.cpp         # 独立客户端测试程序
├── customservice.rc        # init 启动配置
└── bash.sh                 # 手动调试脚本

## 3. 服务名约定

- Binder 服务描述符与注册名统一为 `custom.service`（见 `IYourService.cpp` 的
  `IMPLEMENT_META_INTERFACE`、 `YourService::getServiceName()` 与 `test_client.cpp` 的
  `getService`）。
- 二进制与 rc 中的 service 名为 `customservice`。

如需改名，请全局替换上述三处字符串，保持一致即可。

## 4. SELinux（按需）

如果服务需要自定义权限，需在 sepolicy 中添加对应的 service 定义与 domain，
否则默认策略下可能无法以 `user root` 启动或被访问拒绝。

## 5. 调试

    adb shell start customservice
    adb shell service list | grep custom.service
    adb shell service call custom.service 1 s16 "test_param"
    adb shell service call custom.service 2


-----------
优化 以及放到合适的地方

我先重新读取这些文件，确认当前确切内容，同时看看项目的整体结构以确定合适的位置。
我已读取全部文件。现在了解项目结构，以确定合适放置位置，并核对 `BinderService::publish` 的真实签名。
已确认关键事实：`BinderService::instantiate()` 本身只调 `publish()`（仅注册不进循环），而原代码错误地覆写成 `publishAndJoinThreadPool(false)` 导致死锁；正确做法应像 mediaserver 那样 `instantiate()`（=publish）后在 `main` 里 `startThreadPool()` + `joinThreadPool()`。

我先确认 `frameworks/native/services/` 是否存在，以决定落点。
`frameworks/native/services/` 是 AOSP 原生服务守护进程的标准所在目录（surfaceflinger、inputflinger 等都在这里），非常适合放置这个模板。我现在把优化后的完整模板写到 `frameworks/native/services/customservice/`，并清理旧目录。

首先创建所有优化后的文件：
新版模板已全部写入 `frameworks/native/services/customservice/`。现在清理旧的 `frameworks/customservice/` 目录，并确认没有其它构建文件引用旧路径。
没有 `.mk` 引用旧路径，只有旧目录自身的 `Android.bp`。安全删除旧目录全部文件：
全部旧文件已删除，新版模板已落到 `frameworks/native/services/customservice/`。完成后的目录结构：
已完成优化并把模板移到了 AOSP 约定的原生服务目录 `frameworks/native/services/customservice/`（与 surfaceflinger、inputflinger 同级），旧的 `frameworks/customservice/` 已整体删除。

本次修复的关键问题：

1. 统一服务名：Binder 描述符、注册名、客户端 `getService`、rc service 名全部收敛为 `custom.service`（二进制/rc 为 `customservice`），消除原来 5 处互不一致导致的 `Failed to get service`。

2. 补全缺失的 Bp 代理端：在 `IYourService.h` 中加入 `BpYourService`（继承 `BpInterface<IYourService>`，在 `doSomething/getStatus/setConfig` 里用 `remote()->transact(...)` 打包参数），让 `interface_cast<IYourService>` 与 `test_client` 可以正常编译、跨进程调用。

3. 修复死锁：`YourService::instantiate()` 改为 `BinderService<YourService>::instantiate()`（即仅 `publish()` 注册），阻塞式 Binder 循环统一放到 `main()` 的 `startThreadPool()` + `joinThreadPool()`，与 mediaserver 写法一致。原 `publishAndJoinThreadPool(false)` 会卡死主线程、导致后续启动线程池永远到不了。

4. 修 `main` 的笔误：`# <utils/Log.h>` → `#include <utils/Log.h>`。

5. 修 `Android.bp`：删除错误的 `include_dirs: ["frameworks/your_module/yourservice"]`（头文件在同目录，无需额外路径）；补 `#include <map>`/`String8` 头；新增独立的 `customservice_test` 目标编译 `test_client.cpp`。

6. 清理 `config.md`：删掉混入的那一大段 agent 思考过程，只保留真正的集成步骤（`PRODUCT_PACKAGES += customservice customservice_test`、`service call` 调试命令、改名约定、SELinux 提示）。

文件清单（10 个）：`Android.bp`、`IYourService.h/.cpp`、`YourService.h/.cpp`、`main_customservice.cpp`、`test_client.cpp`、`customservice.rc`、`bash.sh`、`config.md`。

集成方式：在产品 `.mk` 里加 `PRODUCT_PACKAGES += customservice customservice_test`，编译后 `adb shell start customservice` 即可；客户端用 `customservice_test` 或 `adb shell service call custom.service 1 s16 "test_param"` 验证。

注意：此环境无法实际跑 Soong 编译，建议你在本机 `m customservice customservice_test` 验证一次；若服务需要非 root 权限或访问敏感资源，再按 `config.md` 第 4 节补充 sepolicy 规则。