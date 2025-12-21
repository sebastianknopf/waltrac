#include <HardwareSerial.h>
#include <WalterModem.h>
#include <esp_mac.h>

#include "Messages.h"

/**
 * @brief COAP profile used for COAP tests
 */
#define COAP_PROFILE 1

/**
 * @brief The modem instance.
 */
WalterModem modem;

/**
 * @brief Response object containing command response information.
 */
WalterModemRsp* rsp = {};

/**
 * @brief The buffer to transmit to the COAP server.
 */
uint8_t dataBuf[8] = {0};

/**
 * @brief Buffer for incoming COAP response
 */
uint8_t incomingBuf[256] = {0};

/**
 * @brief The counter used in the ping packets.
 */
uint16_t counter = 0;

/**
 * @brief This function checks if we are connected to the lte network
 *
 * @return True when connected, False otherwise
 */
bool lteConnected() {
    WalterModemNetworkRegState regState = modem.getNetworkRegState();
    return (regState == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME || regState == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING);
}

/**
 * @brief This function waits for the modem to be connected to the Lte network.
 * @return true if the connected, else false on timeout.
 */
bool waitForNetwork() {
  /* Wait for the network to become available */
    int timeout = 0;
    while (!lteConnected()) {
        delay(1000);

        if (++timeout > 300) {
            return false;
        }
    }

    Serial.println("Connected to the network");
    return true;
}

/**
 * @brief This function tries to connect the modem to the cellular network.
 * @return true if the connection attempt is successful, else false.
 */
bool lteConnect() {
    if (modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
        Serial.println("Successfully set operational state to NO RF");
    } else {
        Serial.println("Error: Could not set operational state to NO RF");
        return false;
    }

    /* Create PDP context */
    if (modem.definePDPContext()) {
        Serial.println("Created PDP context");
    } else {
        Serial.println("Error: Could not create PDP context");
        return false;
    }

    /* Set the operational state to full */
    if (modem.setOpState(WALTER_MODEM_OPSTATE_FULL)) {
        Serial.println("Successfully set operational state to FULL");
    } else {
        Serial.println("Error: Could not set operational state to FULL");
        return false;
    }

    /* Set the network operator selection to automatic */
    if (modem.setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC)) {
        Serial.println("Network selection mode to was set to automatic");
    } else {
        Serial.println("Error: Could not set the network selection mode to automatic");
        return false;
    }

    return waitForNetwork();
}

void setup() {
    Serial.begin(115200);
    delay(5000);

    Serial.println("Walter modem coap example v1.0.0");

    /* Get the MAC address for board validation */
    esp_read_mac(dataBuf, ESP_MAC_WIFI_STA);
    Serial.printf("Walter's MAC is: %02X:%02X:%02X:%02X:%02X:%02X\r\n", dataBuf[0], dataBuf[1], dataBuf[2], dataBuf[3], dataBuf[4], dataBuf[5]);

    if (WalterModem::begin(&Serial2)) {
        Serial.println("Modem initialization OK");
    } else {
        Serial.println("Error: Modem initialization ERROR");
        return;
    }

    /* Connect the modem to the lte network */
    if (!lteConnect()) {
        Serial.println("Error: Could Not Connect to LTE");
        return;
    }
}

void loop() {

    dataBuf[6] = counter >> 8;
    dataBuf[7] = counter & 0xFF;

    counter++;

    static short receiveAttemptsLeft = 0;

    Messages::Position *p = new Messages::Position();

    esp_read_mac(p->device, ESP_MAC_WIFI_STA);

    p->header = 0x05;
    p->interval = 10;
    p->latitude = 48.757758;
    p->longitude = 8.887452;
    p->name = "TE-ST6";

    std::vector<uint8_t> data = p->serialize("test");

    if (!modem.coapCreateContext(COAP_PROFILE, "skc-hub.app", 1999)) {
        Serial.println("Error: Could not create COAP context. Better luck next iteration?");
        return;
    } else {
        Serial.println("Successfully created or refreshed COAP context");
    }

    if (modem.coapSetHeader(COAP_PROFILE, counter)) {
        Serial.printf("Set COAP header with message id %d\r\n", counter);
    } else {
        Serial.println("Error: Could not set COAP header");
        delay(1000);
        ESP.restart();
    }

    if(!modem.coapSetOptions(COAP_PROFILE, WALTER_MODEM_COAP_OPT_SET, WALTER_MODEM_COAP_OPT_CODE_URI_PATH, "position")) {
        printf("Failed to configure ZTP CoAP URI path for API version\n");
    }

    if (modem.coapSendData(COAP_PROFILE, WALTER_MODEM_COAP_SEND_TYPE_CON, WALTER_MODEM_COAP_SEND_METHOD_POST, data.size(), &data[0])) {
        Serial.println("Sent COAP datagram");
        receiveAttemptsLeft = 3;
    } else {
        Serial.println("Error: Could not send COAP datagram");
        delay(1000);
        ESP.restart();
    }

    delay(10000);
}