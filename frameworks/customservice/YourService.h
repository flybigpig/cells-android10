// YourService.h
#ifndef __YOUR_SERVICE_H__
#define __YOUR_SERVICE_H__

#include <binder/BinderService.h>
#include "IYourService.h"

namespace android {

class YourService :
    public BinderService<YourService>,
    public BnYourService
{
    friend class BinderService<YourService>;

public:
    YourService();
    ~YourService();

    static const char* getServiceName() { return "your.service"; }
    static void instantiate();

private:
    // 内部状态
    int32_t mStatus;
    std::map<String16, String16> mConfigs;
};

}  // namespace android
#endif
