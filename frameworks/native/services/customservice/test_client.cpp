// test_client.cpp
#include <binder/IServiceManager.h>
#include <binder/IPCThreadState.h>
#include <utils/Log.h>
#include "IYourService.h"

using namespace android;

int main() {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("custom.service"));

    if (binder == nullptr) {
        ALOGE("Failed to get service custom.service");
        return -1;
    }

    sp<IYourService> service = interface_cast<IYourService>(binder);

    // 测试调用
    int32_t result;
    status_t status = service->doSomething(String16("hello"), &result);
    ALOGI("doSomething -> result=%d, status=%d", result, status);

    int32_t s;
    status = service->getStatus(&s);
    ALOGI("getStatus -> status=%d, value=%d", status, s);

    return 0;
}
