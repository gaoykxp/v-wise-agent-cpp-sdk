/**********************************************************************************************************************
    > File Name: offline_cache.cpp
    > Desc: 周期采集数据断连缓存实现
**********************************************************************************************************************/

#include "offline_cache.h"
#include "log.h"
#include "util.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>

namespace cmsr {
    namespace vwise {

        using json = nlohmann::json;

        OfflineCache::OfflineCache() {}

        OfflineCache::~OfflineCache() {}

        void OfflineCache::init(const std::string& dir, const std::string& file, int64_t maxAgeMs) {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_filePath = file;
            m_tmpPath = file + ".tmp";
            m_maxAgeMs = maxAgeMs > 0 ? maxAgeMs : (30 * 60 * 1000);
            if (!dir.empty()) {
                base_tools::util::creatFilePath(dir);
            }
            loadFromFile();
            m_inited = true;
            LogInfo << "OfflineCache init: file=" << m_filePath
                    << ", maxAgeMs=" << m_maxAgeMs
                    << ", loaded=" << m_queue.size() << " records";
        }

        void OfflineCache::loadFromFile() {
            // 调用者持锁
            m_queue.clear();
            std::ifstream ifs(m_filePath);
            if (!ifs.is_open()) {
                return;
            }
            std::string line;
            while (std::getline(ifs, line)) {
                if (line.empty()) {
                    continue;
                }
                try {
                    json j = json::parse(line);
                    CachedRecord rec;
                    rec.ts = j.value("ts", (int64_t)0);
                    rec.topic = j.value("topic", std::string());
                    rec.payload = j.value("payload", std::string());
                    m_queue.push_back(std::move(rec));
                } catch (const json::parse_error& e) {
                    LogError << "OfflineCache load parse error: " << e.what();
                }
            }
        }

        void OfflineCache::persistLocked() {
            // 调用者持锁：全量重写，写 .tmp 再 rename，保证原子
            std::ofstream ofs(m_tmpPath, std::ios::out | std::ios::trunc);
            if (!ofs.is_open()) {
                LogError << "OfflineCache persist: cannot open " << m_tmpPath;
                return;
            }
            for (const auto& rec : m_queue) {
                json j;
                j["ts"] = rec.ts;
                j["topic"] = rec.topic;
                j["payload"] = rec.payload;
                ofs << j.dump() << "\n";
            }
            ofs.flush();
            ofs.close();
            if (std::rename(m_tmpPath.c_str(), m_filePath.c_str()) != 0) {
                LogError << "OfflineCache persist: rename failed";
            }
        }

        void OfflineCache::push(const std::string& topic, const std::string& payload, int64_t ts) {
            std::lock_guard<std::mutex> lk(m_mtx);
            // 丢新策略：缓存非空且新数据与最早记录间隔超 maxAgeMs -> 停止缓存
            if (!m_queue.empty() && (ts - m_queue.front().ts) > m_maxAgeMs) {
                LogInfo << "OfflineCache span > " << m_maxAgeMs << "ms, drop new data";
                return;
            }
            m_queue.push_back(CachedRecord{ts, topic, payload});
            persistLocked();
        }

        size_t OfflineCache::flush(const std::function<bool(const std::string&, const std::string&)>& sender) {
            size_t sent = 0;
            while (true) {
                CachedRecord rec;
                {
                    std::lock_guard<std::mutex> lk(m_mtx);
                    if (m_queue.empty()) {
                        break;
                    }
                    rec = m_queue.front();
                }
                // 发送在锁外执行，避免长持锁阻塞 push
                if (!sender(rec.topic, rec.payload)) {
                    break;  // 失败/断连：保留剩余，待下次恢复续传
                }
                {
                    std::lock_guard<std::mutex> lk(m_mtx);
                    // 双重校验：队首仍是该记录才出队
                    if (!m_queue.empty() && m_queue.front().ts == rec.ts) {
                        m_queue.pop_front();
                        persistLocked();
                    }
                }
                ++sent;
            }
            return sent;
        }

        bool OfflineCache::empty() const {
            std::lock_guard<std::mutex> lk(m_mtx);
            return m_queue.empty();
        }

        size_t OfflineCache::size() const {
            std::lock_guard<std::mutex> lk(m_mtx);
            return m_queue.size();
        }

    }  // namespace vwise
}  // namespace cmsr
