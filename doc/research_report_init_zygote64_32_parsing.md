# 解析 `/init.zygote64_32.rc` 并生成 Service 对象 `zygote` / `zygote_secondary`

## Executive Summary

Android init 进程在 `LoadBootScripts()` 中解析 `/init.rc`，其中第 12 行 `import /init.${ro.zygote}.rc` 经属性展开后指向 `/init.zygote64_32.rc`。`ImportParser` 在文件结束时递归调用 `Parser::ParseConfig()` 解析该文件；文件中的两个 `service` 段被注册的 `ServiceParser` 接管，`ParseSection()` 为每个段 `new` 一个 `Service` 对象，随后各子行经 `Service::ParseLine()` + `OptionParserMap` 逐条填充属性，最终在 `EndSection()` 中通过 `ServiceList::AddService()` 提交进全局服务表。结果是 `ServiceList` 中新增两个对象：`zygote`（app_process64）与 `zygote_secondary`（app_process32）。

## 1. 起点：init 启动时加载 `/init.rc`

在 `system/core/init/init.cpp` 的 `LoadBootScripts()` 中构造 `Parser` 并注册三个段解析器：

```123:132:system/core/init/init.cpp
Parser CreateParser(ActionManager &action_manager, ServiceList &service_list) {
    Parser parser;
    parser.AddSectionParser("service", std::make_unique<ServiceParser>(&service_list, subcontexts));
    parser.AddSectionParser("on", std::make_unique<ActionParser>(&action_manager, subcontexts));
    parser.AddSectionParser("import", std::make_unique<ImportParser>(&parser));
    return parser;
}
```

非虚拟化环境下走默认分支，逐个解析 `/init.rc`、`/system/etc/init` 等（`init.cpp` 的 `LoadBootScripts()`，第 169–195 行）。

## 2. `/init.rc` 中的 import 展开为 `/init.zygote64_32.rc`

```7:12:system/core/rootdir/init.rc
import /init.environ.rc
import /init.usb.rc
import /init.${ro.hardware}.rc
import /vendor/etc/init/hw/init.${ro.hardware}.rc
import /init.usb.configfs.rc
import /init.${ro.zygote}.rc
```

`Parser::ParseData()` 用 `next_token()` 逐 token 切分。当 `args[0] == "import"` 命中已注册的段解析器时，调用 `ImportParser::ParseSection()`：

```26:42:system/core/init/import_parser.cpp
Result<Success> ImportParser::ParseSection(std::vector<std::string>&& args,
                                           const std::string& filename, int line) {
    if (args.size() != 2) {
        return Error() << "single argument needed for import\n";
    }
    std::string conf_file;
    bool ret = expand_props(args[1], &conf_file);
    if (!ret) {
        return Error() << "error while expanding import";
    }
    LOG(INFO) << "Added '" << conf_file << "' to import list";
    if (filename_.empty()) filename_ = filename;
    imports_.emplace_back(std::move(conf_file), line);
    return Success();
}
```

`expand_props()` 把 `${ro.zygote}` 替换为属性值。在同时支持 64/32 位的设备上 `ro.zygote=zygote64_32`，因此 `conf_file == "/init.zygote64_32.rc"`，被暂存进 `imports_`。

注意 import 是**延迟执行**的：真正解析发生在文件结束时。`Parser::ParseData()` 在读到 `T_EOF` 时对每个段解析器调用 `EndFile()`，于是触发 `ImportParser::EndFile()`：

```48:54:system/core/init/import_parser.cpp
void ImportParser::EndFile() {
    auto current_imports = std::move(imports_);
    imports_.clear();
    for (const auto& [import, line_num] : current_imports) {
        parser_->ParseConfig(import);
    }
}
```

即递归调用 `parser_->ParseConfig("/init.zygote64_32.rc")`，进入下一层解析。

## 3. `Parser::ParseData()` 的状态机：如何识别 `service` 段

`ParseData()` 是核心分发器，逐 token 处理，`T_NEWLINE` 时按 `args[0]` 分派：

```99:121:system/core/init/parser.cpp
} else if (section_parsers_.count(args[0])) {
    end_section();
    section_parser = section_parsers_[args[0]].get();
    section_start_line = state.line;
    if (auto result =
            section_parser->ParseSection(std::move(args), filename, state.line);
        !result) {
        parse_error_count_++;
        LOG(ERROR) << filename << ": " << state.line << ": " << result.error();
        section_parser = nullptr;
        bad_section_found = true;
    }
} else if (section_parser) {
    if (auto result = section_parser->ParseLineSection(std::move(args), state.line);
        !result) {
        parse_error_count_++;
        LOG(ERROR) << filename << ": " << state.line << ": " << result.error();
    }
} else if (!bad_section_found) {
    parse_error_count_++;
    LOG(ERROR) << filename << ": " << state.line
               << ": Invalid section keyword found";
}
```

```


```

关键逻辑：
- 首 token 命中 `section_parsers_`（`service`/`on`/`import`）→ 先 `end_section()` 收尾上一个段，再 `ParseSection()` 开启新段。
- 否则若当前有 `section_parser` → 走 `ParseLineSection()` 作为段的子行。
- 二者都不满足 → 报 "Invalid section keyword found"。

## 4. 第一个 Service 对象：`zygote`

### 4.1 段首行 → `ServiceParser::ParseSection()`

`/init.zygote64_32.rc` 首行：

```1:1:system/core/rootdir/init.zygote64_32.rc
service zygote /system/bin/app_process64 -Xzygote /system/bin --zygote --start-system-server --socket-name=zygote
```

token 化后 `args = {"service","zygote","/system/bin/app_process64","-Xzygote","/system/bin","--zygote","--start-system-server","--socket-name=zygote"}`，命中 `ServiceParser::ParseSection()`：

```
    // 添加各章节解析器到解析器中
    parser.AddSectionParser("service", std::make_unique<ServiceParser>(&service_list, subcontexts));
    parser.AddSectionParser("on", std::make_unique<ActionParser>(&action_manager, subcontexts));
    parser.AddSectionParser("import", std::make_unique<ImportParser>(&parser));        

```

```1419:1452:system/core/init/service.cpp
Result<Success> ServiceParser::ParseSection(std::vector<std::string>&& args,
                                            const std::string& filename, int line) {
    if (args.size() < 3) {
        return Error() << "services must have a name and a program";
    }

    const std::string& name = args[1];
    if (!IsValidName(name)) {
        return Error() << "invalid service name '" << name << "'";
    }

    filename_ = filename;

    Subcontext* restart_action_subcontext = nullptr;
    if (subcontexts_) {
        for (auto& subcontext : *subcontexts_) {
            if (StartsWith(filename, subcontext.path_prefix())) {
                restart_action_subcontext = &subcontext;
                break;
            }
        }
    }

    std::vector<std::string> str_args(args.begin() + 2, args.end());

    if (SelinuxGetVendorAndroidVersion() <= __ANDROID_API_P__) {
        if (str_args[0] == "/sbin/watchdogd") {
            str_args[0] = "/system/bin/watchdogd";
        }
    }

    service_ = std::make_unique<Service>(name, restart_action_subcontext, str_args);
    return Success();
}
```

要点：
- 名称合法性由 `IsValidName()` 校验，实质是判断 `init.svc.<name>` 是否为合法属性名且长度不超过 `PROP_VALUE_MAX`（因为启停服务靠写 `ctl.start/ctl.stop` 属性）。
- `str_args` = 从 `args[2]` 起的完整命令行，即 `{"/system/bin/app_process64","-Xzygote","/system/bin","--zygote","--start-system-server","--socket-name=zygote"}`。
- `std::make_unique<Service>(name, subcontext, str_args)` 调用 3 参构造，转发到 9 参构造：

```235:260:system/core/init/service.cpp
Service::Service(const std::string& name, Subcontext* subcontext_for_restart_commands,
                 const std::vector<std::string>& args)
    : Service(name, 0, 0, 0, {}, 0, "", subcontext_for_restart_commands, args) {}

Service::Service(const std::string& name, unsigned flags, uid_t uid, gid_t gid,
                 const std::vector<gid_t>& supp_gids, unsigned namespace_flags,
                 const std::string& seclabel, Subcontext* subcontext_for_restart_commands,
                 const std::vector<std::string>& args)
    : name_(name),
      classnames_({"default"}),
      flags_(flags),
      pid_(0),
      crash_count_(0),
      uid_(uid),
      gid_(gid),
      supp_gids_(supp_gids),
      namespace_flags_(namespace_flags),
      seclabel_(seclabel),
      onrestart_(false, subcontext_for_restart_commands, "<Service '" + name + "' onrestart>", 0,
                 "onrestart", {}),
      ioprio_class_(IoSchedClass_NONE),
      ioprio_pri_(0),
      priority_(0),
      oom_score_adjust_(-1000),
      start_order_(0),
      args_(args) {}
```

此时 `service_` 已是一个完整的 `Service`，`name_="zygote"`、`classnames_={"default"}`、`uid_=0`、`gid_=0`、`priority_=0`、`oom_score_adjust_=-1000`、`args_` 为命令行。

### 4.2 子行 → `ServiceParser::ParseLineSection()` → `Service::ParseLine()`

```1454:1456:system/core/init/service.cpp
Result<Success> ServiceParser::ParseLineSection(std::vector<std::string>&& args, int line) {
    return service_ ? service_->ParseLine(std::move(args)) : Success();
}
```

`Service::ParseLine()` 用 `OptionParserMap`（继承自 `KeywordMap`）按首关键字查表并校验参数个数，再 `std::invoke` 到对应成员函数：

```871:878:system/core/init/service.cpp
Result<Success> Service::ParseLine(std::vector<std::string>&& args) {
    static const OptionParserMap parser_map;
    auto parser = parser_map.FindFunction(args);

    if (!parser) return parser.error();

    return std::invoke(*parser, this, std::move(args));
}
```

关键字表（`{最小参数, 最大参数, 成员函数指针}`）：

```822:866:system/core/init/service.cpp
static const Map option_parsers = {
    {"capabilities", {0, kMax, &Service::ParseCapabilities}},
    {"class",       {1,     kMax, &Service::ParseClass}},
    {"console",     {0,     1,    &Service::ParseConsole}},
    {"critical",    {0,     0,    &Service::ParseCritical}},
    {"disabled",    {0,     0,    &Service::ParseDisabled}},
    ...
    {"group",       {1,     NR_SVC_SUPP_GIDS + 1, &Service::ParseGroup}},
    ...
    {"onrestart",   {1,     kMax, &Service::ParseOnrestart}},
    ...
    {"priority",    {1,     1,    &Service::ParsePriority}},
    ...
    {"socket",      {3,     6,    &Service::ParseSocket}},
    ...
    {"user",        {1,     1,    &Service::ParseUser}},
    {"writepid",    {1,     kMax, &Service::ParseWritepid}},
};
```

针对 `zygote` 段的每一行，映射如下：

| rc 子行 | 关键字 | 处理函数 | 对 Service 的写入 |
|---|---|---|---|
| `class main` | `class` | `ParseClass` | `classnames_ = {"main"}`（覆盖默认 `{"default"}`） |
| `priority -20` | `priority` | `ParsePriority` | `priority_ = -20` |
| `user root` | `user` | `ParseUser` | `uid_ = DecodeUid("root") = 0` |
| `group root readproc reserved_disk` | `group` | `ParseGroup` | `gid_ = 0`，`supp_gids_ = {readproc, reserved_disk}` |
| `socket zygote stream 660 root system` | `socket` | `ParseSocket` → `AddDescriptor<SocketInfo>` | 追加 `SocketInfo` 到 `descriptors_` |
| `socket usap_pool_primary stream 660 root system` | `socket` | 同上 | 追加第二个 `SocketInfo` |
| `onrestart write /sys/...` 等 | `onrestart` | `ParseOnrestart` | 向 `onrestart_` Action 追加命令 |
| `writepid /dev/cpuset/foreground/tasks` | `writepid` | `ParseWritepid` | `writepid_files_ = {...}` |

`ParseClass` 直接整体替换 class 集合；`ParseGroup` 第一个参数是主 gid，其余是补充 gid：

```463:514:system/core/init/service.cpp
Result<Success> Service::ParseClass(std::vector<std::string>&& args) {
    classnames_ = std::set<std::string>(args.begin() + 1, args.end());
    return Success();
}
...
Result<Success> Service::ParseGroup(std::vector<std::string>&& args) {
    auto gid = DecodeUid(args[1]);
    if (!gid) {
        return Error() << "Unable to decode GID for '" << args[1] << "': " << gid.error();
    }
    gid_ = *gid;

    for (std::size_t n = 2; n < args.size(); n++) {
        gid = DecodeUid(args[n]);
        ...
        supp_gids_.emplace_back(*gid);
    }
    return Success();
}
```

`socket` 行先校验类型（`stream`/`dgram`/`seqpacket`），再走 `AddDescriptor<SocketInfo>()`：`perm` 按八进制解析（`660` → `0660`），`uid`/`gid` 解析为 `root=0` / `system=1000`，构造 `SocketInfo(name,type,uid,gid,perm,context)` 追加到 `descriptors_`。

```766:773:system/core/init/service.cpp
// name type perm [ uid gid context ]
Result<Success> Service::ParseSocket(std::vector<std::string>&& args) {
    if (!StartsWith(args[2], "dgram") && !StartsWith(args[2], "stream") &&
        !StartsWith(args[2], "seqpacket")) {
        return Error() << "socket type must be 'dgram', 'stream' or 'seqpacket'";
    }
    return AddDescriptor<SocketInfo>(std::move(args));
}
```

注意这些 `socket` 只是**登记描述符**，真正创建/发布 socket（`ANDROID_SOCKET_zygote` 环境变量）发生在服务启动时 `Service::Start()` 的 `DescriptorInfo::CreateAndPublish()`（`service.cpp:1018-1019`，`descriptors.cpp:52-68`、`SocketInfo::Create` 位于 `descriptors.cpp:83-89`）。

### 4.3 段结束 → `EndSection()` 提交对象

遇到下一段（或文件结束）时 `end_section()` 调用 `ServiceParser::EndSection()`：

```1458:1480:system/core/init/service.cpp
Result<Success> ServiceParser::EndSection() {
    if (service_) {
        Service* old_service = service_list_->FindService(service_->name());
        if (old_service) {
            if (!service_->is_override()) {
                return Error() << "ignored duplicate definition of service '" << service_->name()
                               << "'";
            }
            ...
            service_list_->RemoveService(*old_service);
            old_service = nullptr;
        }
        // 添加到service list
        service_list_->AddService(std::move(service_));
    }
    return Success();
}
```

`AddService()` 把 `unique_ptr<Service>` 追加进 `ServiceList::services_`：

```1288:1290:system/core/init/service.cpp
void ServiceList::AddService(std::unique_ptr<Service> service) {
    services_.emplace_back(std::move(service));
}
```

至此 `zygote` 对象完成注册。

## 5. 第二个 Service 对象：`zygote_secondary`

`/init.zygote64_32.rc` 第 17 行开启第二段：

```17:25:system/core/rootdir/init.zygote64_32.rc
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

流程与 `zygote` 完全一致：`ParseSection()` 创建 `Service("zygote_secondary", ..., {"/system/bin/app_process32","-Xzygote","/system/bin","--zygote","--socket-name=zygote_secondary","--enable-lazy-preload"})`，子行分别写入 `classnames_={"main"}`、`priority_=-20`、`uid_=0`、`gid_=0`、`supp_gids_={readproc,reserved_disk}`、两个 socket 描述符（`zygote_secondary`、`usap_pool_secondary`）、`onrestart_` 中一条 `restart zygote` 命令、`writepid_files_`。文件 `T_EOF` 时 `end_section()` → `EndSection()` → `AddService()` 提交。

注意 `onrestart restart zygote` 的语义：`ParseOnrestart` 只把命令加进 `onrestart_` 这个内嵌 Action，实际执行在 `Service::Restart()` 之后（重启 `zygote_secondary` 时联动重启 `zygote`）。

## 6. 完整调用链与对象结果

调用链（自上而下）：

```
init.cpp: LoadBootScripts()
  └─ Parser::ParseConfig("/init.rc")
       └─ Parser::ParseConfigFile → Parser::ParseData
            ├─ "import /init.${ro.zygote}.rc" → ImportParser::ParseSection → expand_props → imports_
            └─ (T_EOF) ImportParser::EndFile()
                 └─ Parser::ParseConfig("/init.zygote64_32.rc")
                      └─ Parser::ParseData
                           ├─ "service zygote ..."            → ServiceParser::ParseSection   → new Service("zygote")
                           ├─ "class/priority/user/group/..." → ServiceParser::ParseLineSection → Service::ParseLine → OptionParserMap → ParseXxx
                           ├─ "service zygote_secondary ..."  → end_section() → ServiceParser::EndSection() → AddService(zygote)
                           │                                   → ServiceParser::ParseSection → new Service("zygote_secondary")
                           ├─ (子行...)                        → Service::ParseLine
                           └─ (T_EOF) end_section() → ServiceParser::EndSection() → AddService(zygote_secondary)
```

最终 `ServiceList::services_` 新增两个 `Service`：

| 字段 | zygote | zygote_secondary |
|---|---|---|
| `name_` | `zygote` | `zygote_secondary` |
| `args_` | `/system/bin/app_process64 -Xzygote /system/bin --zygote --start-system-server --socket-name=zygote` | `/system/bin/app_process32 -Xzygote /system/bin --zygote --socket-name=zygote_secondary --enable-lazy-preload` |
| `classnames_` | `{main}` | `{main}` |
| `priority_` | `-20` | `-20` |
| `uid_` / `gid_` | `0` / `0` | `0` / `0` |
| `supp_gids_` | `{readproc, reserved_disk}` | `{readproc, reserved_disk}` |
| `descriptors_` | `SocketInfo{zygote,stream,0660,0,1000}`, `SocketInfo{usap_pool_primary,...}` | `SocketInfo{zygote_secondary,...}`, `SocketInfo{usap_pool_secondary,...}` |
| `onrestart_` | `write /sys/android_power/request_state wake`、`write /sys/power/state on`、`restart audioserver/cameraserver/media/netd/wificond` | `restart zygote` |
| `writepid_files_` | `/dev/cpuset/foreground/tasks` | `/dev/cpuset/foreground/tasks` |

## 7. 关键设计要点

1. **段式解析（Section-based parsing）**：`Parser` 只负责按行分派，具体语义由 `SectionParser` 子类（`ServiceParser`/`ActionParser`/`ImportParser`）实现，`ParseSection`/`ParseLineSection`/`EndSection`/`EndFile` 四个生命周期回调构成完整协议（见 `parser.h:27-46`）。
2. **延迟构造、结束提交**：`Service` 在 `ParseSection` 时构造，但直到 `EndSection`（下一段或文件末尾）才 `AddService`，期间子行持续填充同一对象。
3. **关键字驱动**：`KeywordMap::FindFunction()` 统一做关键字查找与参数个数校验，`Service` 只需实现 `ParseXxx` 成员函数并登记到 `OptionParserMap`。
4. **`service_` 为临时态**：`ServiceParser` 用 `std::unique_ptr<Service> service_` 持有“正在解析的段”，`EndSection` 后 `std::move` 转移所有权，`service_` 置空，为下一段做准备。
5. **同名覆盖机制**：若 `FindService()` 命中同名服务，只有带 `override` 的段才允许替换（`is_override()`），否则报 “ignored duplicate definition” 并丢弃新定义。

## References

1. [system/core/init/init.cpp](file:///c:/D/android_project/cells-android10/system/core/init/init.cpp)
2. [system/core/init/parser.cpp](file:///c:/D/android_project/cells-android10/system/core/init/parser.cpp)
3. [system/core/init/parser.h](file:///c:/D/android_project/cells-android10/system/core/init/parser.h)
4. [system/core/init/import_parser.cpp](file:///c:/D/android_project/cells-android10/system/core/init/import_parser.cpp)
5. [system/core/init/service.cpp](file:///c:/D/android_project/cells-android10/system/core/init/service.cpp)
6. [system/core/init/service.h](file:///c:/D/android_project/cells-android10/system/core/init/service.h)
7. [system/core/init/keyword_map.h](file:///c:/D/android_project/cells-android10/system/core/init/keyword_map.h)
8. [system/core/init/descriptors.cpp](file:///c:/D/android_project/cells-android10/system/core/init/descriptors.cpp)
9. [system/core/init/tokenizer.cpp](file:///c:/D/android_project/cells-android10/system/core/init/tokenizer.cpp)
10. [system/core/rootdir/init.rc](file:///c:/D/android_project/cells-android10/system/core/rootdir/init.rc)
11. [system/core/rootdir/init.zygote64_32.rc](file:///c:/D/android_project/cells-android10/system/core/rootdir/init.zygote64_32.rc)
