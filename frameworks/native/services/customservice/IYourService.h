// IYourService.h
#ifndef __IYOUR_SERVICE_H__
#define __IYOUR_SERVICE_H__

#include <binder/IInterface.h>
#include <binder/Parcel.h>
#include <utils/String16.h>

// 通信协议接口（手写 Binder，等价于 AIDL 生成的 IYourService）
namespace android {

    // 客户端代理 + 服务端基类共享的接口定义
    class IYourService : public IInterface {
        DECLARE_META_INTERFACE(YourService);

        // 事务码：必须与服务端 onTransact 保持一致
        enum {
            DO_SOMETHING = IBinder::FIRST_CALL_TRANSACTION,
            GET_STATUS   = IBinder::FIRST_CALL_TRANSACTION + 1,
            SET_CONFIG   = IBinder::FIRST_CALL_TRANSACTION + 2,
        };

        // 客户端调用的纯虚接口
        virtual status_t doSomething(const String16& param, int32_t* result) = 0;
        virtual status_t getStatus(int32_t* status) = 0;
        virtual status_t setConfig(const String16& key, const String16& value) = 0;
    };

    // Bn 端：服务端基类，onTransact 负责解包分发
    class BnYourService : public BnInterface<IYourService> {
    public:
        virtual status_t onTransact(uint32_t code,
                                    const Parcel& data,
                                    Parcel* reply,
                                    uint32_t flags = 0);
    };

    // Bp 端：客户端代理，把调用打包成 transact 发往服务端
    class BpYourService : public BpInterface<IYourService> {
    public:
        explicit BpYourService(const sp<IBinder>& impl)
            : BpInterface<IYourService>(impl) {}

        virtual status_t doSomething(const String16& param, int32_t* result) {
            Parcel data, reply;
            data.writeInterfaceToken(getInterfaceDescriptor());
            data.writeString16(param);
            status_t status = remote()->transact(DO_SOMETHING, data, &reply);
            if (status != NO_ERROR) return status;
            *result = reply.readInt32();
            return NO_ERROR;
        }

        virtual status_t getStatus(int32_t* status) {
            Parcel data, reply;
            data.writeInterfaceToken(getInterfaceDescriptor());
            status_t err = remote()->transact(GET_STATUS, data, &reply);
            if (err != NO_ERROR) return err;
            *status = reply.readInt32();
            return NO_ERROR;
        }

        virtual status_t setConfig(const String16& key, const String16& value) {
            Parcel data, reply;
            data.writeInterfaceToken(getInterfaceDescriptor());
            data.writeString16(key);
            data.writeString16(value);
            return remote()->transact(SET_CONFIG, data, &reply);
        }
    };

} // namespace android

#endif  // __IYOUR_SERVICE_H__
