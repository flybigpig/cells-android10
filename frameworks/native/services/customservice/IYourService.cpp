// IYourService.cpp
#define LOG_TAG "YourService"
#include <utils/Log.h>
#include "IYourService.h"

// 服务端实现：展开 META 接口宏，定义 asInterface / descriptor
// descriptor 必须与服务名、客户端 getService 使用的字符串完全一致
namespace android {

IMPLEMENT_META_INTERFACE(YourService, "custom.service");

status_t BnYourService::onTransact(uint32_t code,
                                    const Parcel& data,
                                    Parcel* reply,
                                    uint32_t flags) {
    switch (code) {
        case DO_SOMETHING: {
            CHECK_INTERFACE(IYourService, data, reply);
            String16 param;
            status_t status = data.readString16(&param);
            if (status != NO_ERROR) return status;

            int32_t result;
            status = doSomething(param, &result);
            reply->writeInt32(result);
            return status;
        }

        case GET_STATUS: {
            CHECK_INTERFACE(IYourService, data, reply);
            int32_t status;
            status_t result = getStatus(&status);
            reply->writeInt32(status);
            return result;
        }

        case SET_CONFIG: {
            CHECK_INTERFACE(IYourService, data, reply);
            String16 key, value;
            status_t status = data.readString16(&key);
            if (status != NO_ERROR) return status;
            status = data.readString16(&value);
            if (status != NO_ERROR) return status;

            return setConfig(key, value);
        }

        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

}  // namespace android
