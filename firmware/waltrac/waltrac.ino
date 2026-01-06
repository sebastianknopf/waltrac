#include <HardwareSerial.h>
#include <WalterModem.h>
#include <esp_mac.h>
#include <esp_log.h>

#include "Messages.h"
#include "WaltracConfig.h"
#include "Waltrac.h"

void setup() 
{
    /* Startup serial output */
    Serial.begin(115200);
    delay(5000);

    ESP_LOGI("WaltracSetup", "Waltrac Realtime GNSS Tracker");

    /* Get the MAC address for board validation */
    esp_read_mac(macBuf, ESP_MAC_WIFI_STA);    
    ESP_LOGI("WaltracSetup", "%02X:%02X:%02X:%02X:%02X:%02X", macBuf[0], macBuf[1], macBuf[2], macBuf[3], macBuf[4], macBuf[5]);
    sprintf(macHex, "%02x%02x%02x%02x%02x%02x", macBuf[0], macBuf[1], macBuf[2], macBuf[3], macBuf[4], macBuf[5]);

    /* Open serial connection to modem */
    if (WalterModem::begin(&Serial2)) {
        ESP_LOGD("WaltracSetup", "Modem initialization successful.");
    } else {
        ESP_LOGE("WaltracSetup", "Modem initialization failed.");
        return;
    }

    /* Configure the GNSS subsystem */
    if(modem.gnssConfig()) {
        ESP_LOGD("WaltracSetup", "GNSS subsystem setup successful.");
    } else {
        ESP_LOGE("WaltracSetup", "GNSS subsystem setup failed.");
        return;
    }

    /* Set the GNSS fix event handler */
    modem.gnssSetEventHandler(gnssEventHandler, NULL);

    /* Set CoAP event handler */
    modem.coapSetEventHandler(coapEventHandler, NULL);

    /* send a discover command at the first connection attempt */
    Messages::Command command = {};
    command.setHeader(Messages::COMMAND_ACTION_DISCOVER);
    command.arg = macHex;

    std::vector<uint8_t> data = command.serialize(WT_CFG_SECRET);
    if (!coapSendCommand(&data[0], data.size())) {
        ESP_LOGE("WaltracSetup", "Could not send discover command.");
    }

    delay(2500);

    // update Command from server once per minute
    if (coapSubscribeCommands()) {
        uint8_t cntMntCmdTimeout = 0;
        while(cmdModeActive && cntMntCmdTimeout++ < CMD_TIMEOUT_SECONDS) {
            if (getCommand(command)) {
                Messages::CommandAction commandAction;
                command.getHeader(commandAction);

                if (commandAction == Messages::COMMAND_ACTION_EXIT) {
                    ESP_LOGD("WaltracSetup", "Recevied Command EXIT.");
                    break;
                } else {
                    ESP_LOGD("WaltracSetup", "Unknown Command.");
                }

                cntMntCmdTimeout = 0;
            }

            delay(1000);
        }

        ESP_LOGI("WaltracSetup", "Command mode time frame ended after %ds. Entering main loop ...", cntMntCmdTimeout);
    } else {
        ESP_LOGW("WaltracSetup", "Cannot subscribe command topic for entering command mode.");
    }
}

void loop() 
{
    static bool latestFixValid = false;
    
    uint64_t procDurationStart = millis();

    // send GNSS data update
    if (!latestFixValid) {
        ESP_LOGI("WaltracMain", "Looking for GNSS satellites ...");
        
        do
        {
            Messages::Position position;
            position.setHeader(false);
            position.interval = WT_CFG_INTERVAL;
            position.confidence = 0;
            position.satellites = gnssFixNumSatellites;
            memcpy(position.device, macBuf, 6);
            position.name = WT_CFG_NAME;

            std::vector<uint8_t> data = position.serialize(WT_CFG_SECRET);
            if (coapSendPositionUpdate(&data[0], data.size())) {
                ESP_LOGI("WaltracMain", "Sent position data update successfully.");
            } else {
                ESP_LOGE("WaltracMain", "Could not send position data update.");
            }

            latestFixValid = waitForInitialGnssFix();
        }
        while(!latestFixValid);
    } else {
        ESP_LOGI("WaltracMain", "Performing GNSS Update ...");

        latestFixValid = attemptGnssFix();
        if (latestFixValid) {
            ESP_LOGI("WaltracMain", "Sending GNSS data update ...");

            Messages::Position position;
            position.setHeader(true);
            position.interval = WT_CFG_INTERVAL;
            position.confidence = (int)latestGnssFix.estimatedConfidence;
            position.satellites = gnssFixNumSatellites;
            memcpy(position.device, macBuf, 6);
            position.name = WT_CFG_NAME;
            position.latitude = latestGnssFix.latitude;
            position.longitude = latestGnssFix.longitude;

            std::vector<uint8_t> data = position.serialize(WT_CFG_SECRET);
            if (coapSendPositionUpdate(&data[0], data.size())) {
                delay(250);
                ESP_LOGI("WaltracMain", "Sent GNSS data update successfully.");
            } else {
                ESP_LOGE("WaltracMain", "Could not send GNSS data update.");
            }
        }   
    }

    // monitor elapsed time and wait until next interval
    uint32_t procElapsedTime = millis() - procDurationStart;
    int32_t procRemainingTime = WT_CFG_INTERVAL * 1000 - procElapsedTime;
    if (procRemainingTime < 0) {
        procRemainingTime = 0;
    }

    uint32_t procElapsedSeconds = procElapsedTime / 1000;
    if (procElapsedSeconds > WT_CFG_INTERVAL) {
        gnssFixDurationSeconds += procElapsedSeconds;
    } else {
        gnssFixDurationSeconds += WT_CFG_INTERVAL;
    }

    ESP_LOGI("WaltracMain", "Waiting %dms for next interval ...", procRemainingTime);
    delay(procRemainingTime);
}