/**********************************************************************************************************************
    > File Name: ota_manager.h
    > Desc: OTA 固件升级（Agent 自更新）。平台 MQTT 下发元信息(版本/URL/SHA-256)，
    >       SDK 下载 -> SHA-256 校验 -> 原子安装 -> systemd 重启；启动时提交/回滚未决升级。
**********************************************************************************************************************/

#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include "timer.hpp"
#include <atomic>
#include <string>
#include <nlohmann/json.hpp>

namespace cmsr {
    namespace vwise {

        class OtaManager {
        public:
            static OtaManager& getInstance() {
                static OtaManager inst;
                return inst;
            }

            // 读配置、建工作目录、记录当前 exe 路径
            void init();

            // 收到 OTA 指令（内部起工作线程，不阻塞 MQTT 回调）
            void handleOtaCommand(const nlohmann::json& j);

            // 启动时尽早调用：提交(健康计时到期) 或 回滚(连续崩溃超限)
            void checkPendingOta();

        private:
            OtaManager();
            OtaManager(const OtaManager&) = delete;
            OtaManager& operator=(const OtaManager&) = delete;

            void otaWorker(std::string taskId, std::string version,
                           std::string url, std::string sha256, int64_t size);
            bool downloadFile(const std::string& url, const std::string& path);
            std::string sha256File(const std::string& path);          // 流式 EVP SHA-256
            bool installBinary();                                     // 备份+原子替换
            void reportStatus(const std::string& taskId, const std::string& status,
                              const std::string& version, const std::string& extra = "");
            void restartSelf();                                       // systemctl restart / exit
            void commitOta();                                         // 计时器到期：提交
            void rollbackOta();                                       // 回滚到 .bak

            std::atomic<bool> m_running{false};   // 防并发 OTA
            std::string m_workDir = "/mnt/data/ota";
            std::string m_unit = "v_wise_agent";
            std::string m_exePath;                // 当前可执行文件路径
            int m_bootCommitSec = 60;
            int m_maxBootAttempts = 3;

            Timer m_commitTimer;
            std::string m_pendingTaskId;
            std::string m_pendingVersion;
        };

    }  // namespace vwise
}  // namespace cmsr

#endif  // OTA_MANAGER_H
