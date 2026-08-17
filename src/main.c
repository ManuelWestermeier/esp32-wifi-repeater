#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/lwip_napt.h"
#include "lwip/sockets.h"

#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define LED_BUILTIN_GPIO GPIO_NUM_2
#define CONFIG_NAMESPACE "wifi_cfg"
#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 64
#define SETUP_AP_SSID "ESP32-Setup"
#define AP_NETIF_KEY "WIFI_AP_DEF"
#define STA_NETIF_KEY "WIFI_STA_DEF"
#define DNS_PORT 53
#define HTTP_PORT 80
#define BUTTON_LONG_PRESS_MS 10000
#define BUTTON_POLL_MS 100

#if defined(IP_NAPT) && (IP_NAPT)
#define APP_HAS_NAPT 1
#else
#define APP_HAS_NAPT 0
#endif

typedef enum {
    MODE_SETUP_PORTAL = 0,
    MODE_WIFI_REPEATER = 1,
} app_mode_t;

typedef struct {
    char target_ssid[MAX_SSID_LEN + 1];
    char target_pass[MAX_PASS_LEN + 1];
    char repeater_ssid[MAX_SSID_LEN + 1];
    char repeater_pass[MAX_PASS_LEN + 1];
} wifi_config_t;

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static const char *TAG = "ESP32-Portal";

static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static httpd_handle_t s_http_server = NULL;
static TaskHandle_t s_dns_task = NULL;
static TaskHandle_t s_button_task = NULL;
static volatile bool s_restart_pending = false;
static volatile app_mode_t s_mode = MODE_SETUP_PORTAL;
static volatile bool s_portal_active = true;
static volatile bool s_led_blink = false;

// ============================================================================
// LED Control
// ============================================================================

static void led_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_BUILTIN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

static void led_set(bool on) {
    gpio_set_level(LED_BUILTIN_GPIO, on ? 1 : 0);
}

static void led_blink_task(void *arg) {
    while (1) {
        if (s_led_blink) {
            led_set(1);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            led_set(0);
            vTaskDelay(100 / portTICK_PERIOD_MS);
        } else {
            led_set(0);
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
    }
}

// ============================================================================
// NVS Config
// ============================================================================

static esp_err_t config_load(wifi_config_t *cfg) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        memset(cfg, 0, sizeof(wifi_config_t));
        snprintf(cfg->repeater_ssid, MAX_SSID_LEN + 1, "%s", SETUP_AP_SSID);
        snprintf(cfg->repeater_pass, MAX_PASS_LEN + 1, "12345678");
        return ESP_OK;
    }

    size_t len = MAX_SSID_LEN + 1;
    nvs_get_str(handle, "target_ssid", cfg->target_ssid, &len);
    len = MAX_PASS_LEN + 1;
    nvs_get_str(handle, "target_pass", cfg->target_pass, &len);
    len = MAX_SSID_LEN + 1;
    nvs_get_str(handle, "repeater_ssid", cfg->repeater_ssid, &len);
    len = MAX_PASS_LEN + 1;
    nvs_get_str(handle, "repeater_pass", cfg->repeater_pass, &len);
    
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t config_save(const wifi_config_t *cfg) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_str(handle, "target_ssid", cfg->target_ssid);
    nvs_set_str(handle, "target_pass", cfg->target_pass);
    nvs_set_str(handle, "repeater_ssid", cfg->repeater_ssid);
    nvs_set_str(handle, "repeater_pass", cfg->repeater_pass);
    
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t config_reset(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

// ============================================================================
// DNS Captive Portal
// ============================================================================

static void dns_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS: Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS: Failed to bind");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buffer[512];
    struct sockaddr_in client_addr;
    socklen_t client_addr_len;

    while (s_portal_active) {
        client_addr_len = sizeof(client_addr);
        int n = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (n > sizeof(dns_header_t)) {
            dns_header_t *req = (dns_header_t *)buffer;
            dns_header_t *resp = (dns_header_t *)buffer;
            
            resp->flags = htons(0x8180);
            resp->ancount = htons(1);

            int offset = sizeof(dns_header_t);
            while (offset < n && buffer[offset] != 0) {
                offset += buffer[offset] + 1;
            }
            offset++;

            offset += 4;
            memmove(buffer + offset + 16, buffer + offset, n - offset);
            n += 16;

            uint8_t *answer = buffer + offset;
            answer[0] = 0xc0;
            answer[1] = 0x0c;
            answer[2] = 0x00;
            answer[3] = 0x01;
            answer[4] = 0x00;
            answer[5] = 0x01;
            answer[6] = 0x00;
            answer[7] = 0x00;
            answer[8] = 0x00;
            answer[9] = 0x3c;
            answer[10] = 0x00;
            answer[11] = 0x04;
            answer[12] = 192;
            answer[13] = 168;
            answer[14] = 4;
            answer[15] = 1;

            sendto(sock, buffer, n, 0, (struct sockaddr *)&client_addr, client_addr_len);
        }
    }

    close(sock);
    vTaskDelete(NULL);
}

// ============================================================================
// HTTP Server & Captive Portal
// ============================================================================

static esp_err_t http_get_handler(httpd_req_t *req) {
    wifi_config_t cfg;
    config_load(&cfg);

    const char *html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width'><title>WiFi Repeater Setup</title><style>body{font-family:Arial;max-width:500px;margin:50px auto;background:#f0f0f0;padding:20px}h1{color:#333}input{width:100%;padding:10px;margin:10px 0;border:1px solid #ccc;box-sizing:border-box}.btn{width:100%;padding:12px;background:#007bff;color:white;border:none;border-radius:4px;cursor:pointer;font-size:16px}.btn:hover{background:#0056b3}.info{background:#e7f3ff;padding:10px;border-left:4px solid #2196F3;margin-bottom:20px}</style></head><body><h1>🌐 WiFi Repeater Setup</h1><div class='info'>Current Mode: <b>Setup Portal</b><br>LED is blinking = Portal Active</div><form method='POST'><fieldset><legend>🎯 Target Network</legend><label>Target WiFi SSID:</label><input type='text' name='target_ssid' value='%s' maxlength='32' required><label>Target WiFi Password:</label><input type='password' name='target_pass' value='%s' maxlength='64'></fieldset><fieldset><legend>📡 Repeater Network</legend><label>Repeater SSID:</label><input type='text' name='repeater_ssid' value='%s' maxlength='32' required><label>Repeater Password:</label><input type='password' name='repeater_pass' value='%s' maxlength='64' required></fieldset><button type='submit' class='btn'>💾 Save & Restart</button></form><hr><p style='font-size:12px;color:#666'><b>Boot Button:</b> Short press = Toggle Portal | Long press (10s) = Reset All Settings</p></body></html>";

    char response[2048];
    snprintf(response, sizeof(response), html,
             cfg.target_ssid, cfg.target_pass,
             cfg.repeater_ssid, cfg.repeater_pass);

    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static esp_err_t http_post_handler(httpd_req_t *req) {
    char buffer[512];
    int recv_len = httpd_req_recv(req, buffer, sizeof(buffer) - 1);
    if (recv_len <= 0) {
        return ESP_FAIL;
    }
    buffer[recv_len] = '\0';

    wifi_config_t cfg;
    config_load(&cfg);

    char *p = buffer;
    while (*p) {
        if (strncmp(p, "target_ssid=", 12) == 0) {
            p += 12;
            int i = 0;
            while (*p && *p != '&' && i < MAX_SSID_LEN) {
                cfg.target_ssid[i++] = *p++;
            }
            cfg.target_ssid[i] = '\0';
        } else if (strncmp(p, "target_pass=", 12) == 0) {
            p += 12;
            int i = 0;
            while (*p && *p != '&' && i < MAX_PASS_LEN) {
                cfg.target_pass[i++] = *p++;
            }
            cfg.target_pass[i] = '\0';
        } else if (strncmp(p, "repeater_ssid=", 14) == 0) {
            p += 14;
            int i = 0;
            while (*p && *p != '&' && i < MAX_SSID_LEN) {
                cfg.repeater_ssid[i++] = *p++;
            }
            cfg.repeater_ssid[i] = '\0';
        } else if (strncmp(p, "repeater_pass=", 14) == 0) {
            p += 14;
            int i = 0;
            while (*p && *p != '&' && i < MAX_PASS_LEN) {
                cfg.repeater_pass[i++] = *p++;
            }
            cfg.repeater_pass[i] = '\0';
        }
        while (*p && *p != '&') p++;
        if (*p) p++;
    }

    config_save(&cfg);
    ESP_LOGI(TAG, "Config saved. Restarting...");

    httpd_resp_send(req, "Saved! Restarting...", HTTPD_RESP_USE_STRLEN);
    s_restart_pending = true;
    return ESP_OK;
}

static httpd_uri_t uri_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = http_get_handler,
};

static httpd_uri_t uri_post = {
    .uri = "/",
    .method = HTTP_POST,
    .handler = http_post_handler,
};

static httpd_handle_t start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.lru_purge_enable = true;
    config.max_open_sockets = 4;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return NULL;
    }

    httpd_register_uri_handler(server, &uri_get);
    httpd_register_uri_handler(server, &uri_post);

    ESP_LOGI(TAG, "HTTP server started on port %d", HTTP_PORT);
    return server;
}

// ============================================================================
// WiFi Event Handler
// ============================================================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "Station joined AP, MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                     event->mac[0], event->mac[1], event->mac[2],
                     event->mac[3], event->mac[4], event->mac[5]);
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_mode = MODE_WIFI_REPEATER;
            s_led_blink = false;

#if APP_HAS_NAPT
            esp_netif_ip_info_t ap_ip;
            esp_netif_get_ip_info(s_ap_netif, &ap_ip);
            ip_napt_enable(ap_ip.ip.addr, 1);
            ESP_LOGI(TAG, "NAPT enabled on AP IP: " IPSTR, IP2STR(&ap_ip.ip));
#endif
        }
    }
}

static void start_setup_portal(void) {
    ESP_LOGI(TAG, "Starting Setup Portal Mode");
    s_mode = MODE_SETUP_PORTAL;
    s_led_blink = true;
    s_portal_active = true;

    esp_wifi_disconnect();

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    strlcpy((char *)wifi_config.ssid, SETUP_AP_SSID, sizeof(wifi_config.ssid));
    strlcpy((char *)wifi_config.password, "12345678", sizeof(wifi_config.password));
    wifi_config.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.max_connection = 4;

    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_start();

    if (!s_http_server) {
        s_http_server = start_http_server();
    }

    if (!s_dns_task) {
        xTaskCreate(dns_task, "dns_task", 2048, NULL, 5, &s_dns_task);
    }
}

static void start_wifi_repeater(void) {
    wifi_config_t cfg;
    config_load(&cfg);

    if (cfg.target_ssid[0] == '\0') {
        ESP_LOGW(TAG, "No target SSID configured, starting portal");
        start_setup_portal();
        return;
    }

    ESP_LOGI(TAG, "Starting WiFi Repeater Mode");
    s_mode = MODE_WIFI_REPEATER;
    s_led_blink = false;
    s_portal_active = false;

    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }

    if (s_dns_task) {
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }

    wifi_config_t sta_config;
    memset(&sta_config, 0, sizeof(sta_config));
    strlcpy((char *)sta_config.ssid, cfg.target_ssid, sizeof(sta_config.ssid));
    strlcpy((char *)sta_config.password, cfg.target_pass, sizeof(sta_config.password));

    esp_wifi_set_config(WIFI_IF_STA, &sta_config);

    wifi_config_t ap_config;
    memset(&ap_config, 0, sizeof(ap_config));
    strlcpy((char *)ap_config.ssid, cfg.repeater_ssid, sizeof(ap_config.ssid));
    strlcpy((char *)ap_config.password, cfg.repeater_pass, sizeof(ap_config.password));
    ap_config.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.max_connection = 4;

    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_start();

    ESP_LOGI(TAG, "Target SSID: %s", cfg.target_ssid);
    ESP_LOGI(TAG, "Repeater SSID: %s", cfg.repeater_ssid);
}

// ============================================================================
// Button Handler
// ============================================================================

static void button_task(void *arg) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    uint32_t press_duration = 0;

    while (1) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            press_duration += BUTTON_POLL_MS;
            
            if (press_duration == BUTTON_LONG_PRESS_MS) {
                ESP_LOGI(TAG, "Long press detected - resetting config");
                config_reset();
                s_restart_pending = true;
            }
        } else {
            if (press_duration > 0 && press_duration < BUTTON_LONG_PRESS_MS) {
                ESP_LOGI(TAG, "Short press detected - toggling portal");
                if (s_portal_active) {
                    start_wifi_repeater();
                } else {
                    start_setup_portal();
                }
            }
            press_duration = 0;
        }

        vTaskDelay(BUTTON_POLL_MS / portTICK_PERIOD_MS);
    }
}

// ============================================================================
// Main
// ============================================================================

void app_main(void) {
    ESP_LOGI(TAG, "Starting ESP32 WiFi Repeater");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    led_init();
    xTaskCreate(led_blink_task, "led_blink", 1024, NULL, 1, NULL);
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);

    start_setup_portal();

    while (1) {
        if (s_restart_pending) {
            ESP_LOGI(TAG, "Restarting...");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            esp_restart();
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}
