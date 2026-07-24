在分析 HAL 层源码之前，我们先实现一个 Binderized HALs，对 HIDL HAL 有一个相对细节的认知。

Binderized HAL 以进程的形式存在，内部有一个 HwBinder 服务端对象，对外提供 HwBinder 远程调用服务。Framework 通过 HwBinder 远程调用到 HAL 中的函数，这些函数直接访问具体的驱动。

接下来，我们给我们之前自定义的 Product 添加一个 HAL 模块，Product 不太清楚可以参考[添加 Product (opens new window)](https://yuandaimaahao.gitee.io/AndroidFrameworkTutorialPages/002.%E7%8E%A9%E8%BD%ACAOSP%E7%AF%87/003.%20%E6%B7%BB%E5%8A%A0%20Product.html)

## [#](http://ahaoframework.tech/pages/544e09/#hal-%E6%96%87%E4%BB%B6%E5%AE%9E%E7%8E%B0) Hal 文件实现

Hal 层的实现一般放在 hardware、vendor 或者 device 目录下。

我们的示例就放在 vendor 目录下。

创建目录：

```bash
# 系统源码目录下
mkdir -p jelly/hardware/interfaces/hello_hidl/1.0
```



接着在 `vendor/jelly/hardware/interface/hello_hidl/1.0` 目录下创建 Hal 文件

```scss
//定义包名，最后跟一个版本号
package jelly.hardware.hello_hidl@1.0;
//定义 hidl 服务对外提供的接口
interface IHello {
    //for test，generates 后面跟的是返回类型
    addition_hidl(uint32_t a,uint32_t b) generates (uint32_t total);
    //写 hello 驱动
    write(string name) generates (uint32_t result);
    //读 hello 驱动
    read() generates (string name);
};
```


这里的 IHello.hal 定义了我们的服务对外提供了哪些函数。可以认为这就是我们服务的对外协议。协议一般定义好就不会再修改，以保持对外的稳定性。 关于 hal 的写法，可以参考[官方的文档 (opens new window)](https://source.android.com/docs/core/architecture/hidl-cpp)，另外也可以参考 hardware 目录下系统自带的 hal 的写法。

## [#](http://ahaoframework.tech/pages/544e09/#hal-%E6%96%87%E4%BB%B6%E7%94%9F%E6%88%90-cpp-%E4%BB%A3%E7%A0%81) Hal 文件生成 CPP 代码

接着我们使用 hidl-gen 命令将我们写的 hal 文件转换为 C++ 文件：

在系统源码下依次执行下面的命令：

```bash
source build/envsetup.sh
# 项目的完整包名
PACKAGE=jelly.hardware.hello_hidl@1.0 
# 生成代码的存放位置
LOC=vendor/jelly/hardware/interfaces/hello_hidl/1.0/default
# -o 选项指定生成的文件存放的位置
# -Lc++-impl 表示要生成 C++ 代码
# -rjelly.hardware:vendor/jelly/hardware/interfaces 用于指定包名与路径的对应关系
# 最后的 $PACKAGE 指定项目的完整的包名
hidl-gen -o $LOC -Lc++-impl -rjelly.hardware:vendor/jelly/hardware/interfaces $PACKAGE
```


执行完上面的命令后，在 `vendor/jelly/hardware/interfaces/hello_hidl/1.0/default` 目录下会生成 Hello.cpp 和 Hello.h。

接着修改 `vendor/jelly/hardware/interfaces/hello_hidl/1.0/default` 目录下生成的 Hello.cpp:

```cpp
#define LOG_TAG "HELLO_HAL"

#include "Hello.h"
#include <cutils/log.h>


namespace jelly {
namespace hardware {
namespace hello_hidl {
namespace V1_0 {
namespace implementation {

// Methods from ::jelly::hardware::hello_hidl::V1_0::IHello follow.
Return<uint32_t> Hello::addition_hidl(uint32_t a, uint32_t b) {
    ALOGD("addition_hidl....a :%d,b:%d",a,b);
    return uint32_t { a+ b };
}

Return<uint32_t> Hello::write(const hidl_string& name) {
    ALOGD("write %");
    return uint32_t {};
}

Return<void> Hello::read(read_cb _hidl_cb) {
    ALOGD("read");
    return Void();
}


// Methods from ::android::hidl::base::V1_0::IBase follow.

//IHello* HIDL_FETCH_IHello(const char* /* name */) {
    //return new Hello();
//}
//
}  // namespace implementation
}  // namespace V1_0
}  // namespace hello_hidl
}  // namespace hardware
}  // namespace jelly
```


这里就简单打印点信息，实际的 HAL 实现，在这里回去访问具体的驱动程序。

## [#](http://ahaoframework.tech/pages/544e09/#%E6%9C%8D%E5%8A%A1%E7%AB%AF%E5%AE%9E%E7%8E%B0) 服务端实现

接着我们需要写一个 Server 端来向 HwServiceManager 注册我们的服务。在 `vendor/jelly/hardware/interfaces/hello_hidl/1.0/default` 目录下添加 service.cpp：

```cpp
#include <hidl/HidlTransportSupport.h>
#include <utils/Looper.h>
#include <utils/StrongPointer.h>
#include <log/log.h>
#include "Hello.h"

using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;
using jelly::hardware::hello_hidl::V1_0::IHello;
using jelly::hardware::hello_hidl::V1_0::implementation::Hello;

int main() {
    ALOGD("hello-hidl is starting...");

    // 配置 Binder 的线程数
    configureRpcThreadpool(4, true /* callerWillJoin */);

    // 初始化一个 Hello 服务端对象
    android::sp<IHello> service = new Hello();
    // 注册服务
    android::status_t ret = service->registerAsService();

    if (ret != android::NO_ERROR) {
    }

    // 当前线程成为 HwBinder 线程
    joinRpcThreadpool();

    return 0;
    //Passthrough模式
    //return defaultPassthroughServiceImplementation<IHello>(4);
}
```

这里使用的是 libhwbinder 库来做实现，接口上与 libbinder 库大体类似。

我们的服务端需要在开机时启动，创建 `vendor/jelly/hardware/interfaces/hello_hidl/1.0/default/jelly.hardware.hello_hidl@1.0-service.rc` 文件：

```sql
service vendor_hello_hidl_service /vendor/bin/hw/jelly.hardware.hello_hidl@1.0-service
class hal
user system
group system
```



接着我们需要添加 VINTF 对象，对于注册到 hwservicemanager 的服务都需要添加一个 VINTF 对象。对于编码来说 VINTF 对象就是一个 xml 文件，创建 `vendor/jelly/hardware/interfaces/hello_hidl/1.0/default/jelly.hardware.hello_hidl@1.0-service.xml` 文件：

```php-template
<manifest version="1.0" type="device">
  <hal format="hidl">
        <name>jelly.hardware.hello_hidl</name>
        <transport>hwbinder</transport>
        <version>1.0</version>
        <interface>
            <name>IHello</name>
            <instance>default</instance>
        </interface>
    </hal>
</manifest>
```


接着我们使用 hidl-gen 命令来生成对应的 Android.bp 文件：

```bash
# 注意和前面使用同一个终端
hidl-gen -o $LOC -Landroidbp-impl -rjelly.hardware:vendor/jelly/hardware/interfaces $PACKAGE
```



这个命令会在 `vendor/jelly/hardware/interfaces/hello_hidl/1.0/default` 目录下生成一个 Android.bp，我们在生成的基础上稍作修改如下：

```perl
// FIXME: your file license if you have one

cc_library_shared {
    name: "jelly.hardware.hello_hidl@1.0-impl",
    relative_install_path: "hw",
    proprietary: true,
    srcs: [
        "Hello.cpp",
    ],
    shared_libs: [
        "libhidlbase",
        "libhidltransport",
        "libutils",
        "jelly.hardware.hello_hidl@1.0",
        "liblog",
    ],
}

cc_binary {
    name: "jelly.hardware.hello_hidl@1.0-service",
    init_rc: ["jelly.hardware.hello_hidl@1.0-service.rc"],
    // 这种方式添加 vintf ，在 Android11 以后才支持
    vintf_fragments: ["jelly.hardware.hello_hidl@1.0-service.xml"],
    defaults: ["hidl_defaults"],
    relative_install_path: "hw",
    vendor: true,
    srcs: ["service.cpp", "Hello.cpp"],
    shared_libs: [
        "jelly.hardware.hello_hidl@1.0",
        "libhardware",
        "libhidlbase",
        "libhidltransport",
        "libutils",
        "liblog",
    ],
}
```


上面生成的 Android.bp 里面有一个依赖 `jelly.hardware.hello_hidl@1.0`，目前编译系统中还没有这个库，接着我们来生成 `jelly.hardware.hello_hidl@1.0` 库对应的 Android.bp。

在 `hardware/interfaces` 目录下，将 update-makefiles.sh 拷贝到 `vendor/jelly/hardware/interfaces/` 目录下，并修改如下：

```bash
#!/bin/bash

source $ANDROID_BUILD_TOP/system/tools/hidl/update-makefiles-helper.sh

do_makefiles_update \
  "jelly.hardware:vendor/jelly/hardware/interfaces"
```


接着在系统源码目录下执行：

```swift
./vendor/jelly/hardware/interfaces/update-makefiles.sh
```



就会生成 `vendor/jelly/hardware/interfaces/hello_hidl/1.0/Android.bp`：

```yaml
hidl_interface {
    name: "jelly.hardware.hello_hidl@1.0",
    root: "jelly.hardware",
    product_specific: true,
    srcs: [
        "IHello.hal",
    ],
    interfaces: [
        "android.hidl.base@1.0",
    ],
    gen_java: true,
}
```


其中的 hidl\_interface 是 hidl 独有的，当编译源码时，它会将 out/soong/.intermediates/vendor/jelly/hardware/interfaces/hello\_hidl/1.0/jelly.hardware.hello\_hidl@1.0\_genc++/gen/jelly/hardware/hello\_hidl/1.0 和 out/soong/.intermediates/vendor/jelly/hardware/interfaces/hello\_hidl/1.0/jelly.hardware.hello\_hidl@1.0\_genc++\_headers/gen/jelly/hardware/hello\_hidl/1.0 目录下的源码编译为 jelly.hardware.hello\_hidl@1.0.so 文件，并预制到手机的 /vendor/lib 和 /vendor/lib64/ 目录下。

为了使编译通过，新建 `vendor/jelly/hardware/interfaces/Android.bp` 文件：

```css
hidl_package_root {
    name: "jelly.hardware",
    path: "vendor/jelly/hardware/interfaces",
}
```


这个 Android.bp 的作用是告诉编译系统包名与路径的映射关系。

接着新建 vendor/jelly/hardware/interfaces/current.txt 文件，current.txt 记录了所有 hal 接口的 hash 值，接口有变化时，同时需要更新 current.txt 中的 hash 值，这是我们先随便设置一个 hash 值：

```ruby
123456 jelly.hardware.hello_hidl@1.0::IHello
```



再执行一遍 update-makefiles.sh，这个时候就会发现提示 hash 值不正确了，同时会给出正确的 hash 值，我们把正确的 hash 值替换到 current.txt 即可。

最后修改 `device/generic/goldfish/manifest.xml`,在其中添加：

```php-template
<hal format="hidl">
        <name>jelly.hardware.hello_hidl</name>
        <transport>hwbinder</transport>
        <version>1.0</version>
        <interface>
            <name>IHello</name>
            <instance>default</instance>
        </interface>
    </hal>
```


## [#](http://ahaoframework.tech/pages/544e09/#%E5%AE%A2%E6%88%B7%E7%AB%AF%E5%AE%9E%E7%8E%B0) 客户端实现

在 vendor/jelly/hardware/interfaces/hello\_hidl/1.0/default 目录下创建如下的文件和文件夹：

![20240328163932](https://cdn.jsdelivr.net/gh/stingerzou/MyImages@main/images20240328163932.png)

其中 `hello_hidl_test.cpp`：

```cpp
#include <jelly/hardware/hello_hidl/1.0/IHello.h>
#include <hidl/LegacySupport.h>

#define LOG_TAG "hello_hidl"
#include <log/log.h>

using android::sp;
using jelly::hardware::hello_hidl::V1_0::IHello;
using android::hardware::Return;
using android::hardware::hidl_string;

int main(){
    // 获取服务代理端对象
    android::sp<IHello> hw_device = IHello::getService();
    if (hw_device == nullptr) {
              ALOGD("failed to get hello-hidl");
              return -1;
        }
    ALOGD("success to get hello-hidl....");
    // 通过代理端对象发起远程调用
    Return<uint32_t> total = hw_device->addition_hidl(3,4);
    hw_device->write("hello");
    hw_device->read([&](hidl_string result){
        ALOGD("%s\n", result.c_str());
    });
    return 0;
}
```


调用接口上与 binder 大体一致。

Android.bp：

```bash
cc_binary {
    name: "hello_hidl_test",
    srcs: ["hello_hidl_test.cpp"],
    vendor: true,
    shared_libs: [
        "liblog",
        "jelly.hardware.hello_hidl@1.0",
        "libhidlbase",
        "libhidltransport",
        "libhwbinder",
        "libutils",
    ],
}
```


## [#](http://ahaoframework.tech/pages/544e09/#selinux-%E9%85%8D%E7%BD%AE) selinux 配置

在 device/Jelly/Rice14/sepolicy 目录下添加：

hwservice.te：

```bash
type hello_hidl_hwservice, hwservice_manager_type;
```



hello\_hidl.te:

```scss
type hello_hidl, domain;
type hello_hidl_exec, exec_type, vendor_file_type, file_type;

init_daemon_domain(hello_hidl);
add_hwservice(hello_hidl, hello_hidl_hwservice)
hwbinder_use(hello_hidl)

allow hello_hidl hidl_base_hwservice:hwservice_manager { add };
binder_call(hello_hidl,hwservicemanager)
get_prop(hello_hidl,hwservicemanager_prop)
```


hwservice\_contexts：

```cpp
jelly.hardware.hello_hidl::IHello      u:object_r:hello_hidl_hwservice:s0
```



hello\_hidl\_test.te：

```scss
type  hello_hidl_test, domain;
type  hello_hidl_test_exec, exec_type, vendor_file_type, file_type;

domain_auto_trans(shell, hello_hidl_test_exec, hello_hidl_test);

get_prop(hello_hidl_test, hwservicemanager_prop)
allow hello_hidl_test hello_hidl_hwservice:hwservice_manager find;
hwbinder_use(hello_hidl_test);
```


在 file\_contexts 中添加：

```css
/vendor/bin/hw/jelly\.hardware\.hello_hidl@1\.0-service    u:object_r:hello_hidl_exec:s0
```


这些 SELinux 规则描述的是一个标准 HIDL HAL 服务（`jelly.hardware.hello_hidl@1.0`）在 `device/Jelly/Rice14/sepolicy` 下需要的策略。它要解决三件事：让 `init` 把服务进程放进独立 domain、让服务能通过 `hwbinder` 向 `hwservicemanager` 注册自身、让客户端能 `find` 到它。下面按文件逐条说明。

`hwservice.te`
`type hello_hidl_hwservice, hwservice_manager_type;` 声明一个新类型 `hello_hidl_hwservice`，并打上 `hwservice_manager_type` 属性。这是「hwservice 类型」，专用于 hwservicemanager 的名称空间——只有带这个属性的类型，才能出现在 `hwservice_contexts` 里被登记/查找。`hwservice_contexts` 里的那条映射和客户端 `find` 权限都依赖它。

`hello_hidl.te`（服务进程 domain）
- `type hello_hidl, domain;` 声明 HAL 服务运行时的进程 domain。
- `type hello_hidl_exec, exec_type, vendor_file_type, file_type;` 声明服务二进制文件的类型。`exec_type` 是「可执行文件」属性，`vendor_file_type` 表示它在 vendor 分区（因为 HIDL 服务编进 vendor 镜像），`file_type` 是普通文件属性。这个类型要靠 `file_contexts` 把真实路径贴上，否则 exec 转换不生效。
- `init_daemon_domain(hello_hidl);` 关键宏：允许 `init` 在执行 `hello_hidl_exec` 文件时，把进程从 `init` domain 切换到 `hello_hidl` domain。没有它，服务会留在 init 的 domain 里跑，后续所有 `allow hello_hidl ...` 规则都不会命中。
- `add_hwservice(hello_hidl, hello_hidl_hwservice)` 宏：允许 `hello_hidl` 把 `hello_hidl_hwservice` 注册到 hwservicemanager（即调用 `registerAsService()` 时的权限）。
- `hwbinder_use(hello_hidl)` 宏：授予该 domain 使用 `/dev/hwbinder` 设备（open/read/write/ioctl）的权限，是 binder IPC 的底层前提。
- `allow hello_hidl hidl_base_hwservice:hwservice_manager { add };` 允许向基础 hwservice 类型注册。注意：这一条其实**已经被 `add_hwservice` 宏包含**了——该宏展开后除了允许 `hello_hidl_hwservice`，还会允许 `hidl_base_hwservice`。所以这是一条冗余规则，留着无害，删掉也不影响。
- `binder_call(hello_hidl, hwservicemanager)` 宏：允许 `hello_hidl` 与 `hwservicemanager` 双向 binder 通信（服务注册、获取自身句柄都要走它）。
- `get_prop(hello_hidl, hwservicemanager_prop)` 允许服务读取 `hwservicemanager_prop` 类属性（如 `hwservicemanager.ready` 之类），用于判断 hwservicemanager 是否就绪。

`hwservice_contexts`
`jelly.hardware.hello_hidl::IHello u:object_r:hello_hidl_hwservice:s0` 把 HIDL 接口描述符映射到前面声明的 SELinux 类型。hwservicemanager 查表时依据这一行来给服务打标签，客户端 `find` 时也要匹配它。这里有**一个需要修正的点**：在 Android 10 里，`hwservice_contexts` 的 key 用的是含版本的完整描述符，正确写法应为 `jelly.hardware.hello_hidl@1.0::IHello`（缺了 `@1.0`）。可参照 AOSP 现成条目，如 `android.hardware.light@2.0::ILight u:object_r:hal_light_hwservice:s0`。

`hello_hidl_test.te`（客户端/测试进程 domain）
- `type hello_hidl_test, domain;` 和 `type hello_hidl_test_exec, exec_type, vendor_file_type, file_type;` 与服务端对称，声明测试进程的 domain 与其二进制文件类型。
- `domain_auto_trans(shell, hello_hidl_test_exec, hello_hidl_test);` 当你在 `adb shell` 里运行测试二进制时，进程自动从 `shell` domain 切换到 `hello_hidl_test` domain。
- `get_prop(hello_hidl_test, hwservicemanager_prop)` 允许测试读 hwservicemanager 属性。
- `allow hello_hidl_test hello_hidl_hwservice:hwservice_manager find;` 这是客户端最核心的权限——`find` 允许通过 hwservicemanager 查找到已注册的 `hello_hidl_hwservice`，也就是 `IHello::getService("default")` 能成功的前提。
- `hwbinder_use(hello_hidl_test);` 允许测试进程使用 hwbinder 与服务通信。

`file_contexts`
`/vendor/bin/hw/jelly\.hardware\.hello_hidl@1\.0-service u:object_r:hello_hidl_exec:s0` 用正则（点已转义）把服务二进制路径贴上 `hello_hidl_exec` 类型。这是 `init_daemon_domain` 能触发 domain 切换的根基——路径必须先被正确标记。`file_contexts` 用正则，所以原文件名里的 `.` 必须写成 `\.`。

需要注意的遗漏与补全点：

1. `hwservice_contexts` 接口名应带版本 `@1.0`，改成 `jelly.hardware.hello_hidl@1.0::IHello`。
2. `add_hwservice` 已内含对 `hidl_base_hwservice` 的 add 权限，那条 `allow ... hidl_base_hwservice ... add` 是冗余的，可删可不删。
3. 测试 domain 只声明了 `hello_hidl_test_exec`，却**缺少对应的 `file_contexts` 条目**。如果没有把测试二进制路径（如 `/vendor/bin/hw/jelly.hardware.hello_hidl@1.0-client`）标成 `hello_hidl_test_exec`，`domain_auto_trans(shell, ...)` 不会触发，测试进程就跑在 shell domain 下，前面的 `find`/`hwbinder_use` 规则全都用不上。需要补一行 `file_contexts`：`/vendor/bin/hw/jelly\.hardware\.hello_hidl@1\.0-client u:object_r:hello_hidl_test_exec:s0`（按实际产物名调整）。
4. 这套 `.te` 要真正编进镜像，需在 `device/Jelly/Rice14/BoardConfig.mk` 里确保 `BOARD_SEPOLICY_DIRS` 包含 `device/Jelly/Rice14/sepolicy`（通常 device.mk 已加，确认一下即可）。
5. 类型（`type` 声明）写在各自 `.te` 里会被自动注册，无需额外在 `attributes` 文件登记；但 `hwservicemanager_prop` 必须由 system/sepolicy 提供，通常已存在，不用自己定义。

------



## [#](http://ahaoframework.tech/pages/544e09/#%E7%BC%96%E8%AF%91%E6%89%A7%E8%A1%8C) 编译执行

接着在 device/Jelly/Rice14/Rice14.mk 中添加如下内容：

```graphql
BOARD_SEPOLICY_DIRS += \
    device/Jelly/Rice14/sepolicy

PRODUCT_PACKAGES += \
    jelly.hardware.hello_hidl@1.0-service \
    hello_hidl_test \
    jelly.hardware.hello_hidl@1.0-impl \
```


然后整编系统：

```bash
source build/envsetup.sh
lunch rice14-eng
make -j16
```



最后测试：

```ruby
# 执行客户端程序
hello_hidl_test &
# 查看 log
# logcat | grep hello 
3-28 18:31:40.460  1542  1542 D         : hello-hidl is starting...
03-28 18:31:40.461  1542  1542 I ServiceManagement: Registered jelly.hardware.hello_hidl@1.0::IHello/default (start delay of 77ms)
03-28 18:31:40.461  1542  1542 I ServiceManagement: Removing namespace from process name jelly.hardware.hello_hidl@1.0-service to hello_hidl@1.0-service.
03-28 18:32:35.299  3050  3050 D hello_hidl: success to get hello-hidl....
03-28 18:32:35.299  1542  1561 D HELLO_HAL: write hello
03-28 18:32:35.300  3050  3050 D hello_hidl: test
```
