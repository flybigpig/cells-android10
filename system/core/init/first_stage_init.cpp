/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "first_stage_init.h"

#include <dirent.h>
#include <fcntl.h>
#include <paths.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include <android-base/chrono_utils.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <private/android_filesystem_config.h>

#include "debug_ramdisk.h"
#include "first_stage_mount.h"
#include "reboot_utils.h"
#include "switch_root.h"
#include "util.h"

using android::base::boot_clock;

using namespace std::literals;

namespace fs = std::filesystem;

namespace android {
namespace init {

namespace {

void FreeRamdisk(DIR* dir, dev_t dev) {
    int dfd = dirfd(dir);

    dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        if (de->d_name == "."s || de->d_name == ".."s) {
            continue;
        }

        bool is_dir = false;

        if (de->d_type == DT_DIR || de->d_type == DT_UNKNOWN) {
            struct stat info;
            if (fstatat(dfd, de->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
                continue;
            }

            if (info.st_dev != dev) {
                continue;
            }

            if (S_ISDIR(info.st_mode)) {
                is_dir = true;
                auto fd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY);
                if (fd >= 0) {
                    auto subdir =
                            std::unique_ptr<DIR, decltype(&closedir)>{fdopendir(fd), closedir};
                    if (subdir) {
                        FreeRamdisk(subdir.get(), dev);
                    } else {
                        close(fd);
                    }
                }
            }
        }
        unlinkat(dfd, de->d_name, is_dir ? AT_REMOVEDIR : 0);
    }
}

bool ForceNormalBoot() {
    std::string cmdline;
    android::base::ReadFileToString("/proc/cmdline", &cmdline);
    return cmdline.find("androidboot.force_normal_boot=1") != std::string::npos;
}

}  // namespace

int FirstStageMain(int argc, char** argv) {
    if (REBOOT_BOOTLOADER_ON_PANIC) {
        InstallRebootSignalHandlers();
    }

    boot_clock::time_point start_time = boot_clock::now();

    std::vector<std::pair<std::string, int>> errors;
#define CHECKCALL(x) \
    if (x != 0) errors.emplace_back(#x " failed", errno);

    // Clear the umask.
    umask(0);

    CHECKCALL(clearenv());
    CHECKCALL(setenv("PATH", _PATH_DEFPATH, 1));
    // Get the basic filesystem setup we need put together in the initramdisk
    // on / and then we'll let the rc file figure out the rest.
    CHECKCALL(mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=0755"));
    CHECKCALL(mkdir("/dev/pts", 0755));
    CHECKCALL(mkdir("/dev/socket", 0755));
    CHECKCALL(mount("devpts", "/dev/pts", "devpts", 0, NULL));
#define MAKE_STR(x) __STRING(x)
    CHECKCALL(mount("proc", "/proc", "proc", 0, "hidepid=2,gid=" MAKE_STR(AID_READPROC)));
#undef MAKE_STR
    // Don't expose the raw commandline to unprivileged processes.
    CHECKCALL(chmod("/proc/cmdline", 0440));
    gid_t groups[] = {AID_READPROC};
    CHECKCALL(setgroups(arraysize(groups), groups));
    CHECKCALL(mount("sysfs", "/sys", "sysfs", 0, NULL));
    CHECKCALL(mount("selinuxfs", "/sys/fs/selinux", "selinuxfs", 0, NULL));

    CHECKCALL(mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11)));

    if constexpr (WORLD_WRITABLE_KMSG) {
        CHECKCALL(mknod("/dev/kmsg_debug", S_IFCHR | 0622, makedev(1, 11)));
    }

    CHECKCALL(mknod("/dev/random", S_IFCHR | 0666, makedev(1, 8)));
    CHECKCALL(mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9)));

    // This is needed for log wrapper, which gets called before ueventd runs.
    CHECKCALL(mknod("/dev/ptmx", S_IFCHR | 0666, makedev(5, 2)));
    CHECKCALL(mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3)));

    // These below mounts are done in first stage init so that first stage mount can mount
    // subdirectories of /mnt/{vendor,product}/.  Other mounts, not required by first stage mount,
    // should be done in rc files.
    // Mount staging areas for devices managed by vold
    // See storage config details at http://source.android.com/devices/storage/
    CHECKCALL(mount("tmpfs", "/mnt", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
                    "mode=0755,uid=0,gid=1000"));
    // /mnt/vendor is used to mount vendor-specific partitions that can not be
    // part of the vendor partition, e.g. because they are mounted read-write.
    CHECKCALL(mkdir("/mnt/vendor", 0755));
    // /mnt/product is used to mount product-specific partitions that can not be
    // part of the product partition, e.g. because they are mounted read-write.
    CHECKCALL(mkdir("/mnt/product", 0755));

    // /apex is used to mount APEXes
    CHECKCALL(mount("tmpfs", "/apex", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
                    "mode=0755,uid=0,gid=0"));

    // /debug_ramdisk is used to preserve additional files from the debug ramdisk
    CHECKCALL(mount("tmpfs", "/debug_ramdisk", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
                    "mode=0755,uid=0,gid=0"));
#undef CHECKCALL

    SetStdioToDevNull(argv);
    // Now that tmpfs is mounted on /dev and we have /dev/kmsg, we can actually
    // talk to the outside world...
    InitKernelLogging(argv);

    if (!errors.empty()) {
        for (const auto& [error_string, error_errno] : errors) {
            LOG(ERROR) << error_string << " " << strerror(error_errno);
        }
        LOG(FATAL) << "Init encountered errors starting first stage, aborting";
    }

    LOG(INFO) << "init first stage started!";

    auto old_root_dir = std::unique_ptr<DIR, decltype(&closedir)>{opendir("/"), closedir};
    if (!old_root_dir) {
        PLOG(ERROR) << "Could not opendir(\"/\"), not freeing ramdisk";
    }

    struct stat old_root_info;
    if (stat("/", &old_root_info) != 0) {
        PLOG(ERROR) << "Could not stat(\"/\"), not freeing ramdisk";
        old_root_dir.reset();
    }

    if (ForceNormalBoot()) {
        mkdir("/first_stage_ramdisk", 0755);
        // SwitchRoot() must be called with a mount point as the target, so we bind mount the
        // target directory to itself here.
        if (mount("/first_stage_ramdisk", "/first_stage_ramdisk", nullptr, MS_BIND, nullptr) != 0) {
            LOG(FATAL) << "Could not bind mount /first_stage_ramdisk to itself";
        }
        SwitchRoot("/first_stage_ramdisk");
    }

    // If this file is present, the second-stage init will use a userdebug sepolicy
    // and load adb_debug.prop to allow adb root, if the device is unlocked.
    if (access("/force_debuggable", F_OK) == 0) {
        std::error_code ec;  // to invoke the overloaded copy_file() that won't throw.
        if (!fs::copy_file("/adb_debug.prop", kDebugRamdiskProp, ec) ||
            !fs::copy_file("/userdebug_plat_sepolicy.cil", kDebugRamdiskSEPolicy, ec)) {
            LOG(ERROR) << "Failed to setup debug ramdisk";
        } else {
            // setenv for second-stage init to read above kDebugRamdisk* files.
            setenv("INIT_FORCE_DEBUGGABLE", "true", 1);
        }
    }

    if (!DoFirstStageMount()) {
        LOG(FATAL) << "Failed to mount required partitions early ...";
    }

    struct stat new_root_info;
    if (stat("/", &new_root_info) != 0) {
        PLOG(ERROR) << "Could not stat(\"/\"), not freeing ramdisk";
        old_root_dir.reset();
    }

    if (old_root_dir && old_root_info.st_dev != new_root_info.st_dev) {
        FreeRamdisk(old_root_dir.get(), old_root_info.st_dev);
    }

    SetInitAvbVersionInRecovery();

    static constexpr uint32_t kNanosecondsPerMillisecond = 1e6;
    uint64_t start_ms = start_time.time_since_epoch().count() / kNanosecondsPerMillisecond;
    setenv("INIT_STARTED_AT", std::to_string(start_ms).c_str(), 1);

    const char* path = "/system/bin/init";
    const char* args[] = {path, "selinux_setup", nullptr};
    execv(path, const_cast<char**>(args));

    // execv() only returns if an error happened, in which case we
    // panic and never fall through this conditional.
    PLOG(FATAL) << "execv(\"" << path << "\") failed";

    return 1;
}

/**
 * @brief 第一阶段初始化主函数，负责在系统启动早期完成基本文件系统的挂载、设备节点创建等关键操作。
 *
 * 此函数运行于 init 进程的第一阶段，在内核加载后立即执行。它会设置必要的临时文件系统（如 tmpfs），
 * 创建核心设备节点，并准备后续 init 阶段所需的环境。完成后通过 execv 启动第二阶段的 selinux_setup。
 *
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return 函数正常不会返回；若出错则返回非零值表示失败
 */
    int FirstStageMains(int argc, char** argv) {
        // 若定义了 REBOOT_BOOTLOADER_ON_PANIC，则安装重启信号处理器以应对 panic 情况
        if (REBOOT_BOOTLOADER_ON_PANIC) {
            InstallRebootSignalHandlers();
        }

        // 记录第一阶段启动开始时间，用于性能统计
        boot_clock::time_point start_time = boot_clock::now();



        // 存储过程中发生的错误信息及其对应的 errno
        std::vector<std::pair<std::string, int>> errors;

        // 定义宏 CHECKCALL：用于检查系统调用是否成功，失败时记录错误信息及 errno 到 errors 中
#define CHECKCALL(x) \
    if (x != 0) errors.emplace_back(#x " failed", errno);

        // 清除当前 umask 设置，确保权限控制由具体操作决定
        umask(0);

        // 清空并重新设置环境变量 PATH，使用默认路径
        CHECKCALL(clearenv());
        CHECKCALL(setenv("PATH", _PATH_DEFPATH, 1));

        // 挂载基础的内存文件系统到 /dev 目录下，供后续设备节点使用
        CHECKCALL(mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=0755"));
        CHECKCALL(mkdir("/dev/pts", 0755));           // 创建伪终端目录
        CHECKCALL(mkdir("/dev/socket", 0755));        // 创建 socket 文件夹
        CHECKCALL(mount("devpts", "/dev/pts", "devpts", 0, NULL)); // 挂载 devpts 文件系统

        // 挂载 proc 文件系统，并限制普通用户访问 cmdline 的权限
#define MAKE_STR(x) __STRING(x)
        CHECKCALL(mount("proc", "/proc", "proc", 0, "hidepid=2,gid=" MAKE_STR(AID_READPROC)));
#undef MAKE_STR
        CHECKCALL(chmod("/proc/cmdline", 0440));      // 仅允许 root 和指定组读取 cmdline

        // 设置进程所属附加组，以便访问受限资源
        gid_t groups[] = {AID_READPROC};
        CHECKCALL(setgroups(arraysize(groups), groups));

        // 挂载 sysfs 和 SELinux 虚拟文件系统
        CHECKCALL(mount("sysfs", "/sys", "sysfs", 0, NULL));
        CHECKCALL(mount("selinuxfs", "/sys/fs/selinux", "selinuxfs", 0, NULL));

        // 创建关键字符设备节点
        CHECKCALL(mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11)));     // 内核日志设备
        if constexpr (WORLD_WRITABLE_KMSG) {
            CHECKCALL(mknod("/dev/kmsg_debug", S_IFCHR | 0622, makedev(1, 11))); // 可调试版本
        }
        CHECKCALL(mknod("/dev/random", S_IFCHR | 0666, makedev(1, 8)));    // 随机数源
        CHECKCALL(mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9)));   // 非阻塞随机数源
        CHECKCALL(mknod("/dev/ptmx", S_IFCHR | 0666, makedev(5, 2)));      // 控制伪终端主设备
        CHECKCALL(mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3)));      // 空设备

        // 挂载用于管理外部存储分区的临时目录
        CHECKCALL(mount("tmpfs", "/mnt", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
                        "mode=0755,uid=0,gid=1000"));
        CHECKCALL(mkdir("/mnt/vendor", 0755));         // vendor 分区挂载点
        CHECKCALL(mkdir("/mnt/product", 0755));        // product 分区挂载点

        // 挂载 APEX 包专用目录
        CHECKCALL(mount("tmpfs", "/apex", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
                        "mode=0755,uid=0,gid=0"));

        // 挂载调试 ramdisk 使用的临时目录
        CHECKCALL(mount("tmpfs", "/debug_ramdisk", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
                        "mode=0755,uid=0,gid=0"));

#undef CHECKCALL

        // 将标准输入输出重定向至 /dev/null，避免干扰
        SetStdioToDevNull(argv);

        // 初始化内核日志系统，启用真正的日志输出功能
        InitKernelLogging(argv);

        // 如果有错误发生，则打印所有错误并终止程序
        if (!errors.empty()) {
            for (const auto& [error_string, error_errno] : errors) {
                LOG(ERROR) << error_string << " " << strerror(error_errno);
            }
            LOG(FATAL) << "Init encountered errors starting first stage, aborting";
        }

        LOG(INFO) << "init first stage started!";

        // 打开旧根目录句柄，用于后续释放 ramdisk
        auto old_root_dir = std::unique_ptr<DIR, decltype(&closedir)>{opendir("/"), closedir};
        if (!old_root_dir) {
            PLOG(ERROR) << "Could not opendir(\"/\"), not freeing ramdisk";
        }

        struct stat old_root_info;
        if (stat("/", &old_root_info) != 0) {
            PLOG(ERROR) << "Could not stat(\"/\"), not freeing ramdisk";
            old_root_dir.reset();
        }

        // 根据 ForceNormalBoot 返回结果判断是否切换到保留的 ramdisk
        if (ForceNormalBoot()) {
            mkdir("/first_stage_ramdisk", 0755);
            // 绑定挂载自身以满足 SwitchRoot 对目标必须是挂载点的要求
            if (mount("/first_stage_ramdisk", "/first_stage_ramdisk", nullptr, MS_BIND, nullptr) != 0) {
                LOG(FATAL) << "Could not bind mount /first_stage_ramdisk to itself";
            }
            SwitchRoot("/first_stage_ramdisk");
        }

        // 如果存在 /force_debuggable 文件，则复制调试相关配置文件并设置环境变量
        if (access("/force_debuggable", F_OK) == 0) {
            std::error_code ec;  // 不抛异常的 copy_file 版本
            if (!fs::copy_file("/adb_debug.prop", kDebugRamdiskProp, ec) ||
                !fs::copy_file("/userdebug_plat_sepolicy.cil", kDebugRamdiskSEPolicy, ec)) {
                LOG(ERROR) << "Failed to setup debug ramdisk";
            } else {
                setenv("INIT_FORCE_DEBUGGABLE", "true", 1); // 通知第二阶段使用调试 sepolicy
            }
        }

        // 执行第一阶段挂载任务，包括关键分区的挂载
        if (!DoFirstStageMount()) {
            LOG(FATAL) << "Failed to mount required partitions early ...";
        }

        // 获取新根目录状态，用于比较设备号来判断是否需要释放旧 ramdisk
        struct stat new_root_info;
        if (stat("/", &new_root_info) != 0) {
            PLOG(ERROR) << "Could not stat(\"/\"), not freeing ramdisk";
            old_root_dir.reset();
        }

        // 如果旧根与新根不在同一设备上，则释放旧 ramdisk 占用空间
        if (old_root_dir && old_root_info.st_dev != new_root_info.st_dev) {
            FreeRamdisk(old_root_dir.get(), old_root_info.st_dev);
        }

        // 在 recovery 模式中设置 AVB 版本信息
        SetInitAvbVersionInRecovery();

        // 设置 INIT_STARTED_AT 环境变量，记录 init 启动时间戳（毫秒）
        static constexpr uint32_t kNanosecondsPerMillisecond = 1e6;
        uint64_t start_ms = start_time.time_since_epoch().count() / kNanosecondsPerMillisecond;
        setenv("INIT_STARTED_AT", std::to_string(start_ms).c_str(), 1);

        // 启动下一阶段的 init 流程 —— selinux_setup
        const char* path = "/system/bin/init";
        const char* args[] = {path, "selinux_setup", nullptr};
        execv(path, const_cast<char**>(args));

        // execv 失败才会走到这里，此时应触发致命错误
        PLOG(FATAL) << "execv(\"" << path << "\") failed";

        return 1;
    }


}  // namespace init
}  // namespace android
