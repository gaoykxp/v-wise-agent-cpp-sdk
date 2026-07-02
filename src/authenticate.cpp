/**********************************************************************************************************************
    > File Name: authenticate.cpp
    > Author: htsong huangdongming
    > Date: 5/8/24
**********************************************************************************************************************/
#include "authenticate.h"
#include "httpclient.h"

#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <fstream>
#include <vector>

#include "log.h"
#include "util.h"
#include "global.h"
#include "base_timer.h"
#include "nlohmann/json.hpp"
#include "base64.h"
#include "rpi_gps_info.h"

#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/hmac.h>
#include <openssl/err.h>
#include <cstring>


namespace {

    std::vector<std::string> SetCmimHeader(const std::string &token="")
    {
        long long nowTime = base_tools::BaseTimer::GetMilliTime();
        std::vector<std::string> headers;
        headers.push_back("Content-Type: application/json");
        headers.push_back("charset: utf-8");
//        headers.push_back("Accept: application/json");
//        headers.push_back("requestid:" + std::to_string(nowTime));
        if(!token.empty()){
            headers.push_back("authorization:" + token);
        }
        return headers;
    }

}



namespace cmsr {
    namespace vwise {
        using namespace std;
        using json = nlohmann::json;

        Authenticator &Authenticator::getInstance() {
            static Authenticator m_instance;
            return m_instance;
        }


        void Authenticator::genRsaKeys() {
            int key_length = 1024;  // 密钥长度
            unsigned long exponent = RSA_F4;  // 公钥指数

            // 生成RSA密钥
            RSA *rsa = RSA_new();
            BIGNUM *bn = BN_new();
            BN_set_word(bn, exponent);
            RSA_generate_key_ex(rsa, key_length, bn, NULL);

            // 保存私钥
            FILE *private_key_file = fopen("private.pem", "wb");
            if (private_key_file == NULL) {
                std::cerr << "Unable to open file for writing private key" << std::endl;
                RSA_free(rsa);
                BN_free(bn);
                return;
            }
            PEM_write_RSAPrivateKey(private_key_file, rsa, NULL, NULL, 0, NULL, NULL);
            fclose(private_key_file);

            // 保存公钥
            FILE *public_key_file = fopen("public.pem", "wb");
            if (public_key_file == NULL) {
                std::cerr << "Unable to open file for writing public key" << std::endl;
                RSA_free(rsa);
                BN_free(bn);
                return;
            }
//            PEM_write_RSAPublicKey(public_key_file, rsa);
            PEM_write_RSA_PUBKEY(public_key_file, rsa);
            fclose(public_key_file);

            // 释放资源
            RSA_free(rsa);
            BN_free(bn);
        }

        std::string Authenticator::hmacSHA256(const std::string& key, const std::string& data) {
            unsigned char* digest;
            unsigned int digest_len;
            digest = HMAC(EVP_sha256(), key.c_str(), key.length(),
                          reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), NULL, &digest_len);
            char mdString[2*digest_len+1];
            for (int i = 0; i < digest_len; i++)
                sprintf(&mdString[i*2], "%02x", (unsigned int)digest[i]);

            return std::string(mdString);
        }

        std::string Authenticator::readRsakey(const char *fileName) {
            FILE* file = fopen(fileName, "rb");
            if (file == nullptr) {
                LogError << "Unable to open key file.";
                return "";
            }

//            RSA* rsa = PEM_read_RSA_PUBKEY(file, nullptr, nullptr, nullptr);
            RSA* rsa = PEM_read_RSAPublicKey(file, NULL, NULL, NULL);
            fclose(file);
            if (rsa == nullptr) {
                LogError << "Error reading public key from PEM file.";
                return "";
            }

            BIO* bio = BIO_new(BIO_s_mem());
            if (!PEM_write_bio_RSA_PUBKEY(bio, rsa)) {
                BIO_free(bio);
                std::cerr << "Error writing public key to BIO." << std::endl;
                return "";
            }

            // 从 BIO 中读取数据到字符串
            char* bio_buffer;
            long bio_length = BIO_get_mem_data(bio, &bio_buffer);
            std::string key_string(bio_buffer, bio_length);
            // 释放 BIO 资源
            BIO_free(bio);

            return key_string;
        }

        // 签名函数
        std::string Authenticator::signMessage(const std::string &message, const char* privateKeyFilename) {
            RSA* rsa = nullptr;
            FILE* privateKeyFile = fopen(privateKeyFilename, "rb");
            if (!privateKeyFile) {
                LogError << "Unable to open private key file.";
                return "";
            }
            rsa = PEM_read_RSAPrivateKey(privateKeyFile, &rsa, nullptr, nullptr);
            fclose(privateKeyFile);
            if (!rsa) {
                LogError << "Error reading private key from PEM file.";
                return "";
            }

            unsigned char signature[256];
            unsigned int signature_len;

            unsigned char hash[SHA256_DIGEST_LENGTH];
            SHA256((unsigned char*)message.c_str(), message.length(), hash);
//            if (RSA_sign(NID_sha256, (const unsigned char*)message.c_str(), message.length(), signature, &signature_len, rsa) != 1) {
            if (RSA_sign(NID_sha256, hash, sizeof(hash), signature, &signature_len, rsa) != 1) {
                ERR_print_errors_fp(stderr);
                LogError << "RSA_sign error!";
                return "";
            }
            RSA_free(rsa);

            return std::string((char*)signature, signature_len);
        }

        // 验证签名函数
        bool Authenticator::verifySignature(const std::string &message, const std::string &signature, const char* pubKeyFilename) {
            RSA* rsa = nullptr;
            FILE* pubKeyFile = fopen(pubKeyFilename, "rb");
            if (!pubKeyFile) {
                LogError << "Unable to open public key file.";
                return false;
            }
//            rsa = PEM_read_RSAPublicKey(pubKeyFile, &rsa, nullptr, nullptr);
            rsa = PEM_read_RSA_PUBKEY(pubKeyFile, &rsa, nullptr, nullptr);
            fclose(pubKeyFile);
            if (!rsa) {
                LogError << "Error reading public key from PEM file.";
                return false;
            }

            bool verify = RSA_verify(NID_sha256, (const unsigned char*)message.c_str(), message.length(), (const unsigned char*)signature.c_str(), signature.length(), rsa) == 1;
            RSA_free(rsa);
            return verify;
        }


        std::string Authenticator::readKey(const std::string& filename) {
            std::ifstream file(filename);
            if (!file.is_open()) {
                throw std::runtime_error("cannot open file: " + filename);
            }

            std::string line;
            std::stringstream buffer;

            // 读取文件到字符串流，同时跳过PEM文件的头尾标记
            bool inKey = false;
            while (std::getline(file, line)) {
                if (line.find("-----END") != std::string::npos) {
                    break;
                }
                if (inKey) {
                    buffer << line;
                }
                if (line.find("-----BEGIN") != std::string::npos) {
                    inKey = true;
                }
            }
            file.close();

            std::string key = buffer.str();
            // 移除所有换行符，如果有
            key.erase(std::remove(key.begin(), key.end(), '\n'), key.end());
            key.erase(std::remove(key.begin(), key.end(), '\r'), key.end());
            return key;
        }

        string Authenticator::keyRegister() {
            long long nowTime = base_tools::BaseTimer::GetMilliTime();
//            long long nowTime = 1715915180115;
            string key = std::to_string(nowTime);
//            string key = "1715758571487";

//            string vPublicKey = readRsakey("public.pem");
            std::string vPublicKey;
            try {
//                vPublicKey = readKey("public.pem");
                vPublicKey = readKey("rsa_public_obu.pem");
            } catch (const std::exception& e) {
                LogError << "error: " << e.what();
            }
            LogDebug << "vPublicKey: " << vPublicKey;
            std::string data = common::GlobalData::Instance()->getImei();
//            std::string data = "LC6TCGC6670000000";
            data.append(vPublicKey);

            std::string digest = Authenticator::hmacSHA256(key, data);
            LogDebug << "HMAC-SHA256: " << digest;
            string hash;
            bool ret = base_tools::CBase64::Encode(reinterpret_cast<const unsigned char *>(digest.c_str()), digest.length(), hash);
            if(!ret){
                LogError << "base64 error!!!";
                return "";
            }
            LogDebug << "hash: " << hash;

//            std::string signature = signMessage(hash, "./privateKey.txt");
//            std::string signature = signMessage(hash, "./f_private.pem");
            std::string signature = signMessage(hash, "./pri.pem");
//            LogDebug << "signature: " << signature;
            // TODO debug
//            bool is_valid = verifySignature(hash, signature, "./f_public.pem");
            bool is_valid = verifySignature(hash, signature, "./pub.pem");
            if (is_valid) {
                LogDebug << "Signature is valid.";
            } else {
                LogWarn << "Signature is invalid.";
            }
            // end TODO debug

            string sign;
            ret = base_tools::CBase64::Encode(reinterpret_cast<const unsigned char *>(signature.c_str()), signature.length(), sign);
            LogDebug << "sign: " << sign;
            if(!ret){
                LogError << "base64 error!!!";
                return "";
            }

            string strUrl = common::GlobalData::Instance()->getJson()["Url"]["RegisterUrl"];
            std::vector<std::string> headers = SetCmimHeader();

            nlohmann::json jParam;
            jParam["id"] = common::GlobalData::Instance()->getImei();
            jParam["vPublicKey"] = vPublicKey;
            jParam["manufacturer"] = "比亚迪集团(勿删)";
            jParam["specification"] = "CA1258P11K2L7T1";
            jParam["registeAppId"] = "1780146430749450243";
            jParam["hash"] = hash;
            jParam["sign"] = sign;
            jParam["appId"] = "1780146430749450243";
            jParam["timestamp"] = nowTime;
//            jParam["timestamp"] = 1715758571487;
            std::string strParam = jParam.dump();
            LogDebug << "strParam: " << strParam;
//            string param = R"({"userName":"yangqiyan","pwd":"9e41538ef7f2d0f7d0a882a668710345"})";
            string strRes, strCode, strMess;
            // TODO
//            if (base_tools::CHttpClient::getInstance().HTCLPosts(strUrl, strParam, headers, strRes)) {
            if (base_tools::CHttpClient::getInstance().HTCLPost(strUrl, strParam, headers, strRes)) {
                LogInfo << "strRes: " << strRes;
                if (strRes.empty()) {
                    return "";
                } else {
                    if (nlohmann::json::accept(strRes)) {
                        nlohmann::json j = nlohmann::json::parse(strRes);
                        if (j.is_discarded()) {
                            LogWarn << "return json error! ";
                        } else {
                            bool bSuccess = j.value("success", false); 
                            int code = j.value("code", -1);
                            if (!bSuccess) {
                                string msg = j.value("message", "");
                                LogError << "register error: " << code << ", " << msg;
                                if(40201 == code){
                                    return strRes;
                                }
                                else{
                                    return "";
                                }
                            }
                            std::string checkData = j["data"]["checkData"]; //j["data"].value("checkData", "");
                            return checkData;
                        }
                    }
                }
                return "";
            }
            return "";
        }

        string Authenticator::registerConfirm(const std::string& checkData) {
            // 先解密数据
            // 加载RSA私钥
            FILE* privateKeyFile = fopen("rsa_private_obu.pem", "rb");
            if (!privateKeyFile) {
                LogError << "Cannot open private key file!";
                return "";
            }

            RSA* rsa = PEM_read_RSAPrivateKey(privateKeyFile, nullptr, nullptr, nullptr);
            fclose(privateKeyFile);

            if (!rsa) {
                LogError << "Cannot load private key!";
                return "";
            }

            size_t encryptedLength = checkData.length();
            unsigned char encryptedData[encryptedLength + 20];
            unsigned char decryptedData[encryptedLength + 20];
            unsigned long dataLen = encryptedLength + 20;
            bool ret = base_tools::CBase64::Decode(checkData, encryptedData, &dataLen);
//            LogDebug << "encryptedData after CBase64::Decode: " << encryptedData << ", ret:" << ret << ", len: " << dataLen;
            // 使用RSA私钥解密数据
            int decryptedLength = RSA_private_decrypt(dataLen,encryptedData, decryptedData, rsa, RSA_PKCS1_PADDING);

            if (decryptedLength == -1) {
                char err[130];
//                ERR_load_crypto_strings();
                ERR_error_string(ERR_get_error(), err);
                LogError << "decrypt error!";
                RSA_free(rsa);
                return "";
            }
            string checkD{(const char *)decryptedData, static_cast<size_t>(decryptedLength)};
            // 输出解密后的数据
            std::cout << "解密后的数据: " << decryptedData << std::endl;
            // 清理资源
            RSA_free(rsa);

            long long nowTime = base_tools::BaseTimer::GetMilliTime();
//            long long nowTime = 1715915180115;
            string key = std::to_string(nowTime);
            std::string vPublicKey;
            try {
                vPublicKey = readKey("rsa_public_obu.pem");
            } catch (const std::exception& e) {
                LogError << "error: " << e.what();
            }
            LogDebug << "vPublicKey: " << vPublicKey;
            std::string data = common::GlobalData::Instance()->getImei();  // vid
            data.append(vPublicKey);

            std::string digest = Authenticator::hmacSHA256(key, data);
            LogDebug << "HMAC-SHA256: " << digest;
            string hash;
            ret = base_tools::CBase64::Encode(reinterpret_cast<const unsigned char *>(digest.c_str()), digest.length(), hash);
            if(!ret){
                LogError << "base64 error!!!";
                return "";
            }
            LogDebug << "hash: " << hash;

            std::string signature = signMessage(hash, "./rsa_private_obu.pem");
            string sign;
            ret = base_tools::CBase64::Encode(reinterpret_cast<const unsigned char *>(signature.c_str()), signature.length(), sign);
            LogDebug << "sign: " << sign;
            if(!ret){
                LogError << "base64 error!!!";
                return "";
            }

            string strUrl = common::GlobalData::Instance()->getJson()["Url"]["RegisterConfirmUrl"];
            std::vector<std::string> headers = SetCmimHeader();

            nlohmann::json jParam;
            jParam["id"] = common::GlobalData::Instance()->getImei(); // 865697040171875
            jParam["checkData"] = checkD;
            jParam["hash"] = hash;
            jParam["sign"] = sign;
            jParam["keyVersion"] = "0";
            jParam["timestamp"] = nowTime;
            std::string strParam = jParam.dump();
            LogDebug << "strParam: " << strParam;
//            string param = R"({"userName":"yangqiyan","pwd":"9e41538ef7f2d0f7d0a882a668710345"})";
            string strRes, strCode, strMess;
            // TODO
//            if (base_tools::CHttpClient::getInstance().HTCLPosts(strUrl, strParam, headers, strRes)) {
            if (base_tools::CHttpClient::getInstance().HTCLPost(strUrl, strParam, headers, strRes)) {
                LogInfo << "strRes: " << strRes;
                if (strRes.empty()) {
                    return "";
                } else {
                    if (nlohmann::json::accept(strRes)) {
                        nlohmann::json j = nlohmann::json::parse(strRes);
                        if (j.is_discarded()) {
                            LogWarn << "return json error! ";
                        } else {
                            bool bSuccess = j.value("success", false);
                            int code = j.value("code", -1);
                            if (!bSuccess) {
                                string msg = j.value("message", "");
                                LogError << "register confirm error: " << code << ", " << msg;
                                return "";
                            }
                            std::string accessToken = j["data"].value("accessToken", "");
                            return accessToken;
                        }
                    }
                }
                return "";
            }
            return "";
        }

        string Authenticator::authenticate() {
            // 如下和 registerConfirm 一样，可以提取出来

            long long nowTime = base_tools::BaseTimer::GetMilliTime();
//            long long nowTime = 1715915180115;
            string key = std::to_string(nowTime);
            std::string vPublicKey;
            try {
                vPublicKey = readKey("rsa_public_obu.pem");
            } catch (const std::exception& e) {
                LogError << "error: " << e.what();
            }
            LogDebug << "vPublicKey: " << vPublicKey;
            std::string data = common::GlobalData::Instance()->getImei();  // vid
            data.append(vPublicKey);

            std::string digest = Authenticator::hmacSHA256(key, data);
            LogDebug << "HMAC-SHA256: " << digest;
            string hash;
            bool ret = base_tools::CBase64::Encode(reinterpret_cast<const unsigned char *>(digest.c_str()), digest.length(), hash);
            if(!ret){
                LogError << "base64 error!!!";
                return "";
            }
            LogDebug << "hash: " << hash;

            std::string signature = signMessage(hash, "./rsa_private_obu.pem");
            string sign;
            ret = base_tools::CBase64::Encode(reinterpret_cast<const unsigned char *>(signature.c_str()), signature.length(), sign);
            LogDebug << "sign: " << sign;
            if(!ret){
                LogError << "base64 error!!!";
                return "";
            }

            string strUrl = common::GlobalData::Instance()->getJson()["Url"]["AuthUrl"];
            std::vector<std::string> headers = SetCmimHeader();

            nlohmann::json jParam;
            jParam["id"] = common::GlobalData::Instance()->getImei(); // 865697040171875
            jParam["hash"] = hash;
            jParam["sign"] = sign;
            jParam["keyVersion"] = "0";
            jParam["timestamp"] = nowTime;
            std::string strParam = jParam.dump();
            LogDebug << "strParam: " << strParam;
//            string param = R"({"userName":"yangqiyan","pwd":"9e41538ef7f2d0f7d0a882a668710345"})";
            string strRes, strCode, strMess;
            // TODO
//            if (base_tools::CHttpClient::getInstance().HTCLPosts(strUrl, strParam, headers, strRes)) {
            if (base_tools::CHttpClient::getInstance().HTCLPost(strUrl, strParam, headers, strRes)) {
                LogInfo << "strRes: " << strRes;
                if (strRes.empty()) {
                    return "";
                } else {
                    if (nlohmann::json::accept(strRes)) {
                        nlohmann::json j = nlohmann::json::parse(strRes);
                        if (j.is_discarded()) {
                            LogWarn << "return json error! ";
                        } else {
                            bool bSuccess = j.value("success", false);
                            int code = j.value("code", -1);
                            if (!bSuccess) {
                                string msg = j.value("message", "");
                                LogError << "register confirm error: " << code << ", " << msg;
                                return "";
                            }
                            std::string encryptSecret = j["data"].value("encryptSecret", "");
                            if(!encryptSecret.empty()){
                                // 先解密数据
                                // 加载RSA私钥
                                FILE* privateKeyFile = fopen("./pri.pem", "rb");
                                if (!privateKeyFile) {
                                    LogError << "Cannot open private key file!";
                                    return "";
                                }

                                RSA* rsa = PEM_read_RSAPrivateKey(privateKeyFile, nullptr, nullptr, nullptr);
                                fclose(privateKeyFile);

                                if (!rsa) {
                                    LogError << "Cannot load private key!";
                                    return "";
                                }

                                size_t encryptedLength = encryptSecret.length();
                                unsigned char encryptedData[100] = {0};
                                unsigned char decryptedData[100] = {0};
                                unsigned long dataLen = encryptedLength + 20;

                                bool ret = base_tools::CBase64::Decode(encryptSecret, encryptedData, &dataLen);

                                // 使用RSA私钥解密数据
                                int decryptedLength = RSA_private_decrypt(dataLen,encryptedData, decryptedData, rsa, RSA_PKCS1_PADDING);
                                if (decryptedLength == -1) {
                                    char err[130];
//                ERR_load_crypto_strings();
                                    ERR_error_string(ERR_get_error(), err);
                                    LogError << "decrypt error!";
                                    RSA_free(rsa);
                                    return "";
                                }
//                                string checkD{(const char *)decryptedData, static_cast<size_t>(decryptedLength)};
                                string dData{(const char *)decryptedData, strlen((const char*)decryptedData)};
                                // 输出解密后的数据
                                std::cout << "decryptedData: " << dData <<", 长度： " << dData.size() << std::endl;
                                nlohmann::json rwJson(common::GlobalData::Instance()->getRwJson());
                                rwJson["Security"]["EncryptSecret"] = dData;
                                common::GlobalData::Instance()->setRwJson(rwJson);
                                common::GlobalData::Instance()->writeToFile(rwJson); //TODO
                                // 清理资源
                                RSA_free(rsa);
                            }
                            std::string accessToken = j["data"].value("accessToken", "");
                            return accessToken;
                        }
                    }
                }
                return "";
            }
            return "";
        }

        string Authenticator::getServiceInfo(const std::string& token, std::string& strRes) {
            string strUrl = common::GlobalData::Instance()->getJson()["Url"]["ServiceInfoUrl"];
            std::vector<std::string> headers = SetCmimHeader(token);

            GPSData gpsData = GPS::getInstance().gData;

            nlohmann::json jParam;
           jParam["longitude"] = (long long)(gpsData.longitude * 1e7);
           jParam["latitude"] = (long long)(gpsData.latitude * 1e7);
            // jParam["longitude"] = 1140827810L;//武汉
            // jParam["latitude"] = 304539259L;
            // jParam["longitude"] = 1203056189L;
            // jParam["latitude"] = 314789773L;
//            jParam["latitude"] = 1214699598L;
//            jParam["longitude"] = 312326402L;
            jParam["gcs"] = "WGS84";
            // jParam["tac"] = "vehicleTwo20240730";//getCellInfo()获取
            jParam["tac"] = "";//getCellInfo()获取
            std::string strParam = jParam.dump();
            LogDebug << "strParam: " << strParam;

//            strUrl += "longitude=";
//            strUrl += std::to_string(1206271980);
//            strUrl += "&latitude=";
//            strUrl += std::to_string(314243401);
//            strUrl += "&gcs=WGS84";
//            LogInfo << "strUrl: " << strUrl;

            string strCode, strMess;
            // TODO
//            if (base_tools::CHttpClient::getInstance().HTCLPosts(strUrl, strParam, headers, strRes)) {
            if (base_tools::CHttpClient::getInstance().HTCLPost(strUrl, strParam, headers, strRes)) {
//                if (base_tools::CHttpClient::getInstance().HTCLGet(strUrl, headers, strRes)) {
                LogInfo << "strRes: " << strRes;
                if (strRes.empty()) {
                    return "";
                } else {
                    if (nlohmann::json::accept(strRes)) {
                        nlohmann::json j = nlohmann::json::parse(strRes);
                        if (j.is_discarded()) {
                            LogWarn << "return json error! ";
                        } else {
                            bool bSuccess = j.value("success", false);
                            int code = j.value("code", -1);
                            if (!bSuccess) {
                                string msg = j.value("message", "");
                                LogError << "register confirm error: " << code << ", " << msg;
                                return "";
                            }
                            // systemId = j["data"][0].value("systemId", "");
                            std::string security = j["data"].value("security", "");
                            return security;
                        }
                    }
                }
                return "";
            }
            return "";
        }

        string Authenticator::getAuthToken(const std::string& security) {
            string strUrl = common::GlobalData::Instance()->getJson()["Url"]["AuthTokenUrl"];
            strUrl += security;
            LogDebug << "strUrl: " << strUrl;
            std::vector<std::string> headers = SetCmimHeader();
            string strRes, strCode, strMess;
            // TODO
            if (base_tools::CHttpClient::getInstance().HTCLGet(strUrl, headers, strRes)) {
                LogInfo << "strRes: " << strRes;
                if (strRes.empty()) {
                    return "";
                } else {
                    if (nlohmann::json::accept(strRes)) {
                        nlohmann::json j = nlohmann::json::parse(strRes);
                        if (j.is_discarded()) {
                            LogWarn << "return json error! ";
                        } else {
                            bool bSuccess = j.value("success", false);
                            int code = j.value("code", -1);
                            if (!bSuccess) {
                                string msg = j.value("message", "");
                                LogError << "register confirm error: " << code << ", " << msg;
                                return "";
                            }
                            std::string accessToken = j["data"].value("accessToken", "");
                            return accessToken;
                        }
                    }
                }
                return "";
            }
            return "";
        }

        std::tuple<std::string, std::string, std::string> Authenticator::getMqttInfo(const std::string& systemId) {
            string strUrl = common::GlobalData::Instance()->getJson()["Url"]["QueryEmqxByIdUrl"];
            strUrl += systemId;
            LogDebug << "strUrl: " << strUrl;
            std::vector<std::string> headers = SetCmimHeader();
            string strRes, strCode, strMess;
            // TODO
            if (base_tools::CHttpClient::getInstance().HTCLGet(strUrl, headers, strRes)) {
                LogInfo << "strRes: " << strRes;
                if (strRes.empty()) {
                    return make_tuple("", "", "");
                } else {
                    if (nlohmann::json::accept(strRes)) {
                        nlohmann::json j = nlohmann::json::parse(strRes);
                        if (j.is_discarded()) {
                            LogWarn << "return json error! ";
                        } else {
                            bool bSuccess = j.value("success", false);
                            int code = j.value("code", -1);
                            if (!bSuccess) {
                                string msg = j.value("message", "");
                                LogError << "error: " << code << ", " << msg;
                                make_tuple("", "", "");
                            }
                            std::string mqttUrl = j["data"][0].value("url", "");
                            std::string mqttUserName = j["data"][0].value("mqttUserName", "");
                            std::string mqttPassword = j["data"][0].value("mqttPassword", "");
                            LogInfo << "mqtt Url: " << mqttUrl << ", mqttUserName: " << mqttUserName << ", mqttPassword: " << mqttPassword;
                            return make_tuple(mqttUrl, mqttUserName, mqttPassword);
                        }
                    }
                }
                return make_tuple("", "", "");
            }
            return make_tuple("", "", "");
        }

    } // end namespace vwise
} // end namespace cmsr
