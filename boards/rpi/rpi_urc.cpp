#include "rpi_urc.h"

#include <algorithm>
#include <cctype>

#include "log.h"

namespace tbox {

// -----------------------------------------------------------------
// is_final_result
// -----------------------------------------------------------------
bool is_final_result(const std::string& line)
{
    if (line == "OK" || line == "ERROR")
        return true;
    // "+CME ERROR: 3" / "+CMS ERROR: 500"
    return line.rfind("+CME ERROR", 0) == 0 ||
           line.rfind("+CMS ERROR", 0) == 0;
}

// -----------------------------------------------------------------
// LineAccumulator
// -----------------------------------------------------------------
static std::string trim(std::string s)
{
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

void LineAccumulator::feed(const std::string& bytes)
{
    buf_ += bytes;
}

bool LineAccumulator::has_line() const
{
    return buf_.find('\n') != std::string::npos;
}

std::string LineAccumulator::next_line()
{
    const auto nl = buf_.find('\n');
    if (nl == std::string::npos)
        return {};

    // Take up to (not including) '\n'; '\r' is stripped by trim().
    std::string raw(buf_.data(), nl);
    buf_.erase(0, nl + 1);
    return trim(raw);
}

void LineAccumulator::reset()
{
    buf_.clear();
}

// -----------------------------------------------------------------
// UrcDispatcher
// -----------------------------------------------------------------
UrcDispatcher::~UrcDispatcher()
{
    stop();
}

void UrcDispatcher::start()
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (running_)
        return;
    running_ = true;
    worker_ = std::thread(&UrcDispatcher::worker_loop, this);
}

void UrcDispatcher::stop()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_)
            return;
        running_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void UrcDispatcher::register_handler(const std::string& prefix, UrcCallback cb)
{
    std::lock_guard<std::mutex> lk(mtx_);
    handlers_[prefix] = std::move(cb);
}

void UrcDispatcher::unregister(const std::string& prefix)
{
    std::lock_guard<std::mutex> lk(mtx_);
    handlers_.erase(prefix);
}

bool UrcDispatcher::is_urc(const std::string& line) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& kv : handlers_) {
        // Exact bare-word match (e.g. "RDY") or prefix match (e.g. "+CMT:").
        if (line == kv.first || line.rfind(kv.first, 0) == 0)
            return true;
    }
    return false;
}

void UrcDispatcher::dispatch(const std::string& line)
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_) {
            // Worker has stopped: no one will process this. Log and drop rather
            // than silently queueing forever.
            LogWarn << "URC dropped (dispatcher not running): " << line;
            return;
        }
        queue_.push(line);
    }
    cv_.notify_one();
}

void UrcDispatcher::worker_loop()
{
    while (true) {
        std::string line;
        std::map<std::string, UrcCallback> snapshot;

        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this]{ return !running_ || !queue_.empty(); });
            if (!running_ && queue_.empty())
                return;
            if (!queue_.empty()) {
                line = std::move(queue_.front());
                queue_.pop();
            }
            if (!line.empty())
                snapshot = handlers_;  // copy under lock; invoke without lock
        }

        if (line.empty())
            continue;

        bool matched = false;
        for (const auto& kv : snapshot) {
            if (line == kv.first || line.rfind(kv.first, 0) == 0) {
                matched = true;
                try {
                    if (kv.second)
                        kv.second(line);
                } catch (const std::exception& e) {
                    LogError << "URC handler for '" << kv.first << "' threw: " << e.what();
                } catch (...) {
                    LogError << "URC handler for '" << kv.first << "' threw unknown exception";
                }
            }
        }
        if (!matched)
            LogWarn << "URC dispatched but no matching handler: " << line;
    }
}

} // namespace tbox
