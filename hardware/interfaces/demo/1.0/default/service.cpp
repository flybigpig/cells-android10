#define LOG_TAG "android.hardware.demo@1.0-service"

#include <hidl/HidlSupport.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include "Demo.h"

using ::android::OK;
using ::android::sp;
using ::android::hardware::configureRpcThreadpool;
using ::android::hardware::joinRpcThreadpool;
using ::android::hardware::demo::V1_0::IDemo;
using ::android::hardware::demo::V1_0::implementation::Demo;

int main(int /* argc */, char* /* argv */ []) {
    sp<IDemo> service = new Demo();
    configureRpcThreadpool(1, true /* will join */);
    if (service->registerAsService() != OK) {
        ALOGE("Could not register demo service.");
        return 1;
    }
    joinRpcThreadpool();
    ALOGE("Demo service exited!");
    return 1;
}
