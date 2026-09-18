# Android Framework 开发实战汇总

> 适用版本：Android 10 (Q) 及以上，结合日常 ROM / Framework 定制开发经验整理。
> 目录：
> 1. [环境与构建](#1-环境与构建)
> 2. [系统服务开发](#2-系统服务开发)
> 3. [AMS 与四大组件](#3-ams-与四大组件)
> 4. [WMS 与 SystemUI](#4-wms-与-systemui)
> 5. [权限与 SELinux](#5-权限与-selinux)
> 6. [功耗与性能](#6-功耗与性能)
> 7. [设置与默认值定制](#7-设置与默认值定制)
> 8. [日志调试与问题定位](#8-日志调试与问题定位)
> 9. [常见需求速查表](#9-常见需求速查表)

---

## 1. 环境与构建

### 1.1 常用编译命令

```bash
source build/envsetup.sh
lunch aosp_arm64-userdebug          # 选择目标

make framework.jar -j16             # framework 核心库
make services -j16                  # system_server (services.jar)
make SystemUI -j16                  # 系统 UI
make Settings -j16                  # 设置应用
make snod                           # 重新打包 system.img（无增量系统）
make updatepackage                  # OTA 差分包基础
```

### 1.2 单模块增量编译（Soong）

```bash
# Android.bp 模块
mmm frameworks/base/packages/SystemUI       # 老方式
mmma frameworks/base/core/java              # 连依赖一起编

# 新方式（soong_ui）
soong frameworks/base/packages/SystemUI
```

### 1.3 修改 framework 后刷机验证

```bash
adb root && adb remount
adb push out/target/product/xxx/system/framework/framework.jar /system/framework/
adb push out/target/product/xxx/system/framework/services.jar /system/framework/
adb reboot
```

> 注意：`services.jar` 与 `/system/framework/oat/` 下的 vdex/odex 可能不一致导致起不来，
> push 后删掉对应 oat 目录或使用 `adb shell cmd package compile --reset`。

### 1.4 Android.mk 与 Android.bp 对照

| 任务 | Android.mk | Android.bp |
|------|-----------|------------|
| 声明模块 | `LOCAL_MODULE := foo` | `cc_library { name: "foo" }` |
| Java 库 | `BUILD_JAVA_LIBRARY` | `java_library {}` |
| 平台签名 | `LOCAL_CERTIFICATE := platform` | `certificate: "platform"` |
| 依赖 | `LOCAL_STATIC_JAVA_LIBRARIES` | `static_libs: [...]` |
| 隐藏 API 豁免 | `LOCAL_DX_FLAGS` | `libs` + `sdk_version: "core_platform"` |

---

## 2. 系统服务开发

### 2.1 新增一个系统服务（完整流程）

**Step 1：定义 AIDL**

```
frameworks/base/core/java/com/example/IFooService.aidl
```

```java
package com.example;

interface IFooService {
    int getValue(String key);
    void setValue(String key, int value);
}
```

**Step 2：实现服务**

```
frameworks/base/services/core/java/com/example/server/FooService.java
```

```java
public class FooService extends IFooService.Stub {
    private static final String TAG = "FooService";
    private final Context mContext;

    public FooService(Context context) {
        mContext = context;
    }

    @Override
    public int getValue(String key) {
        mContext.enforceCallingOrSelfPermission(
                "com.example.permission.FOO", "need FOO permission");
        return Settings.System.getInt(mContext.getContentResolver(), key, 0);
    }

    @Override
    public void setValue(String key, int value) {
        Settings.System.putInt(mContext.getContentResolver(), key, value);
    }
}
```

**Step 3：在 SystemServer 注册**

```
frameworks/base/services/java/com/android/server/SystemServer.java
```

```java
// startOtherServices() 中：
traceBeginAndSlog("StartFooService");
try {
    ServiceManager.addService("foo", new FooService(context));
} catch (Throwable e) {
    reportWtf("starting FooService", e);
}
traceEnd();
```

**Step 4：提供 Manager 给应用使用**

```
frameworks/base/core/java/android/app/FooManager.java
```

```java
public class FooManager {
    private final IFooService mService;

    /** @hide */
    public FooManager(IFooService service) {
        mService = service;
    }

    public int getValue(String key) {
        try {
            return mService.getValue(key);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
}
```

在 `SystemServiceRegistry.java` 注册：

```java
registerService("foo", FooManager.class,
    new CachedServiceFetcher<FooManager>() {
        @Override
        public FooManager createService(ContextImpl ctx) {
            IBinder b = ServiceManager.getService("foo");
            return new FooManager(IFooService.Stub.asInterface(b));
        }
    });
```

**Step 5：API 定义**

- `current.txt` / `system/current.txt` 中补充 API 签名（`update-api` 自动生成）；
- AIDL 加入 `frameworks/base/Android.bp` 的 `srcs` 列表。

### 2.2 SystemService 生命周期写法（推荐）

```java
public class FooService extends SystemService {
    public FooService(Context context) {
        super(context);
    }

    @Override
    public void onStart() {
        publishBinderService("foo", mBinderImpl);
    }

    @Override
    public void onBootPhase(int phase) {
        if (phase == PHASE_SYSTEM_SERVICES_READY) {
            // 依赖服务已就绪，可安全使用
        }
        if (phase == PHASE_BOOT_COMPLETED) {
            // 开机完成
        }
    }
}
```

### 2.3 Binder 调用权限与 PID/UID 校验

```java
private void checkPermission() {
    final int callingUid = Binder.getCallingUid();
    final int callingPid = Binder.getCallingPid();
    // 注意：跨进程调用后尽快 clearCallingIdentity
    final long token = Binder.clearCallingIdentity();
    try {
        // 执行需要以 system 身份执行的操作
    } finally {
        Binder.restoreCallingIdentity(token);
    }
}
```

---

## 3. AMS 与四大组件

### 3.1 Activity 启动流程关键路径

```
Activity.startActivty()
  → Instrumentation.execStartActivity()
  → ActivityManager.getService().startActivity()   // binder → AMS
  → ActivityStarter.startActivityMayWait()
  → ActivityStarter.startActivityUnchecked()
  → ActivityStackSupervisor.resumeFocusedStackTopActivity()
  → ActivityThread.handleLaunchActivity()
```

排查思路：`adb logcat | grep -E "ActivityTaskManager|ActivityManager"` 观察
`START u0 {...}` 日志，可快速确认 intent 解析、权限拦截、启动模式行为。

### 3.2 常见需求：禁止某个应用启动（黑名单）

修改 `ActivityStarter#startActivityChecked`：

```java
// 新增拦截逻辑
String pkg = r.packageName;
if (mService.mForbiddenPackages.contains(pkg)) {
    Slog.w(TAG, "Blocked startActivity for package: " + pkg);
    return START_BLOCKED;
}
```

### 3.3 常见需求：进程查杀策略

- 低内存查杀：`ProcessList.java` 中的 oom_adj 计算与 `updateOomAdjLSP()`
- 强制停止兜底：`ActivityManagerService.forceStopPackage()`
- 常驻应用保活：在 `ProcessList.computeOomAdjLSP()` 中对白名单包强制提高 adj

### 3.4 ANR 分析

```bash
# ANR trace 位置
/data/anr/traces.txt
adb bugreport → FS/data/anr/

# 关键字段
# "main" prio=5 tid=1 Native/Sleeping/Blocked
# 持锁信息：held by thread xx
```

判断类型：
| 现象 | 原因方向 |
|------|---------|
| main 线程 Blocked，等待 binder | 对端服务阻塞 |
| main Sleeping + 大量 GC | CPU 密集/内存抖动 |
| 主线程 binder 耗时 | IPC 慢，查对端 |

---

## 4. WMS 与 SystemUI

### 4.1 窗口层级（window type）速查

| type | 值 | 用途 |
|------|----|------|
| TYPE_BASE_APPLICATION | 1 | 普通应用窗口 |
| TYPE_APPLICATION_OVERLAY | 2038 | 悬浮窗（需 SYSTEM_ALERT_WINDOW） |
| TYPE_STATUS_BAR | 2000 | 状态栏 |
| TYPE_NAVIGATION_BAR | 2019 | 导航栏 |
| TYPE_INPUT_METHOD | 2011 | 输入法 |
| TYPE_VOICE_INTERACTION | 2031 | 语音助手 |
| TYPE_WALLPAPER | 2013 | 壁纸 |

### 4.2 状态栏修改（Android 10）

代码位置：`frameworks/base/packages/SystemUI/`

- 图标：`res/layout/system_icons.xml`、`status_bar.xml`
- 逻辑：`com.android.systemui.statusbar.phone.StatusBar.java`
- 下拉面板：`NotificationPanelView.java`
- 电池/时间：`BatteryMeterView`、`Clock.java`

### 4.3 常见需求：默认显示网速

`NetworkTraffic` 类此类需求一般是自研节点插入 `status_bar.xml`，
通过 handler 周期读取 `/proc/net/dev` 或 `NetworkStatsManager` 计算速率。

### 4.4 导航栏定制

`frameworks/base/packages/SystemUI/res/layout/navigation_bar.xml`
+ `NavigationBarView.java` / `NavigationBarFragment.java`。

返回键手势区域：`NavigationEdgeBackCallback`（Q 后期版本）。

---

## 5. 权限与 SELinux

### 5.1 SELinux avc denied 处理流程

```bash
# 1. 抓取日志
adb shell dmesg | grep avc
# avc: denied { read } for pid=1234 comm="foo" name="bar" scontext=u:r:foo_t:s0 tcontext=u:object_r:bar_t:s0 tclass=file

# 2. 定位策略目录
system/sepolicy/private/  或  device/<vendor>/sepolicy/

# 3. 在 foo_t 的 .te 文件添加
allow foo_t bar_t:file read;

# 4. 验证
adb shell dmesg | grep avc   # 不再新增 denied
```

### 5.2 运行时权限预授权（privapp）

`/system/etc/permissions/privapp-permissions-xxx.xml`

```xml
<permissions>
    <privapp-permissions package="com.example.app">
        <permission name="android.permission.WRITE_SECURE_SETTINGS"/>
        <permission name="android.permission.MODIFY_PHONE_STATE"/>
    </privapp-permissions>
</permissions>
```

> privapp 白名单缺失会直接 boot 失败或启动报错，看 logcat 中
> `Privileged permission whitelist` 关键字。

### 5.3 AppOps 特殊权限

```bash
adb shell appops get <package>
adb shell appops set <package> SYSTEM_ALERT_WINDOW allow
```

### 5.4 隐藏 API 限制

Android 9+ 对非 SDK 接口有 greylist/blocklist：
- 框架应用豁免：`frameworks/base/config/hiddenapi-*.txt`
- 应用侧解除（userdebug）：`settings put global hidden_api_policy 1`

---

## 6. 功耗与性能

### 6.1 WakeLock 治理

```bash
adb shell dumpsys power | grep -A 20 "Wake Locks"
adb shell dumpsys batterystats --charged <pkg>
```

框架侧限制：`PowerManagerService.java` 中 `acquireWakeLockInternal()`
可按 uid/包名拦截超时释放。

### 6.2 Alarm 对齐 / Doze

- `AlarmManagerService.java`：`setImpl()` 中对 `elapsedRealtime` 做窗口对齐
- Doze 白名单：`dumpsys deviceidle whitelist +<pkg>`
- 位置：`frameworks/base/services/core/java/com/android/server/AlarmManagerService.java`

### 6.3 开机时间优化

1. 抓取 bootchart：

```bash
adb shell 'touch /data/bootchart/enable; reboot'
# 开机后
/system/bin/bootchart get
```

2. 分析 `bootchart.png` 中 SystemServer 各服务启动耗时
3. 对耗时大头：延迟初始化（onBootPhase 拆分）、并行 `traceBeginAndSlog`
4. 常见耗时点：PackageManager 扫描、精简预装 apk、`dex2oat` 预编译

### 6.4 卡顿分析

```bash
adb shell dumpsys gfxinfo <pkg>
# systrace / perfetto 抓取
adb shell atrace -b 16000 -t 10 view wm am > trace.html
```

---

## 7. 设置与默认值定制

### 7.1 SettingsProvider 默认值

`frameworks/base/packages/SettingsProvider/res/values/defaults.xml`

```xml
<!-- 例：默认关闭自动旋转 -->
<bool name="def_accelerometer_rotation">false</bool>
<!-- 例：默认亮度 50% -->
<integer name="def_screen_brightness">128</integer>
```

修改后需要升级 DB 版本号：

```
frameworks/base/packages/SettingsProvider/src/com/android/providers/settings/DatabaseHelper.java
→ DATABASE_VERSION++（老设备才会重跑 onLoad）
```

或直接在 `SettingsProvider` 的 `UpgradeController` 中按 version 迁移。

### 7.2 config.xml 资源覆盖

核心位置：`frameworks/base/core/res/res/values/config.xml`
厂商覆盖目录（覆盖优先级从高到低）：

```
device/<vendor>/<product>/overlay/
vendor/<vendor>/overlay/
```

常用开关举例：

| 需求 | config 项 |
|------|-----------|
| 禁止长按电源截屏 | `config_disableLongPressPowerScreenshot`（或自定义） |
| 支持多窗口 | `config_supportsMultiWindow` |
| 支持分屏 | `config_supportsSplitScreenMultiWindow` |
| 相机双击唤醒 | `config_cameraDoubleTapLogging` |
| 电池低电阈值 | `config_lowBatteryWarningLevel` |

### 7.3 系统属性

定义：`device/<vendor>/<product>/system.prop` 或 `build/make/target/product/*.mk`

```
persist.sys.foo.enable=true
ro.foo.version=1.0
```

代码读取：

```java
import android.os.SystemProperties;
boolean enable = SystemProperties.getBoolean("persist.sys.foo.enable", false);
```

> 需要 SELinux 允许对应 property_contexts，否则 avc denied。

### 7.4 预装应用（预置 APK）

`device/<vendor>/<product>/<pkg>.mk`

```make
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := FooApp
LOCAL_MODULE_CLASS := APPS
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := $(LOCAL_MODULE).apk
LOCAL_CERTIFICATE := platform       # 系统签名
LOCAL_MODULE_PATH := $(TARGET_OUT_SYSTEM_EXT_APPS)   # /system_ext 或 $(TARGET_OUT_APPS)
include $(BUILD_PREBUILT)
```

再把它加进 `PRODUCT_PACKAGES`。

---

## 8. 日志调试与问题定位

### 8.1 logcat 过滤技巧

```bash
adb logcat -s ActivityTaskManager:I WindowManager:I
adb logcat --pid=$(adb shell pidof com.example.app)
adb logcat -b events | grep am_    # 二进制 events log，含 am_create/am_anr 等
```

### 8.2 dumpsys 常用命令

| 命令 | 用途 |
|------|------|
| `dumpsys activity activities` | Activity 栈信息 |
| `dumpsys activity processes` | 进程 + oom_adj |
| `dumpsys window windows` | 所有窗口层级 |
| `dumpsys window displays` | 显示相关信息 |
| `dumpsys package <pkg>` | 包信息、权限 |
| `dumpsys power` | 电源/唤醒锁 |
| `dumpsys batterystats` | 耗电统计 |
| `dumpsys SurfaceFlinger` | 图层信息 |
| `dumpsys input` | 输入事件分发 |

### 8.3 Watchdog 定位

```
/data/system/watchdog/
/data/anr/    # Watchdog 会打印 blocked 线程 trace
```

log 中搜 `WATCHDOG KILLING SYSTEM PROCESS`，看 blocked 线程所属服务。

### 8.4 Binder 调试

```bash
adb shell cat /sys/kernel/debug/binder/binder_stats
adb shell cat /sys/kernel/debug/binder/transactions
adb shell dumpsys binder_calls_stats
```

### 8.5 Tombstone / Crash

```bash
/data/tombstones/
adb shell dumpsys dropbox --print   # 系统级 crash、anr 记录
```

---

## 9. 常见需求速查表

| 需求 | 主要改动点 |
|------|-----------|
| 修改默认语言 | `build/make/target/product/full_base.mk` → `PRODUCT_LOCALES` |
| 修改默认时区 | `system.prop` 或 `persist.sys.timezone` |
| 修改默认输入法 | `frameworks/base/core/res/res/xml` 或 SettingsProvider |
| 修改导航栏高度 | `frameworks/base/core/res/res/values/dimens.xml` → `navigation_bar_height` |
| 禁用 OTA | 注释 `UpdateEngineService` 注册或删 system/priv app |
| 打开 USB 默认 MTP | `frameworks/base/services/usb` 或 `config.xml` |
| 默认开启开发者模式 | SettingsProvider `def_` 系列 |
| 修改信号格数策略 | `frameworks/base/telephony` → SignalStrength |
| 锁屏默认壁纸 | `frameworks/base/core/res/res` 或 WallPaperManagerService |
| 开机自启白名单 | AMS `queryIntentReceivers` 相关或 receivers 配置 |
| 隐藏应用入口 | Launcher 过滤 + PackageManager `setApplicationEnabledSetting` |
| 修改截图声音/路径 | `frameworks/base/packages/SystemUI/src/.../screenshot` |

---

## 附录 A：framework 关键源码目录速查

```
frameworks/base/core/java/android/          # SDK API + Manager 层
frameworks/base/services/core/java/com/android/server/   # system_server 各服务
frameworks/base/packages/SystemUI/          # 系统UI
frameworks/base/packages/SettingsProvider/  # 设置数据库
frameworks/base/telephony/                  # 电话框架
frameworks/base/wifi/                       # Wi-Fi 框架
frameworks/native/services/surfaceflinger/  # 图层合成
frameworks/av/                              # 音视频/相机
frameworks/native/libs/binder/              # native binder
system/sepolicy/                            # SELinux 策略
```

## 附录 B：常用 adb 调试开关

```bash
adb shell settings put global window_animation_scale 0   # 关动画
adb shell service list                                   # 列出系统服务
adb shell cmd activity stack list                        # ActivityStack
adb shell cmd window policy                              # 窗口策略
adb shell setprop persist.log.tag V                      # 全局日志级别
adb shell setprop debug.foo.enable 1                     # 自定义调试开关
```
