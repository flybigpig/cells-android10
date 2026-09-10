我来梳理 init → zygote 的完整启动链路。先定位本仓库的 zygote rc 与服务定义。
读取 64+32 双架构的 zygote rc 定义与 init 启动服务的实现。
已取齐全链路证据。下面是本仓库(AOSP 10)的 **init → zygote** 完整启动链路。

## 一、时序总览

```text
init second_stage
  → 解析 /init.rc(其中 import /init.${ro.zygote}.rc)
      → 解析 /init.zygote64_32.rc → 生成 Service 对象 "zygote" / "zygote_secondary"
  → on post-fs-data 末尾 trigger zygote-start
  → on zygote-start && property:ro.crypto.state=...  → start netd / start zygote / start zygote_secondary
  → do_start → Service::Start()
      → fork()
          子进程:umask → 建 socket 并 setenv ANDROID_SOCKET_zygote → writepid → ZapStdio
                  → SetProcessAttributes(uid/gid/caps/selinux) → exec app_process64
      → 父进程:记 pid、SVC_RUNNING、createProcessGroup
  → app_process64:解析 --zygote → runtime.start("com.android.internal.os.ZygoteInit")
  → AndroidRuntime::startVm/startReg → ZygoteInit.main()
      → preload → gcAndFinalize → Zygote.initNativeState
      → new ZygoteServer(isPrimaryZygote):读 ANDROID_SOCKET_zygote 拿到 init 建的 fd
      → forkSystemServer() → system_server 分支执行 Runnable
      → zygote 自身:runSelectLoop() 阻塞等待 fork 请求
```

## 二、rc 定义:zygote 到底是什么

`init.rc` 用属性动态导入:

```12:12:system/core/rootdir/init.rc
import /init.${ro.zygote}.rc
```

本仓库有 `init.zygote32/32_64/64/64_32` 四套。64 位主 + 32 位辅的方案是:

```1:25:system/core/rootdir/init.zygote64_32.rc
service zygote /system/bin/app_process64 -Xzygote /system/bin --zygote --start-system-server --socket-name=zygote
    class main
    priority -20
    user root
    group root readproc reserved_disk
    socket zygote stream 660 root system
    socket usap_pool_primary stream 660 root system
    onrestart write /sys/android_power/request_state wake
    onrestart write /sys/power/state on
    onrestart restart audioserver
    onrestart restart cameraserver
    onrestart restart media
    onrestart restart netd
    onrestart restart wificond
    writepid /dev/cpuset/foreground/tasks

service zygote_secondary /system/bin/app_process32 -Xzygote /system/bin --zygote --socket-name=zygote_secondary --enable-lazy-preload
    class main
    priority -20
    user root
    group root readproc reserved_disk
    socket zygote_secondary stream 660 root system
    socket usap_pool_secondary stream 660 root system
    onrestart restart zygote
    writepid /dev/cpuset/foreground/tasks
```

要点:以 **root** 运行(要 fork 出 system_server 与任意 uid 的 app,降权在 fork 后做);`priority -20` 最高优先级;`--start-system-server` 只有主 zygote 有,次 zygote 用 `--enable-lazy-preload` 延迟预加载;`usap_pool_*` 是 Android 10 新增的 USAP(Unspecialized App Process)池 socket。

## 三、触发时刻:zygote-start

不在 `class_start main` 里,而是独立 trigger(为 FBE 解密让路):

```634:639:system/core/rootdir/init.rc
on zygote-start && property:ro.crypto.state=unencrypted
    # A/B update verifier that marks a successful boot.
    exec_start update_verifier_nonencrypted
    start netd
    start zygote
    start zygote_secondary
```

三种加密状态(`unencrypted` / `unsupported` / `encrypted && type=file`)各有一段(634-653),都会 `start zygote` + `start zygote_secondary`。该 trigger 由 `on post-fs-data` 末尾发出(346 行 `trigger zygote-start`),FDE 设备则由 `vold.decrypt=trigger_post_fs_data` 触发(779-781)。

## 四、init 侧:`start zygote` → fork/exec

`start` 内建命令很薄,`builtins.cpp:717-724` 直接查表调 `Service::Start()`。核心在 `service.cpp:904`:

```946:961:system/core/init/service.cpp
    struct stat sb;
    if (stat(args_[0].c_str(), &sb) == -1) {
        flags_ |= SVC_DISABLED;
        return ErrnoError() << "Cannot find '" << args_[0] << "'";
    }

    std::string scon;
    if (!seclabel_.empty()) {
        scon = seclabel_;
    } else {
        auto result = ComputeContextFromExecutable(args_[0]);
        ...
        scon = *result;
    }
```

rc 未写 `seclabel`,因此 zygote 的域由**可执行文件标签**推导(`u:object_r:zygote_exec:s0`),exec 后自然转换到 `u:r:zygote:s0`。随后 fork:

```975:980:system/core/init/service.cpp
    pid_t pid = -1;
    if (namespace_flags_) {
        pid = clone(nullptr, nullptr, namespace_flags_ | SIGCHLD, nullptr);
    } else {
        pid = fork();
    }
```

zygote 无 namespace 配置,走普通 `fork()`。子进程关键动作顺序(1014-1079):`setenv` 注入 rc 里 `setenv` 声明的变量 → **创建并发布 socket** → 写 `writepid`(`/dev/cpuset/foreground/tasks`)→ `ZapStdio()` → `SetProcessAttributes()`(uid/gid/补充组/capabilities/SELinux 上下文)→ `ExpandArgsAndExecv(args_, sigstop_)` → 失败则 `_exit(127)`。

父进程记录 pid、置 `SVC_RUNNING`、`createProcessGroup`,并通过 `Reap`/`onrestart` 接管后续。

## 五、socket 交接:init 与 zygote 的唯一通道

```52:64:system/core/init/descriptors.cpp
void DescriptorInfo::CreateAndPublish(const std::string& globalContext) const {
  // Create
  const std::string& contextStr = context_.empty() ? globalContext : context_;
  int fd = Create(contextStr);
  if (fd < 0) return;

  // Publish
  std::string publishedName = key() + name_;
  std::for_each(publishedName.begin(), publishedName.end(),
                [] (char& c) { c = isalnum(c) ? c : '_'; });

  std::string val = std::to_string(fd);
  setenv(publishedName.c_str(), val.c_str(), 1);
```

rc 中的 `socket zygote stream 660 root system` 让 init 在 `/dev/socket/zygote` 建好 socket,再把 fd 以环境变量 `ANDROID_SOCKET_zygote` 传给子进程(注意这里故意不设 `O_CLOEXEC`,否则 exec 后 fd 就没了)。zygote 侧:

```859:877:frameworks/base/core/java/com/android/internal/os/Zygote.java
    static LocalServerSocket createManagedSocketFromInitSocket(String socketName) {
        int fileDesc;
        final String fullSocketName = ANDROID_SOCKET_PREFIX + socketName;

        try {
            String env = System.getenv(fullSocketName);
            fileDesc = Integer.parseInt(env);
        } catch (RuntimeException ex) {
            throw new RuntimeException("Socket unset or invalid: " + fullSocketName, ex);
        }
        ...
        return new LocalServerSocket(fd);
```

这解释了为什么 zygote 不能由人手动启动——没有 `ANDROID_SOCKET_zygote` 就直接抛 `RuntimeException`。

## 六、native 侧:app_process → ART

`frameworks/base/cmds/app_process/app_main.cpp` 的 `main()`:解析到 `--zygote` 后置 `niceName = ZYGOTE_NICE_NAME`("zygote64"),若带 `--start-system-server` 则向 args 追加 `start-system-server`,并追加 ABI list;最后:

```338:341:frameworks/base/cmds/app_process/app_main.cpp
    if (zygote) {
        runtime.start("com.android.internal.os.ZygoteInit", args, zygote);
    } else if (className) {
        runtime.start("com.android.internal.os.RuntimeInit", args, zygote);
```

`AndroidRuntime::start` 会 `startVm`(创建 ART 虚拟机,rc 中的 `-Xzygote` 让 VM 以支持 fork 的方式初始化)、`startReg`(注册 JNI)、再反射调用 `ZygoteInit.main`。

## 七、Java 侧:预加载 → 建服务器 → 孵出 system_server

```899:920:frameworks/base/core/java/com/android/internal/os/ZygoteInit.java
            Zygote.initNativeState(isPrimaryZygote);

            ZygoteHooks.stopZygoteNoThreadCreation();

            zygoteServer = new ZygoteServer(isPrimaryZygote);

            if (startSystemServer) {
                Runnable r = forkSystemServer(abiList, zygoteSocketName, zygoteServer);

                // {@code r == null} in the parent (zygote) process, and {@code r != null} in the
                // child (system_server) process.
                if (r != null) {
                    r.run();
                    return;
                }
            }

            Log.i(TAG, "Accepting command socket connections");

            // The select loop returns early in the child process after a fork and
            // loops forever in the zygote.
            caller = zygoteServer.runSelectLoop(abiList);
```

`new ZygoteServer(isPrimaryZygote)`(ZygoteServer.java:148-161)正是去 `createManagedSocketFromInitSocket("zygote")` 取回 init 建的 fd(次 zygote 取 `zygote_secondary`,另各自取 `usap_pool_*`)。

`forkSystemServer` 内部经 `Zygote.forkSystemServer`(`ZygoteInit.java:782`)fork 出 system_server;父进程返回 `null` 继续往下走 `runSelectLoop()` 进入永久监听,子进程返回 `Runnable` 并 `r.run()` 进入 `SystemServer.main`。至此 init 的启动职责结束,后续 app 进程都由 AMS 通过 `/dev/socket/zygote` 发命令、由 zygote fork 出来。

## 八、死亡处理

zygote 不是 `oneshot`,退出后 init 的 SIGCHLD handler 回收时发现是已注册服务,会执行 rc 中的 `onrestart` 列表:主 zygote 死亡会 `restart audioserver/cameraserver/media/netd/wificond` 并亮屏唤醒锁(`write /sys/power/state on`),次 zygote 死亡则 `restart zygote`——即通过重启整个 Java 框架与网络服务来恢复,而不是单纯重启 zygote 自身。

一句话总结链路:**rc 声明服务 → `start zygote` 触发 fork → 子进程建 socket 并以 `ANDROID_SOCKET_zygote` 环境变量传递 fd → exec `app_process64` → ART 起 VM → `ZygoteInit` 预加载后取回该 socket 并 fork 出 system_server,自身进入 `runSelectLoop` 待命**。


---

继续取证命令解析的落点 `Action::AddCommand` 与关键字查表。
证据齐全。下面把 `start zygote` 这一行文本从**被 ActionParser 解析**到**执行**的全过程串起来。

## 一、`start zygote` 不是 service 段的内容

它在 rc 里的位置是:

```634:639:system/core/rootdir/init.rc
on zygote-start && property:ro.crypto.state=unencrypted
    # A/B update verifier that marks a successful boot.
    exec_start update_verifier_nonencrypted
    start netd
    start zygote
    start zygote_secondary
```

前两轮讲过:`service zygote ...` 由 `ServiceParser` 处理(进 ServiceList),而 `on zygote-start` 这一段由 **`ActionParser`** 处理。所以 `start zygote` 这条命令属于 **ActionParser 的产物**,它引用的是 ServiceParser 之前登记好的服务对象——两个 parser 在解析期互不通信,只在运行期通过 `ServiceList::FindService("zygote")` 汇合。

## 二、ActionParser 的解析三阶段

### 阶段 1:`ParseSection` —— 解析 trigger

```116:146:system/core/init/action_parser.cpp
Result<Success> ActionParser::ParseSection(std::vector<std::string>&& args,
                                           const std::string& filename, int line) {
    std::vector<std::string> triggers(args.begin() + 1, args.end());
    if (triggers.size() < 1) {
        return Error() << "Actions must have a trigger";
    }

    Subcontext* action_subcontext = nullptr;
    if (subcontexts_) {
        for (auto& subcontext : *subcontexts_) {
            if (StartsWith(filename, subcontext.path_prefix())) {
                action_subcontext = &subcontext;
                break;
            }
        }
    }
    ...
    auto action = std::make_unique<Action>(false, action_subcontext, filename, line, event_trigger,
                                           property_triggers);

    action_ = std::move(action);
    return Success();
```

`args[0]` 是 `"on"`,`args[1..]` 才是 trigger。对 `on zygote-start && property:ro.crypto.state=unencrypted`,`ParseTriggers`(80-112)会把它拆成:

- **event trigger**:`zygote-start`(非 `property:` 前缀的都当事件名,且只允许一个,重复报 `multiple event triggers are not allowed`);
- **property trigger**:`ro.crypto.state=unencrypted` → 存进 `property_triggers` map(`ParsePropertyTrigger`,58-78);
- `&&` 是唯一允许的连接符,奇数位必须是它(89-95)。

两者是**与**关系:后面 `CheckEvent` 时事件名和属性值要同时满足。

顺带注意 124-131:和 `ServiceParser` 一样,按 rc 文件路径前缀绑定 subcontext——/vendor、/odm 的 rc 里的 action 命令将交给 `vendor_init` 域执行。

### 阶段 2:每行走 `ParseLineSection` —— 命令入列

```148:151:system/core/init/action_parser.cpp
Result<Success> ActionParser::ParseLineSection(std::vector<std::string>&& args, int line) {
    // 添加启动命令
    return action_ ? action_->AddCommand(std::move(args), line) : Success();
}
```

对 `start zygote` 这行,`args = {"start", "zygote"}`。真正的"命令变成函数指针"发生在这里:

```86:97:system/core/init/action.cpp
Result<Success> Action::AddCommand(std::vector<std::string>&& args, int line) {
    if (!function_map_) {
        return Error() << "no function map available";
    }

    auto function = function_map_->FindFunction(args);
    if (!function) return Error() << function.error();

    //寻找启动函数
    commands_.emplace_back(function->second, function->first, std::move(args), line);
    return Success();
}
```

`FindFunction(args)`(keyword_map.h:39-72)拿 `args[0]` 即 `"start"` 去 `BuiltinFunctionMap::map()` 这张表里查,命中:

```1246:1246:system/core/init/builtins.cpp
        {"start",                   {1,     1,    {false,  do_start}}},
```

三元组含义是 `{最少参数, 最多参数, {是否在 subcontext 执行, 函数指针}}`:`start` 要求恰好 1 个参数,且 **false —— 不在 subcontext 执行**(即 `start` 这类控制类命令永远在 init 域跑,即便它写在 /vendor/etc/init 的 rc 里)。对比 `{"chmod", {2,2,{true, do_chmod}}}` 这类文件操作命令则会派给 subcontext。

查表后 `commands_.emplace_back(function->second, function->first, ...)`:`function->second` 是函数指针 `do_start`,`function->first` 是 `execute_in_subcontext` 布尔位,参数向量 `{"start", "zygote"}` 与行号一并存进 `Command` 对象(action.h:37-52)。

**注意:这里只做字符串 → 函数指针的绑定与参数个数校验,不校验服务是否存在。** 所以 `start` 一个不存在的服务,解析期不会报错,要到运行时 `do_start` 才返回 `service xxx not found`。

### 阶段 3:`EndSection` —— 提交给 ActionManager

```153:159:system/core/init/action_parser.cpp
Result<Success> ActionParser::EndSection() {
    if (action_ && action_->NumCommands() > 0) {
        action_manager_->AddAction(std::move(action_));
    }

    return Success();
}
```

rc 内部总共有三段，总的会添加三个action,每个4条命令 ，但是由于条件冲突，只会执行一条action
空 action(只有 trigger 没有命令)被丢弃。至此 `on zygote-start` 段成为一个 `Action` 对象进入 `ActionManager::actions_`,内含 4 条命令(`exec_start`、`start netd`、`start zygote`、`start zygote_secondary`)。


## 三、运行期:命令如何被执行

init 主循环里 `am.QueueEventTrigger("late-init")` 等把事件投入队,每轮调 `ExecuteOneCommand()`:

```56:66:system/core/init/action_manager.cpp
void ActionManager::ExecuteOneCommand() {
    // Loop through the event queue until we have an action to execute
    while (current_executing_actions_.empty() && !event_queue_.empty()) {
        for (const auto& action : actions_) {
            if (std::visit([&action](const auto& event) { return action->CheckEvent(event); },
                           event_queue_.front())) {
                current_executing_actions_.emplace(action.get());
            }
        }
        event_queue_.pop();
    }
```

`CheckEvent` 是重载的(action.h:69-71),分别匹配 `EventTrigger`/`PropertyChange`/`BuiltinAction` 三种 variant。`on zygote-start && property:...` 要求事件名匹配**且**属性当前值匹配(`CheckPropertyTriggers`)。

匹配后每轮只执行一条命令:

```120:122:system/core/init/action.cpp
void Action::ExecuteCommand(const Command& command) const {
    android::base::Timer t;
    auto result = command.InvokeFunc(subcontext_);
```

`Command::InvokeFunc` 按构造时记录的 `execute_in_subcontext_` 决定:为 false 就在本进程直接调用 `func_(args_)`;为 true 则把命令经 socket 发给 subcontext 子进程执行(这解释了为什么命令参数要在解析期就原样存下来)。

对 `start zygote` 而言,`func_` 就是 `do_start`,于是:

```717:724:system/core/init/builtins.cpp
static Result<Success> do_start(const BuiltinArguments& args) {
    Service* svc = ServiceList::GetInstance().FindService(args[1]);
    if (!svc) return Error() << "service " << args[1] << " not found";
    if (auto result = svc->Start(); !result) {
        return Error() << "Could not start service: " << result.error();
    }
    return Success();
}
```

`FindService("zygote")` 正是回到 ServiceParser 在解析期填好的那张 `ServiceList`,取到对象后 `Start()` → fork/exec(即上一轮讲的流程)。

## 四、完整闭环

```text
解析期
  ┌ ServiceParser: "service zygote /system/bin/app_process64 ..."
  │     → Service 对象 → ServiceList::AddService          【登记,未启动】
  └ ActionParser:  "on zygote-start && property:ro.crypto.state=unencrypted"
        → Action 对象(trigger = zygote-start + 属性条件)
        → AddCommand({"start","zygote"})
              → BuiltinFunctionMap 查表 → do_start + execute_in_subcontext=false
              → commands_ 存下函数指针与参数                 【登记,未执行】
        → ActionManager::AddAction

运行期
  trigger zygote-start(post-fs-data 末尾发出)
  → 属性 ro.crypto.state 已就绪,CheckEvent 通过
  → ExecuteOneCommand 逐条执行
      → command.InvokeFunc()(init 域直调,因子是 false)
          → do_start(args={"start","zygote"})
              → ServiceList::FindService("zygote")   ←── 两个 parser 在此汇合
              → Service::Start() → fork → socket/setenv → exec app_process64
```

一句话总结:**`ActionParser` 负责把 `on <trigger>` 段里的每一行命令,通过 `BuiltinFunctionMap` 关键字查表翻译成"函数指针 + 参数 + 是否派给 subcontext"的 `Command` 对象存起来;真正启动服务是在运行期 `ExecuteOneCommand` 逐条调用 `do_start` 时,由它去 `ServiceList` 里找 `ServiceParser` 早就登记好的 zygote 服务对象再 `Start()`。解析期两者完全解耦,运行期通过服务名汇合。**