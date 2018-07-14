/*
 * Copyright (c) 2016, xhawk18
 * at gmail.com
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once
#ifndef INC_TIMER_HPP_
#define INC_TIMER_HPP_

//
// Promisified timer based on promise-cpp and boost::asio
//
// Functions --
//   Defer yield(boost::asio::io_service &io);
//   Defer delay(boost::asio::io_service &io, uint64_t time_ms);
//   void cancelDelay(Defer d);
// 
//   Defer setTimeout(boost::asio::io_service &io,
//                    const std::function<void(bool /*cancelled*/)> &func,
//                    uint64_t time_ms);
//   void clearTimeout(Defer d);
//


// It seems that disable IOCP is faster on windows
#define BOOST_ASIO_DISABLE_IOCP

#include "promise.hpp"
#include <chrono>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

namespace promise {

Defer yield(boost::asio::io_service &io){
    auto defer = newPromise([&io](Defer d) {
#if BOOST_VERSION >= 106600
        boost::asio::defer(io, [d]() {
            d.resolve();
        });
#else
        io.post([d]() {
            d.resolve();
        });
#endif
    });

    //pm_assert(defer->next_.operator->() == nullptr);
    return defer;
}

Defer delay(boost::asio::io_service &io, uint64_t time_ms) {
    return newPromise([time_ms, &io](Defer &d) {
        boost::asio::steady_timer *timer =
            pm_new<boost::asio::steady_timer>(io, std::chrono::milliseconds(time_ms));
        d->any_ = timer;
        timer->async_wait([d](const boost::system::error_code& error_code) {
            if (!d->any_.empty()) {
                boost::asio::steady_timer *timer = any_cast<boost::asio::steady_timer *>(d->any_);
                d->any_.clear();
                pm_delete(timer);
                d.resolve();
            }
        });
    });
}

void cancelDelay(Defer d) {
    d = d.find_pending();
    if (d.operator->()) {
        if (!d->any_.empty()) {
            boost::asio::steady_timer *timer = any_cast<boost::asio::steady_timer *>(d->any_);
            d->any_.clear();
            timer->cancel();
            pm_delete(timer);
        }
        d.reject();
    }
}

Defer setTimeout(boost::asio::io_service &io,
                 const std::function<void(bool)> &func,
                 uint64_t time_ms) {
    return delay(io, time_ms).then([func]() {
        func(false);
    }, [func]() {
        func(true);
    });
}

void clearTimeout(Defer d) {
    cancelDelay(d);
}





}
#endif