/**********************************************************************************************************************
    > File Name: offline_cache.h
    > Desc: 周期采集数据断连缓存。断网时数据落盘，网络恢复后按时间顺序逐条回传。
    >       最多缓存 maxAgeMs 时长的数据，超过后丢弃新数据（丢新策略）。
**********************************************************************************************************************/

#ifndef OFFLINE_CACHE_H
#define OFFLINE_CACHE_H

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace cmsr {
    namespace vwise {

        struct CachedRecord {
            int64_t ts = 0;       // 毫秒时间戳
            std::string topic;
            std::string payload;  // 原始 JSON 字符串
        };

        class OfflineCache {
        public:
            OfflineCache();
            ~OfflineCache();

            // 初始化：建目录、设置缓存文件路径与最大缓存时长，并加载已有缓存
            void init(const std::string& dir, const std::string& file, int64_t maxAgeMs);

            // 断网写入一条；若缓存时间跨度已超 maxAgeMs 则丢弃（丢新策略）
            void push(const std::string& topic, const std::string& payload, int64_t ts);

            // 按时间顺序（旧->新）逐条回传。sender 返回 true 表示成功，成功即出队并落盘；
            // sender 返回 false（失败/断连）则停止，剩余保留待下次恢复续传。
            // 返回成功发送的条数。
            size_t flush(const std::function<bool(const std::string&, const std::string&)>& sender);

            bool empty() const;
            size_t size() const;

        private:
            void loadFromFile();          // 调用者持锁
            void persistLocked();         // 调用者持锁：全量重写文件（写 .tmp 再 rename）

            std::deque<CachedRecord> m_queue;
            mutable std::mutex m_mtx;
            std::string m_filePath;
            std::string m_tmpPath;
            int64_t m_maxAgeMs = 30 * 60 * 1000;
            bool m_inited = false;
        };

    }  // namespace vwise
}  // namespace cmsr

#endif  // OFFLINE_CACHE_H
