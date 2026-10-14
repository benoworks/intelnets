#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include <sys/time.h>
#include "esp_http_client.h"
#include "driver/gpio.h"
#include "ping/ping_sock.h"
#include "lwip/dns.h"

#include "secrets.h"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "SHAIN_COLLECTOR";

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif = NULL;
static volatile bool s_wifi_connected = false;
static ip_addr_t s_gateway_ip;

static inline int64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

// Ping synchronization context
typedef struct {
    SemaphoreHandle_t sem;
    uint32_t latency;
    bool success;
} ping_result_t;

static void on_ping_success(esp_ping_handle_t hdl, void *args) {
    ping_result_t *res = (ping_result_t *)args;
    uint32_t elapsed = 1000;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    res->latency = elapsed;
    res->success = true;
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    ping_result_t *res = (ping_result_t *)args;
    res->latency = 1000;
    res->success = false;
}

static void on_ping_end(esp_ping_handle_t hdl, void *args) {
    ping_result_t *res = (ping_result_t *)args;
    if (res && res->sem) {
        xSemaphoreGive(res->sem);
    }
}

// Perform a single ICMP echo probe to a target IP with 1000ms timeout
static uint32_t ping_target(const ip_addr_t *target) {
    if (!target || ip_addr_isany(target)) {
        return 1000;
    }

    ping_result_t res = {
        .sem = xSemaphoreCreateBinary(),
        .latency = 1000,
        .success = false,
    };
    if (!res.sem) {
        return 1000;
    }

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = *target;
    ping_config.count = 1;
    ping_config.interval_ms = 0;
    ping_config.timeout_ms = 1000;
    ping_config.task_stack_size = 3072;

    esp_ping_callbacks_t cbs = {
        .cb_args = &res,
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = on_ping_end,
    };

    esp_ping_handle_t ping_hdl = NULL;
    esp_err_t err = esp_ping_new_session(&ping_config, &cbs, &ping_hdl);
    if (err != ESP_OK) {
        vSemaphoreDelete(res.sem);
        return 1000;
    }

    esp_ping_start(ping_hdl);
    // Wait for ping to finish (1000ms timeout + 200ms grace window)
    if (xSemaphoreTake(res.sem, pdMS_TO_TICKS(1200)) != pdTRUE) {
        res.latency = 1000;
    }

    esp_ping_stop(ping_hdl);
    esp_ping_delete_session(ping_hdl);
    vSemaphoreDelete(res.sem);

    return (res.latency > 1000) ? 1000 : res.latency;
}

// Measure DNS resolution time (ms) for google.com
static uint32_t sample_dns_time(void) {
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int64_t start_ms = get_time_ms();
    int err = getaddrinfo("google.com", "80", &hints, &res);
    int64_t elapsed_ms = get_time_ms() - start_ms;

    if (err == 0 && res != NULL) {
        freeaddrinfo(res);
        return (elapsed_ms > 1000) ? 1000 : (uint32_t)elapsed_ms;
    }
    return 1000;
}

// Measure HTTP status code and round-trip latency (ms) for generate_204 endpoint
static void sample_http(int *status_code, uint32_t *elapsed_ms) {
    *status_code = 0;
    *elapsed_ms = 1000;

    esp_http_client_config_t config = {
        .url = "http://clients3.google.com/generate_204",
        .timeout_ms = 1000,
        .method = HTTP_METHOD_GET,
        .disable_auto_redirect = true,
        .buffer_size = 512,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return;
    }

    int64_t start_ms = get_time_ms();
    esp_err_t err = esp_http_client_perform(client);
    int64_t req_time = get_time_ms() - start_ms;

    if (err == ESP_OK) {
        *status_code = esp_http_client_get_status_code(client);
        *elapsed_ms = (req_time > 1000) ? 1000 : (uint32_t)req_time;
    } else {
        *status_code = 0;
        *elapsed_ms = 1000;
    }

    esp_http_client_cleanup(client);
}

// Wi-Fi and IP event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_gateway_ip.type = IPADDR_TYPE_V4;
        s_gateway_ip.u_addr.ip4.addr = event->ip_info.gw.addr;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Telemetry collection task
static void telemetry_task(void *pvParameters) {
    // Block until Wi-Fi connection and IP acquisition are established
    xEventGroupWaitBits(s_wifi_event_group,
                       WIFI_CONNECTED_BIT,
                       pdFALSE,
                       pdTRUE,
                       portMAX_DELAY);

    // Suppress verbose driver/subsystem logs so stdout output remains uncorrupted CSV data
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("ping_sock", ESP_LOG_NONE);
    esp_log_level_set("wifi", ESP_LOG_NONE);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_NONE);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);

    // Print clean CSV header
    printf("timestamp,rssi,local_ping,public_ping,dns_time,http_status,http_time\n");
    fflush(stdout);

    ip_addr_t public_dns_ip;
    ipaddr_aton("8.8.8.8", &public_dns_ip);

    bool led_state = false;

    while (1) {
        uint32_t timestamp = (uint32_t)get_time_ms();

#if defined(SIMULATE_DNS_OUTAGE) && SIMULATE_DNS_OUTAGE
        ip_addr_t bad_dns;
        ipaddr_aton("10.255.255.255", &bad_dns);
        dns_setserver(0, &bad_dns);
        dns_setserver(1, &bad_dns);
#endif

        // 1. Physical Layer: RSSI
        wifi_ap_record_t ap_info;
        int16_t rssi = -100; // Floor penalty for disconnected state
        if (s_wifi_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }

        // 2. Data Link / LAN: Local Gateway Ping
        uint32_t local_ping = 1000;
        if (s_wifi_connected && !ip_addr_isany(&s_gateway_ip)) {
            local_ping = ping_target(&s_gateway_ip);
        }

        // 3. Network Layer / WAN: Public Ping (8.8.8.8)
        uint32_t public_ping = 1000;
        if (s_wifi_connected) {
            public_ping = ping_target(&public_dns_ip);
        }

        // 4. Application Layer / DNS: google.com resolution latency
        uint32_t dns_time = 1000;
        if (s_wifi_connected) {
            dns_time = sample_dns_time();
        }

        // 5 & 6. Transport / Captive Portal: HTTP 204 status code and response latency
        int http_status = 0;
        uint32_t http_time = 1000;
        if (s_wifi_connected) {
            sample_http(&http_status, &http_time);
        }

        // Output clean CSV row
        printf("%lu,%d,%lu,%lu,%lu,%d,%lu\n",
               (unsigned long)timestamp,
               (int)rssi,
               (unsigned long)local_ping,
               (unsigned long)public_ping,
               (unsigned long)dns_time,
               http_status,
               (unsigned long)http_time);
        fflush(stdout);

        // Visual indicator toggle
        led_state = !led_state;
        gpio_set_level(LED_GPIO, led_state ? 1 : 0);

        // Sample interval: exactly 2 seconds
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void) {
    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Configure onboard single-color LED
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, 0);

    // 3. Create synchronization event group
    s_wifi_event_group = xEventGroupCreate();

    // 4. Initialize Netif and default Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    // 5. Initialize Wi-Fi Station
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi initialization complete. Awaiting connection...");

    // 6. Spawn telemetry task with dedicated 8KB stack
    xTaskCreate(telemetry_task, "telemetry_task", 8192, NULL, 5, NULL);
}
