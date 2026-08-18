#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.hpp"

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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/lwip_napt.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#define BOOT_BUTTON_GPIO       GPIO_NUM_0
#define LED_BUILTIN            GPIO_NUM_2
#define CONFIG_NAMESPACE       "wifi_cfg"

#define MAX_SSID_LEN           32
#define MAX_PASS_LEN           64
#define MAX_FORM_BODY_LEN      512

#define SETUP_AP_BASE_SSID     "ESP32-Setup"
#define SETUP_AP_MAX_CONN      4

#define DNS_PORT               53
#define HTTP_PORT              80
#define BUTTON_HOLD_SECONDS    10
#define BUTTON_POLL_MS         100
#define BUTTON_DEBOUNCE_MS     50
#define RESTART_DELAY_MS       1000

#define NVS_KEY_TARGET_SSID    "target_ssid"
#define NVS_KEY_TARGET_PASS    "target_pass"
#define NVS_KEY_HOTSPOT_SSID   "hotspot_ssid"
#define NVS_KEY_HOTSPOT_PASS   "hotspot_pass"
#define NVS_KEY_FORCE_PORTAL   "force_portal"

#if defined(IP_NAPT) && IP_NAPT
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
static volatile bool s_restart_pending;
static volatile app_mode_t s_mode;

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

static void trim_whitespace(char *s)
{
    if (!s) {
        return;
    }

    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) {
        ++start;
    }

    if (start > 0) {
        memmove(s, s + start, strlen(s + start) + 1);
    }

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void url_decode(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }

    size_t di = 0;

    for (size_t si = 0; src && src[si] != '\0'; ++si) {
        if (di + 1 >= dst_size) {
            break;
        }

        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (
            src[si] == '%' &&
            isxdigit((unsigned char)src[si + 1]) &&
            isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }

    dst[di] = '\0';
}

static esp_err_t form_get_value(
    const char *body,
    const char *key,
    char *out,
    size_t out_size)
{
    char encoded[MAX_PASS_LEN + MAX_SSID_LEN + 16] = { 0 };

    esp_err_t err = httpd_query_key_value(body, key, encoded, sizeof(encoded));
    if (err != ESP_OK) {
        return err;
    }

    url_decode(out, out_size, encoded);
    trim_whitespace(out);
    return ESP_OK;
}

static bool config_is_valid(const app_config_t *cfg)
{
    if (!cfg || cfg->target_ssid[0] == '\0' || cfg->hotspot_ssid[0] == '\0') {
        return false;
    }

    const size_t target_pass_len = strlen(cfg->target_pass);
    const size_t hotspot_pass_len = strlen(cfg->hotspot_pass);

    if (target_pass_len > MAX_PASS_LEN || hotspot_pass_len > MAX_PASS_LEN) {
        return false;
    }

    if (strlen(cfg->target_ssid) > MAX_SSID_LEN ||
        strlen(cfg->hotspot_ssid) > MAX_SSID_LEN) {
        return false;
    }

    return hotspot_pass_len == 0 || hotspot_pass_len >= 8;
}

static void config_from_compile_time_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    safe_copy(cfg->target_ssid, sizeof(cfg->target_ssid), TARGET_SSID);
    safe_copy(cfg->target_pass, sizeof(cfg->target_pass), TARGET_PASS);
    safe_copy(cfg->hotspot_ssid, sizeof(cfg->hotspot_ssid), HOTSPOT_SSID);
    safe_copy(cfg->hotspot_pass, sizeof(cfg->hotspot_pass), HOTSPOT_PASS);
}

static esp_err_t save_config_to_nvs(const app_config_t *cfg)
{
    if (!config_is_valid(cfg)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_TARGET_SSID, cfg->target_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_TARGET_PASS, cfg->target_pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_HOTSPOT_SSID, cfg->hotspot_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_HOTSPOT_PASS, cfg->hotspot_pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, NVS_KEY_FORCE_PORTAL, 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static esp_err_t load_config_from_nvs(app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(cfg, 0, sizeof(*cfg));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(cfg->target_ssid);
    err = nvs_get_str(handle, NVS_KEY_TARGET_SSID, cfg->target_ssid, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    len = sizeof(cfg->target_pass);
    err = nvs_get_str(handle, NVS_KEY_TARGET_PASS, cfg->target_pass, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        cfg->target_pass[0] = '\0';
    } else if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    len = sizeof(cfg->hotspot_ssid);
    err = nvs_get_str(handle, NVS_KEY_HOTSPOT_SSID, cfg->hotspot_ssid, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    len = sizeof(cfg->hotspot_pass);
    err = nvs_get_str(handle, NVS_KEY_HOTSPOT_PASS, cfg->hotspot_pass, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        cfg->hotspot_pass[0] = '\0';
    } else if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);
    return config_is_valid(cfg) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t clear_config_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static uint8_t load_force_portal(void)
{
    uint8_t force_portal = 0;
    nvs_handle_t handle;

    if (nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, NVS_KEY_FORCE_PORTAL, &force_portal);
        nvs_close(handle);
    }

    return force_portal;
}

static esp_err_t set_force_portal(uint8_t enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, NVS_KEY_FORCE_PORTAL, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static void set_ap_config_from_credentials(const char *ssid, const char *pass)
{
    wifi_config_t ap_cfg = { 0 };

    safe_copy((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), ssid);
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    safe_copy((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), pass);

    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = SETUP_AP_MAX_CONN;
    ap_cfg.ap.beacon_interval = 100;
    ap_cfg.ap.authmode = ap_cfg.ap.password[0] == '\0'
        ? WIFI_AUTH_OPEN
        : WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
}

static void set_sta_config_from_credentials(const app_config_t *cfg)
{
    wifi_config_t sta_cfg = { 0 };

    safe_copy((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), cfg->target_ssid);
    safe_copy((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), cfg->target_pass);

    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.threshold.authmode = sta_cfg.sta.password[0] == '\0'
        ? WIFI_AUTH_OPEN
        : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
}

static void restart_later(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>ESP32 Config</title>"
        "<style>body{font-family:system-ui,sans-serif;max-width:720px;margin:24px auto;padding:0 16px;}"
        "input{width:100%;padding:10px;margin:6px 0 14px;box-sizing:border-box;}"
        "button{padding:12px 16px;width:100%;background:#007BFF;color:#fff;border:none;border-radius:4px;}"
        "label{font-weight:600;}</style></head><body>"
        "<h1>ESP32 Wi-Fi Setup</h1>"
        "<p>Der ESP32 verbindet sich als Station mit dem Target-WLAN und stellt danach den konfigurierten Hotspot bereit.</p>"
        "<form method='POST' action='/save'>"
        "<label>Target SSID</label><input name='target_ssid' maxlength='32' required>"
        "<label>Target Passwort</label><input name='target_pass' maxlength='64' type='password'>"
        "<label>Hotspot SSID</label><input name='hotspot_ssid' maxlength='32' required>"
        "<label>Hotspot Passwort (leer = offen, sonst mindestens 8 Zeichen)</label>"
        "<input name='hotspot_pass' maxlength='64' type='password'>"
        "<button type='submit'>Speichern und neu starten</button>"
        "</form></body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_FORM_BODY_LEN) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
    }

    const size_t body_len = req->content_len;
    char *body = calloc(1, body_len + 1);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }

    size_t received = 0;
    while (received < body_len) {
        int ret = httpd_req_recv(req, body + received, body_len - received);
        if (ret <= 0) {
            free(body);
            return ret == HTTPD_SOCK_ERR_TIMEOUT
                ? httpd_resp_send_408(req)
                : ESP_FAIL;
        }
        received += (size_t)ret;
    }

    app_config_t cfg = { 0 };
    esp_err_t err = form_get_value(body, NVS_KEY_TARGET_SSID, cfg.target_ssid, sizeof(cfg.target_ssid));
    if (err == ESP_OK) {
        err = form_get_value(body, NVS_KEY_TARGET_PASS, cfg.target_pass, sizeof(cfg.target_pass));
    }
    if (err == ESP_OK) {
        err = form_get_value(body, NVS_KEY_HOTSPOT_SSID, cfg.hotspot_ssid, sizeof(cfg.hotspot_ssid));
    }
    if (err == ESP_OK) {
        err = form_get_value(body, NVS_KEY_HOTSPOT_PASS, cfg.hotspot_pass, sizeof(cfg.hotspot_pass));
    }
    free(body);

    if (err != ESP_OK || !config_is_valid(&cfg)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid config");
    }

    err = save_config_to_nvs(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    }

    static const char response[] =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='2; url=/'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Gespeichert</title></head><body>"
        "<h1>Gespeichert</h1><p>Neustart folgt...</p></body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK && !s_restart_pending) {
        s_restart_pending = true;
        xTaskCreate(restart_later, "restart_later", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    }

    return err;
}

static esp_err_t redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "", 0);
}

static httpd_handle_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.max_open_sockets = 4;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return NULL;
    }

    static const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };

    static const httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };

    static const httpd_uri_t probes[] = {
        { .uri = "/generate_204", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = root_get_handler },
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save));

    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &probes[i]));
    }

    ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_handler));

    ESP_LOGI(TAG, "HTTP server started on port %d", HTTP_PORT);
    return server;
}

static size_t dns_build_captive_response(
    const uint8_t *query,
    size_t query_len,
    uint8_t *response,
    size_t response_size,
    uint32_t ap_ip)
{
    if (!query || !response || query_len < sizeof(dns_header_t)) {
        return 0;
    }

    size_t pos = sizeof(dns_header_t);
    while (pos < query_len && query[pos] != 0) {
        const uint8_t label_len = query[pos];
        if (label_len > 63 || pos + label_len + 1 > query_len) {
            return 0;
        }
        pos += label_len + 1;
    }

    if (pos >= query_len || pos + 5 > query_len) {
        return 0;
    }

    const size_t question_end = pos + 5;
    static const uint8_t answer[] = {
        0xC0, 0x0C,
        0x00, 0x01,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x3C,
        0x00, 0x04,
    };

    if (response_size < question_end + sizeof(answer) + sizeof(ap_ip)) {
        return 0;
    }

    memcpy(response, query, question_end);

    dns_header_t *header = (dns_header_t *)response;
    header->flags = htons(0x8180);
    header->ancount = htons(1);
    header->nscount = 0;
    header->arcount = 0;

    size_t offset = question_end;
    memcpy(response + offset, answer, sizeof(answer));
    offset += sizeof(answer);
    memcpy(response + offset, &ap_ip, sizeof(ap_ip));

    return offset + sizeof(ap_ip);
}

static void dns_captive_task(void *arg)
{
    (void)arg;

    uint8_t rx_buf[512];
    uint8_t tx_buf[512];

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket creation failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    while (s_mode == MODE_CONFIG && !s_restart_pending) {
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        int len = recvfrom(
            sock,
            rx_buf,
            sizeof(rx_buf),
            0,
            (struct sockaddr *)&from_addr,
            &from_len);

        if (len <= 0) {
            continue;
        }

        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_ap_netif, &ip_info) != ESP_OK) {
            continue;
        }

        size_t response_len = dns_build_captive_response(
            rx_buf,
            (size_t)len,
            tx_buf,
            sizeof(tx_buf),
            ip_info.ip.addr);

        if (response_len > 0) {
            sendto(
                sock,
                tx_buf,
                response_len,
                0,
                (struct sockaddr *)&from_addr,
                from_len);
        }
    }

    close(sock);
    vTaskDelete(NULL);
}

static void button_watch_task(void *arg)
{
    (void)arg;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io));

    int64_t pressed_since = -1;
    bool handled_long_press = false;

    while (true) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            if (pressed_since < 0) {
                pressed_since = esp_timer_get_time();
                handled_long_press = false;
            } else if (
                !handled_long_press &&
                esp_timer_get_time() - pressed_since >= (int64_t)BUTTON_HOLD_SECONDS * 1000000LL) {
                ESP_LOGW(TAG, "BOOT button held for %d seconds; clearing config", BUTTON_HOLD_SECONDS);

                esp_err_t err = clear_config_nvs();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to clear config: %s", esp_err_to_name(err));
                }

                handled_long_press = true;
                esp_restart();
            }
        } else {
            if (pressed_since >= 0 && !handled_long_press) {
                int64_t duration = esp_timer_get_time() - pressed_since;
                if (duration >= (int64_t)BUTTON_DEBOUNCE_MS * 1000LL) {
                    uint8_t force_portal = load_force_portal();
                    esp_err_t err = set_force_portal(!force_portal);

                    if (err == ESP_OK) {
                        ESP_LOGW(TAG, "BOOT button clicked; portal mode forced=%u", !force_portal);
                        esp_restart();
                    } else {
                        ESP_LOGE(TAG, "Failed to toggle portal mode: %s", esp_err_to_name(err));
                    }
                }
            }

            pressed_since = -1;
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == WIFI_EVENT_STA_START && s_mode == MODE_ROUTER) {
        esp_wifi_connect();
        return;
    }

    if (event_id == WIFI_EVENT_STA_DISCONNECTED &&
        s_mode == MODE_ROUTER &&
        !s_restart_pending) {
        ESP_LOGW(TAG, "STA disconnected; reconnecting");
        esp_wifi_connect();
    }
}

static void ip_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_ERROR_CHECK(esp_netif_set_default_netif(s_sta_netif));

#if APP_HAS_NAPT
    esp_netif_ip_info_t ap_ip;
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ap_ip) == ESP_OK) {
        ip_napt_enable(ap_ip.ip.addr, 1);
        ESP_LOGI(TAG, "NAPT enabled on AP IP: " IPSTR, IP2STR(&ap_ip.ip));
    }
#endif
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
        return;
    }

    ESP_ERROR_CHECK(err);
}

static void init_led(void)
{
    ESP_ERROR_CHECK(gpio_reset_pin(LED_BUILTIN));
    ESP_ERROR_CHECK(gpio_set_direction(LED_BUILTIN, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(LED_BUILTIN, 0));
}

static void init_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    assert(s_ap_netif != NULL);
    assert(s_sta_netif != NULL);

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL));

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &ip_event_handler,
        NULL));
}

static void start_router_mode(const app_config_t *cfg)
{
    s_mode = MODE_ROUTER;
    gpio_set_level(LED_BUILTIN, 0);

    set_ap_config_from_credentials(cfg->hotspot_ssid, cfg->hotspot_pass);
    set_sta_config_from_credentials(cfg);

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Router mode started; AP SSID: %s", cfg->hotspot_ssid);
}

static void start_config_mode(void)
{
    s_mode = MODE_CONFIG;
    gpio_set_level(LED_BUILTIN, 1);

    uint8_t mac[6] = { 0 };
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));

    char setup_ssid[sizeof(SETUP_AP_BASE_SSID) + 5] = { 0 };
    snprintf(
        setup_ssid,
        sizeof(setup_ssid),
        "%s-%02X%02X",
        SETUP_AP_BASE_SSID,
        mac[4],
        mac[5]);

    set_ap_config_from_credentials(setup_ssid, "");
    ESP_ERROR_CHECK(esp_wifi_start());

    s_dns_task = NULL;
    BaseType_t task_ret = xTaskCreate(
        dns_captive_task,
        "dns_captive",
        4096,
        NULL,
        tskIDLE_PRIORITY + 2,
        &s_dns_task);
    ESP_ERROR_CHECK(task_ret == pdPASS ? ESP_OK : ESP_FAIL);

    s_http_server = start_http_server();
    ESP_ERROR_CHECK(s_http_server != NULL ? ESP_OK : ESP_FAIL);

    ESP_LOGI(TAG, "Config portal started; AP SSID: %s", setup_ssid);
}

void app_main(void)
{
    init_nvs();
    init_led();
    init_wifi();

    BaseType_t task_ret = xTaskCreate(
        button_watch_task,
        "button_watch",
        3072,
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL);
    ESP_ERROR_CHECK(task_ret == pdPASS ? ESP_OK : ESP_FAIL);

    app_config_t app_cfg;
    app_config_t nvs_cfg;
    config_from_compile_time_defaults(&app_cfg);

    const uint8_t force_portal = load_force_portal();
    const bool nvs_config_valid = load_config_from_nvs(&nvs_cfg) == ESP_OK;

    if (force_portal == 0 && nvs_config_valid) {
        app_cfg = nvs_cfg;
        start_router_mode(&app_cfg);
    } else if (force_portal == 0 && config_is_valid(&app_cfg)) {
        // No valid NVS configuration exists yet; use config.hpp defaults.
        start_router_mode(&app_cfg);
    } else {
        start_config_mode();
    }
}