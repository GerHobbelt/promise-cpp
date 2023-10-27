#pragma once
#ifndef INC_PROMISE_INL_HPP_
#define INC_PROMISE_INL_HPP_

#include <cassert>
#include <stdexcept>
#include <vector>
#include <thread>
#include <sstream>
#include <iomanip>
#include <atomic>
#include "promise.hpp"

namespace promise {

// format data and time
static inline std::string toDateTimeString(const std::chrono::system_clock::time_point &clock) {
    time_t in_time_t = std::chrono::system_clock::to_time_t(clock);

    std::stringstream ss;
    struct tm in_tm;
#ifdef _WIN32
    localtime_s(&in_tm, &in_time_t);
#elif defined __GNUC__
    localtime_r(&in_time_t, &in_tm);
#else
    localtime_s(&in_time_t, &in_tm);
#endif
    ss << std::put_time(&in_tm, "%Y-%m-%d_%H:%M:%S");
    return ss.str();
}

void CallStack::dump() const {
    if (locations_ == nullptr) {
        printf("call stack is nullptr\n");
    }
    else if (locations_->size() == 0) {
        printf("call stack is empty\n");
    }
    else {
        printf("call stack size = %d\n", (int)locations_->size());
        size_t count = 0;
        for (auto it = locations_->rbegin(); it != locations_->rend(); ++it, ++count) {
            printf("  %d,%s,%d,%s\n", it->serialNo_, toDateTimeString(it->callTime_).c_str(), it->loc_.line_, it->loc_.file_);
        }
    }
}


static inline void healthyCheck(int line, PromiseHolder *promiseHolder) {
    (void)line;
    (void)promiseHolder;
#ifndef NDEBUG
    if (!promiseHolder) {
        fprintf(stderr, "line = %d, %d, promiseHolder is null\n", line, __LINE__);
        throw std::runtime_error("");
    }

    for (const auto &owner_ : promiseHolder->owners_) {
        auto owner = owner_.lock();
        if (owner && owner->promiseHolder_.get() != promiseHolder) {
            fprintf(stderr, "line = %d, %d, owner->promiseHolder_ = %p, promiseHolder = %p\n",
                line, __LINE__,
                owner->promiseHolder_.get(),
                promiseHolder);
            throw std::runtime_error("");
        }
    }

    for (const std::shared_ptr<Task> &task : promiseHolder->pendingTasks_) {
        if (!task) {
            fprintf(stderr, "line = %d, %d, promiseHolder = %p, task is null\n", line, __LINE__, promiseHolder);
            throw std::runtime_error("");
        }
        if (task->state_ != TaskState::kPending) {
            fprintf(stderr, "line = %d, %d, promiseHolder = %p, task = %p, task->state_ = %d\n", line, __LINE__,
                promiseHolder, task.get(), (int)task->state_);
            throw std::runtime_error("");
        }
        if (task->promiseHolder_.lock().get() != promiseHolder) {
            fprintf(stderr, "line = %d, %d, promiseHolder = %p, task = %p, task->promiseHolder_ = %p\n", line, __LINE__,
                promiseHolder, task.get(), task->promiseHolder_.lock().get());
            throw std::runtime_error("");
        }
    }
#endif
}

void Promise::dump() const {
#ifndef NDEBUG
    printf("Promise = %p, SharedPromise = %p\n", this, this->sharedPromise_.get());
    if (this->sharedPromise_)
        this->sharedPromise_->dump();
#endif
}

CallStack Promise::callStack() const {
    if (this->sharedPromise_)
        return this->sharedPromise_->callStack();
    else
        return CallStack{ nullptr };
}

void SharedPromise::dump() const {
#ifndef NDEBUG
    printf("SharedPromise = %p, PromiseHolder = %p\n", this, this->promiseHolder_.get());
    if (this->promiseHolder_)
        this->promiseHolder_->dump();
#endif
}

CallStack SharedPromise::callStack() const {
    if (this->promiseHolder_)
        return CallStack{ &this->promiseHolder_->callStack_ };
    else
        return CallStack{ nullptr };
}

void PromiseHolder::dump() const {
#ifndef NDEBUG
    printf("PromiseHolder = %p, owners = %d, pendingTasks = %d\n", this, (int)this->owners_.size(), (int)this->pendingTasks_.size());
    for (const auto &owner_ : owners_) {
        auto owner = owner_.lock();
        printf("  owner = %p\n", owner.get());
    }
    for (const auto &task : pendingTasks_) {
        if (task) {
            auto promiseHolder = task->promiseHolder_.lock();
            printf("  task = %p, PromiseHolder = %p\n", task.get(), promiseHolder.get());
        }
        else {
            printf("  task = %p\n", task.get());
        }
    }
#endif
}

static inline void join(const std::shared_ptr<PromiseHolder> &left, const std::shared_ptr<PromiseHolder> &right) {
    healthyCheck(__LINE__, left.get());
    healthyCheck(__LINE__, right.get());
    //left->dump();
    //right->dump();

    for (const std::shared_ptr<Task> &task : right->pendingTasks_) {
        task->promiseHolder_ = left;
    }
    left->pendingTasks_.splice(left->pendingTasks_.end(), right->pendingTasks_);
    left->callStack_.splice(left->callStack_.begin(), right->callStack_);

    std::list<std::weak_ptr<SharedPromise>> owners;
    owners.splice(owners.end(), right->owners_);

    // Looked on resolved if the PromiseHolder was joined to another,
    // so that it will not throw onUncaughtException when destroyed.
    right->state_ = TaskState::kResolved;

    if(owners.size() > 100) {
        fprintf(stderr, "Maybe memory leak, too many promise owners: %d", (int)owners.size());
    }

    for (const std::weak_ptr<SharedPromise> &owner_ : owners) {
        std::shared_ptr<SharedPromise> owner = owner_.lock();
        if (owner) {
            std::shared_ptr<Mutex> mutex = owner->obtainLock();
            std::lock_guard<Mutex> lock(*mutex, std::adopt_lock_t());

            std::atomic_store(&owner->promiseHolder_, left);
            left->owners_.push_back(owner);
        }
    }

    //left->dump();
    //right->dump();
    //fprintf(stderr, "left->promiseHolder_->owners_ size = %d\n", (int)left->promiseHolder_->owners_.size());


    healthyCheck(__LINE__, left.get());
    healthyCheck(__LINE__, right.get());
}

inline std::list<std::shared_ptr<PromiseHolder>> &threadLocalPromiseHolders() {
    static thread_local std::list<std::shared_ptr<PromiseHolder>> promiseHolder;
    return promiseHolder;
}

inline std::atomic<int> &callSerialNo() {
    static std::atomic<int> callSerialNo_{ 0 };
    return callSerialNo_;
}

//Unlock and then lock
#if PROMISE_MULTITHREAD
struct unlock_guard_t {
    inline unlock_guard_t(std::shared_ptr<Mutex> mutex)
        : mutex_(mutex)
        , lock_count_(mutex->lock_count()) {
        mutex_->unlock(lock_count_);
    }
    inline ~unlock_guard_t() {
        mutex_->lock(lock_count_);
    }
    std::shared_ptr<Mutex> mutex_;
    size_t lock_count_;
};
#endif

static inline void call(const Loc &loc, std::shared_ptr<Task> task) {
    std::shared_ptr<PromiseHolder> promiseHolder; //Can hold the temporarily created promise
    while (true) {
        promiseHolder = task->promiseHolder_.lock();
        if (!promiseHolder) return;

        // lock for 1st stage
        {
#if PROMISE_MULTITHREAD
            std::shared_ptr<Mutex> mutex = promiseHolder->mutex_;
            std::unique_lock<Mutex> lock(*mutex);
#endif

            // タスクが kResolved／kRejected なときは何もしない
            if (task->state_ != TaskState::kPending) return;
            // まだ promise が待機中のときは何もしないで処理を戻す。
            // 後で promise::resolve() や Defer::reject() などが呼び出されたときは kPending でなくなり、このあとに処理が進む。
            if (promiseHolder->state_ == TaskState::kPending) return;

            // promiseHolder が kResolved／kRejected のどちらかになっているので、タスクにもその状態を設定する。

            // 待機中のタスクのリストを取得
            std::list<std::shared_ptr<Task>> &pendingTasks = promiseHolder->pendingTasks_;

            //promiseHolder->dump();

            // promiseHolder_->state_ == TaskState::kResolved or kResolved のときは、
            // task 以外の pendingTasks が存在しないはず。
#if PROMISE_MULTITHREAD
            while (pendingTasks.front() != task) {
                mutex->cond_.wait<std::unique_lock<Mutex>>(lock);
            }
#else
            assert(pendingTasks.front() == task);
#endif
            pendingTasks.pop_front();

            auto now = std::chrono::system_clock::now();
            int serialNo = callSerialNo().fetch_add(1);
            promiseHolder->callStack_.push_back(CallRecord{ loc, serialNo, now });

            serialNo = callSerialNo().fetch_add(1);
            promiseHolder->callStack_.push_back(CallRecord{ task->loc_, serialNo, now });

            while (promiseHolder->callStack_.size() > PM_MAX_LOC) promiseHolder->callStack_.pop_front();

            task->state_ = promiseHolder->state_;
            //promiseHolder->dump();

            try {
                if (promiseHolder->state_ == TaskState::kResolved) {
                    if (task->onResolved_.empty()
                        || task->onResolved_.type() == type_id<std::nullptr_t>()) {
                        //to next resolved task
                    }
                    else {
                        promiseHolder->state_ = TaskState::kPending; // avoid recursive task using this state
#if PROMISE_MULTITHREAD
                        std::shared_ptr<Mutex> mutex0 = nullptr;
                        auto call = [&]() -> any {
                            unlock_guard_t lock_inner(mutex);
                            threadLocalPromiseHolders().push_back(promiseHolder);
                            const any &value = task->onResolved_.call(promiseHolder->value_);
                            threadLocalPromiseHolders().pop_back();
                            // Make sure the returned promised is locked before than "mutex"
                            if (value.type() == type_id<Promise>()) {
                                Promise &promise = value.cast<Promise &>();
                                mutex0 = promise.sharedPromise_->obtainLock();
                            }
                            return value;
                        };
                        const any &value = call();

                        if (mutex0 == nullptr) {
                            promiseHolder->value_ = value;
                            promiseHolder->state_ = TaskState::kResolved;
                        }
                        else {
                            // join the promise
                            Promise &promise = value.cast<Promise &>();
                            std::lock_guard<Mutex> lock0(*mutex0, std::adopt_lock_t());
                            join(promise.sharedPromise_->promiseHolder_, promiseHolder);
                            promiseHolder = promise.sharedPromise_->promiseHolder_;
                        }
#else
                        threadLocalPromiseHolders().push_back(promiseHolder);
                        const any &value = task->onResolved_.call(promiseHolder->value_);
                        threadLocalPromiseHolders().pop_back();

                        if (value.type() != type_id<Promise>()) {
                            promiseHolder->value_ = value;
                            promiseHolder->state_ = TaskState::kResolved;
                        }
                        else {
                            // join the promise
                            Promise &promise = value.cast<Promise &>();
                            join(promise.sharedPromise_->promiseHolder_, promiseHolder);
                            promiseHolder = promise.sharedPromise_->promiseHolder_;
                        }
#endif
                    }
                }
                else if (promiseHolder->state_ == TaskState::kRejected) {
                    if (task->onRejected_.empty()
                        || task->onRejected_.type() == type_id<std::nullptr_t>()) {
                        //to next rejected task
                        //promiseHolder->value_ = promiseHolder->value_;
                        //promiseHolder->state_ = TaskState::kRejected;
                    }
                    else {
                        try {
                            promiseHolder->state_ = TaskState::kPending; // avoid recursive task using this state
#if PROMISE_MULTITHREAD
                            std::shared_ptr<Mutex> mutex0 = nullptr;
                            auto call = [&]() -> any {
                                unlock_guard_t lock_inner(mutex);
                                threadLocalPromiseHolders().push_back(promiseHolder);
                                const any &value = task->onRejected_.call(promiseHolder->value_);
                                threadLocalPromiseHolders().pop_back();
                                // Make sure the returned promised is locked before than "mutex"
                                if (value.type() == type_id<Promise>()) {
                                    Promise &promise = value.cast<Promise &>();
                                    mutex0 = promise.sharedPromise_->obtainLock();
                                }
                                return value;
                            };
                            const any &value = call();

                            if (mutex0 == nullptr) {
                                promiseHolder->value_ = value;
                                promiseHolder->state_ = TaskState::kResolved;
                            }
                            else {
                                // join the promise
                                Promise promise = value.cast<Promise>();
                                std::lock_guard<Mutex> lock0(*mutex0, std::adopt_lock_t());
                                join(promise.sharedPromise_->promiseHolder_, promiseHolder);
                                promiseHolder = promise.sharedPromise_->promiseHolder_;
                            }
#else
                            threadLocalPromiseHolders().push_back(promiseHolder);
                            const any &value = task->onRejected_.call(promiseHolder->value_);
                            threadLocalPromiseHolders().pop_back();

                            if (value.type() != type_id<Promise>()) {
                                promiseHolder->value_ = value;
                                promiseHolder->state_ = TaskState::kResolved;
                            }
                            else {
                                // join the promise
                                Promise &promise = value.cast<Promise &>();
                                join(promise.sharedPromise_->promiseHolder_, promiseHolder);
                                promiseHolder = promise.sharedPromise_->promiseHolder_;
                            }
#endif
                        }
                        catch (const bad_any_cast &) {
                            //just go through if argument type is not match
                            promiseHolder->state_ = TaskState::kRejected;
                        }
                    }
                }
            }
            catch (const promise::bad_any_cast &ex) {
                fprintf(stderr, "promise::bad_any_cast: %s -> %s", ex.from_.name(), ex.to_.name());
                promiseHolder->value_ = std::current_exception();
                promiseHolder->state_ = TaskState::kRejected;
            }
            catch (...) {
                promiseHolder->value_ = std::current_exception();
                promiseHolder->state_ = TaskState::kRejected;
            }

            task->onResolved_.clear();
            task->onRejected_.clear();
        }

        // lock for 2nd stage
        // promiseHolder may be changed, so we need to lock again
        {
            // get next task
#if PROMISE_MULTITHREAD
            std::shared_ptr<Mutex> mutex = promiseHolder->mutex_;
            std::lock_guard<Mutex> lock(*mutex);
#endif
            std::list<std::shared_ptr<Task>> &pendingTasks2 = promiseHolder->pendingTasks_;
            if (pendingTasks2.size() == 0) {
                return;
            }

            task = pendingTasks2.front();
        }
    }
}

Defer::Defer(const std::shared_ptr<Task> &task) {
    std::shared_ptr<SharedPromise> sharedPromise(new SharedPromise{ task->promiseHolder_.lock() });
#if PROMISE_MULTITHREAD
    std::shared_ptr<Mutex> mutex = sharedPromise->obtainLock();
    std::lock_guard<Mutex> lock(*mutex, std::adopt_lock_t());
#endif

    task_ = task;
    sharedPromise_ = sharedPromise;
}


void Defer::resolve(const Loc &loc, const any &arg) const {
#if PROMISE_MULTITHREAD
    std::shared_ptr<Mutex> mutex = this->sharedPromise_->obtainLock();
    std::lock_guard<Mutex> lock(*mutex, std::adopt_lock_t());
#endif

    if (task_->state_ != TaskState::kPending) return;
    std::shared_ptr<PromiseHolder> &promiseHolder = sharedPromise_->promiseHolder_;
    promiseHolder->state_ = TaskState::kResolved;
    promiseHolder->value_ = arg;
    call(loc, task_);
}

void Defer::reject(const Loc &loc, const any &arg) const {
#if PROMISE_MULTITHREAD
    std::shared_ptr<Mutex> mutex = this->sharedPromise_->obtainLock();
    std::lock_guard<Mutex> lock(*mutex, std::adopt_lock_t());
#endif

    if (task_->state_ != TaskState::kPending) return;
    std::shared_ptr<PromiseHolder> &promiseHolder = sharedPromise_->promiseHolder_;
    promiseHolder->state_ = TaskState::kRejected;
    promiseHolder->value_ = arg;
    call(loc, task_);
}


Promise Defer::getPromise() const {
    return Promise{ sharedPromise_ };
}

CallStack Defer::callStack() const {
    return getPromise().callStack();
}


struct DoBreakTag {};

DeferLoop::DeferLoop(const Defer &defer)
    : defer_(defer) {
}

void DeferLoop::doContinue(const Loc &loc) const {
    defer_.resolve(loc);
}

void DeferLoop::doBreak(const Loc &loc, const any &arg) const {
    defer_.reject(loc, DoBreakTag(), arg);
}

void DeferLoop::reject(const Loc &loc, const any &arg) const {
    defer_.reject(loc, arg);
}

Promise DeferLoop::getPromise() const {
    return defer_.getPromise();
}

CallStack DeferLoop::callStack() const {
    return getPromise().callStack();
}

#if PROMISE_MULTITHREAD
Mutex::Mutex()
    : cond_()
    , mutex_()
    , lock_count_(0) {
}

void Mutex::lock() {
    mutex_.lock();
    ++lock_count_;
}

void Mutex::unlock() {
    --lock_count_;
    mutex_.unlock();
}

void Mutex::lock(size_t lock_count) {
    for (size_t i = 0; i < lock_count; ++i)
        this->lock();
}

void Mutex::unlock(size_t lock_count) {
    for (size_t i = 0; i < lock_count; ++i)
        this->unlock();
}
#endif

PromiseHolder::PromiseHolder() 
    : owners_()
    , pendingTasks_()
    , state_(TaskState::kPending)
    , value_()
#if PROMISE_MULTITHREAD
    , mutex_(std::make_shared<Mutex>())
#endif
{
}

PromiseHolder::~PromiseHolder() {
    if (this->state_ == TaskState::kRejected) {
        static thread_local std::atomic<bool> s_inUncaughtExceptionHandler{false};
        if(s_inUncaughtExceptionHandler) return;
        s_inUncaughtExceptionHandler = true;
        struct Releaser {
            Releaser(std::atomic<bool> *inUncaughtExceptionHandler)
                : inUncaughtExceptionHandler_(inUncaughtExceptionHandler) {}
            ~Releaser() {
                *inUncaughtExceptionHandler_ = false;
            }
            std::atomic<bool> *inUncaughtExceptionHandler_;
        } releaser(&s_inUncaughtExceptionHandler);

        CallStack{ &this->callStack_ }.dump();
        PromiseHolder::onUncaughtException(this->value_);
    }
}



any *PromiseHolder::getUncaughtExceptionHandler() {
    static any onUncaughtException;
    return &onUncaughtException;
}

any *PromiseHolder::getDefaultUncaughtExceptionHandler() {
    static any defaultUncaughtExceptionHandler = [](Promise &d) {
        // Make the default UncaughtExceptionHandler receive std::exception_ptr
        // according to the change in any.hpp which not treat exception_ptr as a special case.
        d.fail(PM_LOC, [](std::exception_ptr ep) {
            try {
                std::rethrow_exception(ep);
            } catch(std::runtime_error &err) {
                fprintf(stderr, "onUncaughtException in line %d, %s\n", __LINE__, err.what());
            } catch(...) {
                //go here for all other uncaught parameters.
                fprintf(stderr, "onUncaughtException in line %d\n", __LINE__);
            }
        });
    };

    return &defaultUncaughtExceptionHandler;
}

void PromiseHolder::onUncaughtException(const any &arg) {
    any *onUncaughtException = getUncaughtExceptionHandler();
    if (onUncaughtException == nullptr || onUncaughtException->empty()) {
        onUncaughtException = getDefaultUncaughtExceptionHandler();
    }

    try {
        onUncaughtException->call(reject(PM_LOC, arg));
    }
    catch (...) {
        fprintf(stderr, "onUncaughtException in line %d\n", __LINE__);
    }
}

void PromiseHolder::handleUncaughtException(const any &onUncaughtException) {
    (*getUncaughtExceptionHandler()) = onUncaughtException;
}

#if PROMISE_MULTITHREAD
std::shared_ptr<Mutex> SharedPromise::obtainLock() const {
    while (true) {
        auto holder = std::atomic_load(&this->promiseHolder_);
        std::shared_ptr<Mutex> mutex = holder->mutex_;
        mutex->lock();

        // pointer to mutex may be changed after locked, 
        // in this case we should try to lock and test again
        if (mutex == this->promiseHolder_->mutex_)
            return mutex;
        mutex->unlock();
    }
    return nullptr;
}
#endif

Promise &Promise::then(const Loc &loc, const any &deferOrPromiseOrOnResolved) {
    if (deferOrPromiseOrOnResolved.type() == type_id<Defer>()) {
        Defer &defer = deferOrPromiseOrOnResolved.cast<Defer &>();
        Promise promise = defer.getPromise();
        Promise &ret = then(PM_LOC, [loc, defer](const any &arg) -> any {
            defer.resolve(loc, arg);
            return nullptr;
        }, [loc, defer](const any &arg) ->any {
            defer.reject(loc, arg);
            return nullptr;
        });

        promise.finally(PM_LOC, [=]() {
            ret.reject(PM_LOC);
        });

        return ret;
    }
    else if (deferOrPromiseOrOnResolved.type() == type_id<DeferLoop>()) {
        DeferLoop &loop = deferOrPromiseOrOnResolved.cast<DeferLoop &>();
        Promise promise = loop.getPromise();

        Promise &ret = then(PM_LOC, [loc, loop](const any &arg) -> any {
            (void)arg;
            loop.doContinue(loc);
            return nullptr;
        }, [loc, loop](const any &arg) ->any {
            loop.reject(loc, arg);
            return nullptr;
        });

        promise.finally(PM_LOC, [=]() {
            ret.reject(PM_LOC);
        });

        return ret;
    }
    else if (deferOrPromiseOrOnResolved.type() == type_id<Promise>()) {
        Promise &promise = deferOrPromiseOrOnResolved.cast<Promise &>();

        std::shared_ptr<Task> task;
        {
#if PROMISE_MULTITHREAD
            std::shared_ptr<Mutex> mutex0 = this->sharedPromise_->obtainLock();
            std::lock_guard<Mutex> lock0(*mutex0, std::adopt_lock_t());
            std::shared_ptr<Mutex> mutex1 = promise.sharedPromise_->obtainLock();
            std::lock_guard<Mutex> lock1(*mutex1, std::adopt_lock_t());
#endif

            if (promise.sharedPromise_ && promise.sharedPromise_->promiseHolder_) {
                join(this->sharedPromise_->promiseHolder_, promise.sharedPromise_->promiseHolder_);
                if (this->sharedPromise_->promiseHolder_->pendingTasks_.size() > 0) {
                    task = this->sharedPromise_->promiseHolder_->pendingTasks_.front();
                }
            }
        }
        if(task)
            call(loc, task);
        return *this;
    }
    else {
        return then(loc, deferOrPromiseOrOnResolved, any());
    }
}

Promise &Promise::then(const Loc &loc, const any &onResolved, const any &onRejected) {
    std::shared_ptr<Task> task;
    {
#if PROMISE_MULTITHREAD
        std::shared_ptr<Mutex> mutex = this->sharedPromise_->obtainLock();
        std::lock_guard<Mutex> lock(*mutex, std::adopt_lock_t());
#endif

        task = std::make_shared<Task>(Task {
            TaskState::kPending,
            sharedPromise_->promiseHolder_,
            loc,
            onResolved,
            onRejected
        });
        sharedPromise_->promiseHolder_->pendingTasks_.push_back(task);
    }
    call(loc, task);
    return *this;
}

Promise &Promise::fail(const Loc &loc, const any &onRejected) {
    return then(loc, any(), onRejected);
}

Promise &Promise::always(const Loc &loc, const any &onAlways) {
    return then(loc, onAlways, onAlways);
}

Promise &Promise::finally(const Loc &loc, const any &onFinally) {
    return then(PM_LOC, [loc, onFinally](const any &arg)->any {
        return newPromise(PM_LOC, [loc, onFinally, arg](Defer &defer) {
            try {
                onFinally.call(arg);
            }
            catch (bad_any_cast &) {}
            defer.resolve(loc, arg);
        });
    }, [loc, onFinally](const any &arg)->any {
        return newPromise(PM_LOC, [loc, onFinally, arg](Defer &defer) {
            try {
                onFinally.call(arg);
            }
            catch (bad_any_cast &) {}
            defer.reject(loc, arg);
        });
    });
}


void Promise::resolve(const Loc &loc, const any &arg) const {
    if (!this->sharedPromise_) return;
    std::shared_ptr<Task> task;
    {
#if PROMISE_MULTITHREAD
        std::shared_ptr<Mutex> mutex = this->sharedPromise_->obtainLock();
        std::lock_guard<Mutex> lock(*mutex, std::adopt_lock_t());
#endif

        std::list<std::shared_ptr<Task>> &pendingTasks_ = this->sharedPromise_->promiseHolder_->pendingTasks_;
        if (pendingTasks_.size() > 0) {
            task = pendingTasks_.front();
        }
    }

    if (task) {
        Defer defer(task);
        defer.resolve(loc, arg);
    }
}

void Promise::reject(const Loc &loc, const any &arg) const {
    if (!this->sharedPromise_) return;
    std::shared_ptr<Task> task;
    {
#if PROMISE_MULTITHREAD
        std::shared_ptr<Mutex> mutex = this->sharedPromise_->obtainLock();
        std::lock_guard<Mutex> lock(*mutex, std::adopt_lock_t());
#endif

        std::list<std::shared_ptr<Task>> &pendingTasks_ = this->sharedPromise_->promiseHolder_->pendingTasks_;
        if (pendingTasks_.size() > 0) {
            task = pendingTasks_.front();
        }
    }

    if (task) {
        Defer defer(task);
        defer.reject(loc, arg);
    }
}

void Promise::clear() {
    sharedPromise_.reset();
}

Promise::operator bool() const {
    return sharedPromise_.operator bool();
}

CallStack callStack() {
    std::list<std::shared_ptr<PromiseHolder>> &promiseHolders = threadLocalPromiseHolders();
    if (promiseHolders.size() != 0)
        return CallStack{ &promiseHolders.back()->callStack_ };
    else
        return CallStack{ nullptr };
}

Promise newPromise(const Loc &loc, const std::function<void(Defer &defer)> &run) {
    Promise promise;
    promise.sharedPromise_ = std::make_shared<SharedPromise>();
    promise.sharedPromise_->promiseHolder_ = std::make_shared<PromiseHolder>();
    promise.sharedPromise_->promiseHolder_->owners_.push_back(promise.sharedPromise_);
    
    // return as is
    promise.then(loc, any(), any());
    std::shared_ptr<Task> &task = promise.sharedPromise_->promiseHolder_->pendingTasks_.front();

    Defer defer(task);
    try {
        run(defer);
    }
    catch (...) {
        defer.reject(loc, std::current_exception());
    }
   
    return promise;
}

Promise newPromise(const Loc &loc) {
    Promise promise;
    promise.sharedPromise_ = std::make_shared<SharedPromise>();
    promise.sharedPromise_->promiseHolder_ = std::make_shared<PromiseHolder>();
    promise.sharedPromise_->promiseHolder_->owners_.push_back(promise.sharedPromise_);

    // return as is
    promise.then(loc, any(), any());
    return promise;
}

Promise doWhile(const Loc &loc, const std::function<void(DeferLoop &loop)> &run) {

    return newPromise(loc, [run](Defer &defer) {
        DeferLoop loop(defer);
        run(loop);
    }).then(loc, [loc, run](const any &arg) -> any {
        (void)arg;
        return doWhile(loc, run);
    }, [loc](const any &arg) -> any {
        return newPromise(loc, [loc, arg](Defer &defer) {
            //printf("arg. type = %s\n", arg.type().name());

            bool isBreak = false;
            if (arg.type() == type_id<std::vector<any>>()) {
                std::vector<any> &args = any_cast<std::vector<any> &>(arg);
                if (args.size() == 2
                    && args.front().type() == type_id<DoBreakTag>()
                    && args.back().type() == type_id<std::vector<any>>()) {
                    isBreak = true;
                    defer.resolve(loc, args.back());
                }
            }
            
            if(!isBreak) {
                defer.reject(loc, arg);
            }
        });
    });
}

#if 0
Promise reject(const any &arg) {
    return newPromise([arg](Defer &defer) { defer.reject(arg); });
}

Promise resolve(const any &arg) {
    return newPromise([arg](Defer &defer) { defer.resolve(arg); });
}
#endif

Promise all(const Loc &loc, const std::list<Promise> &promise_list) {
    if (promise_list.size() == 0) {
        return resolve(loc);
    }

    std::shared_ptr<size_t> finished = std::make_shared<size_t>(0);
    std::shared_ptr<size_t> size = std::make_shared<size_t>(promise_list.size());
    std::shared_ptr<std::vector<any>> retArr = std::make_shared<std::vector<any>>();
    retArr->resize(*size);

    return newPromise(loc, [=](Defer &defer) {
        size_t index = 0;
        for (auto promise : promise_list) {
            promise.then(loc, [=](const any &arg) {
                (*retArr)[index] = arg;
                if (++(*finished) >= *size) {
                    defer.resolve(loc, *retArr);
                }
            }, [=](const any &arg) {
                defer.reject(loc, arg);
            });

            ++index;
        }
    });
}

static Promise race(const Loc &loc, const std::list<Promise> &promise_list, std::shared_ptr<int> winner) {
    return newPromise(loc, [=](Defer &defer) {
        int index = 0;
        for (auto it = promise_list.begin(); it != promise_list.end(); ++it, ++index) {
            auto promise = *it;
            promise.then(loc, [=](const any &arg) {
                *winner = index;
                defer.resolve(loc, arg);
                return arg;
            }, [=](const any &arg) {
                *winner = index;
                defer.reject(loc, arg);
                return arg;
            });
        }
    });
}

Promise race(const Loc &loc, const std::list<Promise> &promise_list) {
    std::shared_ptr<int> winner = std::make_shared<int>(-1);
    return race(loc, promise_list, winner);
}

Promise raceAndReject(const Loc &loc, const std::list<Promise> &promise_list) {
    std::shared_ptr<int> winner = std::make_shared<int>(-1);
    return race(loc, promise_list, winner).finally(PM_LOC, [loc, promise_list, winner] {
        int index = 0;
        for (auto it = promise_list.begin(); it != promise_list.end(); ++it, ++index) {
            if (index != *winner) {
                auto promise = *it;
                promise.reject(loc);
            }
        }
    });
}

Promise raceAndResolve(const Loc &loc, const std::list<Promise> &promise_list) {
    std::shared_ptr<int> winner = std::make_shared<int>(-1);
    return race(loc, promise_list, winner).finally(PM_LOC, [loc, promise_list, winner] {
        int index = 0;
        for (auto it = promise_list.begin(); it != promise_list.end(); ++it, ++index) {
            if (index != *winner) {
                auto promise = *it;
                promise.resolve(loc);
            }
        }
    });
}

 
} // namespace promise

#endif
