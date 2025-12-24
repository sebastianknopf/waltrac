#include "WaltracConfig.h"
#include "Waltrac.h"

WalterModem modem = {};
WalterModemGNSSFix latestGnssFix = {};

volatile bool gnssFixRcvd = false;
volatile uint8_t gnssFixNumSatellites = 0;
volatile uint32_t gnssFixDurationSeconds = 0;

uint8_t macBuf[6] = {0};
uint8_t incomingBuf[274] = {0};

uint8_t cntMntInv = 0;
uint8_t cntMntCmd = (60 / WT_CFG_INTERVAL);

bool waitForNetwork()
{
    /* Wait for the network to become available */
    int timeout = 0;
    while (!isLteConnected()) {
        delay(1000);

        if (timeout++ >= MAX_NETWORK_TIMEOUT_SECONDS) {
            Serial.println("Error: Network connection timeout reached.");
            return false;
        }
    }

    Serial.println("Connected to the network.");
    return true;
}

bool lteConnect() 
{
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

bool isLteConnected() 
{
    WalterModemNetworkRegState regState = modem.getNetworkRegState();
    return (regState == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME || regState == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING);
}

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

bool checkAssistanceStatus(WalterModemRsp* rsp, bool* updateAlmanac, bool* updateEphemeris)
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

bool initAndConfigureGnss() 
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

    return true;
}

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
    
    Serial.printf("Received GNSS fix to %.06f, %.06f with %d satellites.\r\n", latestGnssFix.latitude, latestGnssFix.longitude, gnssFixNumSatellites);

    gnssFixDurationSeconds = 0;
    gnssFixRcvd = true;
}

bool waitForInitialGnssFix() 
{    
    initAndConfigureGnss();
    
    const uint8_t maxGnssFixAttempts = MAX_GNSS_FIX_ATTEMPTS;
    for (uint8_t i = 0; i < maxGnssFixAttempts; i++) {
        gnssFixRcvd = false;
        if(!modem.gnssPerformAction()) {
            Serial.println("Error: Could not request GNSS fix.");
            return false;
        }

        Serial.printf("Waiting for GNSS fix attempt %d/%d\r\n", (i + 1), maxGnssFixAttempts);
        while(!gnssFixRcvd) {
            delay(1000);

            // restart the ESP when there're more than 5 minutes passed without a valid GNSS signal
            if (gnssFixDurationSeconds++ >= 300) {
                Serial.println("\r\nGNSS fix timeout. Restarting ESP ...");

                delay(1000);
                ESP.restart();
            }
        }

        /* If confidence is acceptable, stop trying. Otherwise, try again */
        if(latestGnssFix.estimatedConfidence <= MAX_GNSS_CONFIDENCE) {
            Serial.printf("GNSS is available, found %d satellites.\r\n", gnssFixNumSatellites);
            return true;
        } else {
            Serial.printf("GNSS fix confidence %.02f too low, found %d satellites, retrying ...\r\n", latestGnssFix.estimatedConfidence, gnssFixNumSatellites);
        }
    }

    Serial.println("Could not succeed initial GNSS fix.");
    return false; 
}

void delayUntilGnssFixReceived(uint32_t timeout) 
{
    uint32_t cntMntTimeout = 0;
    while (!gnssFixRcvd && cntMntTimeout < timeout) {
        delay(100);
        cntMntTimeout += 100;
    }
}

bool requestGnssFix()
{
    initAndConfigureGnss();
    
    gnssFixRcvd = false;
    if(modem.gnssPerformAction(WALTER_MODEM_GNSS_ACTION_GET_SINGLE_FIX)) {
        Serial.println("Requested GNSS fix.");
    } else {
        Serial.println("Error: Could not request GNSS fix.");
        return false;
    }

    return true;
}

bool cancelGnssFix()
{
    gnssFixRcvd = false;
    if (modem.gnssPerformAction(WALTER_MODEM_GNSS_ACTION_CANCEL)) {
        Serial.println("Cancelled GNSS fix.");
    } else {
        Serial.println("Error: Could not cancel GNSS fix.");
        return false;
    }

    return true;
}

bool waitForResponse(uint8_t* output, size_t& outputLen) 
{
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

bool coapConnect() 
{
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

bool coapRequestPost(const char* resource, uint8_t* data, size_t dataLen) 
{    
    if (!coapConnect()) {
        return false;
    }
    
    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_SET, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, resource)) {
        return false;
    }

    if (!modem.coapSendData(COAP_PROFILE, WALTER_MODEM_COAP_SEND_TYPE_CON, WALTER_MODEM_COAP_SEND_METHOD_POST, dataLen, data)) {
        return false;
    }

    return true;
}

bool coapRequestGet(const char* resource, uint8_t* output, size_t& outputLen) 
{
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