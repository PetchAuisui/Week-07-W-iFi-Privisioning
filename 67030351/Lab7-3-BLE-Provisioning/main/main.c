#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "protocomm_ble.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

static const char *TAG = "LAB7_3_BLE";

#define FACTORY_RESET_BUTTON_GPIO  GPIO_NUM_18   // ปุ่ม Factory Reset (Active-Low ต่อลง GND)
#define LED_PIN_WIFI_STA           GPIO_NUM_2    // LED 1: Wi-Fi STA Status (On-board LED)
#define LED_PIN_BLE_PROV           GPIO_NUM_4    // LED 2: BLE Provisioning Status
#define PROV_POP_KEY               "abcd1234"   // Proof-of-Possession (PoP)

/* ตรวจสอบการกดปุ่ม GPIO 18 ค้างไว้ 3 วินาที (Active Low) */
static bool check_factory_reset_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FACTORY_RESET_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Hold GPIO 18 button for 3 seconds to trigger Factory Reset...");
    int hold_count = 0;
    while (gpio_get_level(FACTORY_RESET_BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        hold_count++;
        if (hold_count % 10 == 0) {
            ESP_LOGI(TAG, "Holding button... %d/3 seconds", hold_count / 10);
        }
        if (hold_count >= 30) {
            ESP_LOGW(TAG, "=================================================");
            ESP_LOGW(TAG, ">>> FACTORY RESET TRIGGERED! ERASING NVS FLASH <<<");
            ESP_LOGW(TAG, "=================================================");
            return true;
        }
    }
    return false;
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
            case NETWORK_PROV_START:
                ESP_LOGI(TAG, "[PROV EVENT]: BLE Provisioning Started (Advertising)!");
                gpio_set_level(LED_PIN_BLE_PROV, 1);
                break;
            case NETWORK_PROV_WIFI_CRED_RECV: {
                wifi_sta_config_t *sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG, "=================================================");
                ESP_LOGI(TAG, "[BLE CREDENTIALS RECEIVED]:");
                ESP_LOGI(TAG, "  -> SSID     : %s", (const char *)sta_cfg->ssid);
                ESP_LOGI(TAG, "  -> Password : %s", (const char *)sta_cfg->password);
                ESP_LOGI(TAG, "=================================================");
                break;
            }
            case NETWORK_PROV_WIFI_CRED_FAIL:
                ESP_LOGE(TAG, "[ERROR]: Wi-Fi Connection failed with provided credentials!");
                break;
            case NETWORK_PROV_WIFI_CRED_SUCCESS:
                ESP_LOGI(TAG, "[SUCCESS]: BLE Provisioning Successful!");
                gpio_set_level(LED_PIN_BLE_PROV, 0); // ปิด LED BLE
                break;
            case NETWORK_PROV_END:
                ESP_LOGI(TAG, "[PROV EVENT]: De-initializing BLE & Releasing BT Memory...");
                network_prov_mgr_deinit();
                break;
            default:
                break;
        }
    } else if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        if (event_id == PROTOCOMM_TRANSPORT_BLE_CONNECTED) {
            ESP_LOGI(TAG, "[BLE]: Smartphone Connected to GATT Server!");
        } else if (event_id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED) {
            ESP_LOGW(TAG, "[BLE]: Smartphone Disconnected from GATT Server");
        }
    } else if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGW(TAG, "[WIFI]: Disconnected, reconnecting...");
            gpio_set_level(LED_PIN_WIFI_STA, 0);
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "=================================================");
        ESP_LOGI(TAG, "[ONLINE]: Connected to Wi-Fi with IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "=================================================");
        gpio_set_level(LED_PIN_WIFI_STA, 1);
    }
}

void app_main(void)
{
    // 1. กำหนด GPIO Output สำหรับ LED 1 (GPIO 2) และ LED 2 (GPIO 4)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN_WIFI_STA) | (1ULL << LED_PIN_BLE_PROV),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_PIN_WIFI_STA, 0);
    gpio_set_level(LED_PIN_BLE_PROV, 0);

    // 2. ตรวจสอบการกดปุ่ม GPIO 18 ค้าง 3 วินาทีเพื่อล้าง NVS Flash
    if (check_factory_reset_button()) {
        ESP_LOGW(TAG, "[RESET]: Factory Reset Button Triggered! Erasing NVS Flash...");
        ESP_ERROR_CHECK(nvs_flash_erase());
    }

    // 3. เริ่มต้น NVS Flash, Netif และ Event Loop
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 4. กำหนดค่า Provisioning Manager เป็น BLE Scheme + คืน RAM BT เมื่อเสร็จ
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(config));

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        char service_name[16];
        snprintf(service_name, sizeof(service_name), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);

        // กำหนด Custom 128-bit UUID สำหรับ GATT Service
        uint8_t custom_service_uuid[] = {
            0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
            0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
        };
        network_prov_scheme_ble_set_service_uuid(custom_service_uuid);

        ESP_LOGI(TAG, "Starting BLE Provisioning (Name: %s, PoP: %s)", service_name, PROV_POP_KEY);

        network_prov_security_t security = NETWORK_PROV_SECURITY_1;
        const char *pop = PROV_POP_KEY;

        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(security, (const void *)pop, service_name, NULL));

        ESP_LOGI(TAG, "--------------------------------------------------");
        ESP_LOGI(TAG, "[QR CODE URL]: Click or copy the URL below:");
        ESP_LOGI(TAG, "https://espressif.github.io/esp-jumpstart/qrcode.html?data=%%7B%%22ver%%22%%3A%%22v1%%22%%2C%%22name%%22%%3A%%22%s%%22%%2C%%22pop%%22%%3A%%22%s%%22%%2C%%22transport%%22%%3A%%22ble%%22%%7D",
                 service_name, pop);
        ESP_LOGI(TAG, "Payload JSON: {\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}",
                 service_name, pop);
        ESP_LOGI(TAG, "--------------------------------------------------");
    } else {
        ESP_LOGI(TAG, "Already provisioned! Starting Wi-Fi Station");
        network_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
}
