/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include <sys/resource.h>

#include <sched.h>

#include <android/frameworks/displayservice/1.0/IDisplayService.h>
#include <android/hardware/configstore/1.0/ISurfaceFlingerConfigs.h>
#include <android/hardware/graphics/allocator/2.0/IAllocator.h>
#include <android/hardware/graphics/allocator/3.0/IAllocator.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <configstore/Utils.h>
#include <displayservice/DisplayService.h>
#include <hidl/LegacySupport.h>
#include <processgroup/sched_policy.h>
#include "SurfaceFlinger.h"
#include "SurfaceFlingerFactory.h"
#include "SurfaceFlingerProperties.h"

using namespace android;

static status_t startGraphicsAllocatorService() {
    //
    using android::hardware::configstore::getBool;
    using android::hardware::configstore::V1_0::ISurfaceFlingerConfigs;
    //  硬件 开关
    if (!android::sysprop::start_graphics_allocator_service(false)) {
        return OK;
    }

    // 数据传输 通道
    // V2   V3
    status_t result = hardware::registerPassthroughServiceImplementation<
            android::hardware::graphics::allocator::V3_0::IAllocator>();
    if (result == OK) {
        return OK;
    }

    result = hardware::registerPassthroughServiceImplementation<
            android::hardware::graphics::allocator::V2_0::IAllocator>();
    if (result != OK) {
        ALOGE("could not start graphics allocator service");
        return result;
    }

    return OK;
}

static status_t startDisplayService() {
    using android::frameworks::displayservice::V1_0::implementation::DisplayService;
    using android::frameworks::displayservice::V1_0::IDisplayService;

    sp<IDisplayService> displayservice = new DisplayService();
    status_t err = displayservice->registerAsService();

    if (err != OK) {
        ALOGE("Could not register IDisplayService service.");
    }

    return err;
}

/**
 * 主程序启动
 *  onFirstRef() -> init() -> run()
 * @return
 */
int main(int, char **) {
    // 1. 拉起其它伴随系统服务的主循环（如部分 HIDL 服务的独立 looper），在 SurfaceFlinger 之前先就绪。
    OtherSystemServiceLoopRun();

    // 2. 忽略 SIGPIPE，避免向已关闭的 socket 写数据时进程被信号杀死。
    signal(SIGPIPE, SIG_IGN);

    // 3. 配置 HIDL RPC 线程池（最大 1 个线程，调用方不 join），供后续注册 passthrough HAL 服务使用。
    hardware::configureRpcThreadpool(1 /* maxThreads */,
                                     false /* callerWillJoin */);

    /**
     * 分配硬件 服务
     */
    // 4. 启动图形内存分配器 HAL 服务（优先 V3，回退 V2），供后续 BufferQueue/GraphicBuffer 分配内存。
    startGraphicsAllocatorService();

    // When SF is launched in its own process, limit the number of
    // binder threads to 4.
    // 5. SurfaceFlinger 独立进程运行，将 Binder 线程池上限限制为 4，避免线程膨胀。
    ProcessState::self()->setThreadPoolMaxThreadCount(4);

    // start the thread pool
    // 6. 取得 Binder 进程状态并启动线程池，开始接收跨进程请求。
    sp<ProcessState> ps(ProcessState::self());
    ps->startThreadPool();

    // instantiate surfaceflinger
    // 7. 通过工厂函数创建 SurfaceFlinger 实例（真正的合成引擎对象）。
    sp<SurfaceFlinger> flinger = surfaceflinger::createSurfaceFlinger();

    // 8. 提升主线程优先级为 URGENT_DISPLAY，并将其调度策略置为前台，保证合成实时性。
    setpriority(PRIO_PROCESS, 0, PRIORITY_URGENT_DISPLAY);

    set_sched_policy(0, SP_FOREGROUND);

    // Put most SurfaceFlinger threads in the system-background cpuset
    // Keeps us from unnecessarily using big cores
    // Do this after the binder thread pool init
    // 9. 若启用 cpusets，将主线程放入 system-background cpuset，避免占用大核；需在 Binder 线程池初始化之后执行。
    if (cpusets_enabled()) set_cpuset_policy(0, SP_SYSTEM);

    // initialize before clients can connect
    // 10. 在客户端连接前完成初始化（创建 HWComposer、事件队列、Display 设备等）。
    flinger->init();

    // publish surface flinger
    // 11. 将 SurfaceFlinger 以临界优先级 + proto dump 标志注册到 ServiceManager，供其它进程（如 WMS）获取。
    sp<IServiceManager> sm(defaultServiceManager());
    sm->addService(String16(SurfaceFlinger::getServiceName()), flinger, false,
                   IServiceManager::DUMP_FLAG_PRIORITY_CRITICAL | IServiceManager::DUMP_FLAG_PROTO);

    // 12. 注册 DisplayService（依赖上面 SF 已注册，部分显示相关查询依赖 SF）。
    startDisplayService(); // dependency on SF getting registered above

    // 13. 将主线程调度策略设为 SCHED_FIFO、优先级 2，进一步确保合成循环的硬实时响应。
    struct sched_param param = {0};
    param.sched_priority = 2;
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        ALOGE("Couldn't set SCHED_FIFO");
    }

    // run surface flinger in this thread
    // 14. 在当前主线程中进入 SurfaceFlinger 主循环（消息队列 + 合成循环），此后 main 线程即成为 SF 主循环线程。
    flinger->run();

    return 0;
}
