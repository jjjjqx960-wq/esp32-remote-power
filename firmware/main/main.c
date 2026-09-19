#include "driver/gpio.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const gpio_num_t relayPin = GPIO_NUM_3;
static const char *TAG = "relay";
static const char *WIFI_TAG = "wifi_scan";
static EventGroupHandle_t wifi_event_group;
static esp_netif_t *sta_netif;
static SemaphoreHandle_t command_mutex;

#define WIFI_CONNECTED_BIT BIT0
#define FIRMWARE_VERSION "wol-ack-20260919"

#if RELAY_ACTIVE_LOW
static const int relayOn = 0;
static const int relayOff = 1;
#else
static const int relayOn = 1;
static const int relayOff = 0;
#endif

static void relay_pulse(void)
{
    ESP_ERROR_CHECK(gpio_set_level(relayPin, relayOn));
    ESP_LOGI(TAG, "RELAY PULSE ON (400ms)");
    vTaskDelay(pdMS_TO_TICKS(400));
    ESP_ERROR_CHECK(gpio_set_level(relayPin, relayOff));
    ESP_LOGI(TAG, "RELAY PULSE OFF");
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Accept AA:BB:CC:DD:EE:FF, AA-BB-CC-DD-EE-FF, AABBCCDDEEFF,
 * and the dotted form. Separators are ignored, all other characters reject. */
static bool parse_mac(const char *s, uint8_t mac[6])
{
    char digits[12];
    size_t n = 0;

    if (s == NULL) {
        return false;
    }
    while (isspace((unsigned char)*s)) {
        s++;
    }
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (isxdigit(c)) {
            if (n >= sizeof(digits)) {
                return false;
            }
            digits[n++] = (char)c;
        } else if (c == ':' || c == '-' || c == '.' || isspace(c)) {
            continue;
        } else {
            return false;
        }
    }
    if (n != sizeof(digits)) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        int hi = hex_value(digits[i * 2]);
        int lo = hex_value(digits[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        mac[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static int wol_send_packets(int fd, const uint8_t pkt[102], uint32_t addr)
{
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(9),
        .sin_addr.s_addr = addr,
    };
    ip4_addr_t log_addr = { .addr = addr };
    int sent = 0;
    for (int i = 0; i < 3; i++) {
        int n = sendto(fd, pkt, 102, 0, (struct sockaddr *)&dst, sizeof(dst));
        if (n == 102) {
            sent++;
        } else {
            ESP_LOGW(TAG, "WOL send failed to " IPSTR " (errno=%d)",
                     IP2STR(&log_addr), errno);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return sent;
}

static bool wol_send_once(const uint8_t mac[6])
{
    if (!(xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WOL refused: Wi-Fi has no IP");
        return false;
    }
    uint8_t pkt[102];
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(pkt + 6 + i * 6, mac, 6);
    }

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "WOL socket failed (errno=%d)", errno);
        return false;
    }
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) < 0) {
        ESP_LOGE(TAG, "WOL broadcast option failed (errno=%d)", errno);
        close(fd);
        return false;
    }

    uint32_t directed = INADDR_BROADCAST;
    esp_netif_ip_info_t info;
    if (sta_netif != NULL && esp_netif_get_ip_info(sta_netif, &info) == ESP_OK &&
        info.netmask.addr != 0) {
        directed = info.ip.addr | ~info.netmask.addr;
        ESP_LOGI(TAG, "WOL network IP=" IPSTR " mask=" IPSTR " directed broadcast=" IPSTR,
                 IP2STR(&info.ip), IP2STR(&info.netmask), IP2STR((ip4_addr_t *)&directed));
    } else {
        ESP_LOGW(TAG, "WOL network information unavailable; using 255.255.255.255");
    }

    int sent = wol_send_packets(fd, pkt, directed);
    if (directed != INADDR_BROADCAST) {
        sent += wol_send_packets(fd, pkt, INADDR_BROADCAST);
    }
    close(fd);
    ESP_LOGI(TAG, "WOL_RESULT=%s packets=%d target=%02X:%02X:%02X:%02X:%02X:%02X",
             sent > 0 ? "UDP_SENT" : "FAIL", sent,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return sent > 0;
}

static bool wol_send(const uint8_t mac[6])
{
    for (int attempt = 1; attempt <= 2; attempt++) {
        if (wol_send_once(mac)) {
            return true;
        }
        if (attempt < 2) {
            ESP_LOGW(TAG, "WOL retry %d/2", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    return false;
}

static const char *authmode_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    default: return "OTHER";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(WIFI_TAG, "Station ready; connection supervisor active");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(WIFI_TAG, "Wi-Fi disconnected (reason=%d); reconnect scheduled",
                 disconnected->reason);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = event_data;
        esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ps_err != ESP_OK) {
            ESP_LOGW(WIFI_TAG, "Could not disable Wi-Fi power save: %s",
                     esp_err_to_name(ps_err));
        }
        ESP_LOGI(WIFI_TAG, "Wi-Fi connected; IP=" IPSTR " GW=" IPSTR,
                 IP2STR(&got_ip->ip_info.ip), IP2STR(&got_ip->ip_info.gw));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_connect(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(WIFI_TAG, "Could not create Wi-Fi station interface");
        return;
    }
    if (wifi_event_group == NULL) {
        ESP_LOGE(WIFI_TAG, "Wi-Fi event group missing");
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    /* The application supplies the credentials from Kconfig.  Do not erase
     * NVS on every boot; retaining PHY calibration makes reconnects faster
     * and avoids destroying unrelated NVS data. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_RELAY_WIFI_SSID,
            .password = CONFIG_RELAY_WIFI_PASSWORD,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .failure_retry_cnt = 5,
            .pmf_cfg.capable = true,
            .pmf_cfg.required = false,
            .rm_enabled = 0,
            .btm_enabled = 0,
            .mbo_enabled = 0,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    const wifi_country_t country = {
        .cc = CONFIG_RELAY_WIFI_COUNTRY,
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
    /* ESP32-C3 "Super Mini" class boards ship with marginal RF layouts:
     * at the default ~20 dBm the AP often never answers 802.11 auth frames
     * (symptom: disconnect reason 2 / AUTH_EXPIRE). Lowering TX power fixes it.
     * CONFIG_RELAY_WIFI_TX_POWER is in units of 0.25 dBm: 60 = 15 dBm. */
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_RELAY_WIFI_TX_POWER));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(WIFI_TAG, "Station flow started; relay remains idle");

    /* Remain alive after the first connection; reconnect also when a connect
     * request fails without generating another Wi-Fi event. */
    for (;;) {
        if (!(xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT)) {
            esp_err_t connect_err = esp_wifi_connect();
            if (connect_err != ESP_OK && connect_err != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(WIFI_TAG, "Connection request: %s", esp_err_to_name(connect_err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void trim_command(char *s)
{
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

static bool command_is(const char *a, const char *b)
{
    return strcasecmp(a, b) == 0;
}

static bool dispatch_command(const char *input)
{
    char command[96];
    bool locked = false;
    bool ok = false;
    if (input == NULL || strlen(input) >= sizeof(command)) {
        return false;
    }
    strncpy(command, input, sizeof(command) - 1);
    command[sizeof(command) - 1] = '\0';
    trim_command(command);

    if (command_mutex != NULL) {
        if (xSemaphoreTake(command_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "Command busy; dropping duplicate request");
            return false;
        }
        locked = true;
    }

    if (command_is(command, "PULSE")) {
        relay_pulse();
        ok = true;
        goto done;
    }
    if (command_is(command, "STATUS")) {
        ESP_LOGI(TAG, "RELAY READY (idle), trigger=PULSE");
        ok = true;
        goto done;
    }
    if (strncasecmp(command, "WOL", 3) == 0 &&
        (command[3] == '\0' || isspace((unsigned char)command[3]))) {
        const char *arg = command + 3;
        while (*arg && isspace((unsigned char)*arg)) {
            arg++;
        }
        const char *macstr = *arg ? arg : CONFIG_RELAY_WOL_DEFAULT_MAC;
        uint8_t mac[6];
        if (!parse_mac(macstr, mac)) {
            ESP_LOGW(TAG, "WOL bad MAC arg: %s", macstr);
            goto done;
        }
        ok = wol_send(mac);
        goto done;
    }
    ESP_LOGW(TAG, "Unknown command: %s", command);

done:
    if (locked) {
        xSemaphoreGive(command_mutex);
    }
    return ok;
}

static void command_task(void *arg)
{
    char command[96];
    (void)arg;
    for (;;) {
        if (fgets(command, sizeof(command), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        (void)dispatch_command(command);
    }
}

/* Poll relayd over HTTPS. Base URL and token come from Kconfig
 * (sdkconfig.defaults / idf.py menuconfig) — never hardcode them here. */
#define RELAY_BASE CONFIG_RELAY_BASE_URL
#define RELAY_TOKEN CONFIG_RELAY_TOKEN

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool truncated;
} http_body_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_body_t *body = (http_body_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && body != NULL &&
        evt->data != NULL && evt->data_len > 0) {
        size_t room = body->cap - 1 - body->len;
        size_t take = evt->data_len < room ? evt->data_len : room;
        if (take > 0) {
            memcpy(body->buf + body->len, evt->data, take);
            body->len += take;
            body->buf[body->len] = '\0';
        }
        if ((size_t)evt->data_len > take) {
            body->truncated = true;
        }
    }
    return ESP_OK;
}

static bool poll_fetch_cmd(char *out, size_t outlen, uint32_t *id, int *attempt,
                           bool *has_cmd)
{
    char url[256];
    char bodybuf[512] = {0};
    http_body_t body = {
        .buf = bodybuf,
        .cap = sizeof(bodybuf),
    };
    snprintf(url, sizeof(url), "%s/poll?t=%s&wait=25&client=esp32",
             RELAY_BASE, RELAY_TOKEN);
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 35000,
        .method = HTTP_METHOD_GET,
        .buffer_size = 1024,
        .event_handler = http_event_handler,
        .user_data = &body,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        ESP_LOGW(TAG, "relay HTTP init failed");
        return false;
    }
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    bool complete = esp_http_client_is_complete_data_received(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200 || !complete) {
        ESP_LOGW(TAG, "relay poll fail: err=%s status=%d",
                 esp_err_to_name(err), status);
        return false;
    }
    if (body.truncated) {
        ESP_LOGW(TAG, "relay response too large");
        return false;
    }

    cJSON *root = cJSON_Parse(bodybuf);
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const cJSON *cmd_id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *cmd_attempt = cJSON_GetObjectItemCaseSensitive(root, "attempt");
    bool empty = cJSON_IsNull(command) && cJSON_IsNumber(cmd_id) &&
                 cmd_id->valuedouble == 0;
    bool valid = cJSON_IsString(command) && command->valuestring[0] &&
                 strlen(command->valuestring) < outlen &&
                 cJSON_IsNumber(cmd_id) && cmd_id->valuedouble >= 1 &&
                 cmd_id->valuedouble <= UINT32_MAX &&
                 cmd_id->valuedouble == (uint32_t)cmd_id->valuedouble &&
                 cJSON_IsNumber(cmd_attempt) && cmd_attempt->valueint >= 1 &&
                 cmd_attempt->valueint <= 3;
    if (valid) {
        strcpy(out, command->valuestring);
        *id = (uint32_t)cmd_id->valuedouble;
        *attempt = cmd_attempt->valueint;
        if (has_cmd != NULL) *has_cmd = true;
    }
    if (empty && has_cmd != NULL) *has_cmd = false;
    cJSON_Delete(root);
    return valid || empty;
}

static bool relay_ack(uint32_t id, int attempt, bool ok, const char *detail)
{
    char url[256];
    char payload[192];
    char response[512] = {0};
    http_body_t body = {.buf = response, .cap = sizeof(response)};
    snprintf(url, sizeof(url), "%s/ack?t=%s", RELAY_BASE, RELAY_TOKEN);
    snprintf(payload, sizeof(payload),
             "{\"id\":%lu,\"attempt\":%d,\"client\":\"esp32\",\"ok\":%s,\"detail\":\"%s\"}",
             (unsigned long)id, attempt, ok ? "true" : "false", detail);
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .method = HTTP_METHOD_POST,
        .buffer_size = 256,
        .event_handler = http_event_handler,
        .user_data = &body,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return false;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, payload, strlen(payload));
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    bool complete = esp_http_client_is_complete_data_received(c);
    esp_http_client_cleanup(c);
    cJSON *root = cJSON_Parse(response);
    bool accepted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"));
    cJSON_Delete(root);
    if (err != ESP_OK || status != 200 || !complete || body.truncated || !accepted) {
        ESP_LOGW(TAG, "relay ack fail id=%lu err=%s status=%d",
                 (unsigned long)id, esp_err_to_name(err), status);
        return false;
    }
    ESP_LOGI(TAG, "relay ack id=%lu ok=%s", (unsigned long)id, ok ? "true" : "false");
    return true;
}

static void relay_poll_task(void *arg)
{
    char cmd[96];
    uint32_t completed_ids[16] = {0};
    unsigned next_slot = 0;
    (void)arg;
    ESP_LOGI(TAG, "relay poll task ready -> %s", RELAY_BASE);
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                               WIFI_CONNECTED_BIT,
                                               pdFALSE, pdFALSE,
                                               portMAX_DELAY);
        if (!(bits & WIFI_CONNECTED_BIT)) {
            continue;
        }
        uint32_t id = 0;
        int attempt = 0;
        bool has_cmd = false;
        if (!poll_fetch_cmd(cmd, sizeof(cmd), &id, &attempt, &has_cmd)) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        if (!has_cmd) continue;
        ESP_LOGI(TAG, "relay cmd id=%lu: %s", (unsigned long)id, cmd);
        bool cached = false;
        for (unsigned i = 0; i < 16; ++i) {
            if (completed_ids[i] == id) cached = true;
        }
        bool ok = cached || dispatch_command(cmd);
        if (cached) ESP_LOGI(TAG, "Duplicate id=%lu; resending result only", (unsigned long)id);
        if (ok && !cached) completed_ids[next_slot++ % 16] = id;
        const char *detail = !ok ? "execution_failed" :
                             !strncasecmp(cmd, "WOL", 3) ? "udp_sent" : "executed";
        for (int n = 0; n < 3; ++n) {
            if (relay_ack(id, attempt, ok, detail)) break;
            if (!(xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT)) break;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void app_main(void)
{
    /* Preload inactive latch before enabling output driver. */
    ESP_ERROR_CHECK(gpio_set_level(relayPin, relayOff));
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << GPIO_NUM_3,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));

    ESP_LOGI(TAG, "GPIO3 trigger=%s ON=%d OFF=%d; idle until PULSE",
             RELAY_ACTIVE_LOW ? "LOW (L)" : "HIGH (H)", relayOn, relayOff);
    ESP_LOGI(TAG, "Firmware %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "RELAY READY (idle); type PULSE + Enter for one 400ms press");
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        ESP_LOGE(WIFI_TAG, "Could not create Wi-Fi event group");
        return;
    }
    command_mutex = xSemaphoreCreateMutex();
    if (command_mutex == NULL) {
        ESP_LOGE(TAG, "Could not create command mutex");
        return;
    }
    xTaskCreate(command_task, "relay_command", 4096, NULL, 4, NULL);
    xTaskCreate(relay_poll_task, "relay_poll", 8192, NULL, 4, NULL);
    wifi_connect();
}


