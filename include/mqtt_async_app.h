/*******************************************************************************
 * Copyright (c) 2009, 2020 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    https://www.eclipse.org/legal/epl-2.0/
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Ian Craggs - initial API and implementation and/or initial documentation
 *******************************************************************************/

#ifndef MQTT_ASYNC_APP_H
#define MQTT_ASYNC_APP_H

#include "MQTTAsync.h"
#include "MQTTClientPersistence.h"
#include "nlohmann/json.hpp"
#include "util.h"
#include <future>
#include "timer.hpp"
#include <vector>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
//#include <functional>



namespace cmsr {
    namespace vwise {

	typedef	int (*recevieMsgHander)(void* context, std::string input);

	typedef struct {
		std::string  topicName;
		recevieMsgHander msgHander;
	}TopicMsgTable;

	typedef struct {
		std::string  address;
		std::string  username;
		std::string  password;
		std::string  clientId;
		std::vector<std::string> topics;
		MQTTAsync client;
	}MqttBroker;

	class MQTTAPP {
	public:
		MQTTAPP();
		~MQTTAPP()
		{
			std::cout << " ~MQTTAPP\n";
			
			if(client != nullptr)
			{
				MQTTAsync_destroy(&client);
				client = nullptr;
				mBroker.client = nullptr;
			}
			
		};

		void start(MQTTAsync_messageArrived* msgSubscriberHandler, MqttBroker broker);
		void stop();
		void messageSend(std::string topic, std::string payload);
		void readJsonFile(const std::string &cfgPath);

		static void connLost(void* context, char* cause);
		//static int msgSubscriberHandler1(void* context, char* topicName, int topicLen, MQTTAsync_message* message);
		static void onDisconnectSuccess(void* context, MQTTAsync_successData* response);
		static void onDisconnectFailure(void* context, MQTTAsync_failureData* response);
		static void onConnectSuccess(void* context, MQTTAsync_successData* response);
		static void onConnectFailure(void* context, MQTTAsync_failureData* response);
		static void onSendSuccess(void* context, MQTTAsync_successData* response);
		static void onSendFailure(void* context, MQTTAsync_failureData* response);
		static void onSubscribeSuccess(void* context, MQTTAsync_successData* response);
		static void onSubscribeFailure(void* context, MQTTAsync_failureData* response);


	private:
		MQTTAsync client = nullptr;
		MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
		MQTTAsync_disconnectOptions disc_opts = MQTTAsync_disconnectOptions_initializer;
		
	private:
		MqttBroker mBroker;
		std::promise<std::string> promise;
		Timer timer;

// added raw
	private:
		std::string m_address;
		std::string m_clientId;
		std::string m_username;
		std::string m_password;

    public:
        std::string m_pingAddress;
		std::string m_imei;
		std::string TOPIC_USER_ONLINE;
		std::string TOPIC_USER_ONLINE_ACK;
		std::string TOPIC_DVICE_PING_UP;
		std::string TOPIC_DVICE_WIRELESS_UP;
		std::string TOPIC_PLAT_BSM_DOWN;
		std::string TOPIC_PLAT_RSM_DOWN;
		std::string TOPIC_PLAT_RSI_DOWN;
		std::string TOPIC_PLAT_SPAT_DOWN;
		std::string TOPIC_PLAT_MAP_DOWN;
		std::string TOPIC_PLAT_UPLOAD_DOWN;
		std::string TOPIC_PLAT_UPLOAD_DOWN_ACK;
		std::string TOPIC_USER_HEARTBEAT;

		std::string TOPIC_USER_UU_REPORT;
		std::string TOPIC_USER_SWITCHING_CHANNELS;
		std::string TOPIC_USER_SWITCHING_CHANNELS_ACK;
		
	};

	}
}



#endif
