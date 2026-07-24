#include "Demo.h"
#include <log/log.h>

namespace android {
namespace hardware {
namespace demo {
namespace V1_0 {
namespace implementation {

Demo::Demo() : mValue(0), mCounter(0) {}

Return<Result> Demo::setValue(uint32_t value) {
    mValue = value;
    mCounter++;
    ALOGI("setValue(%u), counter=%u", value, mCounter);
    if (mCallback) {
        mCallback->onValueChanged(mValue);
    }
    return Result::OK;
}

Return<void> Demo::getValue(getValue_cb _hidl_cb) {
    _hidl_cb(Result::OK, mValue);
    return Void();
}

Return<Result> Demo::setCallback(const sp<IDemoCallback>& cb) {
    if (cb == nullptr) return Result::INVALID_ARG;
    mCallback = cb;
    return Result::OK;
}

Return<void> Demo::getStatus(getStatus_cb _hidl_cb) {
    DemoStatus status;
    status.ready = true;
    status.counter = mCounter;
    status.message = "demo service alive";
    _hidl_cb(status);
    return Void();
}

Return<Result> Demo::notifyStatus() {
    if (!mCallback) return Result::NOT_SUPPORTED;
    DemoStatus status;
    status.ready = true;
    status.counter = mCounter;
    status.message = "status update";
    mCallback->onStatusChanged(status);
    return Result::OK;
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace demo
}  // namespace hardware
}  // namespace android
