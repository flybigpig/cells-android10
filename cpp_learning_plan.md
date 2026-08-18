# C++ 学习路径（有 C 基础 + 现代 C++ + SurfaceFlinger 实战）

目标读者：已懂 C（指针、结构体、堆内存、makefile），没写过 C++ 的类和 STL。
学习方式：每个现代 C++ 主题先讲概念，再配一个本仓库 SurfaceFlinger 的真实代码例子。

## 模块顺序

1. 从 C 结构体到 C++ 类（struct/class、access、ctor/dtor、this）
2. 内存与 RAII（栈对象、析构即清理 —— 替代 C 的手动 free）
3. 引用 vs 指针（为什么 C++ 多用引用）
4. 构造函数进阶：成员初始化列表、=default/=delete、explicit
5. 继承与多态（virtual、override、纯虚接口 —— 对应 LayerFE）
6. 智能指针（unique_ptr / shared_ptr / weak_ptr —— 对应 make_shared 那轮）
7. STL 容器与算法（vector / map / set / unordered_map / algorithm）
8. 模板与 auto（泛型、类型推导）
9. Lambda 与 std::function / std::bind（对应 makeResyncCallback 那轮）
10. 移动语义与右值引用（std::move、避免拷贝）
11. 头文件与编译模型（include guard、forward declaration、.h/.cpp 分离）
12. 并发与原子（std::atomic、线程 —— 对应 MessageQueue/EventThread）

## 每个模块 = 概念 + 仓库实例 + 小练习

你选择了「有 C 基础」+「现代 C++ 系统课 + 结合本仓库实战」两条路线并行。这正好：用现代 C++ 概念补齐你缺的类/STL/智能指针，再用我们已经读过的 SurfaceFlinger 代码当现成教材,所见即所得。

我先把学习路径规划写成文件，再按模块展开。
下面开始**模块 1–3**（最基础、能立刻上手的部分）。我会把 C 你已经懂的东西作为跳板。

---

## 模块 1：从 C 结构体到 C++ 类

你熟悉的 C 写法：

```c
struct Point { int x; int y; };
void move(struct Point* p, int dx, int dy) { p->x += dx; p->y += dy; }
```

C++ 把「数据 + 操作数据的函数」包进同一个结构，函数叫**成员函数**（method），用 `this` 指向调用者：

```cpp
struct Point {
    int x = 0;   // C++11 起成员可直接给默认初值
    int y = 0;
    void move(int dx, int dy) { x += dx; y += dy; }  // 隐含 this->x
};
Point p; p.move(2, 3);   // 调用时 this 指向 p
```

**class 与 struct 的唯一区别**是默认访问权限：`struct` 默认 `public`（谁都能访问），`class` 默认 `private`（只有类内可访问）。习惯上数据隐藏用 `class`，纯数据用 `struct`。

**构造函数 / 析构函数**：名字与类同名、无返回值的叫构造函数，对象创建时自动跑；名字前加 `~` 的叫析构函数，对象销毁时自动跑。

```cpp
class Buffer {
    char* data;
public:
    Buffer(size_t n) { data = new char[n]; }   // 构造：申请内存
    ~Buffer()      { delete[] data; }          // 析构：释放内存
};
```

**仓库实例**——`DisplayDevice`（你之前读过的 `DisplayDevice.h:62`）就是典型的 C++ 类：把显示设备的状态（宽高、layerStack、变换、HWC 后端指针）和 `getCompositionDisplay()`、`setProjection()` 等操作封装在一起，`DisplayDevice` 构造函数里装配 `mCompositionDisplay`，析构时释放。这正是 C 里「结构体 + 一堆全局函数」的升级写法。

> 小练习：把上面 C 的 `Point` 改成 C++ class，加一个 `distanceTo(const Point& other)` 成员函数，用 `this` 访问自己。

---

## 模块 2：内存与 RAII（C 程序员最该建立的新直觉）

C 里你 `malloc` 就必须记得 `free`，漏了就内存泄漏。C++ 的核心惯用法叫 **RAII**（Resource Acquisition Is Initialization，资源获取即初始化）：**把资源绑到一个对象的生命周期上，对象构造时拿资源，析构时自动还**。这样无论函数是正常 return、还是抛异常、还是中途 break，资源都会被清理——你不用到处写 `free`。

```cpp
void old_c_style() {
    FILE* f = fopen("a.txt", "r");
    if (something) { fclose(f); return; }  // 容易漏一路
    // ... fclose(f) 还要写一遍
}
// C++ RAII：fstream 析构自动关文件
void new_style() {
    std::ifstream f("a.txt");
    if (something) return;   // f 离开作用域，自动 fclose
}
```

这对 SurfaceFlinger 这种「长期运行、到处持有 buffer/fence/显示句柄」的服务至关重要——Android 大量用 RAII 包装 `sp<>`（强指针）、`unique_ptr` 来避免泄漏。你之前看到的 `std::make_shared<FenceTime>` 就是让 fence 的引用计数随对象析构自动归零、自动释放。

> 关键转变：从「我什么时候 free」变成「这个资源该属于哪个对象，让它替我管」。

---

## 模块 3：引用 vs 指针

C 只有指针，C++ 多了**引用**（`Type&`），它是「已绑定、不能为 null、不能重新指向」的别名。函数传参时首选引用，比指针更安全、语义更清晰：

```cpp
void scale(Point& p, int k) { p.x *= k; p.y *= k; }  // 改的是实参本身，语法像值传递
Point p; scale(p, 2);   // 不用写 &p 和 -> ，但效果和传指针改原值一样
```

规则：
- 大对象传参用 `const Type&`（只读，避免拷贝，又不怕被改）；需要修改原值用 `Type&`。
- 可能为空、需要重新指向时用指针。
- 你之前读 `SurfaceFlinger::init()` 里 `getDefaultDisplayDeviceLocked()` 返回的就是 `DisplayDevice*`（可能为空，所以要指针 + FATAL 校验）；而 `const auto& [token, display]` 用 `const auto&` 遍历 map 避免拷贝整个 pair。

> 小练习：写一个 `swap(int& a, int& b)` 用引用交换两数，对比 C 里 `void swap(int* a, int* b)` 的调用写法。

---


继续展开**模块 4–6**。这三个模块是你从「能写 C++ 类」跨到「能读懂 SurfaceFlinger」的关键门槛。

---

## 模块 4：构造函数进阶

### 成员初始化列表（member initializer list）
C 里你在函数体里 `data = malloc(n)`。C++ 推荐在构造函数冒号后、函数体前初始化成员，效率更高（直接构造，而非先默认构造再赋值）：

```cpp
class Buffer {
    char* data;
    size_t len;
public:
    Buffer(size_t n) : data(new char[n]), len(n) {}  // 冒号后初始化
};
```

仓库实例——`DisplayDevice` 的构造函数（`DisplayDevice.cpp`）就用长长一串初始化列表装配所有成员：`mCompositionDisplay(...)`、`mDisplaySurface(...)`、`mIsSecure(...)` 等，全部在冒号后完成，函数体只做少量后续装配。

### `= default` / `= delete`
```cpp
Buffer() = default;                 // 让编译器生成默认构造，不用手写空函数体
Buffer(const Buffer&) = delete;     // 禁止拷贝（比如管理独占资源的对象不该被浅拷贝）
```
`= delete` 在 Android 里极常见：管理底层资源/单例的类（如 `Scheduler`、`SurfaceFlinger` 的核心成员）常删掉拷贝构造和拷贝赋值，防止意外复制。你之前读的 `Scheduler` 里很多内部状态就是不可拷贝的。

### `explicit`（防止隐式转换）
```cpp
explicit Buffer(size_t n);   // 禁止 Buffer b = 10; 这种隐式把 int 转成 Buffer
```
构造只有一个参数的类时几乎总是加 `explicit`，避免编译器偷偷做类型转换引入难查的 bug。

### 拷贝 vs 移动（先预告，模块 10 细讲）
C++ 默认会给类生成「拷贝构造」「拷贝赋值」。若类管理资源（指针、文件），默认的浅拷贝会**双重释放**——所以要么自己写深拷贝，要么 `= delete`。

---

## 模块 5：继承与多态（对应 `LayerFE`）

你懂 C 的结构体嵌套，但 C 没有「子类自动拥有父类成员 + 可当父类用」的机制。C++ 继承解决这个：

```cpp
struct Animal { virtual void speak() = 0; };   // 纯虚函数 = 接口，=0 表示无实现
struct Dog : Animal { void speak() override { /* 汪 */ } };  // override 显式覆盖
struct Cat : Animal { void speak() override { /* 喵 */ } };

Animal* a = new Dog();   // 父类指针指向子类对象 —— 多态
a->speak();              // 实际调 Dog::speak()，由虚表在运行时决定
```

三个关键字：
- `virtual`：声明「这个函数可能在子类被重写，调用时看实际对象类型」。
- `= 0` 纯虚：强制子类必须实现，带纯虚函数的类叫**抽象类**，不能实例化——就是 C++ 的「接口」。
- `override`：告诉编译器「我意图重写父类虚函数」，写错签名会编译报错（强烈建议永远加）。

**仓库实例**（你第一轮梳理的核心结构）：
```
compositionengine::LayerFE        // 纯虚接口（=0 的虚函数）
        ▲ virtual
   Layer                            // 抽象基类
        ▲
   BufferLayer / ColorLayer / ContainerLayer
        ▲
   BufferQueueLayer / BufferStateLayer
```
`LayerFE` 就是 C++ 接口：定义「合成前端该提供什么能力」。`Layer` 及其子类各自 `override` 这些虚函数（如 `prepareClientLayer`、`getCompositionLayer`），而合成主流程 `handleMessageRefresh` 只需持有 `LayerFE*`/`Layer*`，调用虚函数就能多态地驱动任意具体图层——这正是为什么 SurfaceFlinger 能统一处理 buffer 图层、纯色图层、容器图层，而不用写一堆 `if (type == COLOR)`。

> 小练习：写一个 `Shape` 接口（纯虚 `area()`），派生 `Circle` 和 `Rectangle`，用父类指针数组遍历打印面积。

---

## 模块 6：智能指针（彻底讲透你之前的 `make_shared` 疑问）

C 里你 `malloc`/`free` 全手动。C++ 用**智能指针**把「堆对象的生命周期」交给引用计数或独占语义自动管理，本质就是 RAII 套在指针上。三种：

### `unique_ptr<T>` —— 独占所有权
同一时刻只有一个 `unique_ptr` 指向该对象，离开作用域自动 `delete`。不能拷贝，只能 `std::move` 转移：
```cpp
auto p = std::make_unique<Buffer>(1024);  // C++14 起
// p 离开作用域 → 自动 delete，零泄漏
```
适合「这东西归我独有」的场景。SurfaceFlinger 里很多内部持有、不共享的对象用 `unique_ptr`。

### `shared_ptr<T>` —— 共享所有权（引用计数）
多个 `shared_ptr` 可指向同一对象，**最后一个销毁时才释放**。你之前那轮搜到的 `make_shared` 全是它：
```cpp
auto layer = std::make_shared<compositionengine::Layer>(...);  // 引用计数 = 1
auto alias = layer;   // 引用计数 = 2，不会拷贝对象
// 两个都离开作用域 → 计数归零 → 自动 delete
```
**为什么 `make_shared` 优于 `shared_ptr<T>(new T(...))`？**
1. 只分配**一次**内存（对象 + 引用计数块合并），而 `shared_ptr<T>(new T)` 是两次分配（先 `new T`，再为控制块分配）。
2. 异常安全：若构造参数里另一个 `new` 抛异常，`shared_ptr<T>(new T)` 可能泄漏，而 `make_shared` 不会。

仓库实例——`CompositionEngine/src/Layer.cpp:32` 的 `return std::make_shared<Layer>(...)` 创建后端图层，`Scheduler.h` 的 `mPrimaryVsyncState{std::make_shared<VsyncState>(*this)}`（你上次看的 `makeResyncCallback` 捕获的就是它的 `weak_ptr`）。

### `weak_ptr<T>` —— 不增加计数的旁观者
`weak_ptr` 指向 `shared_ptr` 管理的对象，但**不增加引用计数**，用来打破循环引用、或「我想用但不拥有」。`lock()` 尝试升级成 `shared_ptr`（若对象还活着就成功，否则返回空）：
```cpp
std::weak_ptr<VsyncState> wp = mPrimaryVsyncState;
if (auto sp = wp.lock()) { sp->resync(...); }  // 对象还在才操作
```
这正是 `makeResyncCallback`（`Scheduler.cpp:267`）的做法——回调捕获 `weak_ptr`，调用时 `ptr.lock()`，避免回调持有强引用导致 `VsyncState` 永远不释放（循环引用泄漏）。

> 对应关系：你之前问 `makeResyncCallback` 那段，现在应该串起来了——它用 `weak_ptr` 安全持有主显示 VSync 状态，用 `shared_ptr` 的 `FenceTime` 管理 fence 生命周期，全部是智能指针在替你管内存。

> 小练习：用 `shared_ptr` 模拟一个「图层被 DisplayDevice 和 LayerStack 同时引用」的场景，打印引用计数变化；再改成一用 `weak_ptr` 打破假设会出现的循环引用。

---


继续展开**模块 7–9**（STL 容器、模板与 auto、Lambda 与 `std::bind`）。这三个是读懂现代 C++ 代码「长什么样」的关键。

---

## 模块 7：STL 容器与算法

C 里你用数组 + `malloc` 数组 + 手写循环。C++ 标准库（STL）提供现成、安全、经过优化的容器，免你重造轮子。最常用的几个：

### `std::vector<T>` —— 动态数组（最常用）
```cpp
std::vector<int> v = {1, 2, 3};
v.push_back(4);              // 尾部追加，自动扩容
v.emplace_back(5);           // 比 push_back 更高效：原地构造，不拷贝
for (int x : v) { /* 范围 for，遍历每个元素 */ }
```
`emplace_back` 在 Android 源码里遍地都是（如 `mDisplays` 装配、`LayerHistory` 存 `LayerInfo`）。它直接在容器尾部构造对象，省一次临时对象拷贝——这正是现代 C++ 强调的「避免无谓拷贝」。

### `std::map` / `std::unordered_map` —— 键值对
```cpp
std::map<int, DisplayDevice*> displays;          // 红黑树，按 key 有序
std::unordered_map<int, sp<Layer>> layerMap;     // 哈希表，O(1) 查找，无序
displays[1] = device;
auto it = displays.find(1);
```
**仓库实例**——你之前读的 `handleMessageRefresh` 里：
```cpp
for (const auto& [token, display] : mDisplays) { ... }
```
`mDisplays` 就是 `std::map`（或类似关联容器），`[token, display]` 是 C++17 的**结构化绑定**，把 pair 的 `first`/`second` 直接拆成两个变量名，比写 `it->first`/`it->second` 清爽得多。

### `std::set` —— 去重有序集合
用于「不重复的元素集合」，如 `mLayersWithQueuedFrames`（你之前注释里提到的「有队列帧的图层集合」）就适合用 `set` 或 `vector` 管理。

### 算法 `<algorithm>`
```cpp
std::sort(v.begin(), v.end());
auto n = std::count_if(v.begin(), v.end(), [](int x){ return x > 3; });
```
别手写排序/查找——STL 算法通常更快且经过充分测试。SurfaceFlinger 里 `rebuildLayerStacks` 按 Z 序排序可见图层，底层就依赖这类排序。

---

## 模块 8：模板与 auto（类型推导）

### 模板 —— 让函数/类「对任意类型工作」
你已经懂 C 的 `void*` 泛型（不安全、要强转）。C++ 模板在编译期生成对应类型的代码，类型安全：
```cpp
template <typename T>
T max(T a, T b) { return a > b ? a : b; }
max(3, 5);        // 编译器推导出 T=int
max(1.2, 3.4);    // T=double，生成另一份代码
```
STL 容器本身就是模板：`vector<int>`、`map<int, DisplayDevice*>`。你之前看到的 `std::make_shared<Layer>` 的 `<Layer>` 也是模板参数——指定要创建的对象类型。

### `auto` —— 让编译器推导类型
当你不想（或很难）写出完整类型时，用 `auto` 交给编译器：
```cpp
auto layer = std::make_shared<compositionengine::Layer>(...);  // 类型自动是 shared_ptr<Layer>
// 没有 auto 你得写：std::shared_ptr<compositionengine::Layer> layer = ...
```
配合 `const` 和引用：`const auto&` 表示「只读、不拷贝的引用」（你之前在 `for (const auto& [token, display] : mDisplays)` 见过）。`auto` 在链式调用和迭代器场景几乎是必用，否则类型名会非常冗长。

> 注意：`auto` 不是「动态类型」，C++ 仍是静态类型，只是编译器帮你写类型名。

---

## 模块 9：Lambda 与 `std::bind`（讲透 `makeResyncCallback` 那段）

### Lambda —— 匿名内联函数
C 没有函数嵌套，要用回调得声明独立函数或函数指针。C++ Lambda 让你在调用处直接写一小段逻辑：
```cpp
auto doubler = [](int x) { return x * 2; };   // [] 捕获列表，() 参数，{} 函数体
doubler(21);   // 42
```
**捕获列表** `[...]` 决定 Lambda 能访问哪些外部变量：
- `[]` 不捕获任何东西
- `[=]` 按值捕获所有用到的外部变量
- `[&]` 按引用捕获
- `[x, &y]` 混用
- `[this]` 捕获当前对象（成员函数里写 Lambda 几乎总需要）

**仓库实例**——你上次问的 `makeResyncCallback`（`Scheduler.cpp:267`）：
```cpp
return [ptr, getVsyncPeriod = std::move(getVsyncPeriod)]() {
    if (const auto vsync = ptr.lock()) {
        vsync->resync(getVsyncPeriod);
    }
};
```
拆解：
- `[ptr, getVsyncPeriod = std::move(getVsyncPeriod)]`：捕获 `VsyncState` 的 `weak_ptr`（叫 `ptr`），并把外面传进来的 `getVsyncPeriod` 函数对象**移动**进 Lambda 内部（避免拷贝，呼应模块 10 的移动语义）。
- `()` 无参数：这个回调被调用时不需要传参，内部自己用捕获的 `ptr` 和 `getVsyncPeriod`。
- 函数体：先 `ptr.lock()` 尝试拿到强引用，活着才执行 `resync`——这就是 `weak_ptr` 防泄漏的体现（模块 6 讲过）。

### `std::bind` —— 把函数「绑死」部分参数，生成新可调用对象
你上次看到的另一段：
```cpp
std::bind(&SurfaceFlinger::getVsyncPeriod, this)
```
意思是：把成员函数 `getVsyncPeriod` 和对象 `this` 绑在一起，生成一个「不需要再传对象、直接 callable」的无参函数。等价于一个只调 `this->getVsyncPeriod()` 的 Lambda：
```cpp
auto f = std::bind(&SurfaceFlinger::getVsyncPeriod, this);
f();   // 实际就是 this->getVsyncPeriod()
```
**现代 C++ 更推荐用 Lambda 替代 `std::bind`**（可读性更好、编译器更容易优化），但 Android 老代码大量用 `bind`，所以你得认识它。上面那行用 Lambda 改写就是：
```cpp
auto f = [this]() { return getVsyncPeriod(); };
```

> 现在 `makeResyncCallback` 整段应该完全通了：外层 `std::bind` 把 `getVsyncPeriod` 绑成无参函数 → 内层 Lambda 把它和 `weak_ptr` 一起捕获 → 回调触发时安全地重同步 VSync。

---


继续展开**模块 10–12**（移动语义、头文件与编译模型、并发与原子）。这是最后三个模块，讲完你就具备完整读懂 SurfaceFlinger 现代 C++ 的语法基础了。

---

## 模块 10：移动语义与 `std::move`

C 里赋值/传参就是按字节拷贝（或你自己 `memcpy`）。C++ 默认也是**拷贝**，但对大对象（容器、字符串、智能指针控制块）拷贝很贵。C++11 引入**右值引用 `&&`** 和**移动语义**：把「临时对象」的资源直接「偷」过来，而不是复制。

### 左值 vs 右值（直觉版）
- 左值：有名字、有地址、能取 `&` 的东西（`int a;`、`Layer layer;`）。
- 右值：临时的、即将销毁的（函数返回的临时对象、`42`、`std::make_shared<...>()` 的结果）。

### 移动构造：偷资源而非拷贝
```cpp
std::vector<int> a = {1,2,3};
std::vector<int> b = std::move(a);   // 移动：b 直接接管 a 的内部数组，a 变成空
// 此后 a 不能再安全使用（除非重新赋值），但避免了 3 次元素拷贝
```
`std::move(x)` 本身**不移动任何东西**——它只是把 `x` 强制转换成右值引用，告诉编译器「我允许你移动它」；真正的移动发生在接收方的移动构造函数里。

### 为什么到处用 `std::move`？
- 把局部大对象返回/传入时，移动避免深拷贝。
- 你之前在 `makeResyncCallback` 里见过：`getVsyncPeriod = std::move(getVsyncPeriod)`，把外部传进来的函数对象移动进 Lambda，避免拷贝它（函数对象可能持有状态，拷贝无意义且费）。
- `CompositionEngine` 创建后端对象时 `std::make_shared<Layer>(compositionEngine, std::move(args))`——把构造参数 `args` 移动进新对象，而不是拷贝整个参数包。

### 与模块 4 的呼应
类如果想支持移动，要自己写（或让编译器生成）**移动构造** `T(T&&)` 和**移动赋值** `T& operator=(T&&)`。管理资源的类通常：`= delete` 拷贝（防浅拷贝双释放）、`= default` 或自写移动。这正是 `Scheduler`/`SurfaceFlinger` 内部状态类常见的写法。

> 记忆口诀：`std::move` = 「我不要这个对象的旧身份了，谁要谁拿走它的资源」。

---

## 模块 11：头文件与编译模型

C 里你用 `.h` 声明 + `.c` 实现 + `#include`。C++ 完全一样，但有几个要点避免你踩坑：

### `#include` 守卫 / `#pragma once`
防止同一头文件被重复包含导致「重定义」：
```cpp
// 传统守卫
#ifndef DISPLAY_DEVICE_H
#define DISPLAY_DEVICE_H
... 头文件内容 ...
#endif

// 现代写法（编译器普遍支持，更简洁）
#pragma once
```
你读的每个 `Layer.h`、`DisplayDevice.h` 顶部都有这类保护。

### `.h` 与 `.cpp` 分离（声明 vs 定义）
- `.h`：写类声明、函数签名（告诉编译器「有什么」）。
- `.cpp`：写函数体实现（告诉链接器「具体怎么做」）。
- 你之前读的 `SurfaceFlinger.cpp:621` 的 `void SurfaceFlinger::init()` 就是 `.h` 里声明、`cpp` 里实现的典型分离——`SurfaceFlinger.h` 只有 `void init();`，实现全在 `cpp`。

### 前向声明（forward declaration）—— 减少编译依赖
C 里要用了就得 `#include` 整个头。C++ 可以只声明「这有个类」而不引入完整定义，缩短编译时间：
```cpp
class DisplayDevice;   // 前向声明，不 include 它的头
class SurfaceFlinger {
    DisplayDevice* mDefaultDisplay;   // 指针/引用只需前向声明就够
};
```
Android 大型项目（SurfaceFlinger 上千文件）极度依赖前向声明来避免「改一个头、全项目重编」。`SurfaceFlinger.h` 里大量用前向声明隔离 `Layer`、`DisplayDevice`、`HWComposer` 等。

---

## 模块 12：并发与原子（衔接 MessageQueue / VSync）

C 里你用 `pthread`。C++11 起标准库自带 `<thread>`、`<atomic>`、`<mutex>`，SurfaceFlinger 的线程安全核心就靠它们。

### `std::atomic` —— 无锁原子变量
VSync 主循环里大量标志位需要多线程读写（合成线程写、binder 线程读），但不能用普通 `int`（会有数据竞争）。原子变量保证读写是「不可分割」的：
```cpp
std::atomic<bool> mRefreshPending{false};
mRefreshPending = true;          // 原子写
if (mRefreshPending.exchange(false)) { ... }  // 原子读+写，返回旧值
```
你之前注释过的 `mRefreshPending.exchange(false)`（在 `handleMessageRefresh` 里）就是原子地「取出并清零」——合成线程消费刷新请求时，不会和别的线程的置位操作打架。`mRepaintEverything.exchange(false)` 同理。

### `std::mutex` / `std::lock_guard` —— 互斥锁（RAII 版）
```cpp
std::mutex mStateLock;
{
    std::lock_guard<std::mutex> lock(mStateLock);  // 构造即加锁
    mCurrentState = mDrawingState;                 // 临界区
}                                                     // 离开作用域自动解锁（RAII！）
```
`SurfaceFlinger::init()` 一开头就 `std::lock_guard<std::mutex> lock(mStateLock);`——这正是模块 2 讲的 RAII：加锁对象离开作用域保证解锁，绝不忘记 `unlock`。你之前读的几乎所有 SF 成员函数开头都有这把锁，保护 `mCurrentState`/`mDrawingState` 不被多线程同时改。

### `std::thread` —— 标准线程
Android 里还有自己的 `android::Thread`（你之前读的 `RegionSamplingThread`、`StartPropertySetThread` 都继承它），但它底层就是封装了 `std::thread`/`pthread`。概念一致：把一段逻辑丢到独立线程跑，用 `join()` 等它结束。

### 与合成主流程的衔接
现在你能看懂整条线程安全链路了：
- binder 线程（应用提交事务）→ 原子置 `transactionFlags` / `mRefreshPending` → `mStateLock` 保护状态切换。
- `EventThread` 持有 VSync，`MessageQueue` 收到 VSync 后 `onMessageReceived` 在合成线程跑 `handleMessageTransaction/Invalidate/Refresh`。
- `postComposition` 里用 `std::make_shared<FenceTime>` + 原子标志跟踪每帧 fence，跨线程安全传递上屏时间。

---

## 学习路径收尾

12 个模块全部覆盖。复习建议（结合本仓库）：

1. 重读 `SurfaceFlinger.cpp` 的 `onMessageReceived` / `handleMessageRefresh`（模块 1/5/6/9/12 全用得上）。
2. 重看 `Scheduler.cpp` 的 `makeResyncCallback`（模块 6/8/9 的集合体）。
3. 翻 `DisplayDevice.h` 的构造函数和成员（模块 1/4/11 的实例）。
4. 找几处 `std::move` 和 `emplace_back`（模块 7/10 的实战）。

---

## 全部模块小练习（含参考答案与概念对照）

下面每个练习都可独立编译运行（用 `g++ -std=c++17 xxx.cpp`）。建议每题先自己写，再对照答案。

### 模块 1 练习：C struct → C++ class
**要求**：把 C 的 `Point` 改写成 C++ class，加成员函数 `distanceTo(const Point& other)` 用 `this` 访问自己，并在 `main` 里测试。

<details><summary>参考答案</summary>

```cpp
#include <cmath>
#include <iostream>

class Point {
public:
    int x = 0;
    int y = 0;
    Point() = default;                       // 默认构造
    Point(int x_, int y_) : x(x_), y(y_) {}  // 成员初始化列表（模块 4 预告）
    double distanceTo(const Point& other) const {  // const 成员：不修改自己
        int dx = this->x - other.x;          // 显式用 this 访问成员
        int dy = this->y - other.y;
        return std::sqrt(dx * dx + dy * dy);
    }
};

int main() {
    Point a(0, 0), b(3, 4);
    std::cout << b.distanceTo(a) << "\n";    // 输出 5
}
```
**涉及**：class/struct、成员函数、`this`、默认参数初值、`const` 成员函数。
**对照仓库**：`DisplayDevice` 把状态+操作封装进类，构造函数装配 `mCompositionDisplay`。

</details>

### 模块 2 练习：RAII 管理文件
**要求**：写一个 `FileGuard` 类，构造时 `fopen`、析构时 `fclose`，用它在函数里写文件，验证「中途 return 也不漏关文件」。

<details><summary>参考答案</summary>

```cpp
#include <cstdio>

class FileGuard {
    FILE* f;
public:
    FileGuard(const char* path, const char* mode) : f(std::fopen(path, mode)) {}
    ~FileGuard() { if (f) std::fclose(f); }   // 析构自动关，无论怎么退出
    FILE* get() { return f; }
    FileGuard(const FileGuard&) = delete;      // 禁止拷贝，避免 double fclose
    FileGuard& operator=(const FileGuard&) = delete;
};

void writeSomething() {
    FileGuard fg("demo.txt", "w");
    if (!fg.get()) return;                    // 提前 return，析构仍会 fclose
    std::fprintf(fg.get(), "hello raii\n");
}

int main() { writeSomething(); }
```
**涉及**：RAII、析构、`=delete` 拷贝。
**对照仓库**：`std::make_shared<FenceTime>` 用引用计数 RAII 管理 fence 生命周期。

</details>

### 模块 3 练习：引用交换 vs 指针交换
**要求**：写 `void swap_ref(int& a, int& b)` 和 C 风格的 `void swap_ptr(int* a, int* b)`，在 `main` 里对比调用写法。

<details><summary>参考答案</summary>

```cpp
#include <iostream>

void swap_ref(int& a, int& b) { int t = a; a = b; b = t; }
void swap_ptr(int* a, int* b) { int t = *a; *a = *b; *b = t; }

int main() {
    int x = 1, y = 2;
    swap_ref(x, y);
    std::cout << x << y << "\n";   // 21
    int m = 3, n = 4;
    swap_ptr(&m, &n);
    std::cout << m << n << "\n";   // 43
}
```
**涉及**：引用 vs 指针、调用语法差异。
**对照仓库**：`const auto& [token, display]` 用 const 引用遍历避免拷贝；`getDefaultDisplayDeviceLocked()` 返回 `DisplayDevice*`（可能为空）。

</details>

### 模块 4 练习：初始化列表 + =delete + explicit + 移动
**要求**：写一个 `Buffer` 类，用初始化列表构造，删掉拷贝构造/赋值，`explicit` 单参构造，并提供移动构造。

<details><summary>参考答案</summary>

```cpp
#include <cstddef>
#include <utility>

class Buffer {
    char* data;
    size_t len;
public:
    explicit Buffer(size_t n) : data(new char[n]), len(n) {}
    ~Buffer() { delete[] data; }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept : data(other.data), len(other.len) {
        other.data = nullptr; other.len = 0;
    }
    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) { delete[] data; data = other.data; len = other.len;
                               other.data = nullptr; other.len = 0; }
        return *this;
    }
};

int main() {
    Buffer b(64);
    // Buffer c = b;        // 编译错误：拷贝被删
    Buffer d = std::move(b);  // OK：移动
}
```
**涉及**：成员初始化列表、`explicit`、`=delete`、移动构造（衔接模块 10）。
**对照仓库**：`DisplayDevice` 构造函数用长串初始化列表；`Scheduler` 内部状态类删拷贝。

</details>

### 模块 5 练习：多态 Shape 体系
**要求**：定义纯虚接口 `Shape`（虚 `area()`），派生 `Circle` 和 `Rectangle`，用父类指针数组遍历打印面积。

<details><summary>参考答案</summary>

```cpp
#include <iostream>
#include <vector>

struct Shape { virtual double area() const = 0; virtual ~Shape() = default; };

struct Circle : Shape {
    double r;
    explicit Circle(double r_) : r(r_) {}
    double area() const override { return 3.14159 * r * r; }
};
struct Rectangle : Shape {
    double w, h;
    Rectangle(double w_, double h_) : w(w_), h(h_) {}
    double area() const override { return w * h; }
};

int main() {
    std::vector<Shape*> shapes = {new Circle(2.0), new Rectangle(3.0, 4.0)};
    for (Shape* s : shapes) std::cout << s->area() << "\n";
    for (Shape* s : shapes) delete s;
}
```
**涉及**：纯虚函数、抽象类、`override`、多态、`virtual` 析构。
**对照仓库**：`LayerFE` 纯虚接口 → `Layer` → `BufferLayer/ColorLayer/ContainerLayer` 多态体系。

</details>

### 模块 6 练习：shared_ptr 计数 + weak_ptr
**要求**：用 `shared_ptr` 模拟「图层被两处同时引用」，打印 `use_count()`；再用 `weak_ptr` 改写其中一个持有方，观察计数不再增长。

<details><summary>参考答案</summary>

```cpp
#include <iostream>
#include <memory>

struct Layer { int id; explicit Layer(int i) : id(i) {} };

void demo() {
    auto layer = std::make_shared<Layer>(1);     // count=1
    std::shared_ptr<Layer> holderA = layer;      // count=2
    std::weak_ptr<Layer> holderB = layer;        // count 仍为 2（weak 不增计数）
    std::cout << "use_count=" << layer.use_count() << "\n";   // 2
    if (auto sp = holderB.lock()) std::cout << "locked id=" << sp->id << "\n";
}

int main() { demo(); }
```
**涉及**：`make_shared`、`use_count`、`weak_ptr::lock`。
**对照仓库**：`CompositionEngine::Layer` 用 `make_shared`；`makeResyncCallback` 用 `weak_ptr` 持 `VsyncState`。

</details>

### 模块 7 练习：vector / map / 算法
**要求**：用 `vector` 存图层 Z 值，`emplace_back` 添加，`std::sort` 升序；再用 `unordered_map<int,int>` 存 `layerId→z` 并查找。

<details><summary>参考答案</summary>

```cpp
#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <vector>

int main() {
    std::vector<int> z = {5, 1, 9, 3};
    z.emplace_back(7);
    std::sort(z.begin(), z.end());
    for (int v : z) std::cout << v << " ";  // 1 3 5 7 9
    std::cout << "\n";
    std::unordered_map<int, int> layers = {{10, 5}, {11, 1}, {12, 9}};
    if (auto it = layers.find(11); it != layers.end())
        std::cout << "layer 11 z=" << it->second << "\n";   // 1
}
```
**涉及**：`vector::emplace_back`、范围 for、`std::sort`、`unordered_map::find`。
**对照仓库**：`for (const auto& [token, display] : mDisplays)` 结构化绑定遍历 map。

</details>

### 模块 8 练习：模板 + auto
**要求**：写模板函数 `min_of(T a, T b)`；再用 `auto` 接收 `std::make_shared` 的结果。

<details><summary>参考答案</summary>

```cpp
#include <iostream>
#include <memory>

template <typename T>
T min_of(T a, T b) { return a < b ? a : b; }

struct Dummy { int v; explicit Dummy(int x) : v(x) {} };

int main() {
    std::cout << min_of(3, 5) << "\n";
    std::cout << min_of(1.2, 3.4) << "\n";
    auto d = std::make_shared<Dummy>(42);
    std::cout << d->v << "\n";
}
```
**涉及**：函数模板、类型推导、`auto` 简化类型名。
**对照仓库**：`std::make_shared<Layer>` 的 `<Layer>` 是模板参数；`auto layer = make_shared<...>` 免写长类型。

</details>

### 模块 9 练习：Lambda 捕获 + 改写 bind
**要求**：(a) 写 Lambda 按值捕获 `base` 返回 `x+base`；(b) 把 `std::bind(&Foo::bar, &f)` 改写成等价 Lambda。

<details><summary>参考答案</summary>

```cpp
#include <functional>
#include <iostream>

struct Foo { int k = 10; int bar() const { return k; } };

int main() {
    int base = 100;
    auto add = [base](int x) { return x + base; };
    std::cout << add(5) << "\n";                      // 105
    Foo f;
    std::function<int()> viaBind = std::bind(&Foo::bar, &f);
    auto viaLambda = [&f]() { return f.bar(); };
    std::cout << viaBind() << " " << viaLambda() << "\n";  // 10 10
}
```
**涉及**：Lambda 捕获列表 `[=]`/`[&]`、`std::function`、`std::bind` → Lambda 改写。
**对照仓库**：`makeResyncCallback` 的 `[ptr, getVsyncPeriod = std::move(...)](){}`；`std::bind(&SurfaceFlinger::getVsyncPeriod, this)`。

</details>

### 模块 10 练习：移动语义
**要求**：写 `vector<string>` 场景，把临时 vector `std::move` 给另一个，验证移动后源为空，再重新赋值。

<details><summary>参考答案</summary>

```cpp
#include <iostream>
#include <string>
#include <utility>
#include <vector>

int main() {
    std::vector<std::string> a = {"x", "y", "z"};
    std::vector<std::string> b = std::move(a);
    std::cout << "b size=" << b.size() << "\n";   // 3
    std::cout << "a size=" << a.size() << "\n";   // 0
    a = {"new"};
    std::cout << "a after reassign=" << a.size() << "\n";  // 1
}
```
**涉及**：`std::move`、移动构造偷资源、移动后源可重新赋值。
**对照仓库**：`getVsyncPeriod = std::move(getVsyncPeriod)` 把函数对象移入 Lambda；`make_shared<Layer>(..., std::move(args))`。

</details>

### 模块 11 练习：头文件分离 + 前向声明
**要求**：把 `Display` 拆成 `display.h`+`display.cpp`；在 `surfaceflinger.h` 里只前向声明 `Display` 而非 include，用指针成员。

<details><summary>参考答案</summary>

`display.h`：
```cpp
#pragma once
class Display {
    int width, height;
public:
    Display(int w, int h);
    int area() const;
};
```
`display.cpp`：
```cpp
#include "display.h"
Display::Display(int w, int h) : width(w), height(h) {}
int Display::area() const { return width * height; }
```
`surfaceflinger.h`：
```cpp
#pragma once
class Display;              // 前向声明
class SurfaceFlinger {
    Display* mDisplay;      // 指针成员只需前向声明
public:
    void setDisplay(Display* d);
};
```
**涉及**：`#pragma once`、`.h`/`.cpp` 分离、前向声明减少依赖。
**对照仓库**：`SurfaceFlinger.h` 用前向声明隔离 `Layer`/`DisplayDevice`/`HWComposer`。

</details>

### 模块 12 练习：atomic + mutex
**要求**：用 `atomic<bool>` 模拟刷新标志原子取清零（`exchange`）；用 `mutex`+`lock_guard` 保护多线程计数器自增。

<details><summary>参考答案</summary>

```cpp
#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

int main() {
    std::atomic<bool> pending{true};
    bool old = pending.exchange(false);
    std::cout << "old=" << old << " now=" << pending.load() << "\n";  // 1 0

    std::mutex m;
    int counter = 0;
    auto worker = [&]() {
        for (int i = 0; i < 1000; ++i) {
            std::lock_guard<std::mutex> lock(m);
            ++counter;
        }
    };
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i) ts.emplace_back(worker);
    for (auto& t : ts) t.join();
    std::cout << "counter=" << counter << "\n";    // 4000
}
```
**涉及**：`std::atomic::exchange/load`、`std::mutex`、`std::lock_guard`、`std::thread`、`join`。
**对照仓库**：`mRefreshPending.exchange(false)`、`init()` 开头 `lock_guard(mStateLock)`。

</details>

### 综合小项目（可选，串起全部模块）
写一个迷你「图层合成器」：
- `Layer` 抽象基类（纯虚 `compose()`，模块 5），派生 `BufferLayer`/`ColorLayer`（模块 1/4/5）。
- 用 `std::vector<std::shared_ptr<Layer>>` 管理图层栈（模块 6/7）。
- `Display` 类用 `unique_ptr` 持有后端 buffer、`atomic<bool>` 标记脏（模块 6/12）。
- 用 Lambda 写「按 Z 序排序 + 遍历合成」回调（模块 9）。
- 成员用初始化列表、`=delete` 拷贝（模块 4）。
这基本是 SurfaceFlinger 核心数据结构的玩具版，写完你对前面所有模块会有立体认识。


