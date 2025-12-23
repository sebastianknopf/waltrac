#include <HardwareSerial.h>
#include <WalterModem.h>
#include <esp_mac.h>

#include "Messages.h"

#define WT_SERVER_HOST "waltrac.skc-hub.app"
#define WT_SERVER_PORT 1999

#define WT_INTERVAL_CMD_UPDATE 60
#define WT_INTERVAL_GNSS_UPDATE 0

#define WT_CFG_NAME "TE-ST6"
#define WT_CFG_SECRET "test"

/**
 * @brief COAP profile used for connection.
 */
#define COAP_PROFILE 1

/**
 * @brief COAP timeout for responses.
 */
#define COAP_TIMEOUT_SECONDS 30

/**
 * @brief All fixes with a confidence below this number are considered ok.
 */
#define MAX_GNSS_CONFIDENCE 200.0

/**
 * @brief Number of attempts for getting a valid GNSS fix.
 */
#define MAX_GNSS_FIX_ATTEMPTS 3

/**
 * @brief Maximum number of seconds a GNSS fix may take. If the fix exceeds this limit, the ESP should be restarted.
 */
#define MAX_GNSS_FIX_DURATION_SECONDS 300

/**
 * @brief The modem instance.
 */
WalterModem modem;

/**
 * @brief Flag used to signal when a fix is received.
 */
volatile bool gnssFixRcvd = false;

/**
 * @brief Number of (good) satellites found for the last fix.
 */
volatile uint8_t gnssFixNumSatellites = 0;

/**
 * @brief The last received GNSS fix.
 */
WalterModemGNSSFix latestGnssFix = {};

/**
 * @brief The buffer for the MAC adress to be stored.
 */
uint8_t macBuf[6] = {0};

/**
 * @brief The buffer to transmit to the COAP server.
 */
uint8_t dataBuf[8] = {0};

/**
 * @brief Buffer for incoming COAP response
 */
uint8_t incomingBuf[274] = {0};

/**
 * @brief The counter for maintaining dynamic command update interval.
 */
uint8_t counterCmdUpd = 0;

/**
 * @brief The counter for maintaining dynamic GNSS update interval.
 */
uint8_t counterGnssUpd = 0;

/**
 * @brief This function waits for the modem to be connected to the Lte network.
 * @return true if the connected, else false on timeout.
 */
bool waitForNetwork() {
    /* Wait for the network to become available */
    int timeout = 0;
    while (!isLteConnected()) {
        delay(1000);

        if (++timeout > 300) {
            Serial.println("Error: Network connection timeout reached.");
            return false;
        }
    }

    Serial.println("Connected to the network.");
    return true;
}

/**
 * @brief This function tries to connect the modem to the cellular network.
 * @return true if the connection attempt is successful, else false.
 */
bool lteConnect() {
    if (modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
        Serial.println("Successfully set operational state to NO RF.");
    } else {
        Serial.println("Error: Could not set operational state to NO RF.");
        return false;
    }

    /* Create PDP context */
    if (modem.definePDPContext()) {
        Serial.println("Created PDP context.");
    } else {
        Serial.println("Error: Could not create PDP context.");
        return false;
    }

    /* Set the operational state to full */
    if (modem.setOpState(WALTER_MODEM_OPSTATE_FULL)) {
        Serial.println("Successfully set operational state to FULL.");
    } else {
        Serial.println("Error: Could not set operational state to FULL.");
        return false;
    }

    /* Set the network operator selection to automatic */
    if (modem.setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC)) {
        Serial.println("Network selection mode to was set to AUTOMATIC.");
    } else {
        Serial.println("Error: Could not set the network selection mode to AUTOMATIC.");
        return false;
    }

    return waitForNetwork();
}

/**
 * @brief This function checks if we are connected to the lte network
 *
 * @return True when connected, False otherwise
 */
bool isLteConnected() {
    WalterModemNetworkRegState regState = modem.getNetworkRegState();
    return (regState == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME || regState == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING);
}

/**
 * @brief Disconnect from the LTE network.
 *
 * This function will disconnect the modem from the LTE network and block until
 * the network is actually disconnected. After the network is disconnected the
 * GNSS subsystem can be used.
 *
 * @return true on success, false on error.
 */
bool lteDisconnect()
{
    /* Set the operational state to minimum */
    if(modem.setOpState(WALTER_MODEM_OPSTATE_MINIMUM)) {
        Serial.println("Successfully set operational state to MINIMUM.");
    } else {
        Serial.println("Error: Could not set operational state to MINIMUM.");
        return false;
    }

    /* Wait for the network to become available */
    WalterModemNetworkRegState regState = modem.getNetworkRegState();
    while(regState != WALTER_MODEM_NETWORK_REG_NOT_SEARCHING) {
        delay(100);
        regState = modem.getNetworkRegState();
    }

    Serial.println("Disconnected from the network.");
    return true;
}

/**
 * @brief Inspect GNSS assistance status and optionally set update flags.
 *
 * Prints the availability and recommended update timing for the
 * almanac and real-time ephemeris databases.  If update flags are provided,
 * they are set to:
 *   - true  : update is required (data missing or time-to-update <= 0)
 *   - false : no update required
 *
 * @param rsp Pointer to modem response object.
 * @param updateAlmanac   Optional pointer to bool receiving almanac update.
 * @param updateEphemeris Optional pointer to bool receiving ephemeris update.
 *
 * @return true  If assistance status was successfully retrieved and parsed.
 * @return false If the assistance status could not be retrieved.
 */
bool checkAssistanceStatus(WalterModemRsp* rsp, bool* updateAlmanac = nullptr, bool* updateEphemeris = nullptr)
{
    /* Check assistance data status */
    if(!modem.gnssGetAssistanceStatus(rsp) ||  rsp->type != WALTER_MODEM_RSP_DATA_TYPE_GNSS_ASSISTANCE_DATA) {
        Serial.println("Could not request GNSS assistance status");
        return false;
    }

    /* Default output flags to false if provided */
    if(updateAlmanac) {
        *updateAlmanac = false;
    }
        
    if(updateEphemeris) {
        *updateEphemeris = false;
    }

    /* Lambda to reduce repetition for each data type */
    auto reportAndSetUpdateFlag = [](const char* name, const auto& data, bool* updateFlag) {
        Serial.printf("%s data is ", name);
        if(data.available) {
            Serial.printf("available and should be updated within %ds.\r\n", data.timeToUpdate);
            if(updateFlag) {
                *updateFlag = (data.timeToUpdate <= 0);
            }
        } else {
            Serial.println("not available.");
            if(updateFlag) {
                *updateFlag = true;
            }
        }
    };

    /* Check both data sets */
    reportAndSetUpdateFlag("Almanac", rsp->data.gnssAssistance.almanac, updateAlmanac);
    reportAndSetUpdateFlag("Realtime Ephemeris", rsp->data.gnssAssistance.realtimeEphemeris, updateEphemeris);

    return true;
}

/**
 * @brief Update GNSS assistance data if required.
 *
 * Steps performed:
 *   1. Ensure the system clock is valid (sync with LTE if needed).
 *   2. Check the status of GNSS assistance data (almanac & ephemeris).
 *   3. Connect to LTE (if not already) and download any missing data.
 *
 * LTE is only connected when necessary.
 *
 * @param rsp Pointer to modem response object.
 *
 * @return true  Assistance data is valid (or successfully updated).
 * @return false Failure to sync time, connect LTE, or update assistance data.
 */
bool updateGNSSAssistance(WalterModemRsp* rsp)
{
    bool updateAlmanac = false;
    bool updateEphemeris = false;

    /* Get the latest assistance data */
    if(!checkAssistanceStatus(rsp, &updateAlmanac, &updateEphemeris)) {
        Serial.println("Error: Could not check GNSS assistance status.");
        return false;
    }

    /* No update needed */
    if(!updateAlmanac && !updateEphemeris) {
        return true;
    }

    /* Connect to LTE to download assistance data */
    if(!isLteConnected() && !lteConnect()) {
        return false;
    }

    /* Update almanac data if needed */
    if(updateAlmanac) {
        if(modem.gnssUpdateAssistance(WALTER_MODEM_GNSS_ASSISTANCE_TYPE_ALMANAC)) {
            Serial.println("Almanac data updated successfully.");
        } else {
            Serial.println("Error: Almanac data could not be updated.");
            return false;
        }
    }

    /* Update real-time ephemeris data if needed */
    if(updateEphemeris) {
        if(modem.gnssUpdateAssistance(WALTER_MODEM_GNSS_ASSISTANCE_TYPE_REALTIME_EPHEMERIS)) {
            Serial.println("Realtime ephemeris data updated successfully.");
        } else {
            Serial.println("Error: Realtime ephemeris data could not be updated.");
            return false;
        }
    }

    /* Recheck assistance data to ensure its valid */
    if(!checkAssistanceStatus(rsp)) {
        Serial.println("Error: Could not check GNSS assistance status.");
        return false;
    }

    Serial.println("Successfully updated GNSS assistance data.");

    return true;
}

/**
 * @brief Ensure the GNSS subsystem clock is valid, syncing with LTE if needed.
 *
 * If the clock is invalid, this function will attempt to connect to LTE
 * (if not already connected) and sync the clock up to 5 times.
 *
 * @param rsp Pointer to modem response object.
 *
 * @return true If the clock is valid or successfully synchronized.
 * @return false If synchronization fails or LTE connection fails.
 */
bool validateGNSSClock(WalterModemRsp* rsp)
{
    /* Validate the GNSS subsystem clock */
    modem.gnssGetUTCTime(rsp);
    if(rsp->data.clock.epochTime > 4) {
        return true;
    }

    Serial.println("System clock invalid, LTE time sync required.");

    /* Connect to LTE (required for time sync) */
    if(!isLteConnected() && !lteConnect()) {
        Serial.println("Error: Could not connect to LTE network");
        return false;
    }

    /* Attempt sync clock up to 5 times */
    for(int i = 0; i < 5; ++i) {
        /* Validate the GNSS subsystem clock */
        modem.gnssGetUTCTime(rsp);
        if(rsp->data.clock.epochTime > 4) {
            Serial.printf("System clock synchronized to UNIX timestamp %" PRIi64 ".\r\n", rsp->data.clock.epochTime);
            return true;
        }

        delay(2000);
    }

    Serial.println("Error: Could not sync time with network. Does the network support NITZ?");
    return false;
}

/**
 * @brief GNSS event handler
 *
 * Handles GNSS fix events.
 * @note This callback is invoked from the modem driver’s event context.
 *       It must never block or call modem methods directly.
 *       Use it only to set flags or copy data for later processing.
 *
 * @param fix The fix data.
 * @param args User argument pointer passed to gnssSetEventHandler
 *
 * @return None.
 */
void gnssEventHandler(const WalterModemGNSSFix* fix, void* args)
{
    latestGnssFix = *fix;

    /* Count satellites with good signal strength */
    gnssFixNumSatellites = 0;
    for(int i = 0; i < latestGnssFix.satCount; ++i) {
        if(latestGnssFix.sats[i].signalStrength >= 30) {
            gnssFixNumSatellites++;
        }
    }

    gnssFixRcvd = true;
}

/**
 * @brief This function starts a GNSS fix and waits until the fix is received. If the fix is not confident enough, fixes are triggered until the confidence level is reached or a timeout occured.
 * @return true if a valid GNSS fix has be obtained, false if not.
 */
bool attemptGNSSFix()
{
    WalterModemRsp rsp = {};

    if(!validateGNSSClock(&rsp)) {
        Serial.println("Error: Could not validate GNSS clock.");
        return false;
    }

    /* Ensure assistance data is current */
    if(!updateGNSSAssistance(&rsp)) {
        Serial.println("Warning: Could not update GNSS assistance data. Continuing without assistance.");
    }

    /* Disconnect from the network (Required for GNSS) */
    if(isLteConnected() && !lteDisconnect()) {
        Serial.println("Error: Could not disconnect from the LTE network.");
        return false;
    }

    /* Optional: Reconfigure GNSS with last valid fix - This might speed up consecutive fixes */
    if(latestGnssFix.estimatedConfidence <= MAX_GNSS_CONFIDENCE) {
        /* Reconfigure GNSS for potential quick fix */
        if(modem.gnssConfig(WALTER_MODEM_GNSS_SENS_MODE_HIGH, WALTER_MODEM_GNSS_ACQ_MODE_HOT_START)) {
            Serial.println("GNSS reconfigured for potential quick fix.");
        } else {
            Serial.println("Error: Could not reconfigure GNSS for potential quick fix.");
        }
    }

    uint32_t gnssFixTimeoutCounter = 0;
    uint8_t gnssFixTimeoutPrescaler = 2;

    const uint8_t maxGnssFixAttempts = MAX_GNSS_FIX_ATTEMPTS;
    for (uint8_t i = 0; i < maxGnssFixAttempts; i++) {
        gnssFixRcvd = false;
        if(!modem.gnssPerformAction()) {
            Serial.println("Error: Could not request GNSS fix.");
            return false;
        }

        Serial.printf("Waiting for GNSS fix attempt %d/%d ", (i + 1), maxGnssFixAttempts);
        while(!gnssFixRcvd) {
            Serial.print(".");
            delay(1000 / gnssFixTimeoutPrescaler);

            if (gnssFixTimeoutCounter++ >= MAX_GNSS_FIX_DURATION_SECONDS * gnssFixTimeoutPrescaler) {
                Serial.println("\r\nGNSS fix timeout. Restarting ESP ...");

                delay(1000);
                ESP.restart();
            }
        }
        Serial.printf("\r\n");

        /* If confidence is acceptable, stop trying. Otherwise, try again */
        if(latestGnssFix.estimatedConfidence <= MAX_GNSS_CONFIDENCE) {
            Serial.printf("GNSS fix successful, found %d (%d good) satellites, position is %.06f, %.06f\r\n", latestGnssFix.satCount, gnssFixNumSatellites, latestGnssFix.latitude, latestGnssFix.longitude);
            return true;
        } else {
            Serial.printf("GNSS fix confidence %.02f too low, found %d (%d good) satellites, retrying ...\r\n", latestGnssFix.estimatedConfidence, latestGnssFix.satCount, gnssFixNumSatellites);
        }
    }

    Serial.println("Could not find valid GNSS fix.");
    return false;    
}

/**
 * @brief This function waits for a CoAP response and writes the data to the supplied buffer.
 *
 * @param output Pointer to the output buffer the data are written to.
 * @param outputLen Size of the output data.
 *
 * @return true if the response was received, else false.
 */
bool waitForResponse(uint8_t* output, size_t& outputLen) {
    WalterModemRsp rsp = {};
    uint8_t buffer[274] = {0};
    
    int i = COAP_TIMEOUT_SECONDS;
    while(i && !modem.coapDidRing(COAP_PROFILE, buffer, sizeof(buffer), &rsp)) {
        delay(1000);
        i--;
    }

    outputLen = rsp.data.coapResponse.length;
    memcpy(output, buffer, outputLen);

    if (i > 0) {
        return true;
    } else {
        return false;
    }
}

/**
 * @brief This functions connects to the LTE network and creates / refreshes the CoAP context.
 * @return true if the CoAP context could be created, false if not.
 */
bool coapConnect() {
    /* Enable LTE network and create CoAP context. */
    if (!isLteConnected() && !lteConnect()) {
        return false;
    }

    /* Configure CoAP context */
    if (modem.coapCreateContext(COAP_PROFILE, WT_SERVER_HOST, WT_SERVER_PORT)) {
        Serial.println("CoAP server context created successfully.");
        return true;
    } else {
        Serial.println("Error: CoAP server context could not be created.");
        return false;
    }
}

/**
 * @brief This function performas a CoAP POST request on the given resource.
 *
 * @param resource Name of the resource to be connected to.
 * @param data Pointer to the data sent in this request.
 * @param dataLen Size of the dataset sent in this request.
 * @param output Pointer to the output buffer the data are written to.
 * @param outputLen Size of the output data.
 *
 * @return true if the request was successful, else false.
 */
bool coapRequestPost(const char* resource, uint8_t* data, size_t dataLen, uint8_t* output, size_t& outputLen) {    
    if (!coapConnect()) {
        return false;
    }
    
    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_SET, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, resource)) {
        return false;
    }

    if (!modem.coapSendData(COAP_PROFILE, WALTER_MODEM_COAP_SEND_TYPE_CON, WALTER_MODEM_COAP_SEND_METHOD_POST, dataLen, data)) {
        return false;
    }

    return waitForResponse(output, outputLen);
}

/**
 * @brief This function performas a CoAP POST request on the given resource.
 *
 * @param output Pointer to the output buffer the data are written to.
 * @param outputLen Size of the output data.
 *
 * @return true if the request was successful, else false.
 */
bool coapRequestGet(const char* resource, uint8_t* output, size_t& outputLen) {
    if (!coapConnect()) {
        return false;
    }
    
    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_SET, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, resource)) {
        return false;
    }

    if (!modem.coapSendData(COAP_PROFILE, WALTER_MODEM_COAP_SEND_TYPE_CON, WALTER_MODEM_COAP_SEND_METHOD_GET, 0, nullptr)) {
        return false;
    }

    return waitForResponse(output, outputLen);
}

void setup() {

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

void loop() {

    /*size_t len;

    if (requestGet("command", incomingBuf, len)) {
        Serial.print("Successfully sent GET: ");

        for (size_t i = 0; i < len; ++i) {
            if (incomingBuf[i] < 0x10) Serial.print('0');
            Serial.print(incomingBuf[i], HEX);
        }

        Serial.println("");

        std::vector<uint8_t> data(incomingBuf, incomingBuf + len);
        Messages::Command c = Messages::Command::init(data);

        if (!c.verify("test")) {
            Serial.println("Verification of the incoming command failed.");
        } else {
            Serial.println("Verification of the incoming command successful!");
        }
    }  else {
        Serial.println("Could not send GET");
    }

    if (counterCmdUpd == WT_INTERVAL_CMD_UPDATE) {
        Serial.println("Checking for Command Updates ...");
        
        counterCmdUpd = 0;
    } else {
        counterCmdUpd++;
    }*/

    if (counterGnssUpd == WT_INTERVAL_GNSS_UPDATE) {
        Serial.println("Performing GNSS Update ...");

        Messages::Position position;
        position.interval = WT_INTERVAL_GNSS_UPDATE;
        memcpy(position.device, macBuf, 6);
        position.name = WT_CFG_NAME;

        if(attemptGNSSFix()) {
            position.setHeader(true, gnssFixNumSatellites);
            position.latitude = latestGnssFix.latitude;
            position.longitude = latestGnssFix.longitude;
        } else {
            position.setHeader(false, gnssFixNumSatellites);
        }

        size_t len;
        std::vector<uint8_t> data = position.serialize(WT_CFG_SECRET);
        if (coapRequestPost("position", &data[0], data.size(), incomingBuf, len)) {
            Serial.println("Sent GNSS data update successfully.");
        } else {
            Serial.println("Error: Could not send GNSS data update.");
        }

        counterGnssUpd = 0;
    } else {
        counterGnssUpd++;
    }

    delay(1000);
}