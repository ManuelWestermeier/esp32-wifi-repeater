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
#include "esp_restart.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/lwip_napt.h"
#include "lwip/sockets.h"

#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define CONFIG_NAMESPACE "wifi_cfg"
#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 64
#define MAX_FIELD_LEN 128
#define SETUP_AP_BASE_SSID "ESP32-Setup"
#define SETUP_AP_MAX_CONN 4
#define AP_NETIF_KEY "WIFI_AP_DEF"
#define STA_NETIF_KEY "WIFI_STA_DEF"
#define DNS_PORT 53
#define HTTP_PORT 80
#define BUTTON_HOLD_SECONDS 5
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

static void restart_later(void *arg);
static void start_router_mode(const app_config_t *cfg);
static void start_config_portal(void);

static void safe_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static void trim_trailing_spaces(char *s) {
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void trim_leading_spaces(char *s) {
    if (!s) {
        return;
    }
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) {
        i++;
    }
    if (i > 0) {
        memmove(s, s + i, strlen(s + i) + 1);
    }
}

static void trim_whitespace(char *s) {
    trim_leading_spaces(s);
    trim_trailing_spaces(s);
}

static void url_decode(char *dst, size_t dst_size, const char *src) {
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
    if (!body || !key || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char encoded[MAX_FIELD_LEN + 1];
    esp_err_t err = httpd_query_key_value(body, key, encoded, sizeof(encoded));
    if (err != ESP_OK) {
        return err;
    }
    url_decode(out, out_size, encoded);
    trim_whitespace(out);
    return ESP_OK;
}

static bool config_is_valid(const app_config_t *cfg) {
    if (!cfg) {
        return false;
    }
    if (cfg->target_ssid[0] == '\0' || cfg->hotspot_ssid[0] == '\0') {
        return false;
    }
    size_t hotspot_pass_len = strlen(cfg->hotspot_pass);
    if (hotspot_pass_len != 0 && hotspot_pass_len < 8) {
        return false;
    }
    if (strlen(cfg->target_ssid) > MAX_SSID_LEN || strlen(cfg->hotspot_ssid) > MAX_SSID_LEN) {
        return false;
    }
    if (strlen(cfg->target_pass) > MAX_PASS_LEN || strlen(cfg->hotspot_pass) > MAX_PASS_LEN) {
        return false;
    }
    return true;
}

static esp_err_t save_config_to_nvs(const app_config_t *cfg) {
    if (!config_is_valid(cfg)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs_open failed");
    esp_err_t err = nvs_set_str(handle, "target_ssid", cfg->target_ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "target_pass", cfg->target_pass);
    if (err == ESP_OK) err = nvs_set_str(handle, "hotspot_ssid", cfg->hotspot_ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "hotspot_pass", cfg->hotspot_pass);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t load_config_from_nvs(app_config_t *cfg) {
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
    err = nvs_get_str(handle, "target_ssid", cfg->target_ssid, &len);
    if (err != ESP_OK) goto done;
    len = sizeof(cfg->target_pass);
    err = nvs_get_str(handle, "target_pass", cfg->target_pass, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) goto done;
    if (err == ESP_ERR_NVS_NOT_FOUND) cfg->target_pass[0] = '\0';
    len = sizeof(cfg->hotspot_ssid);
    err = nvs_get_str(handle, "hotspot_ssid", cfg->hotspot_ssid, &len);
    if (err != ESP_OK) goto done;
    len = sizeof(cfg->hotspot_pass);
    err = nvs_get_str(handle, "hotspot_pass", cfg->hotspot_pass, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) goto done;
    if (err == ESP_ERR_NVS_NOT_FOUND) cfg->hotspot_pass[0] = '\0';
    err = config_is_valid(cfg) ? ESP_OK : ESP_ERR_INVALID_STATE;

done:
    nvs_close(handle);
    return err;
}

static esp_err_t clear_config_nvs(void) {
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

static void get_setup_ap_ssid(char *out, size_t out_size) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, out_size, "%s-%02X%02X", SETUP_AP_BASE_SSID, mac[4], mac[5]);
}

static void wifi_log_sta_credentials(const app_config_t *cfg) {
    ESP_LOGI(TAG, "Target SSID: %s", cfg->target_ssid);
    ESP_LOGI(TAG, "Hotspot SSID: %s", cfg->hotspot_ssid);
}

static void set_ap_config_from_credentials(const char *ssid, const char *pass) {
    wifi_config_t ap_cfg = { 0 };
    safe_copy((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), ssid);
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    safe_copy((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), pass);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = SETUP_AP_MAX_CONN;
    ap_cfg.ap.beacon_interval = 100;
    if (ap_cfg.ap.password[0] == '\0') {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
}

static void set_sta_config_from_credentials(const app_config_t *cfg) {
    wifi_config_t sta_cfg = { 0 };
    safe_copy((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), cfg->target_ssid);
    safe_copy((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), cfg->target_pass);
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (sta_cfg.sta.password[0] == '\0') {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
}

static void start_dns_captive_task(void *arg);
static void start_dns_proxy_task(void *arg);

static void stop_dns_task(void) {
    if (s_dns_task != NULL) {
        TaskHandle_t task = s_dns_task;
        s_dns_task = NULL;
        vTaskDelete(task);
    }
}

static void restart_later(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void schedule_restart(void) {
    if (s_restart_pending) {
        return;
    }
    s_restart_pending = true;
    xTaskCreate(restart_later, "restart_later", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
}

static void stop_http_server(void) {
    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    const char *page =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>ESP32 Config</title>"
        "<style>body{font-family:system-ui,sans-serif;max-width:720px;margin:24px auto;padding:0 16px;}"
        "input{width:100%;padding:10px;margin:6px 0 14px;box-sizing:border-box;}"
        "button{padding:12px 16px;width:100%;}label{font-weight:600;}</style></head><body>"
        "<h1>ESP32 Wi-Fi Setup</h1>"
        "<p>Target WLAN verbindet der ESP32 als Station. Das Hotspot-WLAN wird danach bereitgestellt.</p>"
        "<form method='POST' action='/save'>"
        "<label>Target SSID</label><input name='target_ssid' maxlength='32' required>"
        "<label>Target Passwort</label><input name='target_pass' maxlength='64' type='password'>"
        "<label>Hotspot SSID</label><input name='hotspot_ssid' maxlength='32' required>"
        "<label>Hotspot Passwort (leer = offen, sonst 8-63 Zeichen)</label><input name='hotspot_pass' maxlength='64' type='password'>"
        "<button type='submit'>Speichern und neu starten</button>"
        "</form></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    const size_t body_len = req->content_len;
    char *body = calloc(1, body_len + 1);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    size_t received = 0;
    while (received < body_len) {
        int r = httpd_req_recv(req, body + received, body_len - received);
        if (r <= 0) {
            free(body);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                return httpd_resp_send_408(req);
            }
            return ESP_FAIL;
        }
        received += (size_t)r;
    }

    app_config_t cfg = { 0 };
    ESP_ERROR_CHECK_WITHOUT_ABORT(form_get_value(body, "target_ssid", cfg.target_ssid, sizeof(cfg.target_ssid)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(form_get_value(body, "target_pass", cfg.target_pass, sizeof(cfg.target_pass)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(form_get_value(body, "hotspot_ssid", cfg.hotspot_ssid, sizeof(cfg.hotspot_ssid)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(form_get_value(body, "hotspot_pass", cfg.hotspot_pass, sizeof(cfg.hotspot_pass)));
    free(body);

    if (!config_is_valid(&cfg)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid config");
    }

    esp_err_t err = save_config_to_nvs(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_config_to_nvs failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    }

    const char *response =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='2; url=/'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Gespeichert</title></head><body>"
        "<h1>Gespeichert</h1><p>Neustart folgt.</p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) {
        schedule_restart();
    }
    return err;
}

static esp_err_t redirect_handler(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static httpd_handle_t start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.max_open_sockets = 8;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return NULL;
    }

    static const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t probes[] = {
        { .uri = "/generate_204", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/gen_204", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = root_get_handler },
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save));
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &probes[i]));
    }
    ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_handler));
    return server;
}

static esp_err_t dns_question_end(const uint8_t *pkt, size_t len, size_t *offset) {
    if (!pkt || len < sizeof(dns_header_t) + 5 || !offset) {
        return ESP_FAIL;
    }
    size_t pos = sizeof(dns_header_t);
    while (pos < len && pkt[pos] != 0) {
        uint8_t label_len = pkt[pos];
        if ((label_len & 0xC0) != 0) {
            return ESP_FAIL;
        }
        pos += (size_t)label_len + 1;
    }
    if (pos + 5 > len) {
        return ESP_FAIL;
    }
    *offset = pos + 5;
    return ESP_OK;
}

static size_t dns_build_captive_response(const uint8_t *query, size_t query_len, uint8_t *resp, size_t resp_size, uint32_t ap_ip) {
    size_t question_end = 0;
    if (dns_question_end(query, query_len, &question_end) != ESP_OK) {
        return 0;
    }
    if (resp_size < question_end + 16) {
        return 0;
    }
    memcpy(resp, query, question_end);

    dns_header_t *h = (dns_header_t *)resp;
    h->flags = htons(0x8180);
    h->ancount = htons(1);
    h->nscount = 0;
    h->arcount = 0;

    size_t off = question_end;
    resp[off++] = 0xC0;
    resp[off++] = 0x0C;
    resp[off++] = 0x00;
    resp[off++] = 0x01;
    resp[off++] = 0x00;
    resp[off++] = 0x01;
    resp[off++] = 0x00;
    resp[off++] = 0x00;
    resp[off++] = 0x00;
    resp[off++] = 0x3C;
    resp[off++] = 0x00;
    resp[off++] = 0x04;
    memcpy(&resp[off], &ap_ip, sizeof(ap_ip));
    off += sizeof(ap_ip);
    return off;
}

static bool get_ap_ip(uint32_t *ip_out) {
    if (!s_ap_netif || !ip_out) {
        return false;
    }
    esp_netif_ip_info_t ip_info = { 0 };
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) != ESP_OK) {
        return false;
    }
    *ip_out = ip_info.ip.addr;
    return true;
}

static bool get_sta_dns(uint32_t *ip_out) {
    if (!s_sta_netif || !ip_out) {
        return false;
    }
    esp_netif_dns_info_t dns = { 0 };
    if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        return false;
    }
    if (dns.ip.type != IPADDR_TYPE_V4) {
        return false;
    }
    if (dns.ip.u_addr.ip4.addr == 0) {
        return false;
    }
    *ip_out = dns.ip.u_addr.ip4.addr;
    return true;
}

static void dns_captive_task(void *arg) {
    (void)arg;
    uint8_t rx_buf[512];
    uint8_t tx_buf[512];
    while (1) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "DNS socket create failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        struct sockaddr_in bind_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(DNS_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
            ESP_LOGE(TAG, "DNS bind failed: errno=%d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "DNS captive server started");
        while (s_mode == MODE_CONFIG && !s_restart_pending) {
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);
            int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&from_addr, &from_len);
            if (len <= 0) {
                continue;
            }
            uint32_t ap_ip = 0;
            if (!get_ap_ip(&ap_ip)) {
                continue;
            }
            size_t resp_len = dns_build_captive_response(rx_buf, (size_t)len, tx_buf, sizeof(tx_buf), ap_ip);
            if (resp_len == 0) {
                continue;
            }
            sendto(sock, tx_buf, resp_len, 0, (struct sockaddr *)&from_addr, from_len);
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void dns_proxy_task(void *arg) {
    (void)arg;
    uint8_t rx_buf[512];
    uint8_t tx_buf[512];
    while (1) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "DNS proxy socket create failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        struct sockaddr_in bind_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(DNS_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
            ESP_LOGE(TAG, "DNS proxy bind failed: errno=%d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "DNS proxy started");
        while (s_mode == MODE_ROUTER && !s_restart_pending) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&client_addr, &client_len);
            if (len <= 0) {
                continue;
            }

            uint32_t upstream_ip = 0;
            if (!get_sta_dns(&upstream_ip)) {
                upstream_ip = inet_addr("1.1.1.1");
            }

            int upstream = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (upstream < 0) {
                continue;
            }
            struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
            setsockopt(upstream, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            struct sockaddr_in upstream_addr = {
                .sin_family = AF_INET,
                .sin_port = htons(DNS_PORT),
                .sin_addr.s_addr = upstream_ip,
            };
            if (connect(upstream, (struct sockaddr *)&upstream_addr, sizeof(upstream_addr)) != 0) {
                close(upstream);
                continue;
            }
            if (send(upstream, rx_buf, len, 0) < 0) {
                close(upstream);
                continue;
            }
            int resp_len = recv(upstream, tx_buf, sizeof(tx_buf), 0);
            close(upstream);
            if (resp_len > 0) {
                sendto(sock, tx_buf, resp_len, 0, (struct sockaddr *)&client_addr, client_len);
            }
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void start_dns_captive_task(void *arg) {
    (void)arg;
    if (s_dns_task != NULL) {
        return;
    }
    xTaskCreate(dns_captive_task, "dns_captive", 4096, NULL, tskIDLE_PRIORITY + 2, &s_dns_task);
}

static void start_dns_proxy_task(void *arg) {
    (void)arg;
    if (s_dns_task != NULL) {
        return;
    }
    xTaskCreate(dns_proxy_task, "dns_proxy", 4096, NULL, tskIDLE_PRIORITY + 2, &s_dns_task);
}

static void ensure_button_task(void);

static void button_watch_task(void *arg) {
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int64_t pressed_since = -1;
    while (1) {
        int level = gpio_get_level(BOOT_BUTTON_GPIO);
        if (level == 0) {
            if (pressed_since < 0) {
                pressed_since = esp_timer_get_time();
            } else if ((esp_timer_get_time() - pressed_since) >= (int64_t)BUTTON_HOLD_SECONDS * 1000000LL) {
                ESP_LOGW(TAG, "BOOT button held; clearing config");
                clear_config_nvs();
                vTaskDelay(pdMS_TO_TICKS(250));
                esp_restart();
            }
        } else {
            pressed_since = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

static void ensure_button_task(void) {
    if (s_button_task != NULL) {
        return;
    }
    xTaskCreate(button_watch_task, "button_watch", 3072, NULL, tskIDLE_PRIORITY + 1, &s_button_task);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    if (event_id == WIFI_EVENT_STA_START) {
        if (s_mode == MODE_ROUTER) {
            ESP_LOGI(TAG, "STA start -> connect");
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "STA disconnected reason=%d", event ? event->reason : -1);
        if (s_mode == MODE_ROUTER && !s_restart_pending) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        }
    } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        if (event) {
            ESP_LOGI(TAG, "AP client connected: " MACSTR, MAC2STR(event->mac));
        }
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        if (event) {
            ESP_LOGI(TAG, "AP client disconnected: " MACSTR, MAC2STR(event->mac));
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (!event) {
            return;
        }
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        esp_netif_set_default_netif(s_sta_netif);
#if APP_HAS_NAPT
        if (s_ap_netif) {
            esp_netif_ip_info_t ap_ip = { 0 };
            if (esp_netif_get_ip_info(s_ap_netif, &ap_ip) == ESP_OK) {
                ip_napt_enable(ap_ip.ip.addr, 1);
                ESP_LOGI(TAG, "NAPT enabled on AP IP: " IPSTR, IP2STR(&ap_ip.ip));
            }
        }
#endif
    }
}

static void wifi_init_common(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static void start_config_portal(void) {
    s_mode = MODE_CONFIG;
    char setup_ssid[33];
    get_setup_ap_ssid(setup_ssid, sizeof(setup_ssid));
    set_ap_config_from_credentials(setup_ssid, "");
    ESP_ERROR_CHECK(esp_wifi_start());
    start_dns_captive_task(NULL);
    s_http_server = start_http_server();
    ESP_LOGI(TAG, "Config AP started: %s", setup_ssid);
}

static void start_router_mode(const app_config_t *cfg) {
    s_mode = MODE_ROUTER;
    wifi_log_sta_credentials(cfg);
    set_ap_config_from_credentials(cfg->hotspot_ssid, cfg->hotspot_pass);
    set_sta_config_from_credentials(cfg);
    ESP_ERROR_CHECK(esp_wifi_start());
    start_dns_proxy_task(NULL);
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "Router mode started");
}

void app_main(void) {
    esp_log_level_set("httpd_uri", ESP_LOG_WARN);
    esp_log_level_set("httpd_txrx", ESP_LOG_WARN);
    esp_log_level_set("httpd_parse", ESP_LOG_WARN);
    esp_log_level_set("dns", ESP_LOG_WARN);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    wifi_init_common();
    ensure_button_task();

    app_config_t cfg = { 0 };
    if (load_config_from_nvs(&cfg) == ESP_OK) {
        start_router_mode(&cfg);
    } else {
        start_config_portal();
    }
}
