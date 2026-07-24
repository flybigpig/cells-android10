#define LOG_TAG "atrace_client"

#include <android/hardware/atrace/1.0/IAtraceDevice.h>
#include <android/hardware/atrace/1.0/types.h>
#include <hidl/HidlSupport.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include <vector>

using ::android::sp;
using ::android::hardware::atrace::V1_0::IAtraceDevice;
using ::android::hardware::atrace::V1_0::Status;
using ::android::hardware::atrace::V1_0::TracingCategory;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;

int main(int /* argc */, char* /* argv */ []) {
    // 启动 binder 线程池以完成跨进程 RPC
    configureRpcThreadpool(1, false /* will not join */);

    sp<IAtraceDevice> atrace = IAtraceDevice::getService("default");
    if (atrace == nullptr) {
        ALOGE("Failed to get android.hardware.atrace@1.0 service");
        return 1;
    }
    ALOGI("Got atrace service, isRemote=%d", atrace->isRemote());

    // 1) listCategories：枚举设备扩展的 trace category
    std::vector<hidl_string> names;
    atrace->listCategories([&](const hidl_vec<TracingCategory>& categories) {
        ALOGI("listCategories: %u category(ies)", categories.size());
        for (const auto& c : categories) {
            ALOGI("  - %s : %s", c.name.c_str(), c.description.c_str());
            names.push_back(c.name);
        }
    });

    bool chainOk = !names.empty();

    // 2) enableCategories：用 list 得到的真实 category 回传，验证往返
    if (!names.empty()) {
        hidl_vec<hidl_string> toEnable;
        toEnable.resize(1);
        toEnable[0] = names[0];
        Status en = atrace->enableCategories(toEnable);
        ALOGI("enableCategories(%s) -> Status=%u",
              toEnable[0].c_str(), static_cast<uint32_t>(en));
        // 注：写 sysfs 节点是否成功取决于运行环境权限，
        //     此处只验证调用链（IPC 往返 + Status 返回）已打通。
    }

    // 3) disableAllCategories：关闭全部 tracing 点
    Status dis = atrace->disableAllCategories();
    ALOGI("disableAllCategories() -> Status=%u", static_cast<uint32_t>(dis));
    if (dis != Status::SUCCESS) chainOk = false;

    ALOGI("call chain verified: %s", chainOk ? "yes" : "no");
    return chainOk ? 0 : 2;
}
