我用 surfaceflinger 作为实例来讲解 native 服务注册与跨进程调用流程。先定位其入口、注册实现和客户端获取逻辑。
现在读取 surfaceflinger 的注册入口、类声明，以及客户端侧 `ComposerService`、接口 `ISurfaceComposer` 的实现，串起完整调用链。
surfaceflinger 直接继承 `BnSurfaceComposer` 并通过 `addService` 注册。现在看客户端 `ComposerService::getComposerService()` 和 `BpSurfaceComposer` 的一个具体方法（如获取显示），把链路补全。
现在读取客户端单例获取 `getComposerService()`、一个具体的 `Bp` 方法 `createDisplay`、对应的 `Bn::onTransact` 分发，以及服务端 `SurfaceFlinger::createDisplay` 实现，形成完整往返示例。
读取 `Bn::onTransact` 的 `CREATE_DISPLAY` 分支、服务端实现，以及客户端 `connectLocked`（实际 `getService` 的地方）。
读取 `connectLocked()` 以确认客户端通过 `getService("SurfaceFlinger")` 拿到代理：
以 surfaceflinger 为例，完整串一遍 native 服务的「注册 → 获取代理 → 跨进程调用」流程。

## 一、服务端：定义与注册

surfaceflinger 的服务类直接继承 Bn 端接口 `BnSurfaceComposer`，并显式声明注册名：

```171:171:frameworks/native/services/surfaceflinger/SurfaceFlinger.h
class SurfaceFlinger : public BnSurfaceComposer,
```

```247:249:frameworks/native/services/surfaceflinger/SurfaceFlinger.h
    static char const* getServiceName() ANDROID_API {
        return "SurfaceFlinger";
    }
```

进程入口 `main_surfaceflinger.cpp` 在 `flinger->init()`（先完成初始化，避免客户端连上来时状态不全）之后，通过 `defaultServiceManager()->addService` 把服务注册进 servicemanager：

```120:123:frameworks/native/services/surfaceflinger/main_surfaceflinger.cpp
    // publish surface flinger
    sp<IServiceManager> sm(defaultServiceManager());
    sm->addService(String16(SurfaceFlinger::getServiceName()), flinger, false,
                   IServiceManager::DUMP_FLAG_PRIORITY_CRITICAL | IServiceManager::DUMP_FLAG_PROTO);
```

注意它和刚才 `customservice` 模板用的是同一套机制，只是 surfaceflinger 没走 `BinderService::publish()` 封装，而是直接 `addService`，并且显式限制了 Binder 线程数为 4（`setThreadPoolMaxThreadCount(4)` + `startThreadPool()`），主线程则 `flinger->run()` 进入自己的消息循环。

## 二、客户端：获取代理（接口转型）

客户端侧统一收口在 `ComposerService::getComposerService()`，内部用单例 + `connectLocked()` 阻塞式地拿到名为 `"SurfaceFlinger"` 的 Binder：

```67:72:frameworks/native/libs/gui/SurfaceComposerClient.cpp
void ComposerService::connectLocked() {
    const String16 name("SurfaceFlinger");
    while (getInitService(name, &mComposerService) != NO_ERROR) {
        usleep(250000);
    }
    assert(mComposerService != nullptr);
```

`getInitService` 内部等价于 `defaultServiceManager()->getService("SurfaceFlinger")` 再 `interface_cast<ISurfaceComposer>(binder)`，拿到 `sp<ISurfaceComposer>` 后即是一个 `BpSurfaceComposer` 代理。上层真正的业务入口（如 `SurfaceComposerClient`、`DisplayEventReceiver`）都复用这个单例，例如：

```90:98:frameworks/native/libs/gui/SurfaceComposerClient.cpp
/*static*/ sp<ISurfaceComposer> ComposerService::getComposerService() {
    ComposerService& instance = ComposerService::getInstance();
    Mutex::Autolock _l(instance.mLock);
    if (instance.mComposerService == nullptr) {
        ComposerService::getInstance().connectLocked();
```

## 三、一次完整调用往返（以 `createDisplay` 为例）

业务方发起调用：`SurfaceComposerClient::createDisplay` → `ComposerService::getComposerService()->createDisplay(name, secure)`。

代理端 `BpSurfaceComposer::createDisplay` 负责「打包参数 + 发 transact」：

```304:312:frameworks/native/libs/gui/ISurfaceComposer.cpp
    virtual sp<IBinder> createDisplay(const String8& displayName, bool secure)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ISurfaceComposer::getInterfaceDescriptor());
        data.writeString8(displayName);
        data.writeInt32(secure ? 1 : 0);
        remote()->transact(BnSurfaceComposer::CREATE_DISPLAY, data, &reply);
        return reply.readStrongBinder();
    }
```

事务码 `CREATE_DISPLAY` 由 `BnSurfaceComposer` 定义（对应你模板里的 `DO_SOMETHING` 等枚举）。Binder 驱动把这次 transact 投递到 surfaceflinger 进程的 Binder 线程，进入 `BnSurfaceComposer::onTransact` 按 code 分发：

```1007:1013:frameworks/native/libs/gui/ISurfaceComposer.cpp
    switch(code) {
        case CREATE_CONNECTION: {
            CHECK_INTERFACE(ISurfaceComposer, data, reply);
            sp<IBinder> b = IInterface::asBinder(createConnection());
            reply->writeStrongBinder(b);
            return NO_ERROR;
        }
```

（`CREATE_DISPLAY` 分支同理：`CHECK_INTERFACE` 校验接口令牌 → 调用虚函数 `createDisplay()` → 把结果 `writeStrongBinder` 写回 reply。）

最终落到真正的服务实现 `SurfaceFlinger::createDisplay`，返回一个 `BBinder` 形式的 display token：

```459:486:frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp
    sp<IBinder> SurfaceFlinger::createDisplay(const String8 &displayName,
                                              bool secure) {
        class DisplayToken : public BBinder {
            ...
        };
        sp<BBinder> token = new DisplayToken(this);
        Mutex::Autolock _l(mStateLock);
        ...
        mCurrentState.displays.add(token, state);
        mInterceptor->saveDisplayCreation(state);
        return token;
    }
```

这个 `BBinder` token 经 Binder 驱动（支持跨进程传递 Binder 对象）回到客户端 `Bp` 侧，被 `reply.readStrongBinder()` 取回——它本质是 surfaceflinger 进程里那个 `DisplayToken` 的句柄，之后客户端再拿它做 `setPowerMode`、`destroyDisplay` 等二次调用。

## 四、和 `customservice` 模板的对应关系

把这套流程映射到刚优化的模板，一一对应：

- `SurfaceFlinger : BnSurfaceComposer` ↔ `YourService : BnYourService`
- `addService("SurfaceFlinger", flinger)` ↔ `BinderService<YourService>::instantiate()`（内部即 `addService("custom.service", new YourService())`）
- `ComposerService::getComposerService()` ↔ `test_client` 里的 `sm->getService(String16("custom.service"))` + `interface_cast<IYourService>`
- `BpSurfaceComposer::createDisplay` 的 `writeInterfaceToken + transact(CREATE_DISPLAY)` ↔ `BpYourService::doSomething` 的 `writeInterfaceToken + transact(DO_SOMETHING)`
- `BnSurfaceComposer::onTransact` 的 `switch(code)` ↔ `BnYourService::onTransact` 的 `switch(code)`

唯一显著区别是：surfaceflinger 用「直接 `addService` + 主线程 `run()` 自有循环」的模式，而 `customservice` 用的是 `BinderService` 封装 + `joinThreadPool()` 模式；前者适合进程里还要跑自己的消息队列（SF 的 vsync/合成循环），后者是更轻量的纯 Binder daemon 写法。两者注册与调用的 Binder 本质完全一致。