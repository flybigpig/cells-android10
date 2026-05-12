
// IYourService.h

#ifndef __IYOUR_SERVICE_H__
#define __IYOUR_SERVICE_H__

#include <binder/IInterface.h>
#include <binder/Parcel.h>
#include <utils/String16.h>

namespace android{

    class IYourService : public IInterface{

        DECLARE_META_INTERFACE(YourService);
        // 声明事务码
        enum {
            DO_SOMETHING = IBinder::FIRST_CALL_TRANSACTION,
            GET_STATUS = IBinder::FIRST_CALL_TRANSACTION + 1,
            SET_CONFIG = IBinder::FIRST_CALL_TRANSACTION + 2,
        };

        // 纯虚函数 -- 客户端调用
        virtual status_t doSomething(const String16& param, int32_t* result) = 0;
        virtual status_t getStatus(int32_t* status) = 0;
        virtual status_t setConfig(const String16& key, const String16& value) = 0;
    };

    class BnYourService : public BnInterface<IYourService>{
    public :
        virtual status_t onTransact(uint32_t code,
                                       const Parcel& data,
                                       Parcel* reply,
                                       uint32_t flags = 0);
    };

} // namespace android


#endif  // __IYOUR_SERVICE_H__