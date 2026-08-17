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
#define LED_BUILTIN GPIO_NUM_2 // Typische Build-in LED bei Standard ESP32 DevBoards
#define CONFIG_NAMESPACE "wifi_cfg"
#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 64
#define MAX_FIELD_LEN 128
#define SETUP_AP_BASE_SSID "ESP32-Setup"
#define SETUP_AP_MAX_CONN 4
#define DNS_PORT 53
#define HTTP_PORT 80
#define BUTTON_HOLD_SECONDS 10
#define BUTTON_POLL_MS 100

#if defined(IP_NAPT) && (IP_NAPT)
#define APP_HAS_NAPT 1
#else
#define APP_HAS_NAPT 0
#endif

typedef enum {
    MODE_CONFIG = 0,
    MODE_ROUTER = 1,
} app_mode_t;

typedef struct {
    char target_ssid[MAX_SSID_LEN + 1];
    char target_pass[MAX_PASS_LEN + 1];
    char hotspot_ssid[MAX_SSID_LEN + 1];
    char hotspot_pass[MAX_PASS_LEN + 1];
} app_config_t;

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static const char *TAG = "wifi_portal";

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static httpd_handle_t s_http_server;
static TaskHandle_t s_dns_task;
static TaskHandle_t s_button_task;
static volatile bool s_restart_pending;
static volatile app_mode_t s_mode;

static void start_dns_captive_task(void *arg);
static void start_dns_proxy_task(void *arg);

static void safe_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    snprintf(dst, dst_size, "%s", src);
}

static void trim_whitespace(char *s) {
    if (!s) return;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
    
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void url_decode(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    size_t di = 0;
    for (size_t si = 0; src && src[si] != '\0'; ++si) {
        if (di + 1 >= dst_size) break;
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' && isxdigit((unsigned char)src[si + 1]) && isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static esp_err_t form_get_value(const char *body, const char *key, char *out, size_t out_size) {
    char encoded[MAX_FIELD_LEN + 1];
    esp_err_t err = httpd_query_key_value(body, key, encoded, sizeof(encoded));
    if (err != ESP_OK) return err;
    url_decode(out, out_size, encoded);
    trim_whitespace(out);
    return ESP_OK;
}

static bool config_is_valid(const app_config_t *cfg) {
    if (!cfg || cfg->target_ssid[0] == '\0' || cfg->hotspot_ssid[0] == '\0') return false;
    size_t hp_len = strlen(cfg->hotspot_pass);
    if (hp_len != 0 && hp_len < 8) return false;
    if (strlen(cfg->target_ssid) > MAX_SSID_LEN || strlen(cfg->hotspot_ssid) > MAX_SSID_LEN) return false;
    if (strlen(cfg->target_pass) > MAX_PASS_LEN || hp_len > MAX_PASS_LEN) return false;
    return true;
}

static esp_err_t save_config_to_nvs(const app_config_t *cfg) {
    if (!config_is_valid(cfg)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs_open failed");
    
    nvs_set_str(handle, "target_ssid", cfg->target_ssid);
    nvs_set_str(handle, "target_pass", cfg->target_pass);
    nvs_set_str(handle, "hotspot_ssid", cfg->hotspot_ssid);
    nvs_set_str(handle, "hotspot_pass", cfg->hotspot_pass);
    nvs_set_u8(handle, "force_portal", 0); 
    
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t load_config_from_nvs(app_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = sizeof(cfg->target_ssid);
    if (nvs_get_str(handle, "target_ssid", cfg->target_ssid, &len) != ESP_OK) goto done;
    
    len = sizeof(cfg->target_pass);
    if (nvs_get_str(handle, "target_pass", cfg->target_pass, &len) == ESP_ERR_NVS_NOT_FOUND) cfg->target_pass[0] = '\0';
    
    len = sizeof(cfg->hotspot_ssid);
    if (nvs_get_str(handle, "hotspot_ssid", cfg->hotspot_ssid, &len) != ESP_OK) goto done;
    
    len = sizeof(cfg->hotspot_pass);
    if (nvs_get_str(handle, "hotspot_pass", cfg->hotspot_pass, &len) == ESP_ERR_NVS_NOT_FOUND) cfg->hotspot_pass[0] = '\0';
    
    err = config_is_valid(cfg) ? ESP_OK : ESP_ERR_INVALID_STATE;
done:
    nvs_close(handle);
    return err;
}

static esp_err_t clear_config_nvs(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_erase_all(handle);
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void set_ap_config_from_credentials(const char *ssid, const char *pass) {
    wifi_config_t ap_cfg = { 0 };
    safe_copy((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), ssid);
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    safe_copy((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), pass);
    
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = SETUP_AP_MAX_CONN;
    ap_cfg.ap.beacon_interval = 100;
    ap_cfg.ap.authmode = (ap_cfg.ap.password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
}

static void set_sta_config_from_credentials(const app_config_t *cfg) {
    wifi_config_t sta_cfg = { 0 };
    safe_copy((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), cfg->target_ssid);
    safe_copy((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), cfg->target_pass);
    
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.threshold.authmode = (sta_cfg.sta.password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
}

static void restart_later(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    const char *page =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>ESP32 Config</title>"
        "<style>body{font-family:system-ui,sans-serif;max-width:720px;margin:24px auto;padding:0 16px;}"
        "input{width:100%;padding:10px;margin:6px 0 14px;box-sizing:border-box;}"
        "button{padding:12px 16px;width:100%;background:#007BFF;color:#fff;border:none;border-radius:4px;}label{font-weight:600;}</style></head><body>"
        "<h1>ESP32 Wi-Fi Setup</h1>"
        "<p>Target WLAN verbindet der ESP32 als Station. Das Hotspot-WLAN wird danach bereitgestellt.</p>"
        "<form method='POST' action='/save'>"
        "<label>Target SSID</label><input name='target_ssid' maxlength='32' required>"
        "<label>Target Passwort</label><input name='target_pass' maxlength='64' type='password'>"
        "<label>Hotspot SSID</label><input name='hotspot_ssid' maxlength='32' required>"
        "<label>Hotspot Passwort (leer = offen, sonst min 8 Zeichen)</label><input name='hotspot_pass' maxlength='64' type='password'>"
        "<button type='submit'>Speichern und neu starten</button>"
        "</form></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    const size_t body_len = req->content_len;
    char *body = calloc(1, body_len + 1);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");

    size_t received = 0;
    while (received < body_len) {
        int r = httpd_req_recv(req, body + received, body_len - received);
        if (r <= 0) {
            free(body);
            return (r == HTTPD_SOCK_ERR_TIMEOUT) ? httpd_resp_send_408(req) : ESP_FAIL;
        }
        received += r;
    }

    app_config_t cfg = { 0 };
    form_get_value(body, "target_ssid", cfg.target_ssid, sizeof(cfg.target_ssid));
    form_get_value(body, "target_pass", cfg.target_pass, sizeof(cfg.target_pass));
    form_get_value(body, "hotspot_ssid", cfg.hotspot_ssid, sizeof(cfg.hotspot_ssid));
    form_get_value(body, "hotspot_pass", cfg.hotspot_pass, sizeof(cfg.hotspot_pass));
    free(body);

    if (!config_is_valid(&cfg)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid config");

    if (save_config_to_nvs(&cfg) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    }

    const char *response =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='2; url=/'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Gespeichert</title></head><body>"
        "<h1>Gespeichert</h1><p>Neustart folgt...</p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    
    esp_err_t err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK && !s_restart_pending) {
        s_restart_pending = true;
        xTaskCreate(restart_later, "restart_later", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    }
    return err;
}

static esp_err_t redirect_handler(httpd_req_t *req, httpd_err_code_t err) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "", 0);
}

static httpd_handle_t start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.max_open_sockets = 4; // <--- HIER WAR DER FEHLER: Reduziert von 8 auf 4
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return NULL;
    }

    static const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    static const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler };
    static const httpd_uri_t probes[] = {
        { .uri = "/generate_204", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = root_get_handler },
    };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        httpd_register_uri_handler(server, &probes[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_handler);
    
    ESP_LOGI(TAG, "HTTP server started successfully");
    return server;
}

static size_t dns_build_captive_response(const uint8_t *query, size_t query_len, uint8_t *resp, size_t resp_size, uint32_t ap_ip) {
    size_t pos = sizeof(dns_header_t);
    while (pos < query_len && query[pos] != 0) {
        pos += query[pos] + 1;
    }
    size_t question_end = pos + 5;

    if (resp_size < question_end + 16 || pos + 5 > query_len) return 0;
    
    memcpy(resp, query, question_end);
    dns_header_t *h = (dns_header_t *)resp;
    h->flags = htons(0x8180);
    h->ancount = htons(1);
    h->nscount = 0;
    h->arcount = 0;

    size_t off = question_end;
    const uint8_t answer[] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04 };
    memcpy(&resp[off], answer, sizeof(answer));
    off += sizeof(answer);
    memcpy(&resp[off], &ap_ip, sizeof(ap_ip));
    return off + sizeof(ap_ip);
}

static void dns_captive_task(void *arg) {
    uint8_t rx_buf[512], tx_buf[512];
    
    while (s_mode == MODE_CONFIG) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        struct sockaddr_in bind_addr = { .sin_family = AF_INET, .sin_port = htons(DNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        while (s_mode == MODE_CONFIG && !s_restart_pending) {
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);
            int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&from_addr, &from_len);
            
            if (len > 0) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
                    size_t resp_len = dns_build_captive_response(rx_buf, len, tx_buf, sizeof(tx_buf), ip_info.ip.addr);
                    if (resp_len > 0) sendto(sock, tx_buf, resp_len, 0, (struct sockaddr *)&from_addr, from_len);
                }
            }
        }
        close(sock);
        break;
    }
    vTaskDelete(NULL);
}

static void button_watch_task(void *arg) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int64_t pressed_since = -1;
    bool handled_long_press = false;

    while (1) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            if (pressed_since < 0) {
                pressed_since = esp_timer_get_time();
                handled_long_press = false;
            } else if (!handled_long_press && (esp_timer_get_time() - pressed_since) >= (int64_t)BUTTON_HOLD_SECONDS * 1000000LL) {
                ESP_LOGW(TAG, "BOOT button held >10s; clearing config");
                clear_config_nvs();
                handled_long_press = true;
                esp_restart();
            }
        } else {
            if (pressed_since > 0 && !handled_long_press) {
                int64_t duration = esp_timer_get_time() - pressed_since;
                if (duration > 50000LL) { // Debounce 50ms
                    ESP_LOGW(TAG, "BOOT button clicked; toggling portal mode");
                    nvs_handle_t handle;
                    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
                        uint8_t force_portal = 0;
                        nvs_get_u8(handle, "force_portal", &force_portal);
                        nvs_set_u8(handle, "force_portal", !force_portal);
                        nvs_commit(handle);
                        nvs_close(handle);
                    }
                    esp_restart();
                }
            }
            pressed_since = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_id == WIFI_EVENT_STA_START && s_mode == MODE_ROUTER) {
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED && s_mode == MODE_ROUTER && !s_restart_pending) {
        ESP_LOGW(TAG, "STA disconnected, reconnecting...");
        esp_wifi_connect();
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        esp_netif_set_default_netif(s_sta_netif);
#if APP_HAS_NAPT
        esp_netif_ip_info_t ap_ip;
        if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ap_ip) == ESP_OK) {
            ip_napt_enable(ap_ip.ip.addr, 1);
            ESP_LOGI(TAG, "NAPT enabled on AP IP: " IPSTR, IP2STR(&ap_ip.ip));
        }
#endif
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    gpio_reset_pin(LED_BUILTIN);
    gpio_set_direction(LED_BUILTIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_BUILTIN, 0);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL);
    
    xTaskCreate(button_watch_task, "button_watch", 3072, NULL, tskIDLE_PRIORITY + 1, &s_button_task);

    uint8_t force_portal = 0;
    nvs_handle_t handle;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "force_portal", &force_portal);
        nvs_close(handle);
    }

    app_config_t app_cfg = { 0 };
    if (force_portal == 0 && load_config_from_nvs(&app_cfg) == ESP_OK) {
        s_mode = MODE_ROUTER;
        set_ap_config_from_credentials(app_cfg.hotspot_ssid, app_cfg.hotspot_pass);
        set_sta_config_from_credentials(&app_cfg);
        ESP_ERROR_CHECK(esp_wifi_start());
        // start_dns_proxy_task(NULL); // Falls du einen Proxy brauchst
    } else {
        s_mode = MODE_CONFIG;
        gpio_set_level(LED_BUILTIN, 1);
        
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        char setup_ssid[33];
        snprintf(setup_ssid, sizeof(setup_ssid), "%s-%02X%02X", SETUP_AP_BASE_SSID, mac[4], mac[5]);
        
        set_ap_config_from_credentials(setup_ssid, "");
        ESP_ERROR_CHECK(esp_wifi_start());
        
        xTaskCreate(dns_captive_task, "dns_captive", 4096, NULL, tskIDLE_PRIORITY + 2, &s_dns_task);
        s_http_server = start_http_server();
    }
}