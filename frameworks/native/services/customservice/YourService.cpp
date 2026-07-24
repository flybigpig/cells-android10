// YourService.cpp
#define LOG_TAG "YourService"
#include <utils/Log.h>
#include <utils/String8.h>
#include "YourService.h"

// 服务端具体实现
namespace android {

YourService::YourService() : mStatus(0) {
    ALOGI("YourService created");
}

YourService::~YourService() {
    ALOGI("YourService destroyed");
}

// 仅向 ServiceManager 注册服务，阻塞式 Binder 循环放到 main() 中统一处理
void YourService::instantiate() {
    BinderService<YourService>::instantiate();
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
