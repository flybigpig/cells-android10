# targetSdk 升级行为变更适配实战（隐藏 API 限制、反射拦截）

> 适用：Android 9 (P) 引入隐藏 API 限制，Android 10 (Q) 起逐步加强。
> 分两部分：一、非 SDK 接口（隐藏 API）限制原理与适配；二、targetSdk 升级各版本行为变更清单与适配。

---

## 一、隐藏 API（Non-SDK Interface）限制

### 1.1 背景与分类

从 Android 9 开始，框架对应用访问非 SDK 接口按三类管理：

| 类别 | 行为 | 来源列表 |
|------|------|---------|
| light-greylist（浅灰） | 可访问，logcat 打 warning | `hiddenapi-light-greylist.txt` |
| dark-greylist（深灰） | targetSdk < P 可用；>= P 被拦截/受限 | `hiddenapi-dark-greylist.txt` |
| blacklist（黑名单） | 一律拦截 | `hiddenapi-force-blacklist.txt` |

关键规则：**同一接口，targetSdk 越高限制越严**。升级 targetSdk 可能使原来"能用"的深灰接口直接失效。

列表文件位置（framework 源码）：

```
build/soong/scripts-hiddenapi/         # 生成工具
frameworks/base/config/hiddenapi-*.txt # 各分类列表
out/.../hiddenapi/                     # 编译产物中的最终列表
```

### 1.2 拦截发生的位置

入口统一在 `libcore`/`art` 的 `hidden_api.cc`：

```
hiddenapi::GetMemberAction()   // 决定 Allow / Warning / Deny
  ↓
deny 后根据访问方式报错：
  - Field read/write  → NoSuchFieldError
  - Method invoke     → NoSuchMethodError
  - reflect getDeclared* → 直接从返回列表中过滤掉该成员
```

注意：**反射获取字段列表时被过滤**（不是抛异常），这是很多"反射突然拿不到字段"问题的直接原因。

### 1.3 判定应用触碰了限制

```bash
# 1. logcat 过滤（最直接）
adb logcat | grep -E "Accessing hidden (method|field)"
# W fooapp: Accessing hidden method Landroid/app/ActivityThread;->currentActivityThread()...

# 2. 每应用汇总统计
adb shell dumpsys hidden_api_policy        # 当前策略
adb shell cat /data/system/appops.xml      # 非 SDK 访问计数

# 3. 静态扫描（上线前）
# art 产物工具
out/host/linux-x86/bin/veridex --app foo.apk --imprecise
# 或 Google 提供的 veridex / hiddenapiscanner
```

### 1.4 调试开关（userdebug/eng 版本）

```bash
# 全局关闭拦截（仅调试！）
adb shell settings put global hidden_api_policy 1
# 取值：0=default  1=just log  2=allow(不拦不打)  3=allow + 仅供测试

adb shell settings delete global hidden_api_policy       # 恢复默认
```

> 也可以在 framework 中改默认值：
> `frameworks/base/core/res/res/values/config.xml` → 不在此处；
> 实际默认策略在 `hiddenapi::EnforcementPolicy` / `dalvik.vm.hiddenapi` 相关配置，
> 厂商机型想全局放开需修改 `hiddenapi-*.txt` 列表重新编译。

### 1.5 适配方案（按优先级）

**方案 A：换成公开 API（正解）**

常见隐藏 API → 公开替代对照：

| 隐藏/受限用法 | 公开替代 |
|--------------|---------|
| `ActivityThread.currentActivityThread()` | 无法直接替代，重构为 Context 驱动 |
| `Settings.System.putStringForUser`（部分重载受限） | 使用公开重载或 `ContentResolver.call` |
| `IPackageManager.getInstalledPackages(flags, userId)` | `PackageManager.getInstalledPackages(flags)` |
| `TelephonyManager.getDeviceId*`（Q 起 targetSdk>=29 拦截） | `IMEI` → `getImei()` / 无权限场景用 `Settings.Secure.ANDROID_ID` |
| `ActivityManager.getService()` | `Context.getSystemService(ActivityManager.class)` |
| `WifiManager.setWifiEnabled(true)`（Q 起对三方失效） | `SettingsPanel` / 跳转设置页 |
| `getRunningTasks` 拿栈顶（Q 起收紧） | `getRunningTasks` 仅返回自己；改用 `AccessibilityService` 或可见性 API |

**方案 B：绕过手段（灰色，仅自研可控场景）**

- 双反射（用受限的 `getDeclaredMethod` 找 `getDeclaredMethod`）—— Q 之后大多被封堵；
- `sun.misc.Unsafe` / JNI `dlsym` 直读 ArtMethod —— 强烈不推荐，R 之后有 hook 检测；
- 自带 framework stub 类（编译期 @hide 引用）——运行期仍走运行时判定，需配合白名单。

> 结论：绕过手段随版本快速失效，**正式产品不要依赖**。

**方案 C：平台方给应用开白名单（ROM 厂商适配自有应用）**

方法 1：把目标签名/包名加入 hiddenapi 豁免 —— 编译时列表生成逻辑：
`build/soong/java/hiddenapi.go` 依据 `hiddenapi-*.txt`；
运行时豁免走 `hiddenapi-unsupported.txt`（视为 light-grey）。

方法 2：privapp 直接豁免 —— `frameworks/base/core/res/` 的
`public.libraries.txt` 不适用；正确做法是把接口挪入：

```
frameworks/base/core/java/  中加 @doc:liza 标记不现实
→ 实际做法：把接口加进 system API（@SystemApi）或 core-platform API，
   重新生成 api/current.txt，privapp 用 sdk_version="system_current" 编译。
```

方法 3：`/system/etc/sysconfig/` 下 `hiddenapi-package-whitelist.xml`：

```xml
<config>
    <hiddenapi-whitelisted-package name="com.example.trustedapp"/>
</config>
```

> 该白名单使包内应用的深灰/黑名单访问降级为"允许"，ROM 预置自家应用常用。
> 文件随系统镜像打包，三方商店应用无法获得。

### 1.6 系统应用/privapp 自身如何规避

- 编译时声明 `sdk_version: "core_platform"` / 使用 `system_current`，合法访问 @SystemApi/@hide；
- 运行时判定对 platform 签名 + 特定 sharedUserId 有天然豁免链路
  （`hiddenapi::GetMemberAction` 中 `IsAppHiddenApiExempt` 逻辑，依赖 `dalvik.vm.hiddenapi` 与包配置）；
- 厂商定制 ROM 可在 `art/runtime/hidden_api.cc` 打政策开关（不建议，升级冲突大）。

---

## 二、targetSdk 各版本行为变更与适配

### 2.1 targetSdk → 28 (Android 9 P)

| 变更 | 影响 | 适配 |
|------|------|------|
| 非 SDK 接口限制生效 | 深灰/黑名单反射失效 | 见上文第一节 |
| Apache HTTP client 移除 | classloader 找不到 | `<uses-library android:name="org.apache.http.legacy"/>` |
| 前台服务需声明类型 | `startForeground` 异常 | `android:foregroundServiceType` + manifest 权限 |
| BUILD私密信息收口 | `Build.SERIAL` 需 `READ_PHONE_STATE` | 改用 `Build.getSerial()` + 运行时权限 |
| 空白页指纹/Hw 加密限制 | `KeyChain` 等行为收紧 | 检查安全相关 API 调用 |

### 2.2 targetSdk → 29 (Android 10 Q)

| 变更 | 影响 | 适配 |
|------|------|------|
| Scoped Storage（分区存储） | 外部存储直接路径失效 | `requestLegacyExternalStorage=true` 过渡；MediaStore API 重建 |
| 设备标识符收紧 | IMEI/MEID 拦截（默认返回 null 或抛异常） | `getImei()` + `READ_PRIVILEGED_PHONE_STATE`（仅系统应用）/ 改用 ANDROID_ID |
| Wi-Fi/定位联动 | `WifiInfo` 需要 `ACCESS_FINE_LOCATION` | 补权限 + 运行时申请 |
| 折叠屏/多分辨率 | `Display.getRealSize` 行为变化 | 用 `WindowMetrics`（后续版本）或兼容判断 |
| 悬浮窗限制 | `TYPE_PHONE/TYPE_SYSTEM_ALERT` 移除 | 统一改 `TYPE_APPLICATION_OVERLAY` (2038) |
| 后台 Activity 启动限制 | 后台无法直接 startActivity | 全屏 Intent 通知 / SYSTEM_ALERT_WINDOW 授权 / 通知 |
| `getRunningTasks/getRecentTasks` 收紧 | 只返回自身任务 | 可见性场景改用 `ActivityLifecycleCallbacks`（自家应用）或 `UsageStatsManager` |

### 2.3 targetSdk → 30 (Android 11 R)

| 变更 | 影响 | 适配 |
|------|------|------|
| Scoped Storage 强制 | `requestLegacyExternalStorage` 失效 | 完全迁移 MediaStore/FileProvider；`MANAGE_EXTERNAL_STORAGE` 仅特殊应用 |
| Package Visibility（包可见性） | `getInstalledPackages` 只看到部分包 | manifest 加 `<queries>` 声明；或 `QUERY_ALL_PACKAGES`（审核限制） |
| 一次性权限 | 麦克风/位置"仅本次" | `shouldShowRequestPermissionRationale` 逻辑补充 |
| 前台服务访问摄像头/麦克风受限 | 后台前台服务拿不到 | `foregroundServiceType=camera|microphone` 声明 |
| Autofill/IME 限制 | 自定义输入场景收紧 | 检查自定义 View 的 autofill hint |

`<queries>` 示例：

```xml
<queries>
    <package android:name="com.example.target" />
    <intent>
        <action android:name="android.intent.action.SEND" />
        <data android:mimeType="image/*" />
    </intent>
</queries>
```

### 2.4 targetSdk → 31 (Android 12 S)

| 变更 | 影响 | 适配 |
|------|------|------|
| PendingIntent 必须 flags | 不写 `FLAG_IMMUTABLE/MUTABLE` 直接崩 | 补 `PendingIntent.FLAG_IMMUTABLE` |
| 前台服务启动限制 | 后台无法 startForegroundService（更严） | WorkManager / 高优先级 FCM |
| Notification trampoline 禁止 | 通知点击里 startActivity 崩 | 改 `PendingIntent.getActivity` 直接跳 |
| 蓝牙权限拆分 | `BLUETOOTH_CONNECT/SCAN` 新权限 | 权限迁移 |
| Splash Screen 强制 | 自定义启动页被覆盖 | `android:windowSplashScreen*` 适配 SplashScreen API |

### 2.5 targetSdk → 33 (Android 13 T) / 34 (U) 速览

- 33：通知运行时权限 `POST_NOTIFICATIONS`；精确闹钟 `USE_EXACT_ALARM` 分离；Wi-Fi 权限拆分 `NEARBY_WIFI_DEVICES`。
- 34：前台服务类型强制全量声明；`SCHEDULE_EXACT_ALARM` 默认拒绝；隐式 Intent 必须 receiver export 显式声明（`android:exported` 强制）。

---

## 三、升级实操 checklist

### 3.1 升级前静态扫描

```bash
# 1. veridex 扫隐藏 API（Google 开源工具）
veridex --app app-release.apk > hidden_api_report.txt

# 2. lint 检查（AGP 内置 NewApi / Deprecated）
./gradlew lint

# 3. 依赖库版本核对（老 support 库 → AndroidX）
grep -rn "com.android.support" app/build.gradle
```

### 3.2 运行时验证

```bash
# 开启隐藏 API 日志（不拦截，仅记录）
adb shell settings put global hidden_api_policy 1

# 真机跑全量主流程，抓触碰记录
adb logcat | grep -cE "Accessing hidden"     # 统计次数
adb logcat | grep -E "Accessing hidden" > violations.log

# 检查行为变更专项
- 存储读写（scoped storage）→ 文件保存/读取/分享
- 包可见性 → 分享、跳转第三方、推送渠道判断
- 通知 → 前台服务通知、trampoline
- PendingIntent → 全部代码路径搜 PendingIntent.getActivity/getService/getBroadcast
```

### 3.3 常见崩溃日志特征（快速对号入座）

| 崩溃日志 | 根因 |
|---------|------|
| `NoSuchMethodError: Landroid/...` | 隐藏 API 被拦 |
| `NoSuchFieldError` | 隐藏字段被拦 |
| `Permission Denial: starting Intent ... not exported` | R 起隐式接收者 exported 强制 |
| `IllegalArgumentException: Must specify FLAG_IMMUTABLE` | S 起 PendingIntent flags |
| `ForegroundServiceStartNotAllowedException` | S 起后台启动 FGS 受限 |
| `One of RECEIVER_EXPORTED/RECEIVER_NOT_EXPORTED` | U 起动态 receiver 必须 flag |

---

## 四、ROM / Framework 开发者附加注意点

1. **系统应用不受 targetSdk 部分限制**：`privapp` + platform 签名可豁免部分拦截，
   但黑名单接口依旧生效，privapp 白名单文件必须同步更新（否则起不来）。

2. **hiddenapi 列表定制**：ROM 里给自家应用放开深灰接口，改
   `frameworks/base/config/hiddenapi-*.txt` 后需要 `make framework` + 刷机验证；
   注意列表是编译期合并产物，直接改 out 目录无效。

3. **CTS/GTS 影响**：修改 hiddenapi 白名单包（`hiddenapi-package-whitelist.xml`）
   需评估 CTS 项 `CtsHiddenApi*`；`QUERY_ALL_PACKAGES`、`MANAGE_EXTERNAL_STORAGE`
   同样有 CTS/Play 审核约束。

4. **targetSdk 与进程优先级无关**：不要混淆 targetSdk（行为兼容）与
   `compileSdk`（可调用 API 上限）与 minSdk（安装下限）三者关系。

5. **反射调 @hide 的系统内部模块（自研）**：优先把接口声明为 `@SystemApi`
   走正规 system API 通道，而不是留在 @hide 被反射访问 —— 后续升级链路更稳。

---

## 附录：关键源码/工具索引

```
art/runtime/hidden_api.cc                          # 运行时拦截实现
libcore/luni/src/main/java/java/lang/Class.java    # 反射入口过滤
frameworks/base/config/hiddenapi-*.txt             # 分类列表
build/soong/java/hiddenapi.go                      # 编译期合并
/system/etc/sysconfig/hiddenapi-package-whitelist.xml  # 运行时包豁免

工具：
veridex        # 静态扫描 apk
perfetto/systrace  # 行为变更相关性能问题
```
