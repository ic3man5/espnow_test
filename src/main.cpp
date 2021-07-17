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
    char mac_str[20] = {0};
    char buffer[128] = {0};
    mac_to_str(mac, mac_str);
    std::string end = "\r\n";
    if (!newline) {
        end = "\0";
    }
    sprintf(buffer, "%s: %s%s", msg, mac_str, end.c_str());
    uart_write_bytes(UART_NUM_0, buffer, strlen(buffer));
}

const uint8_t* get_peer_mac_address()
{
    // Grab our MAC address
    uint8_t mac_buffer[6] = {0};
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac_buffer));
    print_mac(mac_buffer, "MAC ADDRESS");
    // Grab the first MAC address that doesn't match ours
    for (int i=0; i < sizeof MAC_ADDR_LIST / 6; ++i) {
        for (int x=0; x < 6; ++x) {
            if (MAC_ADDR_LIST[i][x] != mac_buffer[x]) {
                return MAC_ADDR_LIST[i];
            }
        }
    }
    return nullptr;
}

void esp_now_recv_callback(const uint8_t *mac_addr, const uint8_t *data, int data_len)
{
    uart_write_bytes(UART_NUM_0, (const char*)data, data_len);
}

void app_main(void) 
{
    // configure the uart, default serial port
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .use_ref_tick = false,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, GPIO_NUM_1, GPIO_NUM_3, GPIO_NUM_22, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));
    // configure ESP NOW
    ESP_ERROR_CHECK(nvs_flash_init());
    const wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(&esp_now_recv_callback));
    // Pair the devices
    const uint8_t* peer_mac = get_peer_mac_address();
    const esp_now_peer_info_t peer_info = {
        .peer_addr = { peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4],peer_mac[5] },
        .lmk = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
        .channel = 1,
        .ifidx = ESP_IF_WIFI_STA,
        .encrypt = false,
        nullptr,
    };
    print_mac(peer_mac, "PEER MAC");
    ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));

    while (1)
    {
        ESP_ERROR_CHECK(esp_now_send(peer_mac, (const uint8_t*)"Hello World!\r\n", strlen("Hello World!\r\n")));
        //uart_write_bytes(UART_NUM_0, "Hello World!\n", 13);
        vTaskDelay(1000/ portTICK_PERIOD_MS);
    }
}
