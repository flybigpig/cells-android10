#define LOG_TAG "demo_client"

#include <android/hardware/demo/1.0/IDemo.h>
#include <android/hardware/demo/1.0/IDemoCallback.h>
#include <android/hardware/demo/1.0/types.h>
#include <hidl/HidlSupport.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include <condition_variable>
#include <mutex>
#include <chrono>

using ::android::sp;
using ::android::hardware::demo::V1_0::IDemo;
using ::android::hardware::demo::V1_0::IDemoCallback;
using ::android::hardware::demo::V1_0::Result;
using ::android::hardware::demo::V1_0::DemoStatus;
using ::android::hardware::Return;
using ::android::hardware::Void;

static std::mutex gMutex;
static std::condition_variable gCv;
static bool gCallbackFired = false;

// 客户端实现的异步回调：服务端 setValue 时会被调用
struct DemoCallback : public IDemoCallback {
    Return<void> onValueChanged(uint32_t value) override {
        ALOGI("callback onValueChanged(%u)", value);
        {
            std::lock_guard<std::mutex> lk(gMutex);
            gCallbackFired = true;
        }
        gCv.notify_one();
        return Void();
    }
};

int main(int /* argc */, char* /* argv */ []) {
    // 启动 binder 线程池，用于接收服务端发起的回调事务
    configureRpcThreadpool(1, false /* will not join */);

    sp<IDemo> demo = IDemo::getService("default");
    if (demo == nullptr) {
        ALOGE("Failed to get android.hardware.demo@1.0 service");
        return 1;
    }
    ALOGI("Got demo service, isRemote=%d", demo->isRemote());

    // 1) 同步单返回值
    Result r = demo->setValue(42);
    ALOGI("setValue(42) -> Result=%u", static_cast<uint32_t>(r));

    // 2) 同步多返回值（callback 回传）
    demo->getValue([&](Result res, uint32_t value) {
        ALOGI("getValue() -> Result=%u value=%u", static_cast<uint32_t>(res), value);
    });

    // 3) 注册异步回调
    sp<DemoCallback> cb = new DemoCallback();
    r = demo->setCallback(cb);
    ALOGI("setCallback() -> Result=%u", static_cast<uint32_t>(r));

    // 4) 触发异步回调（setValue 会通知已注册的回调）
    demo->setValue(100);

    // 等待回调在 binder 线程上被调用（最多 2 秒）
    {
        std::unique_lock<std::mutex> lk(gMutex);
        gCv.wait_for(lk, std::chrono::seconds(2), [] { return gCallbackFired; });
    }
    ALOGI("callback fired: %d", gCallbackFired);

    // 5) 返回复合 struct
    demo->getStatus([&](const DemoStatus& s) {
        ALOGI("getStatus() -> ready=%d counter=%u message=%s",
              s.ready, s.counter, s.message.c_str());
    });

    return gCallbackFired ? 0 : 2;
}
