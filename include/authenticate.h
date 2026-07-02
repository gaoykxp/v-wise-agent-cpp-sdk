/**********************************************************************************************************************
    > File Name: authenticate.h
    > Author: htsong
    > Date: 5/8/24
**********************************************************************************************************************/

#ifndef TANGO_NEXUS_AUTHENTICATOR_H
#define TANGO_NEXUS_AUTHENTICATOR_H

#include <string>
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>

namespace cmsr {
    namespace vwise {
        class Authenticator {
        public:
            std::string keyRegister();
            std::string registerConfirm(const std::string& checkData);
            std::string authenticate();
            std::string getServiceInfo(const std::string& token, std::string& systemId);
            std::string getAuthToken(const std::string& security);
            std::tuple<std::string, std::string, std::string> getMqttInfo(const std::string& systemId);

        private:

        public:
            static Authenticator &getInstance();
            static void genRsaKeys();
            static std::string hmacSHA256(const std::string& key, const std::string& data);
            static std::string readRsakey(const char *fileName);
            static std::string signMessage(const std::string &message, const char* privKeyFilename);
            bool verifySignature(const std::string &message, const std::string &signature, const char* pubKeyFilename);
            std::string readKey(const std::string& filename);

        private:
            Authenticator() = default;

            Authenticator(const Authenticator &) = delete;

            Authenticator &operator=(const Authenticator &) = delete;
        };
    } //end namespace vwise
} //end namespace cmsr

#endif //TANGO_NEXUS_AUTHENTICATOR_H