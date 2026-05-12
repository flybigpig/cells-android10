// YourService.cpp
#define LOG_TAG "YourService"
#include <utils/Log.h>
#include "YourService.h"

namespace android {

YourService::YourService() : mStatus(0) {
    ALOGI("YourService created");
}

YourService::~YourService() {
    ALOGI("YourService destroyed");
}

void YourService::instantiate() {
    publishAndJoinThreadPool(false);  // false = 不阻塞
}

status_t YourService::doSomething(const String16& param, int32_t* result) {
    ALOGD("doSomething: %s", String8(param).string());

    // 业务逻辑示例
    *result = 42;  // 返回值

    return NO_ERROR;
}

status_t YourService::getStatus(int32_t* status) {
    *status = mStatus;
    return NO_ERROR;
}

status_t YourService::setConfig(const String16& key, const String16& value) {
    mConfigs[key] = value;
    ALOGI("setConfig: %s = %s", String8(key).string(), String8(value).string());
    return NO_ERROR;
}

}  // namespace android
