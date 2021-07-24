#include <esp_system.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <nvs_flash.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string.h>
#include <string>


extern "C" {
    void app_main(void);
}

enum SendStatus{
    Idle,
    Busy,
    Success,
    Fail,
};

struct Statistics {
    uint64_t bytesSent;
    uint64_t sentBps;
    uint64_t bytesReceived;
    uint64_t receivedBps;
    uint32_t successes;
    uint32_t failures;
    int32_t lastRssi;
    SendStatus lastStatus;
};

Statistics stats = {
    .bytesSent = 0,
    .sentBps = 0,
    .bytesReceived = 0,
    .receivedBps = 0,
    .successes = 0,
    .failures = 0,
    .lastRssi = -999,
    .lastStatus = SendStatus::Idle,
};

// Left  = MAC Address: 7c:9e:bd:ed:36:94
// Right = MAC Address: 7c:9e:bd:39:9f:68
const uint8_t MAC_ADDR_LIST[2][6] = {
    {0x7C, 0x9E, 0xBD, 0xED, 0x36, 0x94}, // Left
    {0x7C, 0x9E, 0xBD, 0x39, 0x9F, 0x68}, // Right
};

void mac_to_str(const uint8_t* mac, char* buffer)
{
    sprintf(buffer, "%x:%x:%x:%x:%x:%x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void print_mac(const uint8_t* mac, const char* msg, bool newline=true)
{
    char macStr[20] = {0};
    char buffer[128] = {0};
    mac_to_str(mac, macStr);
    std::string end = "\r\n";
    if (!newline) {
        end = "\0";
    }
    sprintf(buffer, "%s: %s%s", msg, macStr, end.c_str());
    uart_write_bytes(UART_NUM_0, buffer, strlen(buffer));
}

const uint8_t* get_peer_mac_address()
{
    // Grab our MAC address
    uint8_t macBuffer[6] = {0};
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(macBuffer));
    print_mac(macBuffer, "MAC ADDRESS");
    // Grab the first MAC address that doesn't match ours
    for (int i=0; i < sizeof MAC_ADDR_LIST / 6; ++i) {
        for (int x=0; x < 6; ++x) {
            if (MAC_ADDR_LIST[i][x] != macBuffer[x]) {
                return MAC_ADDR_LIST[i];
            }
        }
    }
    return nullptr;
}

void esp_now_recv_callback(const uint8_t *mac_addr, const uint8_t *data, int data_len)
{
    gpio_set_level(GPIO_NUM_2, 1);
    //uart_write_bytes(UART_NUM_0, (const char*)data, data_len);
    vTaskDelay(1);
    gpio_set_level(GPIO_NUM_2, 0);
}

void esp_now_send_callback(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS) {
        stats.lastStatus = SendStatus::Success;
    } else {
        stats.lastStatus = SendStatus::Fail;
    }
}

typedef struct {
	uint32_t frame_ctrl : 16;
	uint32_t duration_id : 16;
	uint8_t addr1[6]; /* receiver address */
	uint8_t addr2[6]; /* sender address */
	uint8_t addr3[6]; /* filtering address */
	uint32_t sequence_ctrl : 16;
	uint8_t addr4[6]; /* optional */
} wifi_ieee80211_mac_hdr_t;

typedef struct {
	wifi_ieee80211_mac_hdr_t hdr;
	uint8_t payload[0]; /* network data ended with 4 bytes csum (CRC32) */
} wifi_ieee80211_packet_t;

void promiscuous_rx_callback(void* buffer, wifi_promiscuous_pkt_type_t pkt_type)
{
    const int MAX_RSSI_COUNT = 20;
    static int rssiValues[MAX_RSSI_COUNT] = {};
    static int lastRssiIndex = 0;
    // All espnow traffic uses action frames which are a subtype of the mgmnt frames so filter out everything else.
    if (pkt_type != WIFI_PKT_MGMT) {
        return;
    }
    static const uint8_t ACTION_SUBTYPE = 0xD0;
    static const uint8_t ESPRESSIF_OUI[] = {0x7C, 0X9E, 0XBD};


    const wifi_promiscuous_pkt_t* ppkt = (wifi_promiscuous_pkt_t*)buffer;
    const wifi_ieee80211_packet_t* ipkt = (wifi_ieee80211_packet_t*)ppkt->payload;
    const wifi_ieee80211_mac_hdr_t* hdr = &ipkt->hdr;

    // Only continue processing if this is an action frame containing the Espressif OUI.
    if ((ACTION_SUBTYPE == (hdr->frame_ctrl & 0xFF)) &&
        (memcmp(hdr->addr2, ESPRESSIF_OUI, 3) == 0)) {
        int rssi = ppkt->rx_ctrl.rssi;
        if (stats.lastRssi == -999) {
            // We don't have any data yet so lets fill the entire array right now.
            for (int i=0; i < MAX_RSSI_COUNT; ++i) {
                rssiValues[i] = rssi;
            }
        } else {
            // Add the RSSI Value to the array
            rssiValues[lastRssiIndex++] = rssi;
            // reset the index to the beginning if we hit the end
            if (lastRssiIndex >= MAX_RSSI_COUNT) {
                lastRssiIndex = 0;
            }
        }
        // Calculate the average
        stats.lastRssi = 0;
        for (int i=0; i < MAX_RSSI_COUNT; ++i) {
            stats.lastRssi += rssiValues[i];
        }
        stats.lastRssi /= MAX_RSSI_COUNT;
    }
}



void displayStatsTask(void* pvParameters)
{
    auto last_ticks = xTaskGetTickCount();
    auto start = xTaskGetTickCount();
    while (true) {
        // display information
        if (pdTICKS_TO_MS((xTaskGetTickCount()-last_ticks)) >= 1000) {
            auto elapsedMs = pdTICKS_TO_MS(xTaskGetTickCount()-start);
            double KBPerSec = 0;
            if (stats.bytesSent && elapsedMs) {
                KBPerSec = stats.bytesSent / (elapsedMs/1000.0) / 1000.0;
            }
            printf("Bytes Sent: %lld (%.2fKB/sec) - %d Success / %d Fails (RSSI: %ddB)                               \r", 
                stats.bytesSent, KBPerSec, stats.successes, stats.failures, stats.lastRssi);
            // reset the calculation every 10 seconds
            if (elapsedMs > 10000) {
                start = xTaskGetTickCount();
                stats.bytesSent = 0;
            }
            last_ticks = xTaskGetTickCount();
        }
    }
    //vTaskDelete(NULL);
}

void transmitESPNOWTask(void* pvParameters)
{
    const char msg[] = "Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!Hello World!\r\n";
    const uint8_t* peerMac = get_peer_mac_address();
    while (true) {
        if (stats.lastStatus != SendStatus::Busy) {
            if (stats.lastStatus == SendStatus::Fail) {
                stats.failures += 1;
                //printf("Failed to send (%d)!\r\n", failures);
                // The esp now stack runs in a high priority wifi task
                // we need to give it time to clear the buffer or we get an ESP_ERR_ESPNOW_NO_MEM and abort call
                // vTaskDelay(1 / portTICK_PERIOD_MS);
            } else if (stats.lastStatus == SendStatus::Success) {
                stats.successes += 1;
                stats.bytesSent += sizeof msg;
            }
            stats.lastStatus = SendStatus::Busy;
            ESP_ERROR_CHECK(esp_now_send(peerMac, (const uint8_t*)msg, strlen(msg)));
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
    //vTaskDelete(NULL);
}

void app_main(void) 
{
    // configure the uart, default serial port
    uart_config_t uartConfig = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .use_ref_tick = false,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uartConfig));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, GPIO_NUM_1, GPIO_NUM_3, GPIO_NUM_22, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));
    // Configure the onboard LED
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
    // configure ESP NOW
    ESP_ERROR_CHECK(nvs_flash_init());
    const wifi_init_config_t wifiConfig = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifiConfig));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&promiscuous_rx_callback));
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(&esp_now_recv_callback));
    ESP_ERROR_CHECK(esp_now_register_send_cb(&esp_now_send_callback));
    // Pair the devices
    const uint8_t* peerMac = get_peer_mac_address();
    const esp_now_peer_info_t peerInfo = {
        .peer_addr = { peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4],peerMac[5] },
        .lmk = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
        .channel = 1,
        .ifidx = ESP_IF_WIFI_STA,
        .encrypt = true,
        nullptr,
    };
    print_mac(peerMac, "PEER MAC");
    ESP_ERROR_CHECK(esp_now_add_peer(&peerInfo));
    
    TaskHandle_t txHandle, statsHandle;
    auto success = xTaskCreate(transmitESPNOWTask, "ESPNOW TX", 10000, NULL, tskIDLE_PRIORITY, &txHandle);
    if (success != pdPASS) {
        printf("Failed to create task ESPNOW TX\r\n");
        vTaskDelete(txHandle);
    }
     success = xTaskCreate(displayStatsTask, "stats", 10000, NULL, tskIDLE_PRIORITY, &statsHandle);
    if (success != pdPASS) {
        printf("Failed to create task stats\r\n");
        vTaskDelete(statsHandle);
    }
    while (1)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
