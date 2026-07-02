#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace tbox {

// URC (Unsolicited Result Code) callback.
// Invoked on the UrcDispatcher worker thread, NOT the reader thread, so it is
// safe to issue AT commands from inside it (they will queue behind whatever the
// reader is currently doing). Keep heavy work out of it regardless.
using UrcCallback = std::function<void(const std::string& line)>;

// Returns true if `line` is a final AT result code that terminates a command
// response: "OK", "ERROR", "+CME ERROR: ..." or "+CMS ERROR: ...".
bool is_final_result(const std::string& line);

// Accumulates raw bytes from the serial port and yields trimmed, complete lines
// (delimited by '\n', with '\r' stripped). The reader thread feeds it and pulls
// lines one at a time so URC/response classification happens per-line.
class LineAccumulator
{
public:
    // Append raw bytes just read from the port.
    void feed(const std::string& bytes);

    // True if at least one complete line is ready to be pulled.
    bool has_line() const;

    // Pop and return the next trimmed line. Returns "" if none.
    std::string next_line();

    // Drop any partial content (used on reconnect/reset).
    void reset();

private:
    std::string buf_;
};

// UrcDispatcher owns a single worker thread. The reader thread calls dispatch()
// (non-blocking) for every URC line it strips out of the stream; the worker
// invokes the registered callback. Decoupling the callback from the reader is
// what makes it safe for a callback to itself send AT commands without
// deadlocking the reader.
class UrcDispatcher
{
public:
    UrcDispatcher() = default;
    ~UrcDispatcher();

    UrcDispatcher(const UrcDispatcher&) = delete;
    UrcDispatcher& operator=(const UrcDispatcher&) = delete;

    // Start the worker thread. Safe to call multiple times.
    void start();

    // Signal the worker to stop and join it.
    void stop();

    // Register a handler for a URC prefix or bare word. Prefixes are matched
    // case-sensitively against the start of each line. Re-registering the same
    // prefix replaces the previous callback. Examples: "+CMT:", "+QSIMSTAT:",
    // "RDY", "+CFUN:".
    void register_handler(const std::string& prefix, UrcCallback cb);

    // Remove the handler for `prefix` (no-op if not registered).
    void unregister(const std::string& prefix);

    // True if `line` matches any registered URC prefix. Called by the reader to
    // decide whether to strip a line out of the command response and dispatch it.
    bool is_urc(const std::string& line) const;

    // Enqueue `line` for async delivery to its handler(s). Non-blocking.
    // If no handler is registered for the line, it is logged and dropped.
    void dispatch(const std::string& line);

private:
    void worker_loop();

    mutable std::mutex                    mtx_;
    std::map<std::string, UrcCallback>    handlers_;   // ordered by prefix for stable dispatch
    std::queue<std::string>              queue_;
    std::condition_variable               cv_;
    std::thread                           worker_;
    std::atomic<bool>                    running_{false};
};

} // namespace tbox
