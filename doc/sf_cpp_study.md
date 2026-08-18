# 从 SurfaceFlinger 源码学 C++（实战对照手册）

目标读者：已懂 C（指针、结构体、堆内存），正通过 Android 10 的 SurfaceFlinger 源码学习现代 C++。
本文与 `cpp_learning_plan.md`（按主题讲概念）和 `modern_cpp_study_plan.md`（按标准归类）互补：**这一篇直接用本仓库的真实代码当教材**，让你看到「课本上的 C++ 语法在 SF 里到底长什么样、为什么这么写」。

所有路径相对 `frameworks/native/services/surfaceflinger/`。建议配合源码一起读。

---

## 0. 先看清 SF 的 C++ 风格底色

SF 不是「教学示例代码」，而是大型长期运行的服务进程，它的代码有几个贯穿全局的 C++ 特征：

- 大量使用 Android 自研智能指针 `sp<>`/`wp<>`（`utils/RefBase.h`），以及标准 `std::shared_ptr`/`unique_ptr`。
- 类继承体系深：`Layer` → `BufferLayer` → `BufferQueueLayer`/`BufferStateLayer`，配合 `virtual`/`override`。
- 强类型枚举 `enum class`、成员默认初值、`=delete` 禁拷贝随处可见。
- 并发靠 `std::mutex`/`std::lock_guard`/`std::atomic`，以及 `android::base` 的线程注解。
- C++17 特性（`optional`、`string_view`、结构化绑定）在较新模块里出现。

记住一条主线：**SF 用「对象生命周期绑定资源（RAII）」+「智能指针自动回收」替代了 C 的手动 malloc/free**，这是读源码时最该建立的新直觉。下面按主题逐个对照。

---

## 1. 类、继承、多态（对应 cpp_learning_plan 模块 1/5）

课本概念：C 用「结构体 + 独立函数」，C++ 把数据和操作包进 `class`，并用继承 + 虚函数实现多态。

### 真实代码：Layer 继承体系

`Layer.h:97` 定义基类，继承并实现一个接口（`compositionengine::LayerFE`）：
```cpp
class Layer : public virtual compositionengine::LayerFE {
    static std::atomic<int32_t> sSequence;
public:
    mutable bool contentDirty{false};      // 成员默认初值（C++11）
    Region visibleRegion;                  // 直接内嵌对象成员（不是指针）
    int32_t sequence{sSequence++};         // 初始化列表式的成员初值
    ...
};
```

派生类 `BufferLayer.h:48` 继承 `Layer`，再被 `BufferQueueLayer`/`BufferStateLayer` 继承，形成多态：
```cpp
class BufferLayer : public Layer {            // 继承
public:
    explicit BufferLayer(const LayerCreationArgs& args);
    virtual ~BufferLayer() override;          // override 显式覆盖
    const char* getTypeId() const override { return "BufferLayer"; }  // 多态：不同子类返回不同字符串
    bool isOpaque(const Layer::State& s) const override;
    ...
};
```

`BufferStateLayer.h:34` 继续派生，满屏 `override`：
```cpp
class BufferStateLayer : public BufferLayer {
    void onLayerDisplayed(const sp<Fence>& releaseFence) override;
    bool shouldPresentNow(nsecs_t expectedPresentTime) const override;
    ...
};
```

### 注解
- `override` 让编译器检查「是不是真的覆盖了基类虚函数」，签名写错直接编译报错（对比 C 里函数指针容易被写错还发现不了）。
- `virtual ~BufferLayer() override;` 虚析构保证「通过基类指针 delete 派生对象」时能正确调用派生析构，是 RAII 安全的基础。
- SF 的图层类型（Buffer/Color/Container）就是用这套多态区分行为，你在 `cpp_learning_plan.md` 模块 5 学的纯虚接口正是这里的缩影。

---

## 2. RAII 与智能指针（对应模块 2/6）

课本概念：把资源（内存、锁、文件、显示句柄）绑定到对象生命周期，构造拿资源、析构还资源。SF 用智能指针把这点做到极致。

### 2.1 Android 自研强指针 `sp<>`（RefBase）

SF 大量使用 `sp<T>`（`strong pointer`，基于 `RefBase` 引用计数），例如 `BufferStateLayer.h` 里：
```cpp
void onLayerDisplayed(const sp<Fence>& releaseFence) override;   // sp<Fence> 引用计数管理 fence 生命周期
bool setAcquireFence(const sp<Fence>& fence) override;
```
`sp<>` 析构时引用计数减一，归零自动释放——这就是 RAII，等价于标准 `shared_ptr` 但为 Android 生态定制。`Client.cpp:80` 里把参数 `std::move(metadata)` 后传给 `createLayer`，避免拷贝。

### 2.2 标准 `std::shared_ptr` / `std::make_shared`

`BufferStateLayer.cpp:395` 用 `make_shared` 创建对象并立即交给智能指针：
```cpp
return std::make_shared<FenceTime>(getDrawingState().acquireFence);
```
`DisplayDevice.h:74` 持有合成显示对象：
```cpp
std::shared_ptr<compositionengine::Display> getCompositionDisplay() const {
    return mCompositionDisplay;   // 返回 shared_ptr，引用计数 +1
}
```

### 2.3 `std::move` 转移而非拷贝（对应模块 10）

`Client.cpp:80` 等处的典型写法：
```cpp
return mFlinger->createLayer(name, String8(""), this, w, h, format, flags,
                             std::move(metadata), handle, gbp, ...);   // metadata 被「移走」，不拷贝
```
`Layer.h:81` 的 `LayerCreationArgs` 构造把 `metadata` 用 `std::move` 收入成员：
```cpp
LayerCreationArgs(..., LayerMetadata metadata)
      : ..., metadata(std::move(metadata)) {}   // 移动构造，避免深拷贝整份元数据
```

### 注解
- 凡是你以前在 C 里 `new`/`malloc` 然后到处传指针、最后记得 `free` 的地方，SF 都用智能指针 + `std::move` 替代。
- `make_shared` 一步完成「分配 + 引用计数对象构造」，比手写 `shared_ptr<T>(new T)` 异常安全且少一次分配。
- 这条主线贯穿整个 SF：`sp<GraphicBuffer>`、`sp<IBinder>`、`shared_ptr<FenceTime>` 处处是 RAII。

---

## 3. 引用、const、`explicit`、成员初始化列表（对应模块 3/4）

### 3.1 引用做只读参数

`BufferStateLayer.h:57` 这类 getter 用 `const Layer::State&` 传引用，避免拷贝大结构体：
```cpp
uint32_t getActiveWidth(const Layer::State& s) const override { return s.active.w; }
```
`const &` 是 SF 里「只读传参」的默认写法，对应 C 的 `const struct X*` 但语法更安全。

### 3.2 `explicit` 防隐式转换

`DisplayDevice.h:71`：
```cpp
explicit DisplayDevice(DisplayDeviceCreationArgs&& args);   // explicit：禁止 DisplayDeviceCreationArgs 隐式转成 DisplayDevice
```
`BufferLayer.h:50` 同样：`explicit BufferLayer(const LayerCreationArgs& args);`
`explicit` 防止「单参数构造被编译器偷偷当类型转换用」导致的诡异 bug。

### 3.3 成员初始化列表 / 默认初值

`Layer.h:101` 直接用成员默认初值（C++11 起）：
```cpp
mutable bool contentDirty{false};
int32_t sequence{sSequence++};
```
`LayerCreationArgs` 用构造函数初始化列表（`: flinger(flinger), ...` 那串）初始化所有成员，比在构造函数体内赋值更高效（尤其对非平凡类型）。

### 注解
- C 程序员习惯传指针，C++ 更常用 `const T&` 表达「我只读你、不还给你所有权」。
- 单参数构造函数几乎总是该加 `explicit`，这是 SF 代码规范。

---

## 4. STL 容器与算法（对应模块 7）

SF 头文件 `<map>`、`<unordered_map>`、`<vector>`、`<set>`、`<queue>`、`#include` 一大片（`SurfaceFlinger.h:68-77`），说明它重度依赖标准容器。

典型用法（结合你已见过的结构化绑定）：
```cpp
// SurfaceFlinger.h 里持有显示列表，遍历时用 C++17 结构化绑定拆 key/value
for (const auto& [token, display] : mDisplays) {   // token 是键，display 是值
    display->prepareFrame(...);
}
```
`LayerVector.h/.cpp` 是 SF 自定义的「按 Z 序 + sequence 排序的图层向量」，内部基于 `std::vector` + 自定义比较，对应你 `mini_compositor.cpp` 里用 Lambda 给 `vector<shared_ptr<Layer>>` 按 Z 排序。

### 注解
- C 里你手写链表/动态数组（`malloc` + `realloc`），SF 直接用 `std::vector`/`std::map`，自动扩容、自动析构释放元素。
- 排序、查找用 `<algorithm>`（`std::sort`/`std::find_if`），不用自己写循环。

---

## 5. Lambda 与 `std::function`（对应模块 9）

SF 把 Lambda 当「内联回调」大量使用。`SurfaceFlinger` 的 `MessageQueue`/`EventThread` 用 Lambda 表达「VSync 到了执行什么」。你之前读过的 `makeResyncCallback` 就是典型：
```cpp
// 伪结构（概念来自 Scheduler 的回调封装）
auto callback = [ptr, getVsyncPeriod = std::move(getVsyncPeriod)]() {
    return getVsyncPeriod();   // 捕获 ptr 和「被移动的 getter」，零拷贝持有
};
```
捕获列表里 `getVsyncPeriod = std::move(...)` 把可调用对象「移入」闭包，避免引用外部临时对象导致悬空。这正是模块 9 + 模块 10 的组合技。

### 注解
- Lambda 在 SF 里替代了 C 的「函数指针 + void* 上下文」写法，类型安全、能捕获局部变量。
- 注意按引用 `[&]` 捕获的变量，生命周期必须长于 Lambda 本身（SF 的回调常配合 `sp<>` 延长生命周期）。

---

## 6. 并发：原子与互斥（对应模块 12）

`SurfaceFlinger.h` 大量用 `std::atomic` 和 `std::mutex` + `std::lock_guard`（RAII 锁）。

```cpp
// 概念来自 SurfaceFlinger 的刷新标志
std::atomic<bool> mRefreshPending{false};
// 取清零（原子地读旧值并置 false），VSync 信号到达时调用
bool wasPending = mRefreshPending.exchange(false);

// mStateLock 保护 mCurrentState / mDrawingState，典型加锁：
void SurfaceFlinger::init() {
    std::lock_guard<std::mutex> lock(mStateLock);   // 离开作用域自动解锁（RAII！）
    ...
}
```
`SurfaceFlinger.h:65-77` 直接 `#include <atomic>`、`<mutex>`、`<thread>`，证实并发是 SF 的一等公民。`EventThread` 用 `std::thread` 跑 VSync 分发循环。

### 注解
- `lock_guard` 把「加锁/解锁」绑定到作用域，忘写 `unlock` 也不怕——又是 RAII。
- `atomic` 用于「单标志位、无需加锁」的场景（如刷新待处理、vsync 启用），比 `mutex` 轻量。

---

## 7. C++17 实战特性（对应 cpp_learning_plan 模块 13）

### 7.1 `std::optional` —— 表示「可能没有」

`Layer.h:42` `#include <optional>`，`DisplayDevice.h:23` 同样包含。`Layer` 的很多查询返回「可能没有结果」时用它，替代 C 的「返回 NULL/特殊值」。

### 7.2 `std::string_view` —— 只读零拷贝字符串

`DisplayHardware/DisplayIdentification.h:23` 直接用：
```cpp
#include <string_view>
std::string_view displayName;     // 成员：只读看显示名，不拥有、不拷贝
```
`DisplayIdentification.cpp` 里用 `byte_view = std::basic_string_view<uint8_t>` 解析 EDID 字节流，`parseEdidText` 从字节视图切出文本全程零拷贝；`frameworks/native/cmds/cmd/main.cpp` 把命令行参数收进 `std::vector<std::string_view>`。

### 7.3 结构化绑定

`SurfaceFlinger` 遍历 `mDisplays` 等 map 时使用 `for (const auto& [token, display] : mDisplays)`，把 `pair` 的 `first`/`second` 直接拆成两个具名变量。

### 7.4 `[[nodiscard]]`

SF 中返回错误码 / 资源的函数常标 `[[nodiscard]]`（AOSP 惯例，对应 `android::base::Result` 体系），忽略返回值即编译警告，防止「分配失败/错误码没处理」这类 C 时代最隐蔽的 bug。

### 注解
这三点已在 `cpp_learning_plan.md` 模块 13 详细展开并配练习，这里只是把「仓库里真实出现的位置」指给你，读源码时直接对应即可。

---

## 8. 枚举、头文件守卫、前向声明（对应模块 4/11）

### 8.1 `enum class` 强类型枚举

`SurfaceFlinger.h:117`：
```cpp
enum class DisplayColorSetting : int32_t {
    MANAGED = 0,
    UNMANAGED = 1,
    ENHANCED = 2,
};
```
`Layer.h:113` 用匿名 enum 定义标志位（`eDontUpdateGeometryState` 等）。SF 里 `HWComposer::DisplayType`、`VsyncPeriod` 等全是 `enum class`——作用域限定、不隐式转 int，比 C 的裸 enum 安全。

### 8.2 头文件守卫 / `#pragma once`

`Layer.h:17` 用传统宏守卫：
```cpp
#ifndef ANDROID_LAYER_H
#define ANDROID_LAYER_H
...
#endif
```
而 `BufferLayer.h:17` 用更简洁的 `#pragma once`。两者目的相同：防止头文件被重复包含导致重定义。

### 8.3 前向声明减少依赖

`SurfaceFlinger.h:84-94` 大量前向声明：
```cpp
class Client;
class EventThread;
class HWComposer;
class Layer;
...
```
只在头文件里用到「指针/引用」时声明类名，不必 `#include` 整个头，能大幅加快编译（SF 这种巨型项目编译速度是性命攸关的事）。`cpp_learning_plan.md` 模块 11 讲的「头文件与编译模型」在这里得到印证。

---

## 9. 把 SF 读薄：一张「C → C++ 迁移对照表」

| C 习惯 | C++ / SF 做法 | 出现在哪 |
| --- | --- | --- |
| `malloc`/`free` 手动管理 | `sp<>`/`shared_ptr`/`unique_ptr` + RAII | `Client.cpp`、`BufferStateLayer.cpp` |
| 结构体 + 独立函数 | `class` 成员函数、继承多态 | `Layer`/`BufferLayer` 体系 |
| 裸 `enum` 隐式转 int | `enum class` 强类型枚举 | `SurfaceFlinger.h:117` |
| `char*` + 长度 | `std::string_view` 只读视图 | `DisplayIdentification.h` |
| 返回 NULL 表失败 | `std::optional<T>` | `Layer.h`/`DisplayDevice.h` |
| `pthread_mutex_lock/unlock` | `std::mutex` + `lock_guard`（RAII） | `SurfaceFlinger::init()` |
| 函数指针 + `void*` 上下文 | Lambda + `std::function` | `makeResyncCallback` 等 |
| 链表/动态数组手写 | `std::vector`/`std::map` | `SurfaceFlinger.h` 容器 |
| `NULL`/`0` 空指针 | `nullptr` | 全局 |
| 忽略返回值 | `[[nodiscard]]` | AOSP 惯例 |

---

## 10. 推荐的SF源码阅读顺序（边读边对照上面）

1. `Layer.h` / `Layer.cpp` —— 看基类如何组织状态、虚函数如何被派生类覆盖（模块 1/5）。
2. `BufferLayer.h` → `BufferQueueLayer.h` / `BufferStateLayer.h` —— 看继承链和满屏 `override`（多态）。
3. `DisplayDevice.h` —— 看 `shared_ptr` 成员、`explicit` 构造、`string_view`（模块 4/6/13）。
4. `SurfaceFlinger.h` —— 看容器成员、原子变量、互斥锁、前向声明（模块 7/11/12）。
5. `Client.cpp` 的 `createLayer` 调用链 —— 看 `std::move` 如何贯穿参数传递（模块 10）。
6. `DisplayHardware/DisplayIdentification.cpp` —— 看 `string_view` 真实解析 EDID（模块 13）。

每读到一个语法点，回头对照本文对应小节和 `cpp_learning_plan.md` 的模块，就能把「抽象语法」和「真实工程」缝在一起。

---

## 11. 配套练习索引

- 基础 12 模块练习 + 综合小项目：见 `cpp_learning_plan.md`（含 `mini_compositor.cpp`）。
- C++11/14/17/20 标准练习与示例：见 `modern_cpp_study_plan.md`。
- C++17 实战三特性专讲：`cpp_learning_plan.md` 模块 13（`optional`/`string_view`/`nodiscard`）。

读 SF 时遇到不认识的语法，先在这三篇里搜关键词，基本都能找到对应讲解与仓库出处。
