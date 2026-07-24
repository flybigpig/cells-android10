明白,按既定方案逐处落盘。我先精准定位各文件的插入点,确认现有代码上下文,避免改错位置。
Context 与 SystemServiceRegistry 插入点已定位。还需定位 SystemServer 启动点与 Android.bp 中 aidl 列举的末尾。
插入点基本定位。还需确认 Android.bp 中 aidl 列表末尾确切位置(行401之后是否还有 aidl 行),以及 SystemServer 中 startOtherServices 的精确插入上下文。
aidl 列表尚未结束,我需要找到 `srcs` 块的真正末尾。同时准备在 SystemServer 的 `StartContentService` 之后插入启动代码。
aidl 列表很长,但我已确认可以在任意已定位的 aidl 行后插入(顺序无关)。现在开始落盘:先并行创建三个新增文件,并读取 SystemServiceRegistry 注册块的精确结尾以构造替换。
三个新文件已创建。现在执行四处文件修改(注册常量、Manager 注册、服务启动、AIDL 编译),并定位 SystemServiceRegistry 的 import 区以补充依赖导入。
四处修改已成功。最后补充 SystemServiceRegistry 对 `android.demo` 的 import,并确认 services 模块能自动编译新目录下的 `DemoManagerService.java`(标准 AOSP 多用 glob srcs)。
需要确认 services 模块能自动纳入新目录。直接读取 `frameworks/base/services/Android.bp` 的 srcs 定义。
services 模块本身只编译 `java/**/*.java`,真正的服务实现在 `services.core` 静态库里。需确认 `services/core/Android.bp` 也用 glob 包含新目录。
落盘完成。所有 7 处改动均已写入,并验证了编译可达性:

**新增文件(3 个)**
- `frameworks/base/core/java/android/demo/IDemoManager.aidl` — Binder 接口(`getVersion/getStatus/setStatus/setConfig/getConfig/compute`)
- `frameworks/base/core/java/android/demo/DemoManager.java` — App 侧 Manager,持有 Binder 代理并转发各方法
- `frameworks/base/services/core/java/com/android/server/demo/DemoManagerService.java` — 服务实现,继承 `SystemService`,`onStart()` 中 `publishBinderService(Context.DEMO_SERVICE, mStub)`,纯内存状态,不依赖任何 HAL

**修改文件(4 处)**
- `Context.java` — 新增常量 `public static final String DEMO_SERVICE = "demo";`
- `SystemServiceRegistry.java` — static 块注册 `DemoManager`(已补 `android.demo` 的 import)
- `SystemServer.java` — `startOtherServices()` 内加 `mSystemServiceManager.startService(DemoManagerService.class)`
- `Android.bp` — `framework-defaults` 的 `srcs` 显式追加 `"core/java/android/demo/IDemoManager.aidl"`

**编译可达性确认**
- 服务实现位于 `services/core/java/...`,而 `services/core/Android.bp` 的 `srcs` 使用 glob `"java/**/*.java"`,新文件会被自动纳入 `services.core` → `services` jar,无需改该 bp。
- framework 的 AIDL 是逐条列举,已在 `Android.bp` 显式加入,能编进 `framework.jar`。

**编译验证(需在完整 AOSP 环境)**
```
m framework            # 重编 framework.jar(含新 aidl + DemoManager)
m services             # 重编 services.jar(含 DemoManagerService)
adb shell service list | grep demo     # 期望看到 demo: [android.demo.IDemoManager]
```
客户端用 `getSystemService("demo")` 拿 `DemoManager`,或 `adb shell service call demo 1`(事务码按 AIDL 声明顺序,`getVersion`=1)。

**两点提示**
- 本次为独立业务逻辑,`DemoManager`/接口均标 `@hide`,普通第三方 App 无法编译引用,仅系统/内部 App 可调用;若要开放给普通 App,需加 `@SystemApi` 并补 `hiddenapi` 列表。
- 标准 AOSP 若开启 hiddenapi 严格检查,首次 `m framework` 可能提示新增 `@hide` 符号,运行一次 `m update-api` 或将其加入 hiddenapi 豁免即可。

当前 workspace 是 AOSP 源码片段(仅有 `frameworks/`、`device/`、`hardware/`、`kernel/` 等子目录),需放到完整 AOSP 树中执行 `lunch` + 上述编译命令。需要我再补一个 `frameworks/base/tests/` 下的验证 App 或 `cmd` 命令来方便自测吗?