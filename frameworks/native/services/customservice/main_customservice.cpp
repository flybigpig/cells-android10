// main_customservice.cpp（参考 mediaserver 写法）
#define LOG_TAG "CustomServiceMain"
//#define LOG_NDEBUG 0

#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include "YourService.h"

using namespace android;

int main(int argc __unused, char **argv __unused) {
    signal(SIGPIPE, SIG_IGN);

    // 1. 初始化 Binder 进程状态
    sp<ProcessState> proc(ProcessState::self());
    sp<IServiceManager> sm(defaultServiceManager());

    ALOGI("Starting CustomService...");
    ALOGI("ServiceManager: %p", sm.get());

    // 2. 注册服务（可注册多个，像 mediaserver 一样）
    //    instantiate() 内部只调用 publish()，不会阻塞
    YourService::instantiate();

    // 如果有其他相关服务，可以在这里继续注册...
    // AnotherRelatedService::instantiate();

    // 3. 启动 Binder 线程池并进入主循环（阻塞等待请求）
    ProcessState::self()->startThreadPool();
    IPCThreadState::self()->joinThreadPool();

    return 0;
}
