# SurfaceFlinger 修改实战方案(cells-android10 / Android 10)

## 0. 您仓库里已经具备的"地基"

探查确认,您的工程不是裸 AOSP,而是已带一整套学习资产的定制仓:

- 合成主循环入口 `SurfaceFlinger::onMessageReceived`(`SurfaceFlinger.cpp:1743`)已带中文注释,INVALIDATE/REFRESH 两段分明。
- `handleMessageRefresh`(`SurfaceFlinger.cpp:1863`)是单帧合成总入口,且注释明确"prepareFrame/postFrame 已下沉到 CompositionEngine"。
- 真正的 HWC 设备交互已不在 `SurfaceFlinger.cpp`,而在 `CompositionEngine/src/Display.cpp` 等(经 `mHwc` 调用,但符号在 CE 内)。
- 调试入口齐全:`dumpCritical`(`SurfaceFlinger.cpp:4764`)、`dumpAllLocked`(`:5041`)、`--timestats`/`--vsync`/`--latency` 等 dump 子命令,以及 `TimeStats`、`SurfaceTracing`、`SurfaceInterceptor` 三件套。
- 服务注册沿用 `addService("SurfaceFlinger", flinger)`(`main_surfaceflinger.cpp`),客户端统一经 `ComposerService::getComposerService()` 拿代理。

这意味着四个层面的修改都能在您仓库里**直接挂接真实代码点**,无需改架构。

## 1. 层面 A:合成/显示逻辑定制

目标:改一帧的合成行为。推荐从"最小侵入、可观测"的切入点入手,按难度递增:

### A1. 调试闪光区(最安全的练手)
`doDebugFlashRegions`(`SurfaceFlinger.cpp:2045`)是现成的调试钩子,被 `SurfaceFlingerProperties` 的 `debug_flash_regions` 系统属性开关控制。您可以直接在这里加自己的"强制描边/染色"逻辑,通过 `adb shell setprop debug.sf.showfps 1` 类属性触发,**完全不影响正常显示路径**,最适合第一步确认"我的改动确实被编进并跑到了"。

### A2. 注入一个全局图层变换(中等)
在 `handleMessageRefresh` → `rebuildLayerStacks`(`SurfaceFlinger.cpp:2328`)之后、`doComposition`(`:2561`)之前,插入一段遍历 `mDrawingState.layersSortedByZ` 的逻辑,对特定包名/类型图层施加统一的 alpha、位置偏移或色彩矩阵。挂在这个点能改"最终送显内容",且不破坏 HWC 决策。

### A3. 改 HWC 设备合成决策(进阶)
真要改"哪些层走 HWC、哪些走 GPU",需进 `CompositionEngine/src/Display.cpp` 的 `present`/`prepareFrame` 路径,以及 `chooseCompositionStrategy`。这里涉及 `HWComposer` 的 `getDeviceCompositionType` 返回值。改动风险高,建议先加 ATRACE + 日志,量化当前分层,再动策略。

## 2. 层面 B:新增对外接口/功能

目标:在 `ISurfaceComposer` 上加一个可被 App/系统调用的命令。挂接点是您文档里已经讲透的 Binder 链路。

### 改动点清单(必须四处同步)
1. `frameworks/native/libs/gui/ISurfaceComposer.h`:在事务码 enum 加 `MY_COMMAND = IBinder::FIRST_CALL_TRANSACTION + N`;在 `ISurfaceComposer` 接口类加纯虚 `virtual status_t myCommand(...) = 0;`。
2. `frameworks/native/libs/gui/ISurfaceComposer.cpp` 的 `BpSurfaceComposer`:实现 `myCommand`,`writeInterfaceToken` + 打包参数 + `transact(MY_COMMAND, ...)`。
3. 同文件 `BnSurfaceComposer::onTransact` 的 `switch`:加 `case MY_COMMAND:` 分支,`CHECK_INTERFACE` 后解包调用虚函数并把结果写回 `reply`。
4. `SurfaceFlinger.h`/`SurfaceFlinger.cpp`:`SurfaceFlinger` 实现 `myCommand`(注意加 `mStateLock` 保护,返回 `status_t`)。

### 客户端调用样例
复用 `ComposerService::getComposerService()` 拿 `sp<ISurfaceComposer>`,直接调 `myCommand(...)`,与 `native_service_flow_explain.md` 里的 `createDisplay` 往返完全一致。

### 系统属性旁路(更轻量,无需改接口)
如果只是想从 shell/prop 触发,用 `SurfaceFlingerProperties`(`sysprop/`)加一个 `my_toggle` 布尔属性,在 `SurfaceFlinger::init()` 读一次、`onTransact` 或主循环里轮询,配合 `adb shell setprop`,省去 Binder 接口改动。

## 3. 层面 C:调试/可观测性增强

目标:让 SF 的运行时状态更容易被您看到。您仓库已有三件套,这里给出"增量增强"点:

### C1. 扩展 dumpsys 子命令(最推荐)
在 `SurfaceFlinger::dump` 的参数分发处(`SurfaceFlinger.cpp:4727` 附近的 `dumper(...)` 表)加一行 `{"--my-debug"s, dumper(&SurfaceFlinger::dumpMyDebug)}`,然后实现 `dumpMyDebug`。这样 `adb shell dumpsys SurfaceFlinger --my-debug` 直接出您自定义的状态。`dumpAllLocked`(`:5041`)是放全局汇总的好位置。

### C2. 打通 SurfaceTracing
`SurfaceTracing` 已存在。开启方式:`adb shell service call SurfaceFlinger 1025`(或属性 `debug.sf.enable_tracing`),抓到的 trace 用 `sf2trace` 转 perfetto。可在您改动的关键函数里加 `mTracing->addLayerUpdate(...)` 之类埋点,复现问题帧。

### C3. TimeStats 扩展
`TimeStats`(`TimeStats/TimeStats.h`)负责帧率/掉帧统计,`mTimeStats->incrementMissedFrames()`(`:1768`)等处是埋点。可仿 `dumpTimeStats`(`:4802`)加自己的指标维度。

### C4. 日志与 ATRACE 规范
在合成热路径用 `ATRACE_CALL()`(已在主循环大量使用)+ `ALOGI_IF(DEBUG_SF, ...)`;复杂分支用 `LAYER_CRITICAL`/自定义 tag。避免在主循环打高频 `ALOGE`,否则日志淹没且掉帧。

## 4. 层面 D:mini_compositor 式样例

`doc/mini_compositor.cpp` 是您仓库里独立的"最小合成器"示例。实战建议:**不要直接改 SF 主路径做实验**,而是:

1. 在 `frameworks/native/services/surfaceflinger/` 下新建 `experimental/` 目录,放一个 `MiniCompositor` 类(复用 `HWComposer`/`RenderEngine` 头,但独立 `main` 或挂成 SF 的内部调试指令)。
2. 把它通过层面 B 的"系统属性旁路"或层面 C 的 dump 子命令触发,作为 SF 内一个可选的"影子合成器",对比真实路径输出。
3. 这样实验代码与主干隔离,编译风险低,验证完即删。

## 5. 统一的工作流(编译/部署/验证)

无论哪个层面,都走这条线:

1. **局部编译**:`m surfaceflinger`(或 `mmm frameworks/native/services/surfaceflinger`),只编 libsurfaceflinger + 依赖。改了 `ISurfaceComposer` 还要 `m libsurfaceflinger` 和 `m libgui` 一并编。
2. **部署**:`adb root && adb remount && adb push <out>/system/lib[64]/libsurfaceflinger.so /system/lib[64]/` + `adb push` 同目录 `surfaceflinger` 可执行到 `/system/bin/`,然后 `adb shell stop && adb shell start`(或 `adb shell killall surfaceflinger` 让其被 init 拉起)。
3. **验证**:`adb shell dumpsys SurfaceFlinger`(看您的调试输出)、`adb logcat -b all | grep -i sf`、`adb shell servicelist | grep SurfaceFlinger` 确认服务在、`surfaceflinger` 进程没反复崩溃。
4. **回滚**:保留原 `libsurfaceflinger.so` 备份,崩溃时 `adb push` 回原文件。

> 注:编译宿主若选 **cuttlefish/Linux**,上述 `adb` 流程对 `aosp_cf_x86_64` 同样适用,且崩溃不影响真机;若选 **coral 真机**,需 `device_google_coral` 的 vendor 镜像匹配,且注意 `/system` 分区是否可 remount。

## 6. 推荐的上手顺序(学习曲线)

第一步:层面 C(扩展 `--my-debug` dump 子命令)+ 层面 A1(在 `doDebugFlashRegions` 加自己的染色)。这两个零风险、即时可见,用来确认"编译→push→生效"整条链路通。

第二步:层面 B(加一个最简单的 `myEcho(String8)` 接口,回显字符串),打通 Binder 四件套,验证客户端能收到。

第三步:层面 A2(全局 alpha/偏移注入)+ 层面 D(mini_compositor 影子合成器),做真正的显示行为实验。

第四步:层面 A3(HWC 策略),仅在前面都稳了再碰。

## 7. 关键代码点速查表

| 层面 | 代码点 | 文件:行 |
| --- | --- | --- |
| 主循环 | `onMessageReceived` | `SurfaceFlinger.cpp:1743` |
| 合成总入口 | `handleMessageRefresh` | `SurfaceFlinger.cpp:1863` |
| 图层收集 | `rebuildLayerStacks` | `SurfaceFlinger.cpp:2328` |
| 真合成 | `doComposition` | `SurfaceFlinger.cpp:2561` |
| 调试闪光 | `doDebugFlashRegions` | `SurfaceFlinger.cpp:2045` |
| 掉帧埋点 | `mTimeStats->incrementMissedFrames` | `SurfaceFlinger.cpp:1768` |
| dump 分发 | `dumper(...)` 表 | `SurfaceFlinger.cpp:4727` |
| dump 总入口 | `dumpAllLocked` | `SurfaceFlinger.cpp:5041` |
| onTransact | `SurfaceFlinger::onTransact` | `SurfaceFlinger.cpp:5378` |
| 服务注册 | `addService("SurfaceFlinger", ...)` | `main_surfaceflinger.cpp` |
| Binder 代理 | `BpSurfaceComposer` | `ISurfaceComposer.cpp` |
| Binder 分发 | `BnSurfaceComposer::onTransact` | `ISurfaceComposer.cpp:1007` |
| 客户端单例 | `ComposerService::getComposerService` | `SurfaceComposerClient.cpp` |
| HWC 决策 | `CompositionEngine/src/Display.cpp` | `CompositionEngine/src/` |

---

## 8. 实验 A1:在 `doDebugFlashRegions` 加自定义染色(零风险练手)

目标:确认"改动能编进并跑到",且不影响正常显示。

### 步骤
1. 打开 `frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp`,定位 `SurfaceFlinger::doDebugFlashRegions`(`SurfaceFlinger.cpp:2045`)。
2. 在函数体开头加一段实验染色:当系统属性 `debug.sf.myflash` 为 1 时,给每个图层描一圈红色边。
3. 触发:`adb shell setprop debug.sf.myflash 1`,再 `adb shell setprop debug.sf.showfps 1`(开启闪光区总开关,该函数才会被调用)。观察屏幕图层边缘出现红框即成功。
4. 关闭:`adb shell setprop debug.sf.myflash 0`。

### 代码 diff
在 `doDebugFlashRegions` 函数起始处(现有 `if (!mDebugFlashRegions) return;` 之后)插入:

```cpp
    // [EXPERIMENT A1] 自定义染色:debug.sf.myflash=1 时给图层描红边
    static bool sMyFlash = false;
    char myFlashVal[PROPERTY_VALUE_MAX];
    if (property_get("debug.sf.myflash", myFlashVal, "0") > 0) {
        sMyFlash = (myFlashVal[0] == '1');
    }
    if (sMyFlash) {
        for (auto& layer : mDrawingState.layersSortedByZ) {
            layer->forceClientComposition(); // 保证走 GPU,便于我们描边
            // 真正的描边在 RenderEngine 阶段做;此处仅标记,配合 ALOGI 验证命中
            ALOGI_IF(DEBUG_SF, "[A1] flash-region layer=%s",
                     layer->getName().c_str());
        }
    }
```

> 注:`forceClientComposition` 仅作演示;真实描边需进 `RenderEngine`,本实验目的只是打通链路,看 logcat 出现 `[A1]` 即证明改动生效。

---

## 9. 实验 A2:全局图层 alpha 注入

目标:对指定包名图层施加统一透明度,在主循环合成前生效。

### 步骤
1. 在 `handleMessageRefresh`(`SurfaceFlinger.cpp:1863`)中,`rebuildLayerStacks()`(`:2328`)之后、`doComposition()`(`:2561`)之前的任意位置插入遍历逻辑。
2. 仅对 `mDrawingState.layersSortedByZ` 中 `getOwnerUserId()`/包名匹配的图层调用 `layer->setAlpha(0.5f)`(需该接口存在;若仅读写 `mDrawingState`,可用 `layer->editState().alpha = 0.5f`)。
3. 触发:`adb shell setprop debug.sf.dim 1`,重启 SF,目标 App 窗口整体半透明。

### 代码 diff
在 `rebuildLayerStacks` 调用之后插入:

```cpp
    // [EXPERIMENT A2] 全局降低某包名图层透明度
    char dimVal[PROPERTY_VALUE_MAX];
    if (property_get("debug.sf.dim", dimVal, "0") > 0 && dimVal[0] == '1') {
        for (auto& layer : mDrawingState.layersSortedByZ) {
            if (layer->getName().find(String8("com.android.settings")) >= 0) {
                layer->editState().alpha = 0.5f;
                ALOGI_IF(DEBUG_SF, "[A2] dim layer=%s", layer->getName().c_str());
            }
        }
    }
```

> 风险:直接改 `editState().alpha` 会和下一次事务冲突被覆盖。生产做法应在 `onTransactionHandled` 或 `applyTransactionState` 里拦截,本实验仅验证挂接点。

---

## 10. 实验 B:新增 `myEcho` Binder 接口(回显字符串)

目标:打通 ISurfaceComposer 的四处同步改动,客户端能收到回显。

### 步骤(必须四处同步改)
1. `ISurfaceComposer.h` 事务码 enum 末尾加 `MY_ECHO`。
2. `ISurfaceComposer.h` 接口类加纯虚 `myEcho`。
3. `ISurfaceComposer.cpp` 的 `BpSurfaceComposer` 加实现。
4. `ISurfaceComposer.cpp` 的 `BnSurfaceComposer::onTransact` 加 `case MY_ECHO:`。
5. `SurfaceFlinger.h`/`SurfaceFlinger.cpp` 实现 `myEcho`。

### 代码 diff

#### (1) `libs/gui/include/gui/ISurfaceComposer.h` — 事务码
```cpp
    // 在 ISurfaceComposerTag enum 现有末尾项(如 GET_GPU_CONTEXTS_PRIORITY 之后)加:
    MY_ECHO,
```

#### (2) `ISurfaceComposer.h` — 接口声明(在 `getGpuContextsPriority` 等纯虚函数附近加)
```cpp
    // [EXPERIMENT B] 新增回显接口
    virtual status_t myEcho(const String8& in, String8* out) = 0;
```

#### (3) `libs/gui/ISurfaceComposer.cpp` — Bp 实现(紧跟其他 Bp 方法)
```cpp
    virtual status_t myEcho(const String8& in, String8* out) {
        Parcel data, reply;
        data.writeInterfaceToken(ISurfaceComposer::getInterfaceDescriptor());
        data.writeString8(in);
        status_t err = remote()->transact(BnSurfaceComposer::MY_ECHO, data, &reply);
        if (err != NO_ERROR) return err;
        String8 result = reply.readString8();
        if (out) *out = result;
        return NO_ERROR;
    }
```

#### (4) `ISurfaceComposer.cpp` — Bn onTransact(在 switch 内,任意 `case` 之后加)
```cpp
        case MY_ECHO: {
            CHECK_INTERFACE(ISurfaceComposer, data, reply);
            String8 in = data.readString8();
            String8 out;
            status_t err = myEcho(in, &out);
            reply->writeString8(out);
            return err;
        }
```

#### (5) `SurfaceFlinger.h` 类内声明 + `SurfaceFlinger.cpp` 实现
```cpp
// SurfaceFlinger.h:在 public/protected 接口区加
status_t myEcho(const String8& in, String8* out) override;

// SurfaceFlinger.cpp:实现
status_t SurfaceFlinger::myEcho(const String8& in, String8* out) {
    Mutex::Autolock lock(mStateLock);
    ALOGI("[B] myEcho in=%s", in.c_str());
    if (out) *out = String8::format("echo:%s", in.c_str());
    return NO_ERROR;
}
```

### 客户端调用样例
```cpp
sp<ISurfaceComposer> sf(ComposerService::getComposerService());
String8 out;
sf->myEcho(String8("hello"), &out);
ALOGI("got: %s", out.c_str()); // 输出 echo:hello
```

### 编译注意
改了 `ISurfaceComposer` 后,`libgui`、`libsurfaceflinger` 及所有依赖它们的模块都要重编:
`mmm frameworks/native/libs/gui frameworks/native/services/surfaceflinger`

---

## 11. 实验 C:扩展 `dumpsys SurfaceFlinger --my-debug`

目标:把自定义运行时状态通过 dumpsys 输出,无需动 Binder。

### 步骤
1. 在 `SurfaceFlinger.cpp` 的 `dumper(...)` 表(`SurfaceFlinger.cpp:4727` 附近)加一行 `"--my-debug"s` 映射。
2. 实现 `dumpMyDebug`。
3. 验证:`adb shell dumpsys SurfaceFlinger --my-debug`。

### 代码 diff

#### (1) dump 分发表(在现有 `{"--wide-color"s, ...}` 等条目附近加)
```cpp
    {"--my-debug"s, dumper(&SurfaceFlinger::dumpMyDebug)},
```

#### (2) `SurfaceFlinger.h` 声明
```cpp
    void dumpMyDebug(std::string& result) const;
```

#### (3) `SurfaceFlinger.cpp` 实现(放在其他 dumpXXX 函数附近)
```cpp
void SurfaceFlinger::dumpMyDebug(std::string& result) const {
    result.append("=== [EXPERIMENT C] my-debug ===\n");
    result.appendFormat("layers count: %zu\n", mDrawingState.layersSortedByZ.size());
    result.appendFormat("myflash prop: %s\n",
        android::base::GetProperty("debug.sf.myflash", "0").c_str());
    for (auto& layer : mDrawingState.layersSortedByZ) {
        result.appendFormat("  - %s alpha=%.2f\n",
            layer->getName().c_str(), layer->getDrawingState().alpha);
    }
}
```

> 注:`mDrawingState` 读需持 `mStateLock`;dump 调用链已加锁,本函数直接 const 读时请确保挂在已加锁路径(参考 `dumpAllLocked` 的 `REQUIRES(mStateLock)` 标注)。若编译报锁断言,把声明改为 `void dumpMyDebug(std::string& result) const REQUIRES(mStateLock);` 并在调用点确认持锁。

---

## 12. 实验 D:experimental/MiniCompositor 影子合成器骨架

目标:隔离实验代码,不污染主路径,作为 SF 内可选调试指令。

### 步骤
1. 新建 `frameworks/native/services/surfaceflinger/experimental/MiniCompositor.h/.cpp`。
2. 它复用 `HWComposer`/`RenderEngine` 头,提供一个 `static void runOnce()` 打印当前 display 列表。
3. 通过实验 C 的 `--my-debug` 或实验 B 的属性触发调用,对比真实路径输出。

### 骨架代码
```cpp
// experimental/MiniCompositor.h
#pragma once
#include <utils/String8.h>
namespace android {
class MiniCompositor {
public:
    static void runOnce(const char* tag);
};
} // namespace android

// experimental/MiniCompositor.cpp
#include "MiniCompositor.h"
#include <log/log.h>
namespace android {
void MiniCompositor::runOnce(const char* tag) {
    ALOGI("[D] MiniCompositor runOnce tag=%s (shadow compositor stub)", tag);
    // 真实实现:用 HWComposer::getActiveConfig / RenderEngine 抓一帧做对照
}
} // namespace android
```

### 接入方式(二选一)
- 经实验 B 的属性旁路:在 `SurfaceFlinger::init()` 轮询 `debug.sf.mini`,为 1 时 `MiniCompositor::runOnce("boot")`。
- 经实验 C 的 dump:在 `dumpMyDebug` 里调 `MiniCompositor::runOnce("dump")`。

> 编译:需在 `surfaceflinger/Android.bp` 的 `srcs` 里加 `"experimental/MiniCompositor.cpp"`,否则不会被编入。

---

## 13. 编译 / 部署 / 回滚速查

| 操作 | 命令 |
| --- | --- |
| 局部编译 SF | `mmm frameworks/native/services/surfaceflinger` |
| 改了 libgui 后 | `mmm frameworks/native/libs/gui frameworks/native/services/surfaceflinger` |
| 推送到设备 | `adb root && adb remount && adb push $OUT/system/lib64/libsurfaceflinger.so /system/lib64/ && adb push $OUT/system/bin/surfaceflinger /system/bin/` |
| 重启 SF | `adb shell stop && adb shell start`(或 `adb shell killall surfaceflinger`) |
| 看效果 | `adb shell dumpsys SurfaceFlinger --my-debug`、`adb logcat -s SurfaceFlinger` |
| 服务存活检查 | `adb shell servicelist \| grep SurfaceFlinger` |
| 回滚 | 备份原 `libsurfaceflinger.so`,崩溃时 `adb push` 回原文件 |

> 选 cuttlefish 时崩溃不影响真机;选 coral 真机需 vendor 镜像匹配且 `/system` 可 remount。

## 14. 常见坑

- 改 `ISurfaceComposer` 后**只编 SF 不编 libgui**,会导致 Bp/Bn 版本不一致,调用直接挂。务必一起编。
- 主循环里打高频 `ALOGE` 会淹没日志并掉帧,用 `ALOGI_IF(DEBUG_SF, ...)` + `ATRACE_CALL()`。
- 直接改 `editState().alpha` 会被下一次事务覆盖,A2 实验仅验证挂接点;真要持久改需在事务处理处拦截。
- `dumpMyDebug` 读 `mDrawingState` 必须持 `mStateLock`,否则触发线程安全断言。
