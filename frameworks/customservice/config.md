在你的产品 .mk 文件中添加（例如 cells_build.mk 或 device.mk）
PRODUCT_PACKAGES += yourservice

```agsl
frameworks/customservice/
├── Android.bp              ✅ 构建配置
├── IYourService.h          ✅ Binder 接口声明
├── IYourService.cpp        ✅ Binder 接口实现
├── YourService.h           ✅ 服务头文件
├── YourService.cpp         ✅ 服务具体实现
├── main_yourservice.cpp    ✅ 主程序入口
└── yourservice.rc          ✅ init 配置
```