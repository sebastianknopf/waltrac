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
}

void loop() 
{
    uint64_t procDurationStart = millis();

    // update Command from server once per minute
    if (cntMntCmd >= (60 / WT_CFG_INTERVAL)) {
        ESP_LOGI("WaltracMain", "Checking for Command Updates ...");
        
        size_t len;
        if (coapRequestGet("command", incomingBuf, len)) {
            std::vector<uint8_t> data(incomingBuf, incomingBuf + len);
            Messages::Command c = Messages::Command::init(data);

            ESP_LOGD("WaltracMain", "Got command from server. Processing ...");

            if (c.verify(WT_CFG_SECRET)) {
                ESP_LOGI("WaltracMain", "Command processed successfully.");
            } else {
                ESP_LOGE("WaltracMain", "Verification of the incoming command failed.");
            }
        }

        cntMntCmd = 0;
    } else {
        cntMntCmd++;
    }

    // send GNSS data update
    if (gnssFixNumSatellites < 1) {
        ESP_LOGI("WaltracMain", "Looking for GNSS satellites ...");
        
        do
        {
            Messages::Position position;
            position.setHeader(false, gnssFixNumSatellites);
            position.interval = WT_CFG_INTERVAL;
            memcpy(position.device, macBuf, 6);
            position.name = WT_CFG_NAME;

            std::vector<uint8_t> data = position.serialize(WT_CFG_SECRET);
            if (coapRequestPost("position", &data[0], data.size())) {
                ESP_LOGI("WaltracMain", "Sent position data update successfully.");
            } else {
                ESP_LOGE("WaltracMain", "Could not send position data update.");
            }
        }
        while(!waitForInitialGnssFix());
    } else {
        ESP_LOGI("WaltracMain", "Sending GNSS data update ...");

        Messages::Position position;
        position.setHeader(true, gnssFixNumSatellites);
        position.interval = WT_CFG_INTERVAL;
        memcpy(position.device, macBuf, 6);
        position.name = WT_CFG_NAME;
        position.latitude = latestGnssFix.latitude;
        position.longitude = latestGnssFix.longitude;

        std::vector<uint8_t> data = position.serialize(WT_CFG_SECRET);
        if (coapRequestPost("position", &data[0], data.size())) {
            ESP_LOGI("WaltracMain", "Sent GNSS data update successfully.");
        } else {
            ESP_LOGE("WaltracMain", "Could not send GNSS data update.");
        }

        ESP_LOGI("WaltracMain", "Performing GNSS Update ...");

        /* Request a new GNSS fix. */
        requestGnssFix();
        waitForGnssFixOrCancel((WT_CFG_INTERVAL * 1000));
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