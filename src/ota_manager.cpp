/**********************************************************************************************************************
    > File Name: ota_manager.cpp
    > Desc: OTA 固件升级（Agent 自更新）实现
**********************************************************************************************************************/

#include "ota_manager.h"
#include "probe_mgr.h"
#include "global.h"
#include "httpclient.h"
#include "log.h"
#include "util.h"
#include "base_timer.h"
#include <nlohmann/json.hpp>

#include <openssl/evp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <thread>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <cctype>

namespace {
    // 大小写不敏感比较（SHA-256 hex 可能大小写不一）
    bool iequals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }
}

namespace cmsr {
    namespace vwise {

        using json = nlohmann::json;

        OtaManager::OtaManager() {}

        void OtaManager::init() {
            const auto& root = common::GlobalData::Instance()->getJson();
            if (root.contains("Vwise") && root["Vwise"].is_object()) {
                const auto& vwise = root["Vwise"];
                if (vwise.contains("Ota") && vwise["Ota"].is_object()) {
                    const auto& ota = vwise["Ota"];
                    if (ota.contains("WorkDir") && ota["WorkDir"].is_string())
                        m_workDir = ota["WorkDir"].get<std::string>();
                    if (ota.contains("SystemdUnit") && ota["SystemdUnit"].is_string())
                        m_unit = ota["SystemdUnit"].get<std::string>();
                    if (ota.contains("BootCommitSec") && ota["BootCommitSec"].is_number_integer())
                        m_bootCommitSec = ota["BootCommitSec"].get<int>();
                    if (ota.contains("MaxBootAttempts") && ota["MaxBootAttempts"].is_number_integer())
                        m_maxBootAttempts = ota["MaxBootAttempts"].get<int>();
                }
            }
            m_exePath = common::GlobalData::Instance()->programPath();
            base_tools::util::creatFilePath(m_workDir);
            LogInfo << "OtaManager init: exe=" << m_exePath << ", workDir=" << m_workDir
                    << ", unit=" << m_unit << ", commitSec=" << m_bootCommitSec
                    << ", maxAttempts=" << m_maxBootAttempts;
        }

        void OtaManager::handleOtaCommand(const json& j) {
            if (m_running.exchange(true)) {
                LogWarn << "OTA already running, ignore new command";
                return;
            }
            std::string taskId = j.value("task_id", "");
            std::string version = j.value("version", "");
            std::string url = j.value("url", "");
            std::string sha256 = j.value("sha256", "");
            int64_t size = j.value("size", (int64_t)0);

            if (url.empty() || sha256.empty()) {
                reportStatus(taskId, "failed", version, "missing url or sha256");
                m_running = false;
                return;
            }
            LogInfo << "OTA command: task=" << taskId << ", version=" << version
                    << ", url=" << url << ", size=" << size;
            std::thread(&OtaManager::otaWorker, this, taskId, version, url, sha256, size).detach();
        }

        void OtaManager::otaWorker(std::string taskId, std::string version,
                                   std::string url, std::string sha256, int64_t size) {
            std::string dlPath = m_workDir + "/update.bin";

            // 磁盘空间检查
            if (size > 0) {
                struct statvfs vfs;
                if (statvfs(m_workDir.c_str(), &vfs) == 0) {
                    int64_t avail = (int64_t)vfs.f_bavail * vfs.f_frsize;
                    if (avail < size * 2) {
                        reportStatus(taskId, "failed", version, "insufficient disk space");
                        m_running = false;
                        return;
                    }
                }
            }

            reportStatus(taskId, "accepted", version);
            reportStatus(taskId, "downloading", version);

            // 1. 下载
            if (!downloadFile(url, dlPath)) {
                reportStatus(taskId, "failed", version, "download error");
                std::remove(dlPath.c_str());
                m_running = false;
                return;
            }

            // 2. SHA-256 校验
            std::string actual = sha256File(dlPath);
            if (actual.empty() || !iequals(actual, sha256)) {
                reportStatus(taskId, "verify_failed", version,
                             "expected=" + sha256 + " got=" + actual);
                std::remove(dlPath.c_str());
                m_running = false;
                return;
            }
            LogInfo << "OTA verify ok, sha256=" << actual;

            // 3. 安装
            reportStatus(taskId, "installing", version);
            if (!installBinary()) {
                reportStatus(taskId, "failed", version, "install error");
                std::remove(dlPath.c_str());
                m_running = false;
                return;
            }

            // 4. 写 pending 标志（待重启后提交/回滚）
            {
                json pj;
                pj["task_id"] = taskId;
                pj["version"] = version;
                pj["attempts"] = 0;
                pj["ts"] = base_tools::BaseTimer::GetMilliTime();
                std::ofstream ofs(m_workDir + "/pending");
                if (ofs.is_open()) ofs << pj.dump();
            }

            // 5. 重启
            reportStatus(taskId, "installing", version, "restarting");
            restartSelf();
            // 正常不会执行到这里
            m_running = false;
        }

        bool OtaManager::downloadFile(const std::string& url, const std::string& path) {
            std::vector<std::string> headers;
            return base_tools::CHttpClient::getInstance().HTCLDownloadFile(url, path, headers, nullptr);
        }

        std::string OtaManager::sha256File(const std::string& path) {
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) {
                LogError << "sha256File: cannot open " << path;
                return "";
            }
            EVP_MD_CTX* ctx = EVP_MD_CTX_new();
            if (!ctx) { fclose(f); return ""; }
            EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);

            unsigned char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                EVP_DigestUpdate(ctx, buf, n);
            }
            fclose(f);

            unsigned char hash[EVP_MAX_MD_SIZE];
            unsigned int hashLen = 0;
            EVP_DigestFinal_ex(ctx, hash, &hashLen);
            EVP_MD_CTX_free(ctx);

            std::string hex;
            hex.reserve(hashLen * 2);
            char b[3];
            for (unsigned int i = 0; i < hashLen; ++i) {
                snprintf(b, sizeof(b), "%02x", hash[i]);
                hex += b;
            }
            return hex;
        }

        bool OtaManager::installBinary() {
            std::string srcPath = m_workDir + "/update.bin";
            std::string tmpPath = m_exePath + ".new.tmp";   // 目标文件系统临时文件（保证 rename 原子）
            std::string bakPath = m_exePath + ".bak";

            // 1. 拷贝下载文件到目标文件系统
            {
                std::ifstream src(srcPath, std::ios::binary);
                std::ofstream dst(tmpPath, std::ios::binary | std::ios::trunc);
                if (!src.is_open() || !dst.is_open()) {
                    LogError << "install: copy to target fs failed";
                    return false;
                }
                dst << src.rdbuf();
            }

            // 2. 备份当前 exe（exe 仍在位，无空窗）
            {
                std::ifstream src(m_exePath, std::ios::binary);
                std::ofstream dst(bakPath, std::ios::binary | std::ios::trunc);
                if (!src.is_open() || !dst.is_open()) {
                    LogError << "install: backup failed";
                    std::remove(tmpPath.c_str());
                    return false;
                }
                dst << src.rdbuf();
            }

            // 3. 原子替换 exe（同文件系统 rename）
            if (std::rename(tmpPath.c_str(), m_exePath.c_str()) != 0) {
                LogError << "install: atomic rename failed, errno=" << errno;
                std::remove(tmpPath.c_str());
                return false;
            }
            chmod(m_exePath.c_str(), 0755);

            // 4. 清理下载临时
            std::remove(srcPath.c_str());
            LogInfo << "OTA install ok: " << m_exePath << " replaced";
            return true;
        }

        void OtaManager::reportStatus(const std::string& taskId, const std::string& status,
                                      const std::string& version, const std::string& extra) {
            json j;
            j["cmd_type"] = "ota_upgrade";
            j["task_id"] = taskId;
            j["status"] = status;
            j["version"] = version;
            if (!extra.empty()) j["detail"] = extra;
            std::string payload = j.dump();
            ProbeMgr::getInstance().mqtt.messageSend(
                ProbeMgr::getInstance().TOPIC_PLAT_ORDER_DOWN_ACK, payload);
        }

        void OtaManager::restartSelf() {
            std::string cmd = "systemctl restart " + m_unit;
            LogInfo << "OTA restart: " << cmd;
            std::system(cmd.c_str());
            // 兜底：若 systemctl 未杀掉本进程，退出由 systemd Restart=always 拉起
            std::exit(0);
        }

        void OtaManager::checkPendingOta() {
            std::string pendingPath = m_workDir + "/pending";
            std::ifstream ifs(pendingPath);
            if (!ifs.is_open()) {
                return;  // 无未决升级
            }
            json pj;
            try {
                ifs >> pj;
            } catch (...) {
                ifs.close();
                std::remove(pendingPath.c_str());
                return;
            }
            ifs.close();
            if (pj.is_null() || pj.empty()) {
                std::remove(pendingPath.c_str());
                return;
            }

            m_pendingTaskId = pj.value("task_id", "");
            m_pendingVersion = pj.value("version", "");
            int attempts = pj.value("attempts", 0) + 1;  // 本次启动计数+1
            pj["attempts"] = attempts;
            { std::ofstream ofs(pendingPath); if (ofs.is_open()) ofs << pj.dump(); }

            LogInfo << "OTA pending: version=" << m_pendingVersion << ", attempts=" << attempts;

            if (attempts >= m_maxBootAttempts) {
                // 连续崩溃超限 -> 回滚
                rollbackOta();  // 内部会 restartSelf，不返回
                return;
            }
            // 启动健康计时器：到期且进程仍存活 -> 提交
            m_commitTimer.startOnce(m_bootCommitSec * 1000, [this] { commitOta(); });
        }

        void OtaManager::commitOta() {
            std::string pendingPath = m_workDir + "/pending";
            if (std::remove(pendingPath.c_str()) != 0) {
                return;  // pending 已不存在（已回滚或已提交）
            }
            LogInfo << "OTA commit success: version=" << m_pendingVersion;
            reportStatus(m_pendingTaskId, "success", m_pendingVersion);
            std::remove((m_exePath + ".bak").c_str());
        }

        void OtaManager::rollbackOta() {
            LogWarn << "OTA rollback: attempts exceeded, restoring backup";
            std::string bakPath = m_exePath + ".bak";
            std::ifstream bf(bakPath, std::ios::binary);
            if (bf.good()) {
                bf.close();
                std::remove(m_exePath.c_str());
                if (std::rename(bakPath.c_str(), m_exePath.c_str()) == 0) {
                    chmod(m_exePath.c_str(), 0755);
                    LogInfo << "OTA rollback: restored " << bakPath;
                }
            } else {
                LogError << "OTA rollback: backup missing " << bakPath;
            }
            std::remove((m_workDir + "/pending").c_str());
            restartSelf();  // 重启回旧版本
        }

    }  // namespace vwise
}  // namespace cmsr
