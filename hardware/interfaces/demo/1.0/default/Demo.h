#ifndef ANDROID_HARDWARE_DEMO_V1_0_DEMO_H
#define ANDROID_HARDWARE_DEMO_V1_0_DEMO_H

#include <android/hardware/demo/1.0/IDemo.h>
#include <android/hardware/demo/1.0/IDemoCallback.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>

namespace android {
namespace hardware {
namespace demo {
namespace V1_0 {
namespace implementation {

using ::android::sp;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;

struct Demo : public IDemo {
    Demo();
    Return<Result> setValue(uint32_t value) override;
    Return<void> getValue(getValue_cb _hidl_cb) override;
    Return<Result> setCallback(const sp<IDemoCallback>& cb) override;
    Return<void> getStatus(getStatus_cb _hidl_cb) override;

  private:
    uint32_t mValue;
    uint32_t mCounter;
    sp<IDemoCallback> mCallback;
};

}  // namespace implementation
}  // namespace V1_0
}  // namespace demo
}  // namespace hardware
}  // namespace android

#endif  // ANDROID_HARDWARE_DEMO_V1_0_DEMO_H
