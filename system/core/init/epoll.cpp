/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "epoll.h"

#include <stdint.h>
#include <sys/epoll.h>

#include <chrono>
#include <functional>
#include <map>

namespace android {
namespace init {

Epoll::Epoll() {}

/**
 * @brief 打开epoll文件描述符
 *
 * 该函数创建一个新的epoll实例并获取其文件描述符。
 * 如果epoll_fd_已经有效则直接返回成功。
 * 创建的epoll文件描述符会设置EPOLL_CLOEXEC标志。
 *
 * @return Result<Success> 成功时返回Success()，失败时返回包含错误信息的ErrnoError
 */
Result<Success> Epoll::Open() {
    // 检查epoll文件描述符是否已经打开，避免重复创建
    if (epoll_fd_ >= 0) return Success();

    // 创建epoll实例，使用EPOLL_CLOEXEC标志避免子进程继承
    epoll_fd_.reset(epoll_create1(EPOLL_CLOEXEC));

    // 检查epoll创建是否成功
    if (epoll_fd_ == -1) {
        return ErrnoError() << "epoll_create1 failed";
    }

    return Success();
}


/**
 * @brief 注册一个文件描述符到epoll实例中，并关联对应的事件处理函数
 *
 * @param fd 需要监听的文件描述符
 * @param handler 当事件发生时需要调用的处理函数
 * @param events 需要监听的事件类型（EPOLLIN、EPOLLOUT等的组合）
 * @return Result<Success> 成功返回Success，失败返回包含错误信息的Error
 *
 * 该函数将指定的文件描述符添加到epoll监控集合中，并建立文件描述符与事件处理函数的映射关系。
 * 如果注册失败，会自动清理已添加的映射关系。
 */
Result<Success> Epoll::RegisterHandler(int fd, std::function<void()> handler, uint32_t events) {
    // 检查事件类型是否有效
    if (!events) {
        return Error() << "Must specify events";
    }

    // 将文件描述符和处理函数的映射关系存储到handlers映射表中
    auto [it, inserted] = epoll_handlers_.emplace(fd, std::move(handler));
    // 如果该文件描述符已经存在处理函数，则返回错误
    if (!inserted) {
        return Error() << "Cannot specify two epoll handlers for a given FD";
    }

    // 构造epoll事件结构体
    epoll_event ev;
    ev.events = events;
    // std::map's iterators do not get invalidated until erased, so we use the
    // pointer to the std::function in the map directly for epoll_ctl.
    ev.data.ptr = reinterpret_cast<void*>(&it->second);

    // 调用epoll_ctl将文件描述符添加到epoll实例中
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
        Result<Success> result = ErrnoError() << "epoll_ctl failed to add fd";
        // 如果添加失败，需要回滚之前在handlers映射表中的操作
        epoll_handlers_.erase(fd);
        return result;
    }

    return Success();
}


Result<Success> Epoll::UnregisterHandler(int fd) {
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == -1) {
        return ErrnoError() << "epoll_ctl failed to remove fd";
    }
    if (epoll_handlers_.erase(fd) != 1) {
        return Error() << "Attempting to remove epoll handler for FD without an existing handler";
    }
    return Success();
}

Result<Success> Epoll::Wait(std::optional<std::chrono::milliseconds> timeout) {
    int timeout_ms = -1;
    if (timeout && timeout->count() < INT_MAX) {
        timeout_ms = timeout->count();
    }
    epoll_event ev;
    auto nr = TEMP_FAILURE_RETRY(epoll_wait(epoll_fd_, &ev, 1, timeout_ms));
    if (nr == -1) {
        return ErrnoError() << "epoll_wait failed";
    } else if (nr == 1) {
        std::invoke(*reinterpret_cast<std::function<void()>*>(ev.data.ptr));
    }
    return Success();
}

}  // namespace init
}  // namespace android
