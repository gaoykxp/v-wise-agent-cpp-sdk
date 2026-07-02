
#include <functional>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <iostream>

class Timer
{
public:
 Timer(): _expired(true), _generation(0)
 {}

 Timer(const Timer& timer)
 {
	_expired = timer._expired.load();
	_generation = timer._generation.load();
 }

 ~Timer()
 {
	stop();
 }

 void start(int interval, std::function<void()> task)
 {
    // is started, do not start again
    if (_expired == false)
        return;

    // start async timer, launch thread and wait in that thread
    _expired = false;
    // 新代际：旧线程据此识别自身已过期并自行退出，避免被下面的 start 复位共享标志后继续循环
    uint64_t gen = ++_generation;
    std::thread([this, interval, task, gen]() {
        while (_generation.load() == gen)
       {
            // sleep every interval and do the task again and again until times up
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
            if (_generation.load() == gen) {
                task();
            }
        }
    }).detach();
 }

 void startOnce(int delay, std::function<void()> task)
 {
	std::thread([this, delay, task]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(delay));
		task();
	}).detach();
 }

 void stop()
 {
    _expired = true;
	++_generation; // 使任何在跑的线程循环条件 (_generation == gen) 失效而退出
 }

private:
 std::atomic<bool> _expired; // timer stopped status
 std::atomic<uint64_t> _generation; // 代际：每次 start/stop 递增，旧线程据此自行退出
 std::mutex _mutex;
 std::condition_variable _expired_cond;
};



