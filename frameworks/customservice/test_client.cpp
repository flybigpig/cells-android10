// test_client.cpp
#include <binder/IServiceManager.h>
#include <binder/IPCThreadState.h>
#include "IYourService.h"

using namespace android;

int main() {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("your.custom.service"));

    if (binder == nullptr) {
        ALOGE("Failed to get service");
        return -1;
    }

    sp<IYourService> service = interface_cast<IYourService>(binder);

    // 测试调用
    int32_t result;
    status_t status = service->doSomething(String16("hello"), &result);
    ALOGI("Result: %d, Status: %d", result, status);

    return 0;
}
