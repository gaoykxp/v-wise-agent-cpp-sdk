/**********************************************************************************************************************
    > File Name: main.cpp
    > Author: dmhuang
    > Date: 9/20/24
**********************************************************************************************************************/

#include "authenticate.h"
#include "global.h"
#include "log.h"
#include "probe_mgr.h"
#include "rpi_gps_info.h"
#include "rpi_module_interface.h"
#include "rpi_serial_port.h"
#ifdef ENABLE_SATELLITE
#include "rpi_satellite_terminal.h"
#endif
#include <iostream>
#include <string>

using namespace std;
using namespace cmsr::vwise;
#define MAX_RETRIES 3

// Serial port configuration for 5G modem
static tbox::SerialPort g_serialPort;
static const char* SERIAL_PORT_DEVICE = "/dev/ttyUSB2";
static const int SERIAL_PORT_BAUD_RATE = 115200;

// Satellite terminal configuration
#ifdef ENABLE_SATELLITE
static const char* SATELLITE_PORT_DEVICE = "/dev/ttyUSB1";
static const int SATELLITE_BAUD_RATE = 115200;
#endif

int main(int argc, char **argv) {

    std::string exe_dir = common::GlobalData::Instance()->programPath();
    LogInfo << "Current executable path is :" << exe_dir;
    LogInfo << "imei is :" << common::GlobalData::Instance()->getImei();

    // Initialize serial port for 5G modem
    if (g_serialPort.open(SERIAL_PORT_DEVICE, SERIAL_PORT_BAUD_RATE)) {
        LogInfo << "Serial port " << SERIAL_PORT_DEVICE << " opened successfully at " << SERIAL_PORT_BAUD_RATE << " baud";
    } else {
        LogWarn << "Failed to open serial port " << SERIAL_PORT_DEVICE;
    }

    // Initialize RPi Module Interface with AT port for 5G modem
    rpi::RPIModuleInterface::getInstance().setAtPort(SERIAL_PORT_DEVICE, SERIAL_PORT_BAUD_RATE);
    rpi::RPIModuleInterface::getInstance().init();

    // Initialize Satellite Terminal
#ifdef ENABLE_SATELLITE
    rpi::SatelliteTerminal satelliteTerminal;
    rpi::SatelliteTerminalConfig satConfig;
    satConfig.port_name = SATELLITE_PORT_DEVICE;
    satConfig.baud_rate = SATELLITE_BAUD_RATE;
    satConfig.read_timeout_ms = 2000;
    satConfig.query_timeout_ms = 3000;

    if (satelliteTerminal.open(satConfig)) {
        LogInfo << "Satellite terminal opened on " << SATELLITE_PORT_DEVICE;

        // Query IC information
        LogInfo << "Querying IC information...";
        auto ic_info = satelliteTerminal.query_ic_info(3000);
        if (ic_info && ic_info->valid) {
            LogInfo << "=== IC Information ===";
            LogInfo << "IC Type: " << ic_info->ic_type;
            LogInfo << "IC Status: " << ic_info->ic_status;
        } else {
            LogWarn << "Failed to query IC information or response invalid";
        }

        // Query device information/version
        LogInfo << "Querying device version...";
        auto dev_info = satelliteTerminal.query_device_info(3000);
        if (dev_info && dev_info->valid) {
            LogInfo << "=== Device Information ===";
            LogInfo << "Device Type: " << dev_info->device_type;
            LogInfo << "Hardware Version 1: " << dev_info->hardware_ver1;
            LogInfo << "Hardware Version 2: " << dev_info->hardware_ver2;
            LogInfo << "Software Version: " << dev_info->software_version;
            LogInfo << "Module Version: " << dev_info->module_version;
        } else {
            LogWarn << "Failed to query device information or response invalid";
        }
    } else {
        LogWarn << "Failed to open satellite terminal on " << SATELLITE_PORT_DEVICE;
    }
#endif

    if (argc > 1)
    {
        char mode = argv[1][0];
        switch (mode)
        {
        case 'g':
            LogInfo << "Generate RSA keys selected.";
            Authenticator::getInstance().genRsaKeys();
            LogInfo << "RSA key pair generated and saved to files.";
            break;
        default:
            LogWarn << "Invalid mode specified. Please use 'g' to Generate RSA keys!";
        }
    }

    GPS::getInstance().init();
    GPS::getInstance().start();

    if (1 == common::GlobalData::Instance()->getJson()["Vwise"].value("AuthEnable", 0))
    {
        while (1)
        {
            if (Authenticator::getInstance().authenticate().size() > 0)
            {
                ProbeMgr::getInstance().probeMgrStart();
                break;
            }
            sleep(1);
        }
    }
    else
    {
        ProbeMgr::getInstance().probeMgrStart();
    }

    while (true)
    {
        sleep(5);
    }

    return 0;
}