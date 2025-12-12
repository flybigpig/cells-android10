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

#include "builtins.h"
#include "first_stage_init.h"
#include "init.h"
#include "selinux.h"
#include "subcontext.h"
#include "ueventd.h"

#include <android-base/logging.h>

#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#endif

#if __has_feature(address_sanitizer)
// Load asan.options if it exists since these are not yet in the environment.
// Always ensure detect_container_overflow=0 as there are false positives with this check.
// Always ensure abort_on_error=1 to ensure we reboot to bootloader for development builds.
extern "C" const char* __asan_default_options() {
    return "include_if_exists=/system/asan.options:detect_container_overflow=0:abort_on_error=1";
}

__attribute__((no_sanitize("address", "memory", "thread", "undefined"))) extern "C" void
__sanitizer_report_error_summary(const char* summary) {
    LOG(ERROR) << "Init (error summary): " << summary;
}

__attribute__((no_sanitize("address", "memory", "thread", "undefined"))) static void
AsanReportCallback(const char* str) {
    LOG(ERROR) << "Init: " << str;
}
#endif

using namespace android::init;


/**
 * 程序主入口函数，根据不同的命令行参数执行不同的初始化阶段
 *
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return 返回程序执行结果状态码
 */
int main(int argc, char** argv) {
#if __has_feature(address_sanitizer)
    __asan_set_error_report_callback(AsanReportCallback);
#endif

    // 检查程序是否以"ueventd"名称运行，如果是则执行ueventd主逻辑
    if (!strcmp(basename(argv[0]), "ueventd")) {
        return ueventd_main(argc, argv);
    }

    // 当存在命令行参数时，根据第一个参数决定执行哪个初始化阶段
    if (argc > 1) {
        // 处理子上下文模式
        if (!strcmp(argv[1], "subcontext")) {
            android::base::InitLogging(argv, &android::base::KernelLogger);
            const BuiltinFunctionMap function_map;

            return SubcontextMain(argc, argv, &function_map);
        }

        // 处理SELinux设置阶段，从FirstStageMain过渡到SetupSelinux
        if (!strcmp(argv[1], "selinux_setup")) {
            return SetupSelinux(argv);
        }

        // 处理第二阶段初始化，从SetupSelinux过渡到SecondStageMain
        if (!strcmp(argv[1], "second_stage")) {
            return SecondStageMain(argc, argv);
        }
    }

    // 执行第一阶段初始化，主要负责文件系统、日志等基础环境的初始化，
    // 完成后将过渡到SecondStageMain继续执行
    return FirstStageMain(argc, argv);
}

