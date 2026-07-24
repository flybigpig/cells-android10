// YourService.h
#ifndef __YOUR_SERVICE_H__
#define __YOUR_SERVICE_H__

#include <map>
#include <binder/BinderService.h>
#include "IYourService.h"

// 服务具体实现：同时继承 BinderService（提供 publish/instantiate）与 Bn 端
namespace android {

class YourService :
    public BinderService<YourService>,
    public BnYourService
{
    friend class BinderService<YourService>;

public:
    YourService();
    ~YourService();

    // 注册到 servicemanager 的服务名
    static const char* getServiceName() { return "custom.service"; }

    // 参考 mediaserver：instantiate 仅注册，不进入循环
    static void instantiate();

private:
    // 内部状态示例
    int32_t mStatus;
    std::map<String16, String16> mConfigs;
};

}  // namespace android
#endif
