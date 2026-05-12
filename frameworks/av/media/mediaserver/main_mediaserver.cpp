/*
**
** Copyright 2008, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "mediaserver"
//#define LOG_NDEBUG 0

#include <aicu/AIcu.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include "RegisterExtensions.h"

// from LOCAL_C_INCLUDES
#include "MediaPlayerService.h"
#include "ResourceManagerService.h"

using namespace android;
// native 服务启动
int main(int argc __unused, char **argv __unused) {
    OtherSystemServiceLoopRun();  // 自定义初始化
    signal(SIGPIPE, SIG_IGN);     // 忽略管道信号

    // 1️⃣ 初始化 Binder 进程状态
    sp<ProcessState> proc(ProcessState::self());

    // 2️⃣ 获取 ServiceManager
    sp<IServiceManager> sm(defaultServiceManager());
    ALOGI("ServiceManager: %p", sm.get());

    // 3️⃣ 系统组件初始化（ICU国际化支持）
    AIcu_initializeIcuOrDie();

    // 4️⃣ 注册多个服务到 ServiceManager
    MediaPlayerService::instantiate();      // 媒体播放服务
    ResourceManagerService::instantiate();  // 资源管理服务

    // 5️⃣ 注册扩展服务
    registerExtensions();

    // 6️⃣ 启动 Binder 线程池并加入主循环
    ProcessState::self()->startThreadPool();
    IPCThreadState::self()->joinThreadPool();  // 阻塞等待请求
}

