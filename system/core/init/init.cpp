/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "init.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <seccomp_policy.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <optional>

#include <android-base/chrono_utils.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <fs_avb/fs_avb.h>
#include <fs_mgr_vendor_overlay.h>
#include <keyutils.h>
#include <libavb/libavb.h>
#include <libgsi/libgsi.h>
#include <processgroup/processgroup.h>
#include <processgroup/setup.h>
#include <selinux/android.h>

#ifndef RECOVERY

#include <binder/ProcessState.h>

#endif

#include "action_parser.h"
#include "boringssl_self_test.h"
#include "epoll.h"
#include "first_stage_mount.h"
#include "import_parser.h"
#include "keychords.h"
#include "mount_handler.h"
#include "mount_namespace.h"
#include "property_service.h"
#include "reboot.h"
#include "reboot_utils.h"
#include "security.h"
#include "selinux.h"
#include "sigchld_handler.h"
#include "util.h"

using namespace std::chrono_literals;
using namespace std::string_literals;

using android::base::boot_clock;
using android::base::GetProperty;
using android::base::ReadFileToString;
using android::base::StringPrintf;
using android::base::Timer;
using android::base::Trim;
using android::fs_mgr::AvbHandle;

namespace android {
    namespace init {

        static int property_triggers_enabled = 0;

        static char qemu[32];

        std::string default_console = "/dev/console";

        static int signal_fd = -1;

        static std::unique_ptr <Timer> waiting_for_prop(nullptr);
        static std::string wait_prop_name;
        static std::string wait_prop_value;
        static bool shutting_down;
        static std::string shutdown_command;
        static bool do_shutdown = false;
        static bool load_debug_prop = false;

        std::vector <std::string> late_import_paths;

        static std::vector <Subcontext> *subcontexts;

        void DumpState() {
            ServiceList::GetInstance().DumpState();
            ActionManager::GetInstance().DumpState();
        }

/**
 * 创建并配置一个解析器对象
 *
 * 该函数初始化一个Parser对象，并为其添加三个不同类型的章节解析器：
 * - service解析器：用于处理服务相关的配置
 * - on解析器：用于处理动作相关的配置
 * - import解析器：用于处理导入相关的配置
 *
 * @param action_manager 动作管理器引用，用于处理on章节的动作配置
 * @param service_list 服务列表引用，用于存储service章节的服务配置
 * @return 配置完成的Parser对象
 */
        Parser CreateParser(ActionManager &action_manager, ServiceList &service_list) {
            Parser parser;

            // 添加各章节解析器到解析器中
            parser.AddSectionParser("service", std::make_unique<ServiceParser>(&service_list, subcontexts));
            parser.AddSectionParser("on", std::make_unique<ActionParser>(&action_manager, subcontexts));
            parser.AddSectionParser("import", std::make_unique<ImportParser>(&parser));

            return parser;
        }


// parser that only accepts new services
        Parser CreateServiceOnlyParser(ServiceList &service_list) {
            Parser parser;

            parser.AddSectionParser("service", std::make_unique<ServiceParser>(&service_list, subcontexts));
            return parser;
        }


        /**
         * @brief 加载启动脚本配置文件
         *
         * 该函数根据系统环境加载不同的初始化配置文件。如果存在 /.cell 文件，
         * 则加载虚拟化相关的配置；否则根据 ro.boot.init_rc 属性决定加载默认
         * 配置或自定义配置文件。
         *
         * @param action_manager 动作管理器引用，用于处理配置中的动作定义
         * @param service_list 服务列表引用，用于注册配置中的服务定义
         */
        static void LoadBootScripts(ActionManager &action_manager, ServiceList &service_list) {
            Parser parser = CreateParser(action_manager, service_list);

            // 检查是否为虚拟化环境(cell模式)
            if (access("/.cell", F_OK) == 0) {
                LOG(INFO) << "LOAD VP RC...";
                parser.ParseConfig("/init.rc");
                // 尝试解析系统cell配置，失败则加入延迟加载队列
                if (!parser.ParseConfig("/cells/system")) {
                    late_import_paths.emplace_back("/cells/system");
                }
                // 尝试解析厂商cell配置，失败则加入延迟加载队列
                if (!parser.ParseConfig("/cells/vendor")) {
                    late_import_paths.emplace_back("/cells/vendor");
                }
            } else {
                // 非虚拟化环境，根据属性加载配置
                std::string bootscript = GetProperty("ro.boot.init_rc", "");
                if (bootscript.empty()) {
                    // 默认配置加载流程
                    parser.ParseConfig("/init.rc");
                    // 按优先级顺序尝试加载各分区的初始化配置
                    if (!parser.ParseConfig("/system/etc/init")) {
                        late_import_paths.emplace_back("/system/etc/init");
                    }
                    if (!parser.ParseConfig("/product/etc/init")) {
                        late_import_paths.emplace_back("/product/etc/init");
                    }
                    if (!parser.ParseConfig("/product_services/etc/init")) {
                        late_import_paths.emplace_back("/product_services/etc/init");
                    }
                    if (!parser.ParseConfig("/odm/etc/init")) {
                        late_import_paths.emplace_back("/odm/etc/init");
                    }
                    if (!parser.ParseConfig("/vendor/etc/init")) {
                        late_import_paths.emplace_back("/vendor/etc/init");
                    }
                } else {
                    // 加载自定义启动脚本
                    parser.ParseConfig(bootscript);
                }
            }
        }

        bool start_waiting_for_property(const char *name, const char *value) {
            if (waiting_for_prop) {
                return false;
            }
            if (GetProperty(name, "") != value) {
                // Current property value is not equal to expected value
                wait_prop_name = name;
                wait_prop_value = value;
                waiting_for_prop.reset(new Timer());
            } else {
                LOG(INFO) << "start_waiting_for_property(\""
                          << name << "\", \"" << value << "\"): already set";
            }
            return true;
        }

        void ResetWaitForProp() {
            wait_prop_name.clear();
            wait_prop_value.clear();
            waiting_for_prop.reset();
        }

        void property_changed(const std::string &name, const std::string &value) {
            // If the property is sys.powerctl, we bypass the event queue and immediately handle it.
            // This is to ensure that init will always and immediately shutdown/reboot, regardless of
            // if there are other pending events to process or if init is waiting on an exec service or
            // waiting on a property.
            // In non-thermal-shutdown case, 'shutdown' trigger will be fired to let device specific
            // commands to be executed.
            if (access("/.cell", F_OK) != 0) {
                if (name == "sys.powerctl") {
                    // Despite the above comment, we can't call HandlePowerctlMessage() in this function,
                    // because it modifies the contents of the action queue, which can cause the action queue
                    // to get into a bad state if this function is called from a command being executed by the
                    // action queue.  Instead we set this flag and ensure that shutdown happens before the next
                    // command is run in the main init loop.
                    // TODO: once property service is removed from init, this will never happen from a builtin,
                    // but rather from a callback from the property service socket, in which case this hack can
                    // go away.
                    shutdown_command = value;
                    do_shutdown = true;
                }
            }

            if (property_triggers_enabled) ActionManager::GetInstance().QueuePropertyChange(name, value);

            if (waiting_for_prop) {
                if (wait_prop_name == name && wait_prop_value == value) {
                    LOG(INFO) << "Wait for property '" << wait_prop_name << "=" << wait_prop_value
                              << "' took " << *waiting_for_prop;
                    ResetWaitForProp();
                }
            }
        }

        static std::optional <boot_clock::time_point> HandleProcessActions() {
            std::optional <boot_clock::time_point> next_process_action_time;
            for (const auto &s: ServiceList::GetInstance()) {
                if ((s->flags() & SVC_RUNNING) && s->timeout_period()) {
                    auto timeout_time = s->time_started() + *s->timeout_period();
                    if (boot_clock::now() > timeout_time) {
                        s->Timeout();
                    } else {
                        if (!next_process_action_time || timeout_time < *next_process_action_time) {
                            next_process_action_time = timeout_time;
                        }
                    }
                }

                if (!(s->flags() & SVC_RESTARTING)) continue;

                auto restart_time = s->time_started() + s->restart_period();
                if (boot_clock::now() > restart_time) {
                    if (auto result = s->Start(); !result) {
                        LOG(ERROR) << "Could not restart process '" << s->name() << "': " << result.error();
                    }
                } else {
                    if (!next_process_action_time || restart_time < *next_process_action_time) {
                        next_process_action_time = restart_time;
                    }
                }
            }
            return next_process_action_time;
        }

        static Result<Success> DoControlStart(Service *service) {
            return service->Start();
        }

        static Result<Success> DoControlStop(Service *service) {
            service->Stop();
            return Success();
        }

        static Result<Success> DoControlRestart(Service *service) {
            service->Restart();
            return Success();
        }

        enum class ControlTarget {
            SERVICE,    // function gets called for the named service
            INTERFACE,  // action gets called for every service that holds this interface
        };

        struct ControlMessageFunction {
            ControlTarget target;
            std::function<Result<Success>(Service *)> action;
        };


/**
 * 获取控制消息处理函数映射表
 *
 * 该函数返回一个静态的映射表，将控制消息名称映射到相应的处理函数。
 * 映射表包含了服务和接口的各种控制操作，如启动、停止、重启以及信号控制等。
 *
 * @return 返回一个std::map引用，键为控制消息名称字符串，值为ControlMessageFunction结构体，
 *         其中包含目标类型和对应的处理函数
 */
        static const std::map <std::string, ControlMessageFunction> &get_control_message_map() {
            // clang-format off
            static const std::map <std::string, ControlMessageFunction> control_message_functions = {
                    // 服务信号控制相关命令
                    {"sigstop_on",        {ControlTarget::SERVICE,
                                                                     [](auto *service) {
                                                                         service->set_sigstop(true);
                                                                         return Success();
                                                                     }}},
                    {"sigstop_off",       {ControlTarget::SERVICE,
                                                                     [](auto *service) {
                                                                         service->set_sigstop(false);
                                                                         return Success();
                                                                     }}},
                    // 服务控制相关命令
                    {"start",             {ControlTarget::SERVICE,   DoControlStart}},
                    {"stop",              {ControlTarget::SERVICE,   DoControlStop}},
                    {"restart",           {ControlTarget::SERVICE,   DoControlRestart}},
                    // 接口控制相关命令
                    {"interface_start",   {ControlTarget::INTERFACE, DoControlStart}},
                    {"interface_stop",    {ControlTarget::INTERFACE, DoControlStop}},
                    {"interface_restart", {ControlTarget::INTERFACE, DoControlRestart}},
            };
            // clang-format on

            return control_message_functions;
        }


/**
 * @brief 处理控制消息的函数
 *
 * 根据传入的消息名称查找对应的处理函数，并执行该函数来控制指定的服务或接口。
 * 同时记录日志信息，包括发送控制消息的进程信息。
 *
 * @param msg 控制消息的名称（如 "start"、"stop" 等）
 * @param name 被控制的服务或接口的名称
 * @param pid 发送控制消息的进程 ID
 */
        void HandleControlMessage(const std::string &msg, const std::string &name, pid_t pid) {
            // 获取控制消息映射表并查找对应的消息处理函数
            const auto &map = get_control_message_map();
            const auto it = map.find(msg);

            // 如果未找到对应的消息处理函数，则输出错误日志并返回
            if (it == map.end()) {
                LOG(ERROR) << "Unknown control msg '" << msg << "'";
                return;
            }

            // 构造 cmdline 文件路径以获取发送消息的进程命令行信息
            std::string cmdline_path = StringPrintf("proc/%d/cmdline", pid);
            std::string process_cmdline;

            // 尝试读取进程的命令行信息，并将其中的空字符替换为空格后去除首尾空白
            if (ReadFileToString(cmdline_path, &process_cmdline)) {
                std::replace(process_cmdline.begin(), process_cmdline.end(), '\0', ' ');
                process_cmdline = Trim(process_cmdline);
            } else {
                process_cmdline = "unknown process";
            }

            // 记录接收到控制消息的日志信息
            LOG(INFO) << "Received control message '" << msg << "' for '" << name << "' from pid: " << pid
                      << " (" << process_cmdline << ")";

            // 获取查找到的控制消息对应的函数对象
            const ControlMessageFunction &function = it->second;

            Service *svc = nullptr;

            // 根据目标类型查找对应的服务或接口实例
            switch (function.target) {
                case ControlTarget::SERVICE:
                    svc = ServiceList::GetInstance().FindService(name);
                    break;
                case ControlTarget::INTERFACE:
                    svc = ServiceList::GetInstance().FindInterface(name);
                    break;
                default:
                    // 若目标类型无效，则输出错误日志并返回
                    LOG(ERROR) << "Invalid function target from static map key '" << msg << "': "
                               << static_cast<std::underlying_type<ControlTarget>::type>(function.target);
                    return;
            }

            // 如果没有找到对应的服务或接口，则输出错误日志并返回
            if (svc == nullptr) {
                LOG(ERROR) << "Could not find '" << name << "' for ctl." << msg;
                return;
            }

            // 执行控制操作，如果失败则输出错误日志
            if (auto result = function.action(svc); !result) {
                LOG(ERROR) << "Could not ctl." << msg << " for '" << name << "': " << result.error();
            }
        }


        static Result<Success> wait_for_coldboot_done_action(const BuiltinArguments &args) {
            Timer t;

            LOG(VERBOSE) << "Waiting for " COLDBOOT_DONE "...";

            // Historically we had a 1s timeout here because we weren't otherwise
            // tracking boot time, and many OEMs made their sepolicy regular
            // expressions too expensive (http://b/19899875).

            // Now we're tracking boot time, just log the time taken to a system
            // property. We still panic if it takes more than a minute though,
            // because any build that slow isn't likely to boot at all, and we'd
            // rather any test lab devices fail back to the bootloader.
            if (wait_for_file(COLDBOOT_DONE, 60s) < 0) {
                LOG(FATAL) << "Timed out waiting for " COLDBOOT_DONE;
            }

            property_set("ro.boottime.init.cold_boot_wait", std::to_string(t.duration().count()));
            return Success();
        }

        static Result<Success> console_init_action(const BuiltinArguments &args) {
            std::string console = GetProperty("ro.boot.console", "");
            if (!console.empty()) {
                default_console = "/dev/" + console;
            }
            return Success();
        }

        static Result<Success> SetupCgroupsAction(const BuiltinArguments &) {
            // Have to create <CGROUPS_RC_DIR> using make_dir function
            // for appropriate sepolicy to be set for it
            make_dir(android::base::Dirname(CGROUPS_RC_PATH), 0711);
            if (!CgroupSetup()) {
                return ErrnoError() << "Failed to setup cgroups";
            }

            return Success();
        }

        static void import_kernel_nv(const std::string &key, const std::string &value, bool for_emulator) {
            if (key.empty()) return;

            if (for_emulator) {
                // In the emulator, export any kernel option with the "ro.kernel." prefix.
                property_set("ro.kernel." + key, value);
                return;
            }

            if (key == "qemu") {
                strlcpy(qemu, value.c_str(), sizeof(qemu));
            } else if (android::base::StartsWith(key, "androidboot.")) {
                property_set("ro.boot." + key.substr(12), value);
            }
        }

        static void export_oem_lock_status() {
            if (!android::base::GetBoolProperty("ro.oem_unlock_supported", false)) {
                return;
            }
            import_kernel_cmdline(
                    false, [](const std::string &key, const std::string &value, bool in_qemu) {
                        if (key == "androidboot.verifiedbootstate") {
                            property_set("ro.boot.flash.locked", value == "orange" ? "0" : "1");
                        }
                    });
        }


/**
 * 导出内核启动属性到系统属性中
 *
 * 该函数将从内核启动时获取的只读属性映射并复制到对应的系统属性中，
 * 使得上层应用可以通过标准属性接口访问这些启动信息。
 *
 * 函数内部定义了一个属性映射表，每个条目包含：
 * - 源属性名：从内核启动参数中读取的属性名（ro.boot.*）
 * - 目标属性名：要设置的系统属性名（ro.*）
 * - 默认值：当源属性不存在时使用的默认值
 *
 * 对于每个映射条目，函数会尝试获取源属性的值，如果获取成功且不为空，
 * 则将该值设置到对应的目标属性中。
 */
        static void export_kernel_boot_props() {
            constexpr const char *UNSET = "";

            /* 定义属性映射表，将内核启动属性映射到系统属性 */
            struct {
                const char *src_prop;
                const char *dst_prop;
                const char *default_value;
            } prop_map[] = {
                    {"ro.boot.serialno",   "ro.serialno",   UNSET,},
                    {"ro.boot.mode",       "ro.bootmode",   "unknown",},
                    {"ro.boot.baseband",   "ro.baseband",   "unknown",},
                    {"ro.boot.bootloader", "ro.bootloader", "unknown",},
                    {"ro.boot.hardware",   "ro.hardware",   "unknown",},
                    {"ro.boot.revision",   "ro.revision",   "0",},
            };

            /* 遍历属性映射表，将每个源属性的值复制到目标属性 */
            for (const auto &prop: prop_map) {
                std::string value = GetProperty(prop.src_prop, prop.default_value);
                if (value != UNSET)
                    property_set(prop.dst_prop, value);
            }
        }


        static void process_kernel_dt() {
            if (!is_android_dt_value_expected("compatible", "android,firmware")) {
                return;
            }

            std::unique_ptr<DIR, int (*)(DIR *)> dir(opendir(get_android_dt_dir().c_str()), closedir);
            if (!dir) return;

            std::string dt_file;
            struct dirent *dp;
            while ((dp = readdir(dir.get())) != NULL) {
                if (dp->d_type != DT_REG || !strcmp(dp->d_name, "compatible") || !strcmp(dp->d_name, "name")) {
                    continue;
                }

                std::string file_name = get_android_dt_dir() + dp->d_name;

                android::base::ReadFileToString(file_name, &dt_file);
                std::replace(dt_file.begin(), dt_file.end(), ',', '.');

                property_set("ro.boot."s + dp->d_name, dt_file);
            }
        }

        static void process_kernel_cmdline() {
            // The first pass does the common stuff, and finds if we are in qemu.
            // The second pass is only necessary for qemu to export all kernel params
            // as properties.
            import_kernel_cmdline(false, import_kernel_nv);
            if (qemu[0]) import_kernel_cmdline(true, import_kernel_nv);
        }

        static Result<Success> property_enable_triggers_action(const BuiltinArguments &args) {
            /* Enable property triggers. */
            property_triggers_enabled = 1;
            return Success();
        }

        static Result<Success> queue_property_triggers_action(const BuiltinArguments &args) {
            ActionManager::GetInstance().QueueBuiltinAction(property_enable_triggers_action, "enable_property_trigger");
            ActionManager::GetInstance().QueueAllPropertyActions();
            return Success();
        }


/**
 * 初始化Binder通信机制，用于系统启动过程中的进程间通信
 *
 * 该函数配置init进程的Binder使用限制。由于init进程的特殊性，
 * 对Binder的使用有严格限制：不能创建Binder线程、不能接收
 * 进入的Binder调用、不能将本地服务传递给远程进程、不能使用
 * 死亡通知机制。主要支持单向通知和其他守护进程通信，以及
 * 获取启动必需的数据。
 *
 * 注意：在recovery模式下不会执行Binder初始化
 *
 * @param args 内置参数引用，包含启动相关的参数信息
 * @return 返回成功结果，表示初始化完成
 */
        static Result<Success> InitBinder(const BuiltinArguments &args) {
            // init's use of binder is very limited. init cannot:
            //   - have any binder threads
            //   - receive incoming binder calls
            //   - pass local binder services to remote processes
            //   - use death recipients
            // The main supported usecases are:
            //   - notifying other daemons (oneway calls only)
            //   - retrieving data that is necessary to boot
            // Also, binder can't be used by recovery.

            // 在非recovery模式下配置Binder限制
#ifndef RECOVERY
            // 设置Binder线程池最大线程数为0，禁止创建Binder线程
            android::ProcessState::self()->setThreadPoolMaxThreadCount(0);
            // 设置调用限制，非单向调用将产生错误
            android::ProcessState::self()->setCallRestriction(
                    ProcessState::CallRestriction::ERROR_IF_NOT_ONEWAY);
#endif
            return Success();
        }


// Set the UDC controller for the ConfigFS USB Gadgets.
// Read the UDC controller in use from "/sys/class/udc".
// In case of multiple UDC controllers select the first one.
        static void set_usb_controller() {
            std::unique_ptr < DIR, decltype(&closedir) > dir(opendir("/sys/class/udc"), closedir);
            if (!dir) return;

            dirent *dp;
            while ((dp = readdir(dir.get())) != nullptr) {
                if (dp->d_name[0] == '.') continue;

                property_set("sys.usb.controller", dp->d_name);
                break;
            }
        }

        static void HandleSigtermSignal(const signalfd_siginfo &siginfo) {
            if (siginfo.ssi_pid != 0) {
                // Drop any userspace SIGTERM requests.
                LOG(DEBUG) << "Ignoring SIGTERM from pid " << siginfo.ssi_pid;
                return;
            }

            HandlePowerctlMessage("shutdown,container");
        }

        static void HandleSignalFd() {
            signalfd_siginfo siginfo;
            ssize_t bytes_read = TEMP_FAILURE_RETRY(read(signal_fd, &siginfo, sizeof(siginfo)));
            if (bytes_read != sizeof(siginfo)) {
                PLOG(ERROR) << "Failed to read siginfo from signal_fd";
                return;
            }

            switch (siginfo.ssi_signo) {
                case SIGCHLD:
                    ReapAnyOutstandingChildren();
                    break;
                case SIGTERM:
                    HandleSigtermSignal(siginfo);
                    break;
                default:
                    PLOG(ERROR) << "signal_fd: received unexpected signal " << siginfo.ssi_signo;
                    break;
            }
        }

        static void UnblockSignals() {
            const struct sigaction act{.sa_handler = SIG_DFL};
            sigaction(SIGCHLD, &act, nullptr);

            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGCHLD);
            sigaddset(&mask, SIGTERM);

            if (sigprocmask(SIG_UNBLOCK, &mask, nullptr) == -1) {
                PLOG(FATAL) << "failed to unblock signals for PID " << getpid();
            }
        }

/**
 * @brief 安装信号文件描述符处理器，用于通过epoll监听SIGCHLD和SIGTERM信号
 *
 * 该函数设置信号处理机制，将SIGCHLD和条件性地SIGTERM信号阻塞，
 * 并创建signalfd来通过epoll机制异步接收这些信号。
 * 同时注册了fork处理器以确保子进程能正确处理信号屏蔽。
 *
 * @param epoll 指向Epoll对象的指针，用于注册信号文件描述符的事件处理器
 */
        static void InstallSignalFdHandler(Epoll *epoll) {
            // 设置SIGCHLD信号的处理方式，使用默认处理并设置SA_NOCLDSTOP标志
            // 这样可以避免当子进程停止或继续时触发signalfd的SIGCHLD信号
            const struct sigaction act{.sa_handler = SIG_DFL, .sa_flags = SA_NOCLDSTOP};
            sigaction(SIGCHLD, &act, nullptr);

            // 初始化信号集并添加SIGCHLD信号
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGCHLD);

            // 根据是否具备重启能力决定是否监听SIGTERM信号
            if (!IsRebootCapable()) {
                // 如果init进程没有CAP_SYS_BOOT权限，则认为运行在容器中
                // 此时接收SIGTERM信号会导致系统关闭
                sigaddset(&mask, SIGTERM);
            }

            // 阻塞指定的信号集
            if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
                PLOG(FATAL) << "failed to block signals";
            }

            // 注册fork处理器，在子进程中解除信号阻塞
            const int result = pthread_atfork(nullptr, nullptr, &UnblockSignals);
            if (result != 0) {
                LOG(FATAL) << "Failed to register a fork handler: " << strerror(result);
            }

            // 创建信号文件描述符，用于通过文件描述符接口接收信号
            signal_fd = signalfd(-1, &mask, SFD_CLOEXEC);
            if (signal_fd == -1) {
                PLOG(FATAL) << "failed to create signalfd";
            }

            // 将信号文件描述符注册到epoll中，并关联信号处理回调函数
            if (auto result = epoll->RegisterHandler(signal_fd, HandleSignalFd); !result) {
                LOG(FATAL) << result.error();
            }
        }


        void HandleKeychord(const std::vector<int> &keycodes) {
            // Only handle keychords if adb is enabled.
            std::string adb_enabled = android::base::GetProperty("init.svc.adbd", "");
            if (adb_enabled != "running") {
                LOG(WARNING) << "Not starting service for keychord " << android::base::Join(keycodes, ' ')
                             << " because ADB is disabled";
                return;
            }

            auto found = false;
            for (const auto &service: ServiceList::GetInstance()) {
                auto svc = service.get();
                if (svc->keycodes() == keycodes) {
                    found = true;
                    LOG(INFO) << "Starting service '" << svc->name() << "' from keychord "
                              << android::base::Join(keycodes, ' ');
                    if (auto result = svc->Start(); !result) {
                        LOG(ERROR) << "Could not start service '" << svc->name() << "' from keychord "
                                   << android::base::Join(keycodes, ' ') << ": " << result.error();
                    }
                }
            }
            if (!found) {
                LOG(ERROR) << "Service for keychord " << android::base::Join(keycodes, ' ') << " not found";
            }
        }

        static void GlobalSeccomp() {
            import_kernel_cmdline(false, [](const std::string &key, const std::string &value,
                                            bool in_qemu) {
                if (key == "androidboot.seccomp" && value == "global" && !set_global_seccomp_filter()) {
                    LOG(FATAL) << "Failed to globally enable seccomp!";
                }
            });
        }

        static void UmountDebugRamdisk() {
            if (umount("/debug_ramdisk") != 0) {
                LOG(ERROR) << "Failed to umount /debug_ramdisk";
            }
        }

        int SecondStageMain(int argc, char **argv) {
            if (REBOOT_BOOTLOADER_ON_PANIC) {
                InstallRebootSignalHandlers();
            }

            SetStdioToDevNull(argv);
            InitKernelLogging(argv);
            LOG(INFO) << "init second stage started!";

            // Set init and its forked children's oom_adj.
            if (auto result = WriteFile("/proc/1/oom_score_adj", "-1000"); !result) {
                LOG(ERROR) << "Unable to write -1000 to /proc/1/oom_score_adj: " << result.error();
            }

            // Enable seccomp if global boot option was passed (otherwise it is enabled in zygote).
            GlobalSeccomp();

            // Set up a session keyring that all processes will have access to. It
            // will hold things like FBE encryption keys. No process should override
            // its session keyring.
            keyctl_get_keyring_ID(KEY_SPEC_SESSION_KEYRING, 1);

            // Indicate that booting is in progress to background fw loaders, etc.
            close(open("/dev/.booting", O_WRONLY | O_CREAT | O_CLOEXEC, 0000));

            // 属性
            property_init();

            // If arguments are passed both on the command line and in DT,
            // properties set in DT always have priority over the command-line ones.
            process_kernel_dt();
            process_kernel_cmdline();

            // Propagate the kernel variables to internal variables
            // used by init as well as the current required properties.
            export_kernel_boot_props();

            // Make the time that init started available for bootstat to log.
            property_set("ro.boottime.init", getenv("INIT_STARTED_AT"));
            property_set("ro.boottime.init.selinux", getenv("INIT_SELINUX_TOOK"));

            // Set libavb version for Framework-only OTA match in Treble build.
            const char *avb_version = getenv("INIT_AVB_VERSION");
            if (avb_version) property_set("ro.boot.avb_version", avb_version);

            // See if need to load debug props to allow adb root, when the device is unlocked.
            const char *force_debuggable_env = getenv("INIT_FORCE_DEBUGGABLE");
            if (force_debuggable_env && AvbHandle::IsDeviceUnlocked()) {
                load_debug_prop = "true"s == force_debuggable_env;
            }

            // Clean up our environment.
            unsetenv("INIT_STARTED_AT");
            unsetenv("INIT_SELINUX_TOOK");
            unsetenv("INIT_AVB_VERSION");
            unsetenv("INIT_FORCE_DEBUGGABLE");

            // Now set up SELinux for second stage.
            SelinuxSetupKernelLogging();
            SelabelInitialize();
            SelinuxRestoreContext();

            Epoll epoll;
            if (auto result = epoll.Open(); !result) {
                PLOG(FATAL) << result.error();
            }

            // 注册epoll -- 文件fg监听
            InstallSignalFdHandler(&epoll);

            property_load_boot_defaults(load_debug_prop);
            UmountDebugRamdisk();
            fs_mgr_vendor_overlay_mount_all();
            export_oem_lock_status();
            StartPropertyService(&epoll);
            MountHandler mount_handler(&epoll);
            set_usb_controller();

            const BuiltinFunctionMap function_map;
            Action::set_function_map(&function_map);

            if (!SetupMountNamespaces()) {
                PLOG(FATAL) << "SetupMountNamespaces failed";
            }

            subcontexts = InitializeSubcontexts();

            ActionManager &am = ActionManager::GetInstance();
            ServiceList &sm = ServiceList::GetInstance();

            // 解析init.rc
            LoadBootScripts(am, sm);

            // Turning this on and letting the INFO logging be discarded adds 0.2s to
            // Nexus 9 boot time, so it's disabled by default.
            if (false) DumpState();

            // Make the GSI status available before scripts start running.
            if (android::gsi::IsGsiRunning()) {
                property_set("ro.gsid.image_running", "1");
            } else {
                property_set("ro.gsid.image_running", "0");
            }

            am.QueueBuiltinAction(SetupCgroupsAction, "SetupCgroups");

            am.QueueEventTrigger("early-init");

            // Queue an action that waits for coldboot done so we know ueventd has set up all of /dev...
            am.QueueBuiltinAction(wait_for_coldboot_done_action, "wait_for_coldboot_done");
            // ... so that we can start queuing up actions that require stuff from /dev.
            am.QueueBuiltinAction(MixHwrngIntoLinuxRngAction, "MixHwrngIntoLinuxRng");
            am.QueueBuiltinAction(SetMmapRndBitsAction, "SetMmapRndBits");
            am.QueueBuiltinAction(SetKptrRestrictAction, "SetKptrRestrict");
            Keychords keychords;
            am.QueueBuiltinAction(
                    [&epoll, &keychords](const BuiltinArguments &args) -> Result<Success> {
                        for (const auto &svc: ServiceList::GetInstance()) {
                            keychords.Register(svc->keycodes());
                        }
                        keychords.Start(&epoll, HandleKeychord);
                        return Success();
                    },
                    "KeychordInit");
            am.QueueBuiltinAction(console_init_action, "console_init");

            // Trigger all the boot actions to get us started.
            am.QueueEventTrigger("init");

            // Starting the BoringSSL self test, for NIAP certification compliance.
            am.QueueBuiltinAction(StartBoringSslSelfTest, "StartBoringSslSelfTest");

            // Repeat mix_hwrng_into_linux_rng in case /dev/hw_random or /dev/random
            // wasn't ready immediately after wait_for_coldboot_done
            am.QueueBuiltinAction(MixHwrngIntoLinuxRngAction, "MixHwrngIntoLinuxRng");

            // Initialize binder before bringing up other system services
            am.QueueBuiltinAction(InitBinder, "InitBinder");

            // Don't mount filesystems or start core system services in charger mode.
            std::string bootmode = GetProperty("ro.bootmode", "");
            if (bootmode == "charger") {
                am.QueueEventTrigger("charger");
            } else {
                am.QueueEventTrigger("late-init");
            }

            // Run all property triggers based on current state of the properties.
            am.QueueBuiltinAction(queue_property_triggers_action, "queue_property_triggers");

            while (true) {
                // By default, sleep until something happens.
                auto epoll_timeout = std::optional < std::chrono::milliseconds > {};

                if (do_shutdown && !shutting_down) {
                    do_shutdown = false;
                    if (HandlePowerctlMessage(shutdown_command)) {
                        shutting_down = true;
                    }
                }

                if (!(waiting_for_prop || Service::is_exec_service_running())) {
                    am.ExecuteOneCommand();
                }
                if (!(waiting_for_prop || Service::is_exec_service_running())) {
                    if (!shutting_down) {
                        auto next_process_action_time = HandleProcessActions();

                        // If there's a process that needs restarting, wake up in time for that.
                        if (next_process_action_time) {
                            epoll_timeout = std::chrono::ceil<std::chrono::milliseconds>(
                                    *next_process_action_time - boot_clock::now());
                            if (*epoll_timeout < 0ms) epoll_timeout = 0ms;
                        }
                    }

                    // If there's more work to do, wake up again immediately.
                    if (am.HasMoreCommands()) epoll_timeout = 0ms;
                }

                if (auto result = epoll.Wait(epoll_timeout); !result) {
                    LOG(ERROR) << result.error();
                }
            }

            return 0;
        }

        /**
         * @brief 第二阶段初始化主函数，负责系统启动过程中的关键初始化操作。
         *
         * 此函数是 Android init 进程的第二阶段入口点。它完成 SELinux 初始化、属性服务启动、
         * 挂载命名空间设置、解析 init.rc 脚本，并进入事件循环处理各种触发器和服务管理任务。
         *
         * @param argc 命令行参数个数。
         * @param argv 命令行参数数组。
         * @return 返回值始终为 0（正常退出）。
         */
        int SecondStageMains(int argc, char **argv) {
            // 如果 panic 后需要重启到 bootloader，则安装相应的信号处理器
            if (REBOOT_BOOTLOADER_ON_PANIC) {
                InstallRebootSignalHandlers();
            }

            // 将标准输入输出重定向到 /dev/null，避免干扰控制台
            SetStdioToDevNull(argv);

            // 初始化内核日志系统并打印启动信息
            InitKernelLogging(argv);
            LOG(INFO) << "init second stage started!";

            // 设置 init 及其子进程的 oom_score_adj，降低被杀优先级
            if (auto result = WriteFile("/proc/1/oom_score_adj", "-1000"); !result) {
                LOG(ERROR) << "Unable to write -1000 to /proc/1/oom_score_adj: " << result.error();
            }

            // 根据全局引导选项启用 seccomp 安全机制
            GlobalSeccomp();

            // 创建一个所有进程都能访问的会话密钥环，用于存储如 FBE 加密密钥等敏感数据
            keyctl_get_keyring_ID(KEY_SPEC_SESSION_KEYRING, 1);

            // 创建标记文件表示正在启动中，供后台固件加载程序使用
            close(open("/dev/.booting", O_WRONLY | O_CREAT | O_CLOEXEC, 0000));

            // 初始化属性系统
            property_init();

            // 处理设备树和命令行传入的属性配置，DT 中的属性具有更高优先级
            process_kernel_dt();
            process_kernel_cmdline();

            // 导出内核变量为 init 内部使用的属性
            export_kernel_boot_props();

            // 记录 init 启动时间和 SELinux 初始化耗时
            property_set("ro.boottime.init", getenv("INIT_STARTED_AT"));
            property_set("ro.boottime.init.selinux", getenv("INIT_SELINUX_TOOK"));

            // 设置 AVB 版本号以支持 Treble 架构下的 OTA 匹配
            const char *avb_version = getenv("INIT_AVB_VERSION");
            if (avb_version) property_set("ro.boot.avb_version", avb_version);

            // 若设备解锁且环境变量指定，则允许加载调试属性以便 adb root
            const char *force_debuggable_env = getenv("INIT_FORCE_DEBUGGABLE");
            if (force_debuggable_env && AvbHandle::IsDeviceUnlocked()) {
                load_debug_prop = "true"s == force_debuggable_env;
            }

            // 清除临时使用的环境变量
            unsetenv("INIT_STARTED_AT");
            unsetenv("INIT_SELINUX_TOOK");
            unsetenv("INIT_AVB_VERSION");
            unsetenv("INIT_FORCE_DEBUGGABLE");

            // 初始化第二阶段 SELinux 相关组件
            SelinuxSetupKernelLogging();
            SelabelInitialize();
            SelinuxRestoreContext();

            // 初始化 epoll 实例用于事件驱动模型
            Epoll epoll;
            if (auto result = epoll.Open(); !result) {
                PLOG(FATAL) << result.error();
            }

            // 注册信号处理回调至 epoll
            InstallSignalFdHandler(&epoll);

            // 加载默认属性及调试属性（如果启用）
            property_load_boot_defaults(load_debug_prop);

            // 卸载调试用 RAM disk 并挂载 vendor overlay 分区
            UmountDebugRamdisk();
            fs_mgr_vendor_overlay_mount_all();

            // 导出 OEM 锁状态属性
            export_oem_lock_status();

            // 启动属性服务并与 epoll 关联
            StartPropertyService(&epoll);

            // 初始化挂载句柄并注册到 epoll
            MountHandler mount_handler(&epoll);

            // 配置 USB 控制器相关属性
            set_usb_controller();

            // 设置内置函数映射表供脚本调用
            const BuiltinFunctionMap function_map;
            Action::set_function_map(&function_map);

            // 设置挂载命名空间隔离策略
            if (!SetupMountNamespaces()) {
                PLOG(FATAL) << "SetupMountNamespaces failed";
            }

            // 初始化子上下文环境（用于跨用户服务运行）
            subcontexts = InitializeSubcontexts();

            // 获取单例对象引用：动作管理器与服务列表
            ActionManager &am = ActionManager::GetInstance();
            ServiceList &sm = ServiceList::GetInstance();

            // 解析并加载 init.rc 脚本定义的动作和服务
            LoadBootScripts(am, sm);

            // 如果启用了 GSI（通用系统镜像），则导出运行状态属性
            if (android::gsi::IsGsiRunning()) {
                property_set("ro.gsid.image_running", "1");
            } else {
                property_set("ro.gsid.image_running", "0");
            }

            // 排队执行 cgroup 初始化动作
            am.QueueBuiltinAction(SetupCgroupsAction, "SetupCgroups");

            // 触发 early-init 阶段事件
            am.QueueEventTrigger("early-init");

            // 等待冷启动完成后再继续后续操作
            am.QueueBuiltinAction(wait_for_coldboot_done_action, "wait_for_coldboot_done");

            // 执行硬件随机数混合进 Linux RNG 的动作
            am.QueueBuiltinAction(MixHwrngIntoLinuxRngAction, "MixHwrngIntoLinuxRng");

            // 设置 mmap 地址随机化位数增强安全性
            am.QueueBuiltinAction(SetMmapRndBitsAction, "SetMmapRndBits");

            // 设置 kptr_restrict 限制内核指针泄露
            am.QueueBuiltinAction(SetKptrRestrictAction, "SetKptrRestrict");

            // 初始化按键组合监听功能
            Keychords keychords;
            am.QueueBuiltinAction(
                    [&epoll, &keychords](const BuiltinArguments &args) -> Result<Success> {
                        for (const auto &svc: ServiceList::GetInstance()) {
                            keychords.Register(svc->keycodes());
                        }
                        keychords.Start(&epoll, HandleKeychord);
                        return Success();
                    },
                    "KeychordInit");

            // 初始化控制台界面
            am.QueueBuiltinAction(console_init_action, "console_init");

            // 触发 init 主要初始化流程
            am.QueueEventTrigger("init");

            // 启动 BoringSSL 自检以满足 NIAP 认证要求
            am.QueueBuiltinAction(StartBoringSslSelfTest, "StartBoringSslSelfTest");

            // 再次尝试混入硬件随机数以防首次失败
            am.QueueBuiltinAction(MixHwrngIntoLinuxRngAction, "MixHwrngIntoLinuxRng");

            // 初始化 Binder IPC 通信机制
            am.QueueBuiltinAction(InitBinder, "InitBinder");

            // 判断是否处于充电模式，决定后续触发哪个阶段
            std::string bootmode = GetProperty("ro.bootmode", "");
            if (bootmode == "charger") {
                am.QueueEventTrigger("charger");
            } else {
                am.QueueEventTrigger("late-init");
            }

            // 根据当前属性状态排队执行对应的属性触发器
            am.QueueBuiltinAction(queue_property_triggers_action, "queue_property_triggers");

            // 主事件循环：持续处理命令队列和 epoll 事件
            while (true) {
                // 默认情况下等待事件发生
                auto epoll_timeout = std::optional < std::chrono::milliseconds > {};

                // 处理关机请求逻辑
                if (do_shutdown && !shutting_down) {
                    do_shutdown = false;
                    if (HandlePowerctlMessage(shutdown_command)) {
                        shutting_down = true;
                    }
                }

                // 执行一条待处理命令（若无阻塞条件）
                if (!(waiting_for_prop || Service::is_exec_service_running())) {
                    am.ExecuteOneCommand();
                }

                // 在没有阻塞条件下检查是否有更多工作要做或需定时唤醒
                if (!(waiting_for_prop || Service::is_exec_service_running())) {
                    if (!shutting_down) {
                        auto next_process_action_time = HandleProcessActions();

                        // 如有需重启的服务，则计算下次唤醒时间
                        if (next_process_action_time) {
                            epoll_timeout = std::chrono::ceil<std::chrono::milliseconds>(
                                    *next_process_action_time - boot_clock::now());
                            if (*epoll_timeout < 0ms) epoll_timeout = 0ms;
                        }
                    }

                    // 如果还有未处理完的命令则立即唤醒
                    if (am.HasMoreCommands()) epoll_timeout = 0ms;
                }

                // 使用 epoll 等待事件发生
                if (auto result = epoll.Wait(epoll_timeout); !result) {
                    LOG(ERROR) << result.error();
                }
            }

            return 0;
        }


    }  // namespace init
}  // namespace android
