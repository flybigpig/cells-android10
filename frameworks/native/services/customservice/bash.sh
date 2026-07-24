#!/bin/bash
# 启动服务（如果配置了 disabled）
adb shell start customservice

# 查看服务是否运行
adb shell service list | grep custom.service

# 使用 service call 测试
# DO_SOMETHING = FIRST_CALL_TRANSACTION (1)
adb shell service call custom.service 1 s16 "test_param"

# GET_STATUS = FIRST_CALL_TRANSACTION + 1 (2)
adb shell service call custom.service 2
