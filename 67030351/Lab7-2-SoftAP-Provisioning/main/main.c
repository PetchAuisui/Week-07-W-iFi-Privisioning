#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"

static const char *TAG = "LAB7_2_SOFTAP";

#define LED_PIN_WIFI_STA     GPIO_NUM_2    // LED 1: Wi-Fi STA Status (On-board LED)
#define LED_PIN_SOFTAP_PROV  GPIO_NUM_5    // LED 3: SoftAP Provisioning Status
#define PROV_POP_KEY         "abcd1234"   // Proof-of-Possession (PoP)

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
            case NETWORK_PROV_START:
                ESP_LOGI(TAG, "[PROV EVENT]: SoftAP Provisioning Started!");
                gpio_set_level(LED_PIN_SOFTAP_PROV, 1);
                break;
            case NETWORK_PROV_WIFI_CRED_RECV: {
                wifi_sta_config_t *sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG, "=================================================");
                ESP_LOGI(TAG, "[CREDENTIALS RECEIVED]:");
                ESP_LOGI(TAG, "  -> Target SSID     : %s", (const char *)sta_cfg->ssid);
                ESP_LOGI(TAG, "  -> Target Password : %s", (const char *)sta_cfg->password);
                ESP_LOGI(TAG, "=================================================");
                break;
            }
            case NETWORK_PROV_WIFI_CRED_FAIL:
                ESP_LOGE(TAG, "[ERROR]: Wi-Fi Connection failed with provided credentials!");
                break;
            case NETWORK_PROV_WIFI_CRED_SUCCESS:
                ESP_LOGI(TAG, "[SUCCESS]: Provisioning Completed Successfully!");
                gpio_set_level(LED_PIN_SOFTAP_PROV, 0); // ปิด LED SoftAP
                break;
            case NETWORK_PROV_END:
                ESP_LOGI(TAG, "[PROV EVENT]: De-initializing Provisioning Manager");
                network_prov_mgr_deinit();
                break;
            default:
                break;
        }
    } else if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            ESP_LOGI(TAG, "[SOFTAP]: Mobile Phone connected to ESP32 SoftAP!");
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            ESP_LOGW(TAG, "[SOFTAP]: Mobile Phone disconnected from ESP32 SoftAP");
        } else if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGW(TAG, "[WIFI]: Disconnected, reconnecting...");
            gpio_set_level(LED_PIN_WIFI_STA, 0);
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "=================================================");
        ESP_LOGI(TAG, "[ONLINE]: Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "=================================================");
        gpio_set_level(LED_PIN_WIFI_STA, 1); // เปิดไฟ LED 1 เมื่อต่อ Wi-Fi และได้ IP สำเร็จ
    }
}

void app_main(void)
{
    // 1. ตั้งค่า GPIO Output สำหรับ LED
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN_WIFI_STA) | (1ULL << LED_PIN_SOFTAP_PROV),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_PIN_WIFI_STA, 0);
    gpio_set_level(LED_PIN_SOFTAP_PROV, 0);

    // 2. เริ่มต้นระบบ Flash และ Network
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ลงทะเบียน Event Handlers
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // กำหนดค่า Provisioning Manager เป็น SoftAP Scheme
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(config));

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        // สร้างชื่อ SoftAP เฉพาะตัวจาก MAC Address
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        char service_name[16];
        snprintf(service_name, sizeof(service_name), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);

        ESP_LOGI(TAG, "Starting SoftAP Provisioning (SSID: %s, PoP: %s)", service_name, PROV_POP_KEY);

        // Security 1 with Proof-of-Possession
        network_prov_security_t security = NETWORK_PROV_SECURITY_1;
        const char *pop = PROV_POP_KEY;

        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(security, (const void *)pop, service_name, NULL));

        ESP_LOGI(TAG, "--------------------------------------------------");
        ESP_LOGI(TAG, "[QR CODE URL]: Click or copy the URL below:");
        ESP_LOGI(TAG, "https://espressif.github.io/esp-jumpstart/qrcode.html?data=%%7B%%22ver%%22%%3A%%22v1%%22%%2C%%22name%%22%%3A%%22%s%%22%%2C%%22pop%%22%%3A%%22%s%%22%%2C%%22transport%%22%%3A%%22softap%%22%%7D",
                 service_name, pop);
        ESP_LOGI(TAG, "Payload JSON: {\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"softap\"}",
                 service_name, pop);
        ESP_LOGI(TAG, "--------------------------------------------------");
    } else {
        ESP_LOGI(TAG, "Already provisioned! Starting Wi-Fi Station");
        network_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
}
