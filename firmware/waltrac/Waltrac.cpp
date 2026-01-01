#include <esp_log.h>

#include "WaltracConfig.h"
#include "Waltrac.h"

WalterModem modem = {};
WalterModemGNSSFix latestGnssFix = {};

volatile bool gnssFixRcvd = false;
volatile uint8_t gnssFixNumSatellites = 0;
volatile uint32_t gnssFixDurationSeconds = 0;

volatile bool cmdModeActive = true;

uint8_t macBuf[6] = {0};
char macHex[13] = {0};
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
            ESP_LOGE("Waltrac", "Network connection timeout reached.");
            
            lteDisconnect(); 
            return false;
        }
    }

    ESP_LOGI("Waltrac", "Connected to the network.");
    return true;
}

bool lteConnect() 
{
    /* Set operational state */
    if (modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
        ESP_LOGD("Waltrac", "Successfully set operational state to NO RF.");
    } else {
        ESP_LOGE("Waltrac", "Could not set operational state to NO RF.");
        return false;
    }

    /* Create PDP context */
    if (modem.definePDPContext()) {
        ESP_LOGD("Waltrac", "Created PDP context.");
    } else {
        ESP_LOGE("Waltrac", "Could not create PDP context.");
        return false;
    }

    /* Set the operational state to full */
    if (modem.setOpState(WALTER_MODEM_OPSTATE_FULL)) {
        ESP_LOGD("Waltrac", "Successfully set operational state to FULL.");
    } else {
        ESP_LOGE("Waltrac", "Could not set operational state to FULL.");
        return false;
    }

    /* Set the network operator selection to automatic */
    if (modem.setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC)) {
        ESP_LOGD("Waltrac", "Network selection mode to was set to AUTOMATIC.");
    } else {
        ESP_LOGE("Waltrac", "Could not set the network selection mode to AUTOMATIC.");
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
        ESP_LOGD("Waltrac", "Successfully set operational state to MINIMUM.");
    } else {
        ESP_LOGE("Waltrac", "Could not set operational state to MINIMUM.");
        return false;
    }

    /* Wait for the network to become available */
    WalterModemNetworkRegState regState = modem.getNetworkRegState();
    while(regState != WALTER_MODEM_NETWORK_REG_NOT_SEARCHING) {
        delay(100);
        regState = modem.getNetworkRegState();
    }

    ESP_LOGD("Waltrac", "Disconnected from the network.");
    return true;
}

bool checkAssistanceStatus(WalterModemRsp* rsp, bool* updateAlmanac, bool* updateEphemeris)
{
    /* Check assistance data status */
    if(!modem.gnssGetAssistanceStatus(rsp) ||  rsp->type != WALTER_MODEM_RSP_DATA_TYPE_GNSS_ASSISTANCE_DATA) {
        ESP_LOGE("Waltrac", "Could not request GNSS assistance status");
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
        if(data.available) {
            ESP_LOGI("Waltrac", "%s data is available and should be updated within %ds.", name, data.timeToUpdate);
            if(updateFlag) {
                *updateFlag = (data.timeToUpdate <= 0);
            }
        } else {
            ESP_LOGI("Waltrac", "%s data is not available.", name);
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
        ESP_LOGE("Waltrac", "Could not check GNSS assistance status.");
        return false;
    }

    /* No update needed */
    if(!updateAlmanac && !updateEphemeris) {
        ESP_LOGD("Waltrac", "GNSS assistance up-to-date. No update needed.");
        return true;
    }

    /* Connect to LTE to download assistance data */
    if(!isLteConnected() && !lteConnect()) {
        return false;
    }

    /* Update almanac data if needed */
    if(updateAlmanac) {
        if(modem.gnssUpdateAssistance(WALTER_MODEM_GNSS_ASSISTANCE_TYPE_ALMANAC)) {
            ESP_LOGD("Waltrac", "Almanac data updated successfully.");
        } else {
            ESP_LOGE("Waltrac", "Almanac data could not be updated.");
            return false;
        }
    }

    /* Update real-time ephemeris data if needed */
    if(updateEphemeris) {
        if(modem.gnssUpdateAssistance(WALTER_MODEM_GNSS_ASSISTANCE_TYPE_REALTIME_EPHEMERIS)) {
            ESP_LOGD("Waltrac", "Realtime ephemeris data updated successfully.");
        } else {
            ESP_LOGE("Waltrac", "Realtime ephemeris data could not be updated.");
            return false;
        }
    }

    /* Recheck assistance data to ensure its valid */
    if(!checkAssistanceStatus(rsp)) {
        ESP_LOGD("Waltrac", "Could not check GNSS assistance status.");
        return false;
    }

    ESP_LOGD("Waltrac", "Successfully updated GNSS assistance data.");
    return true;
}

bool validateGNSSClock(WalterModemRsp* rsp)
{
    /* Validate the GNSS subsystem clock */
    modem.gnssGetUTCTime(rsp);
    if(rsp->data.clock.epochTime > 4) {
        return true;
    }

    ESP_LOGI("Waltrac", "System clock invalid, LTE time sync required.");

    /* Connect to LTE (required for time sync) */
    if(!isLteConnected() && !lteConnect()) {
        ESP_LOGE("Waltrac", "Could not connect to LTE network.");
        return false;
    }

    /* Attempt sync clock up to 5 times */
    for(int i = 0; i < 5; ++i) {
        /* Validate the GNSS subsystem clock */
        modem.gnssGetUTCTime(rsp);
        if(rsp->data.clock.epochTime > 4) {
            ESP_LOGI("Waltrac", "System clock synchronized to UNIX timestamp %" PRIi64 ".", rsp->data.clock.epochTime);
            return true;
        }

        delay(2000);
    }

    ESP_LOGE("Waltrac", "Could not sync time with network. Does the network support NITZ?");
    return false;
}

void gnssEventHandler(const WalterModemGNSSFix* fix, void* args)
{
    latestGnssFix = *fix;
    gnssFixRcvd = true;
    
    /* Count satellites with good signal strength */
    gnssFixNumSatellites = 0;
    for(int i = 0; i < latestGnssFix.satCount; ++i) {
        if(latestGnssFix.sats[i].signalStrength >= 30) {
            gnssFixNumSatellites++;
        }
    }

    ESP_LOGI("Waltrac", "Received GNSS fix to %.06f, %.06f with %d satellites after %ds.", latestGnssFix.latitude, latestGnssFix.longitude, gnssFixNumSatellites, gnssFixDurationSeconds);

    gnssFixDurationSeconds = 0;
}

bool waitForInitialGnssFix() 
{    
    WalterModemRsp rsp = {};

    if(!validateGNSSClock(&rsp)) {
        ESP_LOGE("Waltrac", "Could not validate GNSS clock.");
        return false;
    }

    /* Ensure assistance data is current */
    if(!updateGNSSAssistance(&rsp)) {
        ESP_LOGW("Waltrac", "Could not update GNSS assistance data. Continuing without assistance.");
    }

    /* Disconnect from the network if network is connected or in lookup (Required for GNSS) */
    if(!lteDisconnect()) {
        ESP_LOGE("Waltrac", "Could not disconnect from the LTE network.");
        return false;
    }
    
    const uint8_t maxGnssFixAttempts = MAX_GNSS_FIX_ATTEMPTS;
    for (uint8_t i = 0; i < maxGnssFixAttempts; i++) {
        
        gnssFixRcvd = false;
        if(!modem.gnssPerformAction()) {
            ESP_LOGE("Waltrac", "Could not request GNSS fix.");
            return false;
        }

        ESP_LOGI("Waltrac", "Waiting for GNSS lookup attempt %d/%d ...", (i + 1), maxGnssFixAttempts);
        while(!gnssFixRcvd) {
            delay(1000);

            // restart the ESP when there're more than 5 minutes passed without a valid GNSS signal
            if (gnssFixDurationSeconds++ >= 300) {
                ESP_LOGI("Waltrac", "GNSS lookup timeout after %ds. Restarting ESP ...", gnssFixDurationSeconds);

                delay(500);
                ESP.restart();
            }
        }

        /* If confidence is acceptable, stop trying. Otherwise, try again */
        if(latestGnssFix.estimatedConfidence <= MAX_GNSS_CONFIDENCE) {
            ESP_LOGI("Waltrac", "GNSS is available, found %d satellites.", gnssFixNumSatellites);
            return true;
        } else {
            ESP_LOGI("Waltrac", "GNSS fix confidence %.02f too low, found %d satellites, retrying ...", latestGnssFix.estimatedConfidence, gnssFixNumSatellites);
        }
    }

    ESP_LOGE("Waltrac", "Could not succeed GNSS lookup.");
    return false; 
}

bool attemptGnssFix(uint32_t numAttempts)
{
    if (numAttempts > MAX_GNSS_FIX_ATTEMPTS) {
        numAttempts = MAX_GNSS_FIX_ATTEMPTS;
    }

    WalterModemRsp rsp = {};

    if(!validateGNSSClock(&rsp)) {
        ESP_LOGE("Waltrac", "Could not validate GNSS clock.");
        return false;
    }

    /* Ensure assistance data is current */
    if(!updateGNSSAssistance(&rsp)) {
        ESP_LOGW("Waltrac", "Could not update GNSS assistance data. Continuing without assistance.");
    }

    /* Disconnect from the network if network is connected or in lookup (Required for GNSS) */
    if(!lteDisconnect()) {
        ESP_LOGE("Waltrac", "Could not disconnect from the LTE network.");
        return false;
    }

    /* Optional: Reconfigure GNSS with last valid fix - This might speed up consecutive fixes */
    if(latestGnssFix.estimatedConfidence <= MAX_GNSS_CONFIDENCE) {
        /* Reconfigure GNSS for potential quick fix */
        if(modem.gnssConfig(WALTER_MODEM_GNSS_SENS_MODE_HIGH, WALTER_MODEM_GNSS_ACQ_MODE_HOT_START)) {
            ESP_LOGD("Waltrac", "GNSS reconfigured for potential quick fix.");
        } else {
            ESP_LOGE("Waltrac", "Could not reconfigure GNSS for potential quick fix.");
        }
    }

    for (uint8_t i = 0; i < numAttempts; i++) {

        gnssFixRcvd = false;
        if(!modem.gnssPerformAction()) {
            ESP_LOGE("Waltrac", "Could not request GNSS fix.");
            return false;
        }

        ESP_LOGI("Waltrac", "Waiting for GNSS fix attempt %d/%d ...", (i + 1), numAttempts);
        while(!gnssFixRcvd) {
            delay(1000);

            if (gnssFixDurationSeconds++ >= MAX_GNSS_FIX_DURATION_SECONDS) {
                ESP_LOGW("Waltrac", "GNSS fix timeout after %ds. Cancelling GNSS fix ...", gnssFixDurationSeconds);

                if (modem.gnssPerformAction(WALTER_MODEM_GNSS_ACTION_CANCEL)) {
                    ESP_LOGD("Waltrac", "Cancelled GNSS fix.");
                    
                    gnssFixDurationSeconds = 0;

                    delay(1000);
                    break;
                } else {
                    ESP_LOGE("Waltrac", "Could not cancel GNSS fix. Restarting ESP ...");

                    delay(500);
                    ESP.restart();
                }
            }
        }

        if (gnssFixRcvd) {
            /* If confidence is acceptable, stop trying. Otherwise, try again */
            if(latestGnssFix.estimatedConfidence <= MAX_GNSS_CONFIDENCE) {
                ESP_LOGI("Waltrac", "GNSS fix acceptable with confidence %.02f, found %d satellites.", latestGnssFix.estimatedConfidence, gnssFixNumSatellites);
                return true;
            } else {
                ESP_LOGI("Waltrac", "GNSS fix confidence %.02f too low, found %d satellites, retrying ...", latestGnssFix.estimatedConfidence, gnssFixNumSatellites);
            }
        } else {
            ESP_LOGW("Waltrac", "Could not find a valid GNSS fix.");
            return false;
        }
    }

    ESP_LOGE("Waltrac", "Could not succeed GNSS fix.");
    return false;
}

void coapEventHandler(WalterModemCoapEvent event, int profileId, void *args) 
{
    if (event == WALTER_MODEM_COAP_EVENT_DISCONNECTED && profileId == COAP_PROFILE) {
        cmdModeActive = false;
    }
}

bool coapConnect() 
{    
    /* Enable LTE network and create CoAP context. */
    if (!isLteConnected() && !lteConnect()) {
        return false;
    }

    /* Configure CoAP context */
    if (!modem.coapGetContextStatus(COAP_PROFILE)) {
        if (modem.coapCreateContext(COAP_PROFILE, WT_SERVER_HOST, WT_SERVER_PORT)) {
            ESP_LOGD("Waltrac", "CoAP server context created successfully.");
            return true;
        } else {
            ESP_LOGE("Waltrac", "CoAP server context could not be created.");
            return false;
        }
    }

    ESP_LOGD("Waltrac", "CoAP server context still active, no need for new initialisation.");
    return true;
}

bool coapSendPositionUpdate(uint8_t* data, size_t dataLen) 
{    
    if (!coapConnect()) {
        return false;
    }

    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_SET, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, "ps")) {
        return false;
    }

    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_EXTEND, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, "waltrac")) {
        return false;
    }
    
    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_EXTEND, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, "pos")) {
        return false;
    }

    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_EXTEND, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, macHex)) {
        return false;
    }

    if (!modem.coapSendData(COAP_PROFILE, WALTER_MODEM_COAP_SEND_TYPE_CON, WALTER_MODEM_COAP_SEND_METHOD_POST, dataLen, data)) {
        return false;
    }

    return true;
}

bool coapSendCommand(uint8_t* data, size_t dataLen) 
{    
    if (!coapConnect()) {
        return false;
    }

    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_SET, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, "ps")) {
        return false;
    }

    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_EXTEND, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, "waltrac")) {
        return false;
    }
    
    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_EXTEND, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, "cmd")) {
        return false;
    }

    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_EXTEND, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, "control")) {
        return false;
    }

    if (!modem.coapSendData(COAP_PROFILE, WALTER_MODEM_COAP_SEND_TYPE_CON, WALTER_MODEM_COAP_SEND_METHOD_POST, dataLen, data)) {
        return false;
    }

    return true;
}

bool coapSubscribeCommands() 
{
    if (!coapConnect()) {
        return false;
    }
    
    // /ps
    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_SET, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, "ps")) {
        return false;
    }

    // /waltrac
    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_EXTEND, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, "waltrac")) {
        return false;
    }
    
    // /command
    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_EXTEND, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, "cmd")) {
        return false;
    }

    // /{deviceId}
    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_EXTEND, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, macHex)) {
        return false;
    }

    // set Observe = 0
    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_SET, WALTER_MODEM_COAP_OPT_CODE_OBSERVE, "0")) {
        return false;
    }

    // set Token
    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_SET, WALTER_MODEM_COAP_OPT_CODE_TOKEN, macHex)) {
        return false;
    }

    if (!modem.coapSendData(COAP_PROFILE, WALTER_MODEM_COAP_SEND_TYPE_CON, WALTER_MODEM_COAP_SEND_METHOD_GET, 0, nullptr)) {
        return false;
    }

    return true;
}

bool getCommand(Messages::Command &command)
{    
    WalterModemRsp rsp = {};
    uint8_t buffer[274] = {0};

    if (modem.coapDidRing(COAP_PROFILE, buffer, sizeof(buffer), &rsp)) {
        if (rsp.data.coapResponse.length > 0) {
            try {
                std::vector<uint8_t> data(buffer, buffer + rsp.data.coapResponse.length);

                command = Messages::Command::init(data);
                ESP_LOGD("Waltrac", "Got command from server.");

                if (command.verify(WT_CFG_SECRET)) {
                    ESP_LOGI("Waltrac", "Command verified successfully.");
                } else {
                    ESP_LOGE("Waltrac", "Verification of the incoming command failed.");
                }

                return true;
            } catch(const std::runtime_error &e) {
                ESP_LOGE("Waltrac", "Failed to parse incoming data as command.");
                return false;
            }
        } else {
            return false;
        }
    } else {
        return false;
    }
}