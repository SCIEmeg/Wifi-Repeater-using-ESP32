#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_repeater";

#define AP_IP "192.168.4.1"
#define DEFAULT_AP_SSID "ESP32_Repeater"
#define DEFAULT_AP_PASSWORD "repeater123"
#define NVS_NAMESPACE "repeater"

typedef struct {
    char sta_ssid[33];
    char sta_password[65];
    char ap_ssid[33];
    char ap_password[65];
} repeater_config_t;

static repeater_config_t config;
static esp_netif_t *ap_netif;
static esp_netif_t *sta_netif;
static bool station_has_ip;

static void copy_value(char *destination, size_t destination_size, const char *source)
{
    snprintf(destination, destination_size, "%s", source ? source : "");
}

static void load_config(void)
{
    copy_value(config.ap_ssid, sizeof(config.ap_ssid), DEFAULT_AP_SSID);
    copy_value(config.ap_password, sizeof(config.ap_password), DEFAULT_AP_PASSWORD);
    config.sta_ssid[0] = '\0';
    config.sta_password[0] = '\0';

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t size;
#define READ_SETTING(key, member) do { \
    size = sizeof(config.member); \
    nvs_get_str(handle, key, config.member, &size); \
} while (0)
    READ_SETTING("sta_ssid", sta_ssid);
    READ_SETTING("sta_pass", sta_password);
    READ_SETTING("ap_ssid", ap_ssid);
    READ_SETTING("ap_pass", ap_password);
#undef READ_SETTING
    nvs_close(handle);
}

static esp_err_t save_config(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open NVS");
    ESP_RETURN_ON_ERROR(nvs_set_str(handle, "sta_ssid", config.sta_ssid), TAG, "save STA SSID");
    ESP_RETURN_ON_ERROR(nvs_set_str(handle, "sta_pass", config.sta_password), TAG, "save STA password");
    ESP_RETURN_ON_ERROR(nvs_set_str(handle, "ap_ssid", config.ap_ssid), TAG, "save AP SSID");
    ESP_RETURN_ON_ERROR(nvs_set_str(handle, "ap_pass", config.ap_password), TAG, "save AP password");
    esp_err_t result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

static bool valid_password(const char *password)
{
    size_t length = strlen(password);
    return length == 0 || (length >= 8 && length <= 63);
}

static void html_escape(char *destination, size_t destination_size, const char *source)
{
    size_t written = 0;
    for (; *source && written + 6 < destination_size; ++source) {
        const char *replacement = NULL;
        switch (*source) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        default: break;
        }
        if (replacement) {
            written += snprintf(destination + written, destination_size - written, "%s", replacement);
        } else {
            destination[written++] = *source;
        }
    }
    destination[written] = '\0';
}

static void update_ap_dns_from_uplink(void)
{
    esp_netif_dns_info_t upstream_dns = {0};
    esp_err_t err = esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &upstream_dns);

    if (err != ESP_OK || upstream_dns.ip.type != IPADDR_TYPE_V4 || upstream_dns.ip.u_addr.ip4.addr == 0) {
        memset(&upstream_dns, 0, sizeof(upstream_dns));
        err = esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_BACKUP, &upstream_dns);
    }
    if (err != ESP_OK || upstream_dns.ip.type != IPADDR_TYPE_V4 || upstream_dns.ip.u_addr.ip4.addr == 0) {
        ESP_LOGW(TAG, "Uplink has no IPv4 DNS server; AP DHCP DNS was not updated");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &upstream_dns));

    // ESP-IDF 6.x requires DNS to be explicitly enabled in DHCP offers.
    uint8_t dns_offer = 1;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &dns_offer, sizeof(dns_offer)));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
    ESP_LOGI(TAG, "AP DHCP DNS updated from uplink");
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && config.sta_ssid[0]) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        station_has_ip = false;
        if (config.sta_ssid[0]) {
            ESP_LOGW(TAG, "Uplink disconnected; retrying");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        station_has_ip = true;
        update_ap_dns_from_uplink();
        ESP_ERROR_CHECK(esp_netif_napt_enable(ap_netif));
        ESP_LOGI(TAG, "Uplink connected; NAPT enabled");
    }
}

static const char *page_head =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 Wi-Fi Repeater</title><style>body{font-family:Arial,sans-serif;max-width:620px;margin:24px auto;padding:0 16px}"
    "input{box-sizing:border-box;width:100%%;padding:10px;margin:5px 0 16px}button{padding:11px 16px;background:#0678d7;color:#fff;border:0;border-radius:5px}"
    ".status{padding:12px;background:#eef6ff;border-radius:5px}</style></head><body>"
    "<h1>ESP32 Wi-Fi Repeater</h1><div class='status'><b>Uplink:</b> %s<br><b>AP address:</b> " AP_IP "</div>"
    "<form action='/save' method='get'><h2>Upstream Wi-Fi</h2><label>SSID</label><input name='ssid' maxlength='32' value='%s' required>"
    "<label>Password</label><input name='pass' type='password' maxlength='63' placeholder='Leave blank for open Wi-Fi'>"
    "<h2>ESP32 access point</h2><label>SSID</label><input name='apssid' maxlength='32' value='%s' required>"
    "<label>Password</label><input name='appass' type='password' maxlength='63' placeholder='Leave blank for open AP'>"
    "<button type='submit'>Save and restart</button></form></body></html>";

static esp_err_t root_get_handler(httpd_req_t *request)
{
    char safe_sta[193], safe_ap[193], response[1800];
    html_escape(safe_sta, sizeof(safe_sta), config.sta_ssid);
    html_escape(safe_ap, sizeof(safe_ap), config.ap_ssid);
    int length = snprintf(response, sizeof(response), page_head, station_has_ip ? "connected" : "not connected", safe_sta, safe_ap);
    if (length < 0 || length >= sizeof(response)) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Page rendering failed");
    }
    httpd_resp_set_type(request, "text/html");
    return httpd_resp_send(request, response, length);
}

static esp_err_t save_get_handler(httpd_req_t *request)
{
    char query[512];
    if (httpd_req_get_url_query_len(request) >= sizeof(query) ||
        httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "ssid", config.sta_ssid, sizeof(config.sta_ssid)) != ESP_OK ||
        httpd_query_key_value(query, "apssid", config.ap_ssid, sizeof(config.ap_ssid)) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Both SSIDs are required");
    }
    httpd_query_key_value(query, "pass", config.sta_password, sizeof(config.sta_password));
    httpd_query_key_value(query, "appass", config.ap_password, sizeof(config.ap_password));
    if (!valid_password(config.sta_password) || !valid_password(config.ap_password)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Passwords must be blank or 8-63 characters");
    }
    esp_err_t result = save_config();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not save config: %s", esp_err_to_name(result));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save configuration");
    }
    httpd_resp_set_type(request, "text/html");
    httpd_resp_sendstr(request, "<!doctype html><title>Saved</title><h1>Saved</h1><p>The ESP32 is restarting.</p>");
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}

static void start_web_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &server_config));
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_GET, .handler = save_get_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save));
}

static void initialise_wifi(void)
{
    ap_netif = esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t ap = {0};
    copy_value((char *)ap.ap.ssid, sizeof(ap.ap.ssid), config.ap_ssid);
    copy_value((char *)ap.ap.password, sizeof(ap.ap.password), config.ap_password);
    ap.ap.ssid_len = strlen(config.ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = config.ap_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    wifi_config_t sta = {0};
    copy_value((char *)sta.sta.ssid, sizeof(sta.sta.ssid), config.sta_ssid);
    copy_value((char *)sta.sta.password, sizeof(sta.sta.password), config.sta_password);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    load_config();
    initialise_wifi();
    start_web_server();
    ESP_LOGI(TAG, "AP ready: %s at " AP_IP, config.ap_ssid);
}
