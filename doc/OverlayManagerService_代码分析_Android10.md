# OverlayManagerService 代码分析（Android 10 / cells-android10）

> 源文件：`frameworks/base/services/core/java/com/android/server/om/OverlayManagerService.java`（47KB）
> 同包协作类：`OverlayManagerServiceImpl.java`（33KB）、`OverlayManagerSettings.java`（25KB）、`IdmapManager.java`（8KB）、`IdmapDaemon.java`（7KB）、`OverlayManagerShellCommand.java`（10KB）、`DumpState.java`（2KB）

---

## 一、它是什么：RRO / 资源覆盖的管理者

OMS（OverlayManagerService）管理**资源覆盖**：一个 apk（overlay 包）可以给另一个 apk（target 包）追加/替换资源，典型用途是换主题、改配色字体、OEM 定制。文件头注释（`:86-205`）把设计讲得很清楚，有三条关键定位：

1. **OMS 不决定"该开哪个 overlay"**，它只保证 overlay 在技术上和安全上"可以被使用"，并响应外部请求去激活。真正决定开关的是 themes / dynamic customization 那类上层组件。
2. **变更通过 Activity 生命周期传播**——`scheduleApplicationInfoChanged` 让应用在一个良定义的点重读资源，而不是随时热替换。
3. **输入只有三个来源**：SystemService 回调（开机、切用户）、PMS 的包广播、AIDL `IOverlayManager`。

必须掌握的四个术语（`:178-203`）：

| 术语 | 含义 |
|---|---|
| target [package] | 被扩展资源池的普通 apk |
| overlay [package] | 提供额外资源的 apk |
| approved | 技术上+安全上可用（target 已装、资源名匹配、idmap 已建）。**外部客户端不能改** |
| enabled | 已激活、参与资源查找。要求先 approved。**只有外部客户端能改** |
| idmap | target 与 overlay 之间资源 ID 的映射表，资源查找时使用 |

注意 **approved 与 enabled 是两个独立状态**：approved 是系统判定的技术资格，enabled 是用户/应用的开关意愿。这是理解整个状态机的关键。

---

## 二、类结构与分层

文件头画了一张三层图（`:146-167`），本仓库完全对应：

```
        Android framework
       |         ^
  AIDL |         | broadcasts
       v         |
  OverlayManagerService (1)  ← 与框架交互：Binder、广播、SystemService 回调
        \
         \   (测试时被 OverlayManagerTests (3) 替换)
          v
  OverlayManagerServiceImpl (2)  ← 干重活：状态机、approve/enable 判定
       |              |
 OverlayManagerSettings   IdmapManager  ← 持久化 / idmap 文件管理
```

| 类 | 职责 |
|---|---|
| `OverlayManagerService` | SystemService 子类 + `IOverlayManager.Stub` + 两个 BroadcastReceiver + 落盘调度 |
| `OverlayManagerServiceImpl` | 核心状态机：overlay 的 approve/enable、target/overlay 包的增删响应、返回 `OverlayInfo` |
| `OverlayManagerSettings` | `/data/system/overlays.xml` 的读写与内存态，提供 `ChangeListener` 回调 |
| `IdmapManager` / `IdmapDaemon` | 创建/删除 idmap 文件（经 `Installer` 调 native idmap） |
| `OverlayManagerShellCommand` | `adb shell cmd overlay` 的实现 |
| `DumpState` | dump 参数载体 |

---

## 三、构造与启动顺序（`:233-271`）

`onStart()` **故意为空**（`:273-276`），所有工作都在构造函数里完成，这在 SystemService 里不常见：

```233:271:frameworks/base/services/core/java/com/android/server/om/OverlayManagerService.java
public OverlayManagerService(@NonNull final Context context,
        @NonNull final Installer installer) {
    ...
    mSettingsFile = new AtomicFile(
            new File(Environment.getDataSystemDirectory(), "overlays.xml"), "overlays");
    IdmapManager im = new IdmapManager(installer, mPackageManager);
    mSettings = new OverlayManagerSettings();
    mImpl = new OverlayManagerServiceImpl(mPackageManager, im, mSettings,
            getDefaultOverlayPackages(), new OverlayChangeListener());

    final IntentFilter packageFilter = new IntentFilter();
    packageFilter.addAction(ACTION_PACKAGE_ADDED);
    packageFilter.addAction(ACTION_PACKAGE_CHANGED);
    packageFilter.addAction(ACTION_PACKAGE_REMOVED);
    packageFilter.addDataScheme("package");
    getContext().registerReceiverAsUser(new PackageReceiver(), UserHandle.ALL, ...);

    final IntentFilter userFilter = new IntentFilter();
    userFilter.addAction(ACTION_USER_ADDED);
    userFilter.addAction(ACTION_USER_REMOVED);
    getContext().registerReceiverAsUser(new UserReceiver(), UserHandle.ALL, ...);

    restoreSettings();
    initIfNeeded();
    onSwitchUser(UserHandle.USER_SYSTEM);

    publishBinderService(Context.OVERLAY_SERVICE, mService);
    publishLocalService(OverlayManagerService.class, this);
}
```

顺序要点：先建持久化文件句柄 → 建 impl → 注册两个广播（**注意 `UserHandle.ALL`，对所有用户生效**）→ 恢复存档 → 初始化所有用户 → 切到 system 用户 → 最后才 publish 服务（保证客户端连上时内部状态已就绪）。

**`getDefaultOverlayPackages()`（`:312`）** 读系统属性 `ro.boot.vendor.overlay.theme`（`:217`，分号分隔的包名列表），这是 **vendor/OEM 默认启用主题 overlay 的入口**——刷机时通过该属性预置默认主题，无需用户手动开。

---

## 四、三类输入的处理

### 4.1 系统回调

- **`initIfNeeded()`（`:278`）**：对 `!supportsSwitchTo()` 且非 system 的用户也要 `updateOverlaysForUser` + `updateOverlayPaths`。注释解释得很直白：这些用户永远走不到 `onSwitchUser()`，不在这里初始化就再没机会。
- **`onSwitchUser()`（`:297`）**：`updateOverlaysForUser` → `updateAssets` → `schedulePersistSettings`。

### 4.2 PMS 广播（`PackageReceiver`，`:327`）

overlay 是普通 apk，所以 PMS 的包广播就是 OMS 的感知渠道。分发逻辑：

```
ACTION_PACKAGE_ADDED     → onPackageAdded     → isOverlayPackage() ? onOverlayPackageAdded    : onTargetPackageAdded
ACTION_PACKAGE_CHANGED   → onPackageChanged   → ... onOverlayPackageChanged / onTargetPackageChanged
ACTION_PACKAGE_REPLACING → onPackageReplacing → forgetPackageInfo + onOverlayPackageReplacing
ACTION_PACKAGE_REPLACED  → onPackageReplaced  → ... onOverlayPackageReplaced / onTargetPackageReplaced
ACTION_PACKAGE_REMOVED   → onPackageRemoved   → forgetPackageInfo + (有 OverlayInfo ? onOverlayPackageRemoved : onTargetPackageRemoved)
```

三个细节：用 `pi.isOverlayPackage()`（检查 `android:isOverlay`/targetPackage 元数据）区分 overlay 与 target 包；**按 userId 逐个处理**（overlay 可每用户不同）；replacing/removed 前先 `mPackageManager.forgetPackageInfo()` 清 PMS 缓存，避免读到旧包信息。

### 4.3 AIDL `IOverlayManager.Stub`（`mService`，`:524`）

读接口（只校验用户身份）：`getAllOverlays`、`getOverlayInfosForTarget`、`getOverlayInfo`。

写接口（需 `CHANGE_OVERLAY_PACKAGES` 权限）：`setEnabled`、`setEnabledExclusive`、`setEnabledExclusiveInCategory`（同类别独占启用）、`setPriority` / 同类最高优先级。

权限模型（`:835-849`）：

```835:849:frameworks/base/services/core/java/com/android/server/om/OverlayManagerService.java
private int handleIncomingUser(final int userId, @NonNull final String message) {
    return ActivityManager.handleIncomingUser(Binder.getCallingPid(),
            Binder.getCallingUid(), userId, false, true, message, null);
}
...
private void enforceChangeOverlayPackagesPermission(@NonNull final String message) {
    getContext().enforceCallingOrSelfPermission(
            android.Manifest.permission.CHANGE_OVERLAY_PACKAGES, message);
}
```

读权限 = 同用户 或 `INTERACT_ACROSS_USERS_FULL`；写权限 = 读权限 + `CHANGE_OVERLAY_PACKAGES`。每个写方法都用 `Binder.clearCallingIdentity()` / `restoreCallingIdentity()` 包裹（因为内部要回调 PMS/AMS，不能带着调用者身份）。

另外 `onShellCommand`（`:743`）转给 `OverlayManagerShellCommand`（即 `adb shell cmd overlay`），`dump`（`:752`）用 `DumpState` 且支持 `--user`。

---

## 五、变更如何传播（全篇最核心的一段）

`OverlayChangeListener`（`:863`）是 impl 状态变化回到 OMS 的唯一出口：

```863:887:frameworks/base/services/core/java/com/android/server/om/OverlayManagerService.java
private final class OverlayChangeListener
        implements OverlayManagerServiceImpl.OverlayChangeListener {
    @Override
    public void onOverlaysChanged(@NonNull final String targetPackageName, final int userId) {
        schedulePersistSettings();
        FgThread.getHandler().post(() -> {
            updateAssets(userId, targetPackageName);
            final Intent intent = new Intent(Intent.ACTION_OVERLAY_CHANGED,
                    Uri.fromParts("package", targetPackageName, null));
            intent.setFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT);
            ...
            ActivityManager.getService().broadcastIntent(null, intent, null, null, 0,
                    null, null, null, android.app.AppOpsManager.OP_NONE, null, false, false,
                    userId);
        });
    }
}
```

两条线并行：**同步落盘**（`schedulePersistSettings`）+ **抛到 FgThread** 做两件事——更新资源、发 `ACTION_OVERLAY_CHANGED` 广播。注意 `FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT`：只投给开机前就注册的接收者，防止开机后被滥用监听主题变化。

**`updateAssets`（`:947`）** = 两步：

```947:955:frameworks/base/services/core/java/com/android/server/om/OverlayManagerService.java
private void updateAssets(final int userId, List<String> targetPackageNames) {
    updateOverlayPaths(userId, targetPackageNames);
    final IActivityManager am = ActivityManager.getService();
    try {
        am.scheduleApplicationInfoChanged(targetPackageNames, userId);
    } catch (RemoteException e) {
        // Intentionally left empty.
    }
}
```

**`updateOverlayPaths`（`:893`）** 是真正写回 PMS 的地方，把「某 target 该启用哪些 overlay」算成列表交给 `pm.setEnabledOverlayPackages(userId, target, list)`。其中有个容易漏的规则（`:915-917`）：

```915:918:frameworks/base/services/core/java/com/android/server/om/OverlayManagerService.java
List<String> list = new ArrayList<>();
if (!"android".equals(targetPackageName)) {
    list.addAll(frameworkOverlays);
}
list.addAll(mImpl.getEnabledOverlayPackageNames(targetPackageName, userId));
```

**非 `android` 的 target 也会叠加 framework overlay**——即针对 `android`（框架资源包）启用的 overlay，对所有 app 生效。另一处特例（`:901-904`）：若变更目标里含 `"android"`，则把 target 列表换成 `pm.getTargetPackageNames(userId)`（全部 target），因为框架资源变了，所有应用的资源都得重建。

最后 `am.scheduleApplicationInfoChanged` 让 AMS 在 Activity 生命周期节点重新加载 ApplicationInfo，这就是文件头说的"changes propagate to running applications as part of the Activity lifecycle"。

---

## 六、持久化

- **`schedulePersistSettings()`（`:957`）**：`mPersistSettingsScheduled.getAndSet(true)` 做**合并去重**（连续多次变更只落一次盘），然后抛到 `IoThread` 异步写。用 `AtomicFile` 三态保证原子性：`startWrite()` → `finishWrite(stream)`，异常则 `failWrite(stream)`，绝不留下写一半的 `overlays.xml`。
- **`restoreSettings()`（`:980`）**：读回 `overlays.xml`，并**清掉已不存在用户的数据**——注释说明这是为「设备在收到 `USER_REMOVED` 前就重启」的情况兜底。

---

## 七、线程与锁

| 机制 | 用途 |
|---|---|
| `mLock`（`:219`） | 保护 `mImpl` / `mSettings` 的内存态，跨 Binder、广播、回调线程 |
| `FgThread.getHandler().post(...)`（`:868`） | 变更后更新资源 + 发广播（要调 AMS，不能持锁做） |
| `IoThread.getHandler().post(...)`（`:961`） | 落盘 IO，与主流程解耦 |
| `AtomicFile` | 文件级原子写 + 自带锁 |
| `traceBegin/traceEnd(TRACE_TAG_RRO, ...)` | 每个 Binder 方法都用 try/finally 包裹打 systrace，RRO 相关卡顿可直接抓 |

---

## 八、与本项目（cells）的关系

OMS 本身**没有 cells 多容器定制**，是标准 AOSP Android 10 实现。唯一与 OEM 相关的钩子是 `ro.boot.vendor.overlay.theme`（`:217`）——刷机/vendor 分区可用它预置默认主题 overlay 包。如果你的定制涉及"不同 cell 用不同主题"，那不在 OMS 里做，而应由上层按 cell 调 `setEnabled` 并给每个 userId / 每个 target 分别配置。

---

## 九、方法索引表

| 方法 | 行号 | 说明 |
|---|---|---|
| 类注释（架构图 + 术语） | :86-205 | 最佳入门材料 |
| `OverlayManagerService(context, installer)` | :233 | 全部启动工作在此完成 |
| `onStart` | :273 | **故意为空** |
| `initIfNeeded` | :278 | 初始化不可切换用户 |
| `onSwitchUser` | :297 | 切用户时刷新 overlay |
| `getDefaultOverlayPackages` | :312 | 读 `ro.boot.vendor.overlay.theme` |
| `PackageReceiver` | :327 | PMS 包广播分发 |
| `UserReceiver` | :484 | 用户增删 |
| `mService`（IOverlayManager.Stub） | :524 | Binder 门面 |
| `onShellCommand` / `dump` | :743 / :752 | shell 与 dumpsys |
| `handleIncomingUser` | :835 | 用户身份校验 |
| `enforceChangeOverlayPackagesPermission` | :847 | 写权限校验 |
| `OverlayChangeListener` | :863 | 状态变化 → 落盘 + 广播 |
| `updateOverlayPaths` | :893 | 计算并写回 PMS 的启用列表 |
| `updateAssets` | :947 | 更新资源 + 通知 AMS 重载 |
| `schedulePersistSettings` | :957 | 合并去重 + IoThread 原子落盘 |
| `restoreSettings` | :980 | 恢复存档 + 清理死用户数据 |

---

## 十、小结

OMS 的骨架是「**一个薄门面 + 一个重状态机 + 一个持久化 + 一个 native 桥**」：`OverlayManagerService` 只管与框架打交道（Binder、广播、SystemService 回调、落盘调度），`OverlayManagerServiceImpl` 负责 approved/enabled 状态机，`OverlayManagerSettings` 管 `overlays.xml`，`IdmapManager` 管 idmap 文件。抓住三件事就能读懂它：**approved 与 enabled 分离**、**变更经 `OverlayChangeListener` → `updateOverlayPaths` → `pm.setEnabledOverlayPackages` 回到 PMS**、以及 **framework overlay 对所有 target 生效 + `android` 变更触发全量 target 重建**这两条特殊规则。

---

## 参考源码

- `frameworks/base/services/core/java/com/android/server/om/OverlayManagerService.java`
- 同包 `OverlayManagerServiceImpl.java`、`OverlayManagerSettings.java`、`IdmapManager.java`、`IdmapDaemon.java`、`OverlayManagerShellCommand.java`、`DumpState.java`
- `android.content.om.IOverlayManager`（AIDL）、`android.content.om.OverlayInfo`
