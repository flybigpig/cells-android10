# 为什么 Zygote 用 Socket 而不是 Binder

> 分析基于本仓库 `cells-android10`（Android 10）源码。

## 事实确认

`init.zygote64_32.rc:6` 里 zygote 服务由 init 创建本地 Unix 域 socket：

```6:6:system/core/rootdir/init.zygote64_32.rc
    socket zygote stream 660 root system
```

即 `/dev/socket/zygote`，属主 `root:system`、权限 `660`。Zygote 启动后在一个 `Os.poll()` 单线程循环上等待该 socket（`ZygoteServer.runSelectLoop`，`ZygoteServer.java:373,432`）。选择 socket 是多重原因叠加的**有意设计**。

---

## 一、核心原因：Zygote 必须能安全 `fork()`，而 Binder 是多线程的

`fork()` 只复制**调用线程**，但会**连同整个地址空间的锁状态一起复制**。若进程里有别的线程正持有某个 mutex（Binder 线程池里的线程随时可能在拿锁），子进程就会继承一个“永远没人来解锁”的死锁锁 → 子进程随即卡死。所以**要安全 fork 的进程必须基本单线程**。

- Binder 通信依赖**线程池**（每个 Binder 调用可能在不同 Binder 线程上执行），一旦 Zygote 接 Binder，它就必然变成多线程，fork 就不可行。
- 因此 Zygote 刻意**不接入 Binder 线程池**，只用单个 `poll` 循环收命令、在主线程上 `fork`（`ZygoteServer.java:432` 的 `Os.poll`；fork 前后有 `ZygoteHooks.preFork()/postForkCommon()` 在 `:319/:333` 停/恢复少量其它线程）。

---

## 二、启动顺序：Binder 此时还“没出生”

`system_server`（AMS 所在，真正发“启动 app”请求的进程）**本身就是 Zygote fork 出来的**。也就是说：

- 接收“fork app”请求的实体（system_server）是 Zygote 的孩子；
- system_server 起来之后才建立 Binder 通信框架。

如果 Zygote 用 Binder 收请求，就会出现“先有鸡还是先有蛋”的依赖——Zygote 不能依赖一个由它自己 fork 出来、且要等它先运行的框架。本地 socket 是**独立于 Android 服务框架、init 一启动就建好**的通道，没有这个循环依赖。

---

## 三、鉴权：Unix 域 socket 有内核级 `SO_PEERCRED`

socket 连接建好后，Zygote 立刻取对方凭据：

```103:103:ZygoteConnection.java
            peer = mSocket.getPeerCredentials();
```

底层是 `getsockopt(fd, SOL_SOCKET, SO_PEERCRED, ...)`（`android_net_LocalSocketImpl.cpp:399`），由内核保证、不可伪造。Zygote 据此做 `applyUidSecurityPolicy(args, peerCredentials)`（`Zygote.java:558`），只允许特权调用方 fork app。

再叠加文件层防护：socket 节点权限是 `660 root system`（`init.zygote64_32.rc:6`），普通 app 连都连不上。两层防护保证“只有 system_server 这类特权进程能请求 fork”。

---

## 四、文件描述符传递：`SCM_RIGHTS`

Unix 域 socket 支持通过 `sendmsg/recvmsg` 携带 `SCM_RIGHTS` 把**已打开的 fd** 发给对方（`LocalSocketImpl.java:553` 注释 “See man 7 unix SCM_RIGHTS”）。Zygote fork 新 app 时需要把预打开的 fd（如 app 的打开文件、共享内存、相关 socket 等）传给子进程，这个能力用 Binder 跨进程传 fd 也能做，但 socket 这条通道已经天然具备，且和上面“单线程 + 凭据”的模型统一。

---

## 五、协议简单、攻击面小

“连接 → 发一段 ZygoteArguments 文本参数 → fork → 回 pid” 的流式协议足够简单。Zygote 进程刻意保持精简——**不挂 Binder 栈、不加复杂框架**，只做一件事：收命令、fork、特化。越少代码越少攻击面（Zygote 以 root 运行，是安全敏感进程）。

---

## 六、可平滑扩展到 USAP 池

Android 10 已引入 USAP（Unspecialized App Process）池，复用同一 socket 机制：预 fork 出的 `usap` 进程 `accept()` 同一个池 socket、读参数、再 `specialize`（`Zygote.java:506-558`）。说明这套 socket 模型天然便于池化，进一步降低每次启动的 fork 开销。

---

## 一句话总结

Zygote 用 socket 而不是 Binder，本质是因为 **Zygote 要 fork 子进程，而 fork 要求进程基本单线程；Binder 自带线程池会让 fork 不安全**；同时 socket 还具备 **启动期即可用（无 Binder 循环依赖）、内核级 `SO_PEERCRED` 鉴权、文件权限 `660 root system` 隔离、`SCM_RIGHTS` 传 fd、协议精简攻击面小** 等优势，完美契合“特权、单线程、只负责 fork”的 Zygote 角色。

---

## 关键源码索引

| 主题 | 文件 | 行号 |
| --- | --- | --- |
| init 创建本地 socket（660 root system） | `system/core/rootdir/init.zygote64_32.rc` | 6 / 22 |
| 单线程 poll 循环收命令 | `frameworks/base/core/java/com/android/internal/os/ZygoteServer.java` | 373 / 432 |
| fork 前后停/恢复线程 | `ZygoteServer.java` | 319 / 333 |
| 取对端凭据 SO_PEERCRED | `frameworks/base/core/java/com/android/internal/os/ZygoteConnection.java` | 103 |
| 凭据底层实现 getsockopt | `frameworks/base/core/jni/android_net_LocalSocketImpl.cpp` | 399 |
| 按凭据做 uid 安全策略 | `frameworks/base/core/java/com/android/internal/os/Zygote.java` | 558 |
| SCM_RIGHTS 传 fd | `frameworks/base/core/java/android/net/LocalSocketImpl.java` | 553 |
| USAP 池复用同一 socket | `frameworks/base/core/java/com/android/internal/os/Zygote.java` | 506-558 |

---

## 七、补充：AMS → Zygote 的完整调用链（Process.start 如何连 socket、写 ZygoteArguments）

前面只讲了 Zygote 端“收命令”，下面补全 **AMS（system_server）侧如何发起一次 app 启动、连上 `/dev/socket/zygote`、把参数写过去并读回 pid** 的链路。

### 调用链总览

```
ProcessList.startProcessLocked
  → Process.start(entryPoint, ...)                        // AMS 进程
    → ZYGOTE_PROCESS.start(...)                           // android.os.Process
      → ZygoteProcess.startViaZygote(...)                 // 组装 ZygoteArguments
        → openZygoteSocketIfNeeded(abi)
          → attemptConnectionToPrimaryZygote()
            → ZygoteState.connect(...)                    // LocalSocket 连 /dev/socket/zygote
        → zygoteSendArgsAndGetResult(state, useUsapPool, args)
          → attemptZygoteSendArgsAndGetResult()           // 写 argc+args 文本，读回 pid
            [Zygote 侧] runSelectLoop → ZygoteConnection.readArgumentList
                       → ZygoteArguments.parseArgs → Zygote.forkAndSpecialize → 回写 pid
```

### 1. AMS 发起：`ProcessList.startProcessLocked`

`ProcessList.java:1823` 在普通/primary 路径下调 `Process.start`，把 app 各项参数传入（entryPoint 默认 `android.app.ActivityThread`，外加 uid/gid/gids/runtimeFlags/seInfo/abi/instructionSet/dataDir/packageName，最后 append 一个 `PROC_START_SEQ_IDENT + app.startSeq` 的 `zygoteArgs`）：

```1823:1827:ProcessList.java
                startResult = Process.start(entryPoint,
                        app.processName, uid, uid, gids, runtimeFlags, mountExternal,
                        app.info.targetSdkVersion, seInfo, requiredAbi, instructionSet,
                        app.info.dataDir, invokeWith, app.info.packageName,
                        new String[] {PROC_START_SEQ_IDENT + app.startSeq});
```

### 2. `Process.start` 委托给单例 `ZYGOTE_PROCESS`

`Process.java:521` 的静态方法只是转发，并默认 `useUsapPool=true`（启用 USAP 池优化）：

```534:537:Process.java
        return ZYGOTE_PROCESS.start(processClass, niceName, uid, gid, gids,
                    runtimeFlags, mountExternal, targetSdkVersion, seInfo,
                    abi, instructionSet, appDataDir, invokeWith, packageName,
                    /*useUsapPool=*/ true, zygoteArgs);
```

### 3. `ZygoteProcess.startViaZygote` 组装 ZygoteArguments

`ZygoteProcess.java:541` 把结构化参数翻译成一行行 `--key=value` 文本：`--setuid/--setgid/--runtime-flags/--target-sdk-version/--setgroups/--nice-name/--seinfo/--instruction-set/--app-data-dir/--package-name`，最后 put 上 `processClass`（即 entryPoint）和 extraArgs（`ZygoteProcess.java:561-630`）。注意注释强调 `--setuid/--setgid/--setgroups` 必须排最前（`:559-560`）。

```626:630:ZygoteProcess.java
        argsForZygote.add(processClass);
        if (extraArgs != null) {
            Collections.addAll(argsForZygote, extraArgs);
        }
```

### 4. 连 socket：`openZygoteSocketIfNeeded` → `ZygoteState.connect`

`openZygoteSocketIfNeeded`（`ZygoteProcess.java:906`）先连 primary zygote，abi 不匹配再连 secondary（64_32 架构的两个 zygote 即来源于此）。连接动作在 `ZygoteState.connect`（`ZygoteProcess.java:175`），用 `LocalSocket` 去连由 init 创建的 `/dev/socket/zygote`（`LocalSocketAddress(Zygote.PRIMARY_SOCKET_NAME, Namespace.RESERVED)`，定义于 `:108`）：

```181:189:ZygoteProcess.java
            final LocalSocket zygoteSessionSocket = new LocalSocket();
            ...
                zygoteSessionSocket.connect(zygoteSocketAddress);
                zygoteInputStream = new DataInputStream(zygoteSessionSocket.getInputStream());
                ...
```

> 这正是前面分析的：`socket zygote stream 660 root system` 这个 Unix 域 socket，只有 system_server（root/system）才有资格连。

### 5. 写参数并读回 pid：`zygoteSendArgsAndGetResult`

`zygoteSendArgsAndGetResult`（`ZygoteProcess.java:381`）先做防注入校验（参数里不允许嵌入 `\n`/`\r`，`:389-393`），再按**线协议**拼成文本消息——先写参数个数，再每行一个参数（协议说明在 `:396-405`）：

```406:406:ZygoteProcess.java
        String msgStr = args.size() + "\n" + String.join("\n", args) + "\n";
```

若启用 USAP 池则走 `attemptUsapSendArgsAndGetResult`（`:451`，发给 USAP 池 socket），否则走 `attemptZygoteSendArgsAndGetResult`（`:422`）：

```428:436:ZygoteProcess.java
            zygoteWriter.write(msgStr);
            zygoteWriter.flush();
            ...
            result.pid = zygoteInputStream.readInt();
            result.usingWrapper = zygoteInputStream.readBoolean();
            if (result.pid < 0) {
                throw new ZygoteStartFailedEx("fork() failed");
            }
```

即 AMS 写完参数后**阻塞读一个 int（子进程 pid）和 bool（是否用了 wrapper）**，拿到 pid 即代表 fork 成功，`Process.ProcessStartResult.pid` 返回给 `ProcessList`。

### 6. Zygote 侧收命令（呼应前文）

回到前面追过的 Zygote 端：`ZygoteServer.runSelectLoop`（`Os.poll` 单线程）收到 POLLIN → `ZygoteConnection` 调 `readArgumentList` 按“count + 逐行参数”解析 → `ZygoteArguments.parseArgs` 解析成结构化参数 → `Zygote.forkAndSpecialize` 真正 fork 并特化（设 uid/gid/seinfo/namespace 等）→ 父进程把子进程 pid 写回这个 socket，AMS 的 `readInt()` 即返回。

### 这一节的关键设计点（串联前几轮结论）

- **整条链复用同一个 Unix 域 socket + 文本线协议**：`argc\narg0\narg1\n...`，简单、无需 Binder。
- **AMS 是 socket 客户端、Zygote 是服务端**，而 Zygote 用 `getPeerCredentials()`（`SO_PEERCRED`）校验调用方就是 system_server 这类特权进程。
- **单一入口、统一出口**：所有“启动进程”的路径（普通 app、webview zygote、app zygote、USAP 池）都收敛到 `ZygoteProcess.start` + `startViaZygote` 组装参数；`--setuid/--setgid` 等安全相关参数由 AMS 写入、Zygote 信任（因为已鉴权），fork 后由 Zygote 在子进程里 `setuid` 降级，避免子进程拿到 root。

### 调用链新增源码索引

| 主题 | 文件 | 行号 |
| --- | --- | --- |
| AMS 发起 Process.start | `frameworks/base/services/core/java/com/android/server/am/ProcessList.java` | 1823 |
| 静态转发给 ZYGOTE_PROCESS | `frameworks/base/core/java/android/os/Process.java` | 521 / 534 |
| 组装 ZygoteArguments | `frameworks/base/core/java/android/os/ZygoteProcess.java` | 541 / 561-630 |
| 选 primary/secondary zygote 并连接 | `ZygoteProcess.java` | 906 / 872-875 |
| LocalSocket 连 /dev/socket/zygote | `ZygoteProcess.java` | 175 / 181-189 |
| 线协议：argc + 逐行参数 | `ZygoteProcess.java` | 406 / 396-405 |
| 写参数并读回 pid | `ZygoteProcess.java` | 422 / 428-436 |
