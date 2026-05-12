// main_yourservice.cpp (参考 mediaserver)
#define LOG_TAG "YourServiceMain"
//#define LOG_NDEBUG 0

#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
 <utils/Log.h>
#include "YourService.h"

using namespace android;

int main(int argc __unused, char **argv __unused) {
    signal(SIGPIPE, SIG_IGN);

    // 初始化 Binder 进程
    sp<ProcessState> proc(ProcessState::self());
    sp<IServiceManager> sm(defaultServiceManager());

    ALOGI("Starting YourService...");
    ALOGI("ServiceManager: %p", sm.get());

    // 注册你的服务（可注册多个，像mediaserver一样）
    YourService::instantiate();

    // 如果有其他相关服务，可以在这里继续注册...
    // AnotherRelatedService::instantiate();

    // 启动 Binder 线程池并进入主循环
    ProcessState::self()->startThreadPool();
    IPCThreadState::self()->joinThreadPool();

    return 0;
}
