# 现代 C++ 语法学习计划（C++11 / 14 / 17 / 20）

目标读者：已懂 C，正在通过本仓库 SurfaceFlinger 源码学习现代 C++。
本文按**标准版本**重新组织语法特性，作为 `cpp_learning_plan.md`（按主题分模块）的补充视角——那篇讲「概念是什么」，这篇讲「哪些特性属于哪个标准、何时引入、现在该用哪个」。

学习方式：每个标准挑「最常用 + 仓库里真会出现」的特性，附一句仓库对照。

---

## 〇、为什么按标准学

Android 10 的 SurfaceFlinger 编译目标通常是 **C++17**（部分模块 C++14/11 混用），所以你读到的代码里：
- C++11 特性（智能指针、Lambda、auto、移动语义、线程）——满地都是，是地基。
- C++14 特性（泛型 Lambda、`make_unique`）——少量但实用。
- C++17 特性（结构化绑定、`if constexpr`、内联变量、`std::optional`、`emplace` 增强）——你已见过 `for (const auto& [token, display] : mDisplays)` 就是 C++17 结构化绑定。
- C++20 特性（协程、概念 Concepts、Ranges、三路比较 `<=>`）——仓库里基本没有，但了解趋势有助于读新代码。

> 记忆：C++11 是「现代化分水岭」，C++14 是小修，C++17 是大补，C++20 是范式升级。

---

## 一、C++11（2011，现代 C++ 起点，必精通）

### 1.1 自动类型推导
- `auto`：编译器推导变量类型（你模块 8 学过）。仓库：`auto layer = make_shared<Layer>(...)`。
- `decltype`：推导表达式类型，用于模板和 `decltype(auto)` 完美转发场景。

示例：
```cpp
std::vector<int> v{1, 2, 3};
auto it = v.begin();                  // auto 推导为 std::vector<int>::iterator，省去又长又易错的写法
decltype(v.size()) n = v.size();      // decltype 推导表达式类型，n 的类型与 v.size() 完全一致（size_t）
```
注解：`auto` 让你写迭代器/lambda 类型时不再被类型名拖累；`decltype` 常在模板里「沿用某个表达式的类型」，比如返回类型尾置、`decltype(auto)` 完美转发。

### 1.2 智能指针（模块 6 全在此标准）
- `std::unique_ptr` / `shared_ptr` / `weak_ptr`，全部 C++11 引入。
- `std::make_shared` 也是 C++11（注意 `make_unique` 是 C++14）。
- 仓库：`make_shared<Layer>`、`make_shared<FenceTime>`、`weak_ptr` 破环。

示例：
```cpp
auto p = std::make_shared<Layer>("bg", 0);   // 引用计数 = 1
std::weak_ptr<Layer> w = p;                  // 弱引用，不增加计数，也不阻止 p 释放
if (auto sp = w.lock()) { /* 提升成功，对象还活着 */ }
```
注解：`weak_ptr` 关键用途是**打破 `shared_ptr` 循环引用**（A 持 B、B 持 A 会导致计数永不归零）。SF 里 `Layer` 与某些回调互相引用时就用它。

### 1.3 移动语义与右值引用（模块 10）
- 右值引用 `T&&`、`std::move`、`std::forward`（完美转发）。
- 移动构造 `T(T&&)`、移动赋值。
- 仓库：`std::move(getVsyncPeriod)` 移入 Lambda、`make_shared<Layer>(..., std::move(args))`。

示例：
```cpp
std::string s = "a very long buffer";
std::string t = std::move(s);   // 把 s 的内部存储「移交」给 t，s 变为空串（不再拷贝数据）
```
注解：`std::move` 不移动任何东西，只是把左值**cast 成右值引用**，从而触发移动构造/移动赋值。移交后原对象仍处于「有效但未指定状态」，别再读它的值。

### 1.4 Lambda 表达式（模块 9）
- `[capture](params) -> ret { body }`，可赋值给 `std::function`。
- 仓库：`makeResyncCallback` 的 `[ptr, getVsyncPeriod = std::move(...)](){}`。

示例：
```cpp
int base = 10;
auto f = [base](int x) { return base + x; };   // 按值捕获 base
std::function<int(int)> g = f;                 // 可存进 std::function
```
注解：捕获列表 `[=]` 全部按值、`[&]` 全部按引用、`[this]` 捕获当前对象。SF 里大量用 Lambda 当回调（如 `makeResyncCallback`），注意按引用捕获的变量生命周期要长于 Lambda。

### 1.5 范围 for（模块 7）
- `for (auto& x : container)`。
- 仓库：`for (const auto& [token, display] : mDisplays)`（C++17 才加结构化绑定，但循环本身是 11）。

示例：
```cpp
std::vector<int> v{1, 2, 3};
int sum = 0;
for (const auto& e : v) sum += e;   // 只读遍历，用 const 引用避免拷贝
```
注解：范围 for 等价于「取迭代器遍历」，写容器遍历最省事。`const auto&` 是只读场景的最佳默认写法；要修改元素用 `auto&`。

### 1.6 右值引用外的「统一初始化」
- 花括号初始化 `Type{...}`、`std::initializer_list`。
- 仓库：`std::vector<int> z = {5,1,9,3};`。

示例：
```cpp
struct P { int x, y; };
P p{1, 2};                          // 统一用花括号，不写 = 也行
std::vector<int> z{5, 1, 9, 3};     // 触发 initializer_list 构造
```
注解：花括号初始化能避免「最棘手的解析」和窄化转换（如 `int x{3.5};` 会编译报错）。C++11 起推荐优先用 `T{...}` 初始化。

### 1.7 `nullptr`
- 替代 `NULL`/`0`，类型是 `std::nullptr_t`，避免重载歧义。

示例：
```cpp
void f(int);
void f(char*);
f(nullptr);     // 明确调用 f(char*)，不会误匹配 f(int)
```
注解：C 里 `NULL` 常是 `0`，遇到 `f(int)`/`f(char*)` 重载会被当成 int。用 `nullptr` 类型安全，所有裸指针空值都该用 `nullptr`。

### 1.8 `= default` / `= delete`（模块 4）
- 控制默认/删除特殊成员函数。
- 仓库：`Buffer(const Buffer&) = delete`、`Scheduler` 内部状态禁拷贝。

示例：
```cpp
struct NoCopy {
    NoCopy() = default;                 // 让编译器生成默认构造
    NoCopy(const NoCopy&) = delete;     // 禁止拷贝构造，防误拷贝
};
```
注解：`=delete` 用来显式「禁用」某函数（不止构造/赋值，普通函数也能删，比如屏蔽某重载）。`=default` 则要求编译器按默认规则生成，常用于把被声明为 `=delete` 或自定义过的特殊成员「要回来」。

### 1.9 `override` / `final`（模块 5）
- `override` 显式覆盖虚函数；`final` 禁止进一步覆盖。
- 仓库：`void speak() override`、`Layer::prepareClientLayer` 等虚函数覆盖。

示例：
```cpp
struct Base { virtual void f(int); };
struct Derived : Base {
    void f(int) override;        // 写错签名（如写成 f(double)）编译直接报错
    // void g() final;           // final 可加在虚函数上禁止子类再覆盖
};
```
注解：`override` 让编译器帮你检查「是不是真的覆盖了基类虚函数」，签名写错会从「悄悄新建函数」变成「编译错误」。读 SF 的 `Layer` 派生体系时满眼都是它。

### 1.10 强类型枚举 `enum class`
- 作用域限定 + 不隐式转 int，替代 C 的裸 enum。
- 仓库：`HWComposer::DisplayType`、`VsyncPeriod` 等大量 `enum class`。

示例：
```cpp
enum class Color { Red, Green, Blue };
Color c = Color::Red;
// int x = c;          // 错误：不会隐式转 int，必须显式 static_cast<int>(c)
```
注解：C 的裸 enum 会隐式转 int、同名常量还容易冲突。`enum class` 把名字关进作用域（`Color::Red`），且默认不转 int，类型更安全。SF 里 `DisplayType`、`Hal` 等枚举全是它。

### 1.11 线程与原子（模块 12）
- `<thread>`、`<mutex>`、`std::lock_guard`、`<atomic>`、`std::atomic`。
- 仓库：`lock_guard(mStateLock)`、`atomic<bool> mRefreshPending`。

示例：
```cpp
std::atomic<int> cnt{0};
std::mutex m;
std::thread t([&] {
    std::lock_guard<std::mutex> lk(m);   // RAII 加锁，作用域结束自动解锁
    cnt.fetch_add(1);
});
t.join();
```
注解：`<thread>` 起线程、`<atomic>` 做无锁原子操作、`lock_guard` 用 RAII 管锁（忘解锁都不怕）。SF 的 `MessageQueue`/`EventThread` 就靠这套保证 VSync 路径线程安全。

### 1.12 其他常用
- `static_assert` 编译期断言。
- 变长模板 `template<typename... Args>`（SFINAE 基础）。
- `std::array` / `std::tuple` / `std::function` / `std::bind`（模块 9 用过 bind）。

示例：
```cpp
static_assert(sizeof(int) == 4, "int 必须是 4 字节");   // 编译期就拦住不符的平台
auto tup = std::make_tuple(1, std::string("a"));         // 异构固定大小元组
```
注解：`static_assert` 把「平台/契约假设」提前到编译期；`std::tuple` 装不同类型；`std::bind` 可把函数和部分参数绑成新可调用对象（SF 老的回调绑定用过）。

---

## 二、C++14（2014，小修小补）

### 2.1 `std::make_unique`
- 补齐 `make_shared` 的对称（11 只有 `make_shared`）。仓库里 `make_unique` 也常见。

示例：
```cpp
auto p = std::make_unique<Layer>("bg", 0);   // 同 make_shared，但产出 unique_ptr
std::unique_ptr<Layer> q = std::move(p);     // 所有权只能移动，不能拷贝
```
注解：C++11 漏了 `make_unique`，14 才补上。它的好处和 `make_shared` 一样——**一步构造 + 异常安全**，避免手写 `new` 后忘了 `delete`。SF 头文件里 `make_unique` 随处可见。

### 2.2 泛型 Lambda
- 参数可用 `auto`：`[](auto x, auto y){ return x+y; }`，无需手敲模板 Lambda。
- 仓库：很多工具 Lambda 直接写 `auto` 参数。

示例：
```cpp
auto add = [](auto a, auto b) { return a + b; };
add(1, 2);                       // int 相加
add(std::string("a"), "b");      // string 相加，同一 Lambda 复用
```
注解：泛型 Lambda 本质是编译器帮你生成了一个带模板 `operator()` 的闭包，省去手写函数模板。写「与参数类型无关」的小工具函数时极其顺手。

### 2.3 返回类型推导
- 函数 `auto foo() { return expr; }` 让编译器推导返回类型（配合 `decltype(auto)`）。

示例：
```cpp
auto make() { return std::make_unique<Layer>("x", 0); }   // 返回类型自动推导为 unique_ptr<Layer>
decltype(auto) ref() { static Layer l{"y", 0}; return (l); }  // 保留引用性
```
注解：`auto` 返回推导适合「返回什么类型很明显」的函数；`decltype(auto)` 会保留引用/值类别，常用于转发函数，避免不小心把引用退化成值拷贝。

### 2.4 放宽 `constexpr`
- 11 的 `constexpr` 只能单行 return；14 允许局部变量、循环、条件，更接近「编译期计算」。

示例：
```cpp
constexpr int factorial(int n) {
    int r = 1;
    for (int i = 2; i <= n; ++i) r *= i;   // 14 起 constexpr 里能用循环/局部变量
    return r;
}
static_assert(factorial(5) == 120, "");    // 编译期就算好
```
注解：C++11 的 `constexpr` 函数体基本只能 `return` 一个表达式；14 放宽后，能在编译期写带循环/分支的真正的「编译期函数」，SF 里一些常量计算开始受益。

### 2.5 二进制字面量 & 数字分隔符
- `0b1010`、`1'000'000`（可读性）。

示例：
```cpp
int mask = 0b1010;          // 二进制字面量，对应十进制 10
int big  = 1'000'000;       // 数字分隔符，纯可读性，编译器忽略
```
注解：写位掩码/寄存器值时 `0b` 直观；`'` 分隔符让长数字不再数零。两者都不改变值，只改善可读性。

### 2.6 变量模板
- `template<typename T> constexpr T pi = T(3.14159);`。

示例：
```cpp
template<typename T>
constexpr T pi = T(3.1415926535897932385);
float  r1 = pi<float>;     // 实例化 float 版
double r2 = pi<double>;    // 实例化 double 版
```
注解：变量模板让你「按类型拥有一族常量」，替代以前用 `template struct traits { static const T value; };` 的笨办法。类型相关的编译期常量现在一行搞定。

---

## 三、C++17（2017，大补，Android 10 主用）

### 3.1 结构化绑定（你见过！）
- `auto [a, b] = pair;`，直接拆出成员。
- 仓库：`for (const auto& [token, display] : mDisplays)` 拆 map 的 key/value。

示例：
```cpp
std::map<int, std::string> m{{1, "a"}, {2, "b"}};
for (const auto& [id, name] : m) {        // 同时拆出 key(id) 和 value(name)
    std::cout << id << ":" << name << "\n";
}
auto [ok, err] = std::make_pair(true, "none");   // 普通 pair/tuple 也能拆
```
注解：结构化绑定让你不必写 `it->first`/`it->second`，直接给成员起名字。SF 里遍历 `mDisplays`、`mPhysicalDisplay` 等 map 全靠它，可读性大增。

### 3.2 `if constexpr`
- 编译期条件分支，避免 SFINAE 地狱，写泛型更直观。

示例：
```cpp
template<typename T>
auto get(T v) {
    if constexpr (std::is_pointer_v<T>)   // 编译期决定走哪支，不成立的支不会实例化
        return *v;
    else
        return v;
}
```
注解：普通 `if` 两个分支都会编译；`if constexpr` 只有满足条件的分支会被编译，所以能在泛型里写「对某种类型才合法」的代码，而不必求助于晦涩的 SFINAE。

### 3.3 内联变量 `inline`
- `inline` 可用于变量，解决头文件里定义全局常量/变量的 ODR 问题（无需再用 `extern` 技巧）。

示例：
```cpp
// header.h
inline constexpr int kMaxLayers = 64;     // 头文件里直接定义，多个 TU 包含也不报重复定义
```
注解：以前头文件里的全局常量得用 `extern` 声明 + cpp 定义两步走；C++17 起 `inline` 变量可在头文件直接定义，链接时自动合并，ODR（单一定义规则）问题消失。

### 3.4 `std::optional<T>`
- 表示一个「可能有值也可能没有」的结果，替代「用特殊值或输出参数表示失败」。
- 仓库：很多查询函数（如找某图层/某显示）可返回 `optional`。

示例：
```cpp
std::optional<int> findId(const std::string& name) {
    if (name.empty()) return std::nullopt;   // 显式「无值」
    return 42;                               // 隐式构造 optional<int>(42)
}
if (auto r = findId("x"); r) std::cout << *r;   // 用 if(r) 先判断有无值
```
注解：`optional` 把「可能失败」写进类型，逼调用者检查，比返回 `-1`/`NULL` 安全。SF 里没有 `std::optional` 的年代用 `android::base::Result` 或 `llvm::Optional` 表达同样语义（见模块 13）。

### 3.5 `std::variant` / `std::any`
- 类型安全的联合体（`variant`）、可存任意类型（`any`）。

示例：
```cpp
std::variant<int, std::string> v = "hello";   // 只能存 int 或 string 之一
std::visit([](auto&& x) { std::cout << x; }, v);  // 用 visit 访问当前类型
std::any a = 3.14;                              // any 可存任意类型，运行时查询
```
注解：`variant` 是「类型安全 union」——知道所有可能类型、访问时强制处理全部类型；`any` 则完全动态（像带类型的 `void*`）。替代 C 的裸 `union`/手搓类型标签。

### 3.6 折叠表达式（fold expressions）
- 变长模板的 `...` 简化：`(args + ...)` 求和。让可变参数模板更易写。

示例：
```cpp
template<typename... Ts>
auto sum(Ts... args) { return (args + ...); }   // 二元左折叠，展开成 ((a+b)+c)+...
auto s = sum(1, 2, 3, 4);                       // s == 10
```
注解：以前要对变长参数做「全部相加/全部打印」得靠递归展开；折叠表达式一行搞定。写日志/转发可变参数时非常好用。

### 3.7 嵌套命名空间 `namespace A::B::C {}`
- 替代层层 `namespace A { namespace B { ... } }`。

示例：
```cpp
namespace android::hardware::graphics {  // 一行顶以前三层嵌套
    class Composer;
}
```
注解：纯语法糖，少敲几层花括号。AOSP 大量深层命名空间（如 `android::surfaceflinger`），用这个写法清爽很多。

### 3.8 文件系统 `<filesystem>`
- 标准库级路径/目录操作（Android 用得少，NDK 较新版本支持）。

示例：
```cpp
namespace fs = std::filesystem;
for (auto& e : fs::directory_iterator("/data"))   // 遍历目录
    std::cout << e.path() << "\n";
```
注解：标准终于把「跨平台路径/目录/文件状态」收编进库，不用再 `opendir`/`FindFirstFile` 分平台写。SF 本身很少碰文件系统，但工具/测试代码会用到。

### 3.9 `[[nodiscard]]` / `[[maybe_unused]]` 属性
- `[[nodiscard]]` 标记「返回值不能忽略」（如分配/错误码），忽略即警告。
- 仓库：不少返回错误码或资源的函数加了 `[[nodiscard]]`。

示例：
```cpp
[[nodiscard]] bool allocateSlot(int id) { return id < 8; }
[[maybe_unused]] int debugFlag = 0;        // 告诉编译器「故意不用，别警告」
void f() {
    allocateSlot(3);    // 开 -Wall 时警告：nodiscard 返回值被忽略
}
```
注解：`[[nodiscard]]` 把「忽略返回值等于埋雷」变成编译期警告（如分配失败、错误码）。`[[maybe_unused]]` 反之，显式压制「未使用变量」警告。SF 返回 `status_t`/`Result` 的函数常标 `nodiscard`（见模块 13）。

### 3.10 其他
- 保证拷贝消除（RVO 强化，省拷贝更确定）。
- `std::string_view`：只读字符串视图，避免无谓拷贝（性能利器，仓库里日志/解析常见）。
- 并行 STL 算法（`std::execution::par`）。

示例：
```cpp
void log(std::string_view s) { std::cout << s; }   // 接 string/char* 都零拷贝
std::vector<int> v{3,1,2};
std::sort(std::execution::par, v.begin(), v.end()); // 并行排序
```
注解：`string_view` 是「指向字符+长度」的只读视图，不拥有、不拷贝，替代 `const char*`+长度（见模块 13 详解，`DisplayIdentification` 实战用到）；`std::execution::par` 让现成算法一键并行。

---

## 四、C++20（2020，范式升级，仓库暂无但值得了解）

### 4.1 概念 Concepts
- 给模板参数加约束：`template<C1 T>`，编译错误更友好，替代 SFINAE。
- 例：`template<std::integral T> T add(T a, T b)`。

示例：
```cpp
#include <concepts>
template<std::integral T>          // 仅整型可实例化，非整型直接编译报错（信息友好）
T add(T a, T b) { return a + b; }
// add(1.0, 2.0);                  // 错误：double 不满足 integral 约束
```
注解：Concepts 把「模板参数必须满足什么」写清楚，报错从「一堆模板展开噪声」变成「T 不满足 integral」。替代以前用 `std::enable_if` 写的 SFINAE 黑魔法。

### 4.2 范围库 Ranges
- 管道式惰性求值：`auto even = nums | std::views::filter(...)`。取代手写循环 + 算法组合。

示例：
```cpp
#include <ranges>
std::vector<int> nums{1,2,3,4,5,6};
auto even = nums | std::views::filter([](int x){ return x % 2 == 0; })
                   | std::views::transform([](int x){ return x * x; });
for (int x : even) std::cout << x << " ";   // 4 16 36，惰性逐个算
```
注解：Ranges 用 `|` 把「过滤/变换/取前 N」串成管道，像写 shell 一样写数据处理，且惰性求值（不生成中间容器）。取代 `std::copy_if`+循环的组合拳。

### 4.3 协程 Coroutines
- `co_await`/`co_yield`/`co_return`，原生支持异步（对理解 future 的异步合成、调度器演进有用，但 SF 当前未用）。

示例：
```cpp
#include <coroutine>
std::generator<int> range(int n) {           // 生成器：每次 co_yield 暂停并返回一个值
    for (int i = 0; i < n; ++i) co_yield i;
}
```
注解：协程让你用「同步写法」表达异步/惰性逻辑，编译器自动生成状态机。对理解现代异步框架（如 future/promise 演进、调度器）很有帮助，但 Android 10 的 SF 完全没用到，属于面向未来的认知。

### 4.4 三路比较运算符 `<=>`
- 「太空船运算符」自动生成 `==`/`<`/`>` 等全部比较，写比较函数极简。

示例：
```cpp
#include <compare>
struct Point {
    int x, y;
    auto operator<=>(const Point&) const = default;  // 一条默认出全套比较
};
// 自动获得 == != < > <= >=，按成员字典序比较
```
注解：以前写一个可比较的结构体要手写 6 个运算符；`<=>` + `=default` 让编译器按成员顺序自动生成全部。`Point a, b; if (a < b) {}` 直接可用。

### 4.5 模块 Modules
- 取代 `#include` 的文本替换（更快编译、无宏污染），但生态迁移慢，Android 暂未用。

示例：
```cpp
// math.cppm
export module math;
export int add(int a, int b) { return a + b; }

// main.cpp
import math;        // 取代 #include "math.h"
```
注解：Modules 用 `import` 取代 `#include` 的文本粘贴，编译更快、没有宏泄漏/重复包含问题。理念很好，但整个 C++ 生态（含 Android 工具链）迁移极慢，所以 SF 暂时还是老 `#include` 世界。

### 4.6 其他
- `std::span`：连续序列的轻量视图（比 `string_view` 泛化到任意 T）。
- `constexpr` 虚函数、Lambda 捕获 `[=, this]` 修正。
- 原子智能指针 `std::atomic<std::shared_ptr<T>>`（无锁共享指针，对并发场景重要）。

示例：
```cpp
#include <span>
void print(std::span<const int> s) {        // 接 vector/数组/指针+长度 都行，零拷贝
    for (int x : s) std::cout << x;
}
std::vector<int> v{1,2,3};
print(v);                                  // span 自动从 vector 构造
```
注解：`std::span` 是 `string_view` 对任意连续类型的泛化——「一段 T 的视图」，函数参数用它替代 `T* + size_t` 或 `const vector<T>&`，既灵活又安全。并发场景的 `atomic<shared_ptr>` 则让共享指针的读写也能无锁原子化。

---

## 五、学习路线建议（结合本仓库）

优先级排序（按「仓库出现频率 × 实用度」）：

1. **C++11 全套**（模块 1-12 已覆盖）——地基，必须熟。
2. **C++17 结构化绑定 + optional + string_view + nodiscard**——读 SF 代码常遇到。
3. **C++14 泛型 Lambda + make_unique**——顺手掌握。
4. **C++20 概念/Ranges/协程**——了解即可，等读更新代码或写新模块再用。

### 对照自检清单
- [ ] 看到 `auto` / `for (auto& x : c)` 立刻懂（11）
- [ ] 看到 `make_shared` / `weak_ptr::lock` 立刻懂（11）
- [ ] 看到 `std::move` 知道是转移而非拷贝（11）
- [ ] 看到 Lambda 捕获列表能说出 `[=]`/`[&]`/`[this]`（11）
- [ ] 看到 `override` / `enum class` 知道多态与强枚举（11）
- [ ] 看到 `for (const auto& [k, v] : map)` 知道是结构化绑定（17）
- [ ] 看到 `[[nodiscard]]` / `std::string_view` 知道含义（17）
- [ ] 能说出 Concepts / Ranges / 协程属于 20

### 与 cpp_learning_plan.md 的关系
- `cpp_learning_plan.md`：**按主题**讲概念（类、RAII、智能指针、Lambda…），配 12 个练习。
- 本文：**按标准版本**归类特性，帮你建立「这个语法是哪一年引入的、现在该不该用」的认知。
- 两者互补：先按主题练熟，再按标准查漏补缺。

---

## 六、小练习（按标准）

### C++11 练习：用 `enum class` + `nullptr` + `override` 写一个小状态机
**要求**：定义 `enum class State { Idle, Running, Stopped };`，写一个 `Machine` 类，虚函数 `tick()` 由子类 `TimerMachine` 用 `override` 实现；指针成员初始化用 `nullptr`。

**参考答案**：
```cpp
#include <iostream>
enum class State { Idle, Running, Stopped };   // 1.10 强类型枚举

class Machine {
protected:
    State mState = State::Idle;                 // 成员默认初值(11)
    int*  mCounter = nullptr;                   // 指针成员用 nullptr 初始化(1.7)
public:
    Machine() { mCounter = new int(0); }
    virtual ~Machine() { delete mCounter; }     // 析构释放(模块2 RAII)
    virtual void tick() = 0;                    // 纯虚(模块5)
    State state() const { return mState; }
};

class TimerMachine : public Machine {           // 继承(模块5)
public:
    void tick() override {                      // 1.9 override 显式覆盖
        ++(*mCounter);
        mState = (mCounter && *mCounter >= 3) ? State::Stopped : State::Running;
        std::cout << "tick=" << *mCounter << "\n";
    }
};

int main() {
    TimerMachine m;
    while (m.state() != State::Stopped) m.tick();
}
```
编译：`g++ -std=c++17 m11.cpp -o m11 && ./m11`（输出 tick=1/2/3）。涉及：C++11 的 `enum class`、`nullptr`、纯虚/`override`、成员默认初值。对照仓库：`HWComposer::DisplayType` 等 `enum class`，以及大量 `override` 虚函数。

### C++14 练习：泛型 Lambda + make_unique
**要求**：用泛型 Lambda `[](auto a, auto b){ return a + b; }` 分别加 int 和 string；用 `make_unique` 创建一个对象并 `std::move` 给另一个 `unique_ptr`。

**参考答案**：
```cpp
#include <iostream>
#include <memory>
#include <string>
#include <utility>

int main() {
    auto add = [](auto a, auto b) { return a + b; };   // 2.2 泛型 Lambda
    std::cout << add(2, 3) << "\n";                    // int: 5
    std::cout << add(std::string("a"), std::string("b")) << "\n";  // string: ab

    auto p = std::make_unique<int>(42);                // 2.1 make_unique(C++14)
    std::unique_ptr<int> q = std::move(p);             // 模块10 移动转移所有权
    if (!p) std::cout << "p moved away, q=" << *q << "\n";   // p 变空
}
```
编译：`g++ -std=c++17 m14.cpp -o m14 && ./m14`。涉及：C++14 泛型 Lambda、`make_unique`，配合 C++11 的 `unique_ptr` 与 `std::move`。对照仓库：`make_unique` 在 SF 头文件里常见，泛型 Lambda 用于工具回调。

### C++17 练习：结构化绑定 + optional + string_view
**要求**：(a) 遍历 `map<int,string>` 用 `[id, name]` 结构化绑定打印；(b) 写函数返回 `std::optional<int>`（查找成功返回值，失败返回 `std::nullopt`）；(c) 用 `std::string_view` 只读解析一个字符串而不拷贝。

**参考答案**：
```cpp
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <optional>

std::optional<int> findScore(const std::map<int,std::string>& m, int id) {  // 3.4 optional
    auto it = m.find(id);
    if (it == m.end()) return std::nullopt;     // 没找到 = 无值
    return it->second.size();                   // 返回名字长度当分数
}

void parse(std::string_view s) {                // 3.10 string_view 只读不拷贝
    auto pos = s.find('=');
    std::string_view key = s.substr(0, pos);
    std::string_view val = s.substr(pos + 1);
    std::cout << "key=" << key << " val=" << val << "\n";
}

int main() {
    std::map<int,std::string> m{{1,"alpha"},{2,"beta"}};
    for (const auto& [id, name] : m)            // 3.1 结构化绑定拆 map
        std::cout << id << ":" << name << "\n";

    if (auto r = findScore(m, 2)) std::cout << "score=" << *r << "\n";
    else                          std::cout << "not found\n";

    parse("refresh=60");                        // 零拷贝解析
}
```
编译：`g++ -std=c++17 m17.cpp -o m17 && ./m17`。涉及：C++17 结构化绑定、`std::optional`/`std::nullopt`、`std::string_view`。对照仓库：`for (const auto& [token, display] : mDisplays)`（结构化绑定）、`DisplayIdentification` 的 `string_view` 解析、`android::base::Result` 同类「可能无值」语义。

### C++20 练习（纸上/新编译器）：Concepts + 三路比较
**要求**：用 `template<std::integral T>` 约束一个 `add` 函数；为一个 `Point` 类写 `auto operator<=>(const Point&) const = default;` 自动获得全部比较运算符。

**参考答案**（需 `-std=c++20`）：
```cpp
#include <compare>     // C++20 三路比较
#include <iostream>
#include <concepts>    // C++20 concepts

template<std::integral T>   // 4.1 概念约束：仅整型可实例化
T add(T a, T b) { return a + b; }

struct Point {                   // 4.4 太空船运算符
    int x, y;
    auto operator<=>(const Point&) const = default;  // 自动生成 == < > <= >=
};

int main() {
    std::cout << add(1, 2) << "\n";        // ok: int 是 integral
    // add(1.0, 2.0);                      // 编译错误：double 不满足 integral
    Point a{1,2}, b{1,2};
    std::cout << (a == b) << " " << (a < Point{3,4}) << "\n";  // 1 1
}
```
编译（需支持 C++20 的编译器）：`g++ -std=c++20 m20.cpp -o m20 && ./m20`。涉及：C++20 Concepts（`std::integral` 约束）、`operator<=>` 默认生成全套比较。对照仓库：SF（Android 10）尚未使用，但读新 AOSP / 写新模块时会出现。

---

## 七、参考资源
- cppreference.com（最权威的语法/标准查阅站）
- 《Effective Modern C++》（Scott Meyers）——11/14 惯用法必读
- 《C++20 The Complete Guide》（Nicolai Josuttis）——20 新特性
- 编译器支持表：https://en.cppreference.com/w/cpp/compiler_support
