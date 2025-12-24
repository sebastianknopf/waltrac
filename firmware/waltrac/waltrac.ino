#include <HardwareSerial.h>
#include <WalterModem.h>
#include <esp_mac.h>

#include "Messages.h"
#include "WaltracConfig.h"
#include "Waltrac.h"

void setup() 
{
    /* Startup serial output */
    Serial.begin(115200);
    delay(5000);

    Serial.println("Waltrac Realtime GNSS Tracker");

    /* Get the MAC address for board validation */
    esp_read_mac(macBuf, ESP_MAC_WIFI_STA);
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X\r\n", macBuf[0], macBuf[1], macBuf[2], macBuf[3], macBuf[4], macBuf[5]);

    /* Open serial connection to modem */
    if (WalterModem::begin(&Serial2)) {
        Serial.println("Modem initialization successful.");
    } else {
        Serial.println("Error: Modem initialization failed.");
        return;
    }

    /* Configure the GNSS subsystem */
    if(modem.gnssConfig()) {
        Serial.println("GNSS subsystem setup successful.");
    } else {
        Serial.println("Error: GNSS subsystem setup failed.");
        return;
    }

    /* Set the GNSS fix event handler */
    modem.gnssSetEventHandler(gnssEventHandler, NULL);
}

void loop() 
{
    if (counterCmdUpd == WT_INTERVAL_CMD_UPDATE) {
        Serial.println("Checking for Command Updates ...");
        
        size_t len;
        if (coapRequestGet("command", incomingBuf, len)) {
            std::vector<uint8_t> data(incomingBuf, incomingBuf + len);
            Messages::Command c = Messages::Command::init(data);

            Serial.println("Got command from server. Processing ...");

            if (c.verify(WT_CFG_SECRET)) {
                Serial.println("Command processed successfully.");
            } else {
                Serial.println("Error: Verification of the incoming command failed.");
            }
        }

        counterCmdUpd = 0;
    } else {
        counterCmdUpd++;
    }

    if (counterGnssUpd == WT_INTERVAL_GNSS_UPDATE) {
        if (gnssFixNumSatellites < 1) {
            Serial.println("Looking for GNSS satellites ...");
            
            do
            {
                Messages::Position position;
                position.setHeader(false, gnssFixNumSatellites);
                position.interval = WT_INTERVAL_GNSS_UPDATE;
                memcpy(position.device, macBuf, 6);
                position.name = WT_CFG_NAME;

                std::vector<uint8_t> data = position.serialize(WT_CFG_SECRET);
                if (coapRequestPost("position", &data[0], data.size())) {
                    Serial.println("Sent position data update successfully.");
                } else {
                    Serial.println("Error: Could not send position data update.");
                }
            }
            while(!waitForInitialGnssFix());
        } else {
            Serial.println("Sending GNSS data update ...");

            Messages::Position position;
            position.setHeader(true, gnssFixNumSatellites);
            position.interval = WT_INTERVAL_GNSS_UPDATE;
            memcpy(position.device, macBuf, 6);
            position.name = WT_CFG_NAME;
            position.latitude = latestGnssFix.latitude;
            position.longitude = latestGnssFix.longitude;

            std::vector<uint8_t> data = position.serialize(WT_CFG_SECRET);
            if (coapRequestPost("position", &data[0], data.size())) {
                Serial.println("Sent GNSS data update successfully.");
            } else {
                Serial.println("Error: Could not send GNSS data update.");
            }
            
            if (gnssFixRcvd) {
                Serial.println("Performing GNSS Update ...");

                /* Request a new GNSS fix. IMPORTANT: Wait for around 5s here as the LTE signals may disturb GNSS signals and prevent a new fix! */
                requestGnssFix();
                delay(5000);
            }
        }

        counterGnssUpd = 0;
    } else {
        counterGnssUpd++;
    }

    if (gnssFixTimeoutCounter++ >= MAX_GNSS_FIX_DURATION_SECONDS) {
        Serial.println("\r\nGNSS fix timeout. Restarting ESP ...");

        delay(1000);
        ESP.restart();
    }

    delay(1000);
}