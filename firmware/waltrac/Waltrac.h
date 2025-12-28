#pragma once

#include <HardwareSerial.h>
#include <WalterModem.h>
#include <esp_mac.h>

#include "Messages.h"

/**
 * @brief COAP profile used for connection.
 */
#define COAP_PROFILE 1

/**
 * @brief COAP timeout for responses.
 */
#define COAP_TIMEOUT_SECONDS 30

/**
 * @brief Network timeout for connecting.
 */
#define MAX_NETWORK_TIMEOUT_SECONDS 30

/**
 * @brief Time frame for the command mode to be active at startup.
 */
#define CMD_TIMEOUT_SECONDS 60

/**
 * @brief All fixes with a confidence below this number are considered ok.
 */
#define MAX_GNSS_CONFIDENCE 200.0

/**
 * @brief Number of attempts for getting a valid GNSS fix.
 */
#define MAX_GNSS_FIX_ATTEMPTS 3

/**
 * @brief Maximum number of seconds a GNSS fix may take. If the fix exceeds this limit, the fix should be cancelled.
 */
#define MAX_GNSS_FIX_DURATION_SECONDS 60

/**
 * @brief The modem instance.
 */
extern WalterModem modem;

/**
 * @brief The last received GNSS fix.
 */
extern WalterModemGNSSFix latestGnssFix;

/**
 * @brief Flag used to signal when a fix is received.
 */
extern volatile bool gnssFixRcvd;

/**
 * @brief Number of (good) satellites found for the last fix.
 */
extern volatile uint8_t gnssFixNumSatellites;

/**
 * @brief The duration counter for maintaining GNSS timeouts.
 */
extern volatile uint32_t gnssFixDurationSeconds;

/**
 * @brief Flag used to signal when the command mode was left.
 */
extern volatile bool cmdModeActive;

/**
 * @brief The buffer for the MAC adress to be stored.
 */
extern uint8_t macBuf[6];

/**
 * @brief Buffer for incoming COAP response
 */
extern uint8_t incomingBuf[274];

/**
 * @brief The counter for maintaining dynamic interval.
 */
extern uint8_t cntMntInv;

/**
 * @brief The counter for maintaining command update interval.
 */
extern uint8_t cntMntCmd;

/**
 * @brief This function waits for the modem to be connected to the Lte network.
 * @return true if the connected, else false on timeout.
 */
bool waitForNetwork();

/**
 * @brief This function tries to connect the modem to the cellular network.
 * @return true if the connection attempt is successful, else false.
 */
bool lteConnect();

/**
 * @brief This function checks if we are connected to the lte network
 *
 * @return True when connected, False otherwise
 */
bool isLteConnected();

/**
 * @brief Disconnect from the LTE network.
 *
 * This function will disconnect the modem from the LTE network and block until
 * the network is actually disconnected. After the network is disconnected the
 * GNSS subsystem can be used.
 *
 * @return true on success, false on error.
 */
bool lteDisconnect();

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
bool checkAssistanceStatus(WalterModemRsp* rsp, bool* updateAlmanac = nullptr, bool* updateEphemeris = nullptr);

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
bool updateGNSSAssistance(WalterModemRsp* rsp);

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
bool validateGNSSClock(WalterModemRsp* rsp);

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
void gnssEventHandler(const WalterModemGNSSFix* fix, void* args);

/**
 * @brief This function starts an initial GNSS fix and runs a configurable number of attempts to find satellites.
 * @return true if satellites were found and confidence is enough, else false.
 */
bool waitForInitialGnssFix();

/**
 * @brief This function runs the number of GNSS fixes specified by the given parameter.
 *
 * @param numAttempts The number of attempts to try.
 *
 * @return true if a valid GNSS fix was found, else false
 */
bool attemptGnssFix(uint32_t numAttempts = MAX_GNSS_FIX_ATTEMPTS);

/**
 * @brief CoAP event handler. Handles CoAP events.
 *
 * @param event The CoAP event occured.
 * @param profileId The profile ID on which the CoAP event occured.
 * @param args User argument pointer passed to coapSetEventHandler.
 */
void coapEventHandler(WalterModemCoapEvent event, int profileId, void *args);

/**
 * @brief This functions connects to the LTE network and creates / refreshes the CoAP context.
 * @return true if the CoAP context could be created, false if not.
 */
bool coapConnect();

/**
 * @brief This function sends a position update to the CoAP gateway server. Response is not awaited, the function does simple fire & forget.
 *
 * @param data Pointer to the data sent in this request.
 * @param dataLen Size of the dataset sent in this request.
 *
 * @return true if the request was successful, else false.
 */
bool coapSendPositionUpdate(uint8_t* data, size_t dataLen);

/**
 * @brief This function subscribes the commands ressource from CoAP server.
 *
 * @return true if the subscription was successful, else false.
 */
bool coapSubscribeCommands();

/**
 * @brief This functions checks the CoAP input buffer and tries to fill a command object.
 *
 * @param command Reference to the command object to be filled.
 *
 * @return true if a valid command could be obtained, else false.
 */
bool getCommand(Messages::Command &command);