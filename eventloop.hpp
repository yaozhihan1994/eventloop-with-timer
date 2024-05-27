/*!
 * \file eventloop.hpp
 * \version 1.0.0
 * \brief A eventloop with timer
 *
 * \author yaozhihan <yaozhihan1994@163.com>
 * \license SPDX-License-Identifier: MIT
 * \date 27 May 2024
 */

#pragma once

#include <vector>
#include <stdexcept>
#include <functional>
#include <queue>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <set>
#include <future>

#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>

using system_clock = std::chrono::system_clock;
using steady_clock = std::chrono::steady_clock;

template<typename T = steady_clock, typename = typename 
    std::enable_if<std::is_same<T, system_clock>::value || std::is_same<T, steady_clock>::value>::type,
    typename M = std::chrono::milliseconds, typename = typename 
    std::enable_if<std::is_same<T, system_clock>::value || std::is_same<T, steady_clock>::value>::type>
inline uint64_t gettimenow() { return std::chrono::duration_cast<M>(T::now().time_since_epoch()).count(); }

namespace details
{
// 获取当前系统时间并按照字符串格式输出，精确到微妙
inline std::string gettime()
{
    auto now = system_clock::now();
    auto now_c = system_clock::to_time_t(now);
    auto now_m = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1000000;
    static __thread char buf[64] = {0};
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now_c));
    sprintf(buf + strlen(buf), ".%06ld", now_m);
    return buf;
}


// 字符串拼接，在printf前面加上时间
#ifdef LOG_STDOUT
#define LOG(fmt, args...) printf("[%s %s:%d] " fmt "", \
    details::gettime().c_str(), __FUNCTION__, __LINE__, ##args)
#else
#define LOG(fmt, args...) void(0)
#endif

template<typename T, typename Comparator = std::less<T>>
class Heap
{
public:
    inline void push(const T& t) {
        LOG("push\n");
        data_.push_back(t);
        heap_up(data_, 0, data_.size());
    }

    inline void push(T&& t) {
        LOG("push\n");
        data_.emplace_back(std::move(t));
        heap_up(data_, 0, data_.size());
    }

    inline T pop() {
        if(data_.empty()) throw std::runtime_error("empty heap");
        T ret;
        std::swap(ret, data_.front());
        std::swap(data_.front(), data_.back());
        data_.pop_back();
        heap_down(data_, 0, data_.size());
        return ret;
    }

    inline void sort() { heap_sort(data_); }

    inline size_t size() { return data_.size(); }

    inline const std::vector<T>& data() { return data_; }

    inline const T& top() { return data_.front(); }

    inline bool empty() { return data_.empty(); }

private:

    void heap_up(std::vector<T>& data, int start, int end) {
        for (int index = end - 1, p = (index - 1) / 2; 
            index > 0 && cp_(data[index], data[p]);
            index = p, p = (index - 1) / 2) 
            std::swap(data[index], data[p]);
    }

    void heap_down(std::vector<T>& data, int start, int end) {
        int index = start;
        for (int lc = index * 2 + 1, rc = index * 2 + 2; 
            lc < end || rc < end; 
            lc = index * 2 + 1, rc = index * 2 + 2) {

            if (lc < end && cp_(data[lc], data[index]))
                index = lc;
            
            if (rc < end && cp_(data[rc], data[index]))
                index = rc;

            if (index == start) break;

            std::swap(data[index], data[start]);
            start = index;
        }   
    }

    void heap_sort(std::vector<T>& data) {
        for (int i = 0; i < data.size(); i++) {
            heap_up(data, 0, data.size() - i);
            std::swap(data[0], data[data.size() - i - 1]);
        }
    }

private:
    std::vector<T> data_;
    Comparator cp_;
};

}

using TaskCb = std::function<void()>;

struct Timer
{
    bool repeat;
    uint32_t id;
    uint32_t interval;
    uint64_t time;
    TaskCb cb;

    inline bool operator <(const Timer& t) const { return time < t.time; }
    inline bool operator >(const Timer& t) const { return time > t.time; }
};

class EventLoop
{
public:

    EventLoop() {   
        events_.resize(8);
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        LOG("event_fd: %d epoll_fd: %d\n", event_fd_, epoll_fd_);
    }

    ~EventLoop()
    {
        quit();
    }

    inline void loop()
    {
        thr_ = std::make_unique<std::thread>(&EventLoop::thrRun, this);
        pid_ = thr_->get_id();
    }

    void quit()
    {
        exit_ = true;
        wake();

        close(epoll_fd_);
        close(event_fd_);

        if(thr_)
        {
            thr_->join();
            thr_.reset();
        }
    }

    inline void cancelTimer(int fd)
    {
        runInLoop([this, fd](){
            timers_.erase(fd);
            LOG("cancel timer: %d\n", fd);
        });
    }

    template<class F, class... Args>
    inline uint32_t runAfter(uint32_t interval, F&& f, Args&&... args)
    {
        std::function<void()> cb(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        return addTimer(interval, std::move(cb), false, false);
    }

    template<class F, class... Args>
    inline uint32_t runEvery(uint32_t interval, bool start_now, F&& f, Args&&... args)
    {
        std::function<void()> cb(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        return addTimer(interval, std::move(cb), true, start_now);
    }

    inline void queueInLoop(TaskCb cb)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_.emplace(std::move(cb));
        }
        wake();
    }

    template<class F, class... Args>
    inline auto queueInLoopWithFuture(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type>
    {
        using return_type = typename std::result_of<F(Args...)>::type;
        auto ptask = std::make_shared< std::packaged_task<return_type()> >(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...) );
        std::future<return_type> res = ptask->get_future();
        std::function<void()> func([ptask](){ (*ptask)(); });
        queueInLoop(std::move(func));
        return res;
    }

    template<class F, class... Args>
    void queueInLoop(F&& f, Args&&... args)
    {
        std::function<void()> task(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        queueInLoop(std::move(task));
    }

    void runInLoop(TaskCb cb)
    {
        LOG("runInLoop id: %zu %zu\n", std::this_thread::get_id(), thr_->get_id());
        if(std::this_thread::get_id() == pid_)
            cb();
        else 
            this->queueInLoop(std::move(cb));
    }

    template<class F, class... Args>
    inline auto runInLoopWithFuture(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type>
    {
        using return_type = typename std::result_of<F(Args...)>::type;
        auto ptask = std::make_shared< std::packaged_task<return_type()> >(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...) );
        std::future<return_type> res = ptask->get_future();
        std::function<void()> func([ptask](){ (*ptask)(); });
        runInLoop(std::move(func));
        return res;
    }

    template<class F, class... Args>
    void runInLoop(F&& f, Args&&... args)
    {
        std::function<void()> task(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        runInLoop(std::move(task));
    }

private:

    uint32_t addTimer(uint32_t interval, TaskCb cb, bool repeat = true, bool start_now = true)
    {
        Timer timer;
        timer.repeat = repeat;
        timer.id = ++timer_id_;
        timer.time = gettimenow() + interval;
        timer.interval = interval;
        timer.cb = std::move(cb);

        this->runInLoop([timer, start_now, this](){ 
            heap_.push(timer);
            timers_.emplace(timer.id);
            if(start_now) timer.cb();
            LOG("add timer: %d %" PRIu64" %" PRIu64"\n", timer.id, timer.time, heap_.top().time);
        });

        return timer.id;
    }

    uint64_t doTimers()
    {
        auto now = gettimenow();
        LOG("now: %" PRIu64"  %" PRIu64"\n", now, heap_.top().time);
        LOG("heap size: %zu\n", heap_.size());

        while (!heap_.empty() && now >= heap_.top().time)         
        {
            // 立即处理定时任务
            auto task(heap_.pop()); 
            if(timers_.find(task.id) == timers_.end()) {
                // 无效的定时器
                LOG("invalid timer: %d\n", task.id);
                continue; 
            }

            task.cb();
            // 是否重复
            if(task.repeat > 0 && task.interval > 0)
            {
                task.time = now + task.interval;
                heap_.push(task);
            }
        }
        // 这里如果定时器时间已经过期了，那么直接返回0，否则返回最近的定时器时间
        auto timeout = heap_.empty()? 1000 : heap_.top().time - now;
        LOG("timeout: %" PRIu64" %" PRIu64" %" PRIu64"\n", timeout, now, heap_.top().time);
        return timeout;
    }

    void doEvents(int nums)
    {
        uint64_t one;
        read(event_fd_, &one, 8);
        LOG("do_events: %d\n", nums);
        // 扩容
        if(nums == events_.size()) events_.resize(events_.size() * 2);
        // 处理事件
        for (size_t i = 0; i < nums; i++)
        {
            LOG("event: %d %d\n", events_[i].data.fd, events_[i].events);
            if(events_[i].events & EPOLLIN) {
                // event TODO
                
            }
        }
    }

    void doTasks()
    {
        std::queue<TaskCb> tasks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks.swap(task_);
        }

        LOG("tasks: %d\n", tasks.size());
        while (!tasks.empty()) 
        {
            tasks.front()();
            tasks.pop();
        }
    }


    inline void wake()
    {
        uint64_t one = 1;
        write(event_fd_, &one, 8);
        LOG("wake\n");
    }

    void thrRun()
    {
        int timeout = 1000;
        struct epoll_event event;
        event.data.fd = event_fd_;
        event.events = EPOLLIN;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &event);

        LOG("loop\n");
        while (!exit_)
        {
            LOG("timeout: %d\n", timeout);
            auto ret = epoll_wait(epoll_fd_, &(events_[0]), events_.size(), timeout);
            LOG("ret: %d\n", ret);
            if(ret == -1 || exit_) break;

            doTasks();

            if(ret > 0)
                doEvents(ret);
                
            timeout = doTimers();
        }

        LOG("exit\n");
    }

private:
    bool exit_ {false};
    std::vector<struct epoll_event> events_;
    int event_fd_, epoll_fd_;
    std::unique_ptr<std::thread> thr_;
    std::thread::id pid_;
    details::Heap<Timer> heap_;
    std::mutex mutex_;  
    std::queue<TaskCb> task_;
    std::atomic<uint64_t> timer_id_;
    std::set<int> timers_;
};
