/*******************************************************************************
 * Copyright (c) 2012, 2020 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *   https://www.eclipse.org/legal/epl-2.0/
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Ian Craggs - initial contribution
 *******************************************************************************/

#include "mqtt_async_app.h"
#include <stdint.h>
#if !defined(_WIN32)
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#else
// #include <WinSock2.h>
#include <Iphlpapi.h>
#include <iostream>
#include <windows.h>
#pragma comment(lib, "Iphlpapi.lib")
#endif
#include "global.h"
#include "log.h"
#include <fstream>

#if defined(_WRS_KERNEL)
#include <OsWrapper.h>
#endif

namespace cmsr {
    namespace vwise {

#define MQTT_KEEPALIVE 20

        using json = nlohmann::json;
        using namespace std;
        //MQTTAPP *mqtt = NULL;

#define QOS 0
#define TIMEOUT 10000L

        MQTTAPP::MQTTAPP() {}


        void MQTTAPP::onConnectSuccess(void *context, MQTTAsync_successData *response) {
            //MQTTAsync client = (MQTTAsync)context;
            MqttBroker *broker = (MqttBroker *) context;
            MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
            int rc;

            LogInfo << "Successful connection";
            LogInfo << "Subscribing to topic for client using QOS:" << QOS;

            opts.onSuccess = onSubscribeSuccess;
            opts.onFailure = onSubscribeFailure;
            opts.context = broker;
            //LogInfo >>"broker->topics.size():" >>broker->topics.size();

            for (auto &topic: broker->topics) {
                if ((rc = MQTTAsync_subscribe(broker->client, topic.data(), QOS, &opts)) != MQTTASYNC_SUCCESS) {
                    // LogInfo <<  "Failed to start subscribe [" << topic << "], return code:" << rc;
                } else {
                    LogInfo << "subcribe [" << topic << "] success";
                }
            }
        }

        void MQTTAPP::onConnectFailure(void *context, MQTTAsync_failureData *response) {
            LogInfo << "Connect failed, rc:" << response->code;
            //MQTTAsync client = (MQTTAsync)context;
            MqttBroker *broker = (MqttBroker *) context;
            MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
            int rc;

            LogInfo << "onConnectFailure Reconnecting after 2s";
            sleep(2);
            conn_opts.keepAliveInterval = MQTT_KEEPALIVE;
            conn_opts.cleansession = 1;
            conn_opts.onSuccess = onConnectSuccess;
            conn_opts.onFailure = onConnectFailure;
            conn_opts.context = broker;
            conn_opts.username = broker->username.c_str();
            conn_opts.password = broker->password.c_str();

            //conn_opts.automaticReconnect = 1;
            //conn_opts.minRetryInterval = 1;
            //conn_opts.maxRetryInterval = 30;
            //conn_opts.maxInflight = 1;


            if ((rc = MQTTAsync_connect(broker->client, &conn_opts)) != MQTTASYNC_SUCCESS) {
                LogInfo << "Failed to start connect, return code:" << rc;
            }
        }

        void MQTTAPP::messageSend(std::string topic, std::string payload) {
            sendOnce(topic, payload);
        }

        bool MQTTAPP::sendOnce(const std::string &topic, const std::string &payload) {
            if (client == nullptr)
                return false;
            MQTTAsync_responseOptions resp_opts = MQTTAsync_responseOptions_initializer;
            resp_opts.onSuccess = onSendSuccess;
            resp_opts.onFailure = onSendFailure;
            resp_opts.context = client;
            MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
            pubmsg.payload = (void *) payload.data();
            pubmsg.payloadlen = (int) payload.size();
            pubmsg.qos = QOS;
            pubmsg.retained = 0;

            int rc = MQTTAsync_sendMessage(client, topic.data(), &pubmsg, &resp_opts);
            if (rc != MQTTASYNC_SUCCESS) {
                LogError << "Failed to start sendMessage, return code:" << rc;
                return false;
            }
            return true;
        }

        bool MQTTAPP::isConnected() const {
            return client != nullptr && MQTTAsync_isConnected(client);
        }

        void MQTTAPP::connLost(void *context, char *cause) {
            //MQTTAsync client = (MQTTAsync)context;
            MqttBroker *broker = (MqttBroker *) context;
            MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
            int rc;

            LogInfo << "Connection lost\n";
            if (cause) {
                LogInfo << "cause:" << cause;
            }

            LogInfo << "Reconnecting\n";
            conn_opts.keepAliveInterval = MQTT_KEEPALIVE;
            conn_opts.cleansession = 1;
            conn_opts.onSuccess = onConnectSuccess;
            conn_opts.onFailure = onConnectFailure;
            conn_opts.context = broker;
            conn_opts.username = broker->username.c_str();
            conn_opts.password = broker->password.c_str();

           // conn_opts.automaticReconnect = 1;
           // conn_opts.minRetryInterval = 1;
            //conn_opts.maxRetryInterval = 30;
            //conn_opts.maxInflight = 1;


            if ((rc = MQTTAsync_connect(broker->client, &conn_opts)) != MQTTASYNC_SUCCESS) {
                //LogError << "Failed to start connect, return code:" << rc;
            }
            //LogInfo << "Reconnect Successfully";
        }

        void MQTTAPP::onDisconnectFailure(void *context, MQTTAsync_failureData *response) {
            // LOG(INFO) << "Disconnect failed, rc :" << response->code;
        }

        void MQTTAPP::onDisconnectSuccess(void *context, MQTTAsync_successData *response) {
            // LOG(INFO) << "Successful disconnection";
        }

        void MQTTAPP::onSubscribeSuccess(void *context, MQTTAsync_successData *response) {
            // LOG(INFO) << "Subscribe or Unsubscribe succeeded";
        }

        void MQTTAPP::onSubscribeFailure(void *context, MQTTAsync_failureData *response) {
            // LOG(INFO) << "Subscribe or Unsubscribe failed, rc :" << response->code;
        }

        void MQTTAPP::onSendSuccess(void *context, MQTTAsync_successData *response) {
            std::cout << "Message with token value:" << response->token << " delivery confirmed" << std::endl;
        }

        void MQTTAPP::onSendFailure(void *context, MQTTAsync_failureData *response) {
            std::cout << "Message send failed token" << response->token << "error code:" << response->code << std::endl;
        }

        void MQTTAPP::stop() {
            int rc;
            disc_opts.onSuccess = onDisconnectSuccess;
            disc_opts.onFailure = onDisconnectFailure;
            LogInfo << "==================mqtt stop MQTTAsync_disconnect...be";
            if ((rc = MQTTAsync_disconnect(client, &disc_opts)) != MQTTASYNC_SUCCESS) {
                LogInfo << "Failed to start disconnect, return code:" << rc;
            }
            LogInfo << "==================mqtt stop MQTTAsync_disconnect...";
            if (client != nullptr) {
                MQTTAsync_destroy(&client);
                client = nullptr;
            }
            LogInfo << "==================mqtt stop MQTTAsync_destroy...";
        }

        void MQTTAPP::start(MQTTAsync_messageArrived *msgSubscriberHandler, MqttBroker broker) {
            LogInfo << "Enter MQTTAPP::start\n";
            //mqtt = this;
            int rc;
            if ((rc = MQTTAsync_create(&client, broker.address.c_str(), broker.clientId.c_str(), MQTTCLIENT_PERSISTENCE_NONE, NULL)) != MQTTASYNC_SUCCESS) {
                LogInfo << "Failed to create client, return code:" << rc;
                rc = EXIT_FAILURE;
            }
            // MQTTAsync_createOptions options = MQTTAsync_createOptions_initializer;
            // options.maxBufferedMessages = 0;

            // if ((rc = MQTTAsync_createWithOptions(&client, broker.address.c_str(), broker.clientId.c_str(), MQTTCLIENT_PERSISTENCE_NONE, NULL, &options)) != MQTTASYNC_SUCCESS)
            // {
            // 	LogInfo  << "Failed to create client, return code:" << rc;
            // 	rc = EXIT_FAILURE;
            // }

            mBroker = broker;
            mBroker.client = client;

            // if ((rc = MQTTAsync_setCallbacks(client, &mBroker, connLost, msgSubscriberHandler, NULL)) != MQTTASYNC_SUCCESS) {
            //     LogInfo << "MQTTAsync_setCallbacks, return code:" << rc;
            //     rc = EXIT_FAILURE;
            // }
            if ((rc = MQTTAsync_setCallbacks(client, &mBroker, connLost, msgSubscriberHandler, NULL)) != MQTTASYNC_SUCCESS) {
                LogInfo << "MQTTAsync_setCallbacks, return code:" << rc;
                rc = EXIT_FAILURE;
            }

            conn_opts.keepAliveInterval = MQTT_KEEPALIVE;
            conn_opts.cleansession = 1;
            conn_opts.onSuccess = onConnectSuccess;
            conn_opts.onFailure = onConnectFailure;
            conn_opts.context = &mBroker;
            conn_opts.username = mBroker.username.c_str();
            conn_opts.password = mBroker.password.c_str();

          //  conn_opts.automaticReconnect = 1;
           // conn_opts.minRetryInterval = 1;
            //conn_opts.maxRetryInterval = 30;
           // conn_opts.maxInflight = 1;


            if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS) {
                LogInfo << "Failed to start connect, return code:" << rc;
            }
            LogInfo << "Enter MQTTAPP::start step 1\n";
        }
        void MQTTAPP::readJsonFile(const std::string &cfgPath) {
            std::string TopicPrefix;
            std::string OrderDown;
            std::string OrderAck;
            std::string UserOnline;
            std::string UserOnlineAck;
            std::string PingUp;
            std::string WirelessUp;
            std::string PC5Up;
            std::string BsmUp;
            std::string BsmDown;
            std::string RsmDown;
            std::string RsiDown;
            std::string SpatDown;
            std::string MapDown;
            std::string UploadDown;
            std::string UploadDownAck;
            std::string Heartbeat;
            std::string V2xTopicPrefix;

            std::string UuReport;
            std::string Pc5Report;
            std::string SwitchingChannels;
            std::string SwitchingChannelsAck;

            std::fstream fin(cfgPath);
            //if (nlohmann::json::accept(fin))
            {
                nlohmann::json j = nlohmann::json::parse(fin);

                if (j.is_discarded()) {
                    LogInfo << "readJsonFile is error";
                    return;
                } else {
                    if (j["Client"].is_object()) {
                        nlohmann::json jClient = j["Client"];

                        if (jClient["Mqtt"].is_object()) {
                            nlohmann::json jMqtt = jClient["Mqtt"];
                            if (jMqtt["Address"].is_string())
                                jMqtt.at("Address").get_to(m_address);
                            if (jMqtt["UserName"].is_string())
                                jMqtt.at("UserName").get_to(m_username);
                            if (jMqtt["Password"].is_string())
                                jMqtt.at("Password").get_to(m_password);
                            if (m_imei.empty()) {
                                if (jMqtt["Imei"].is_string())
                                    jMqtt.at("Imei").get_to(m_imei);
                            }
                            if (jMqtt["PingAddress"].is_string())
                                jMqtt.at("PingAddress").get_to(m_pingAddress);
                            if (jMqtt["TopicPrefix"].is_string())
                                jMqtt.at("TopicPrefix").get_to(TopicPrefix);
                            if (jMqtt["V2xTopicPrefix"].is_string())
                                jMqtt.at("V2xTopicPrefix").get_to(V2xTopicPrefix);
                            if (jMqtt["UserOnline"].is_string())
                                jMqtt.at("UserOnline").get_to(UserOnline);
                            if (jMqtt["UserOnlineAck"].is_string())
                                jMqtt.at("UserOnlineAck").get_to(UserOnlineAck);
                            if (jMqtt["OrderDown"].is_string())
                                jMqtt.at("OrderDown").get_to(OrderDown);
                            if (jMqtt["OrderAck"].is_string())
                                jMqtt.at("OrderAck").get_to(OrderAck);
                            if (jMqtt["PingUp"].is_string())
                                jMqtt.at("PingUp").get_to(PingUp);
                            if (jMqtt["WirelessUp"].is_string())
                                jMqtt.at("WirelessUp").get_to(WirelessUp);
                            if (jMqtt["PC5Up"].is_string())
                                jMqtt.at("PC5Up").get_to(PC5Up);
                            if (jMqtt["BsmUp"].is_string())
                                jMqtt.at("BsmUp").get_to(BsmUp);
                            if (jMqtt["BsmDown"].is_string())
                                jMqtt.at("BsmDown").get_to(BsmDown);
                            if (jMqtt["RsmDown"].is_string())
                                jMqtt.at("RsmDown").get_to(RsmDown);
                            if (jMqtt["UploadDown"].is_string())
                                jMqtt.at("UploadDown").get_to(UploadDown);
                            if (jMqtt["RsiDown"].is_string())
                                jMqtt.at("RsiDown").get_to(RsiDown);
                            if (jMqtt["SpatDown"].is_string())
                                jMqtt.at("SpatDown").get_to(SpatDown);
                            if (jMqtt["MapDown"].is_string())
                                jMqtt.at("MapDown").get_to(MapDown);
                            if (jMqtt["UploadDownAck"].is_string())
                                jMqtt.at("UploadDownAck").get_to(UploadDownAck);
                            if (jMqtt["Heartbeat"].is_string())
                                jMqtt.at("Heartbeat").get_to(Heartbeat);

                            if (jMqtt["UuReport"].is_string())
                                jMqtt.at("UuReport").get_to(UuReport);
                            if (jMqtt["Pc5Report"].is_string())
                                jMqtt.at("Pc5Report").get_to(Pc5Report);
                            if (jMqtt["SwitchChannels"].is_string())
                                jMqtt.at("SwitchChannels").get_to(SwitchingChannels);
                            if (jMqtt["SwitchChannelsAck"].is_string())
                                jMqtt.at("SwitchChannelsAck").get_to(SwitchingChannelsAck);
                        }
                    }
                }
                fin.close();

                TOPIC_USER_ONLINE = TopicPrefix + m_imei + UserOnline;
                TOPIC_USER_ONLINE_ACK = TopicPrefix + m_imei + UserOnlineAck;
                TOPIC_DVICE_PING_UP = TopicPrefix + m_imei + PingUp;
                TOPIC_DVICE_WIRELESS_UP = TopicPrefix + m_imei + WirelessUp;
                TOPIC_PLAT_BSM_DOWN = V2xTopicPrefix + m_imei + BsmDown;
                TOPIC_PLAT_RSM_DOWN = V2xTopicPrefix + m_imei + RsmDown;
                TOPIC_PLAT_RSI_DOWN = V2xTopicPrefix + m_imei + RsiDown;
                TOPIC_PLAT_SPAT_DOWN = V2xTopicPrefix + m_imei + SpatDown;
                TOPIC_PLAT_MAP_DOWN = V2xTopicPrefix + m_imei + MapDown;
                TOPIC_PLAT_UPLOAD_DOWN = TopicPrefix + m_imei + UploadDown;
                TOPIC_PLAT_UPLOAD_DOWN_ACK = TopicPrefix + m_imei + UploadDownAck;
                TOPIC_USER_HEARTBEAT = TopicPrefix + m_imei + Heartbeat;

                TOPIC_USER_UU_REPORT = TopicPrefix + m_imei + UuReport;
                TOPIC_USER_SWITCHING_CHANNELS = TopicPrefix + m_imei + SwitchingChannels;
                TOPIC_USER_SWITCHING_CHANNELS_ACK = TopicPrefix + m_imei + SwitchingChannelsAck;


                LogInfo << "TOPIC_USER_ONLINE:" << TOPIC_USER_ONLINE;
                LogInfo << "TOPIC_USER_ONLINE_ACK:" << TOPIC_USER_ONLINE_ACK;
                LogInfo << "TOPIC_DVICE_PING_UP:" << TOPIC_DVICE_PING_UP;
                LogInfo << "TOPIC_DVICE_WIRELESS_UP:" << TOPIC_DVICE_WIRELESS_UP;
                LogInfo << "TOPIC_PLAT_BSM_DOWN:" << TOPIC_PLAT_BSM_DOWN;
                LogInfo << "TOPIC_PLAT_RSM_DOWN:" << TOPIC_PLAT_RSM_DOWN;
                LogInfo << "TOPIC_PLAT_UPLOAD_DOWN:" << TOPIC_PLAT_UPLOAD_DOWN;
                LogInfo << "TOPIC_PLAT_RSI_DOWN:" << TOPIC_PLAT_RSI_DOWN;
                LogInfo << "TOPIC_PLAT_SPAT_DOWN:" << TOPIC_PLAT_SPAT_DOWN;
                LogInfo << "TOPIC_PLAT_MAP_DOWN:" << TOPIC_PLAT_MAP_DOWN;
                LogInfo << "TOPIC_PLAT_UPLOAD_DOWN_ACK:" << TOPIC_PLAT_UPLOAD_DOWN_ACK;
                LogInfo << "TOPIC_USER_HEARTBEAT:" << TOPIC_USER_HEARTBEAT;
                LogInfo << "TOPIC_USER_UU_REPORT:" << TOPIC_USER_UU_REPORT;
                LogInfo << "TOPIC_USER_SWITCHING_CHANNELS:" << TOPIC_USER_SWITCHING_CHANNELS;
                LogInfo << "TOPIC_USER_SWITCHING_CHANNELS_ACK:" << TOPIC_USER_SWITCHING_CHANNELS_ACK;

                LogInfo << "address:" << m_address;
                LogInfo << "password:" << m_password;
                LogInfo << "username:" << m_username;
            }
        }

    }// namespace vwise
}// namespace cmsr
