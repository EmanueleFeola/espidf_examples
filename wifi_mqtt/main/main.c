#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "esp_timer.h"
#include "driver/gpio.h"
#include "rom/gpio.h"
#include "esp_sleep.h"

static const char *TAG = "MAIN";
// #define EXAMPLE_ESP_WIFI_SSID "Pixel_8801"
// #define EXAMPLE_ESP_WIFI_PASS "franzogna"
#define EXAMPLE_ESP_WIFI_SSID "CasaFeola-Eolo"
#define EXAMPLE_ESP_WIFI_PASS "straccidinebbialenti"
#define MAX_RETRY 10
static int retry_cnt = 0;
#define OLIMEX_BUT_PIN 34

bool conn_flag_on = false;
bool wifi_status = false;
bool mqtt_connected = false;
TaskHandle_t publisher_task_handle = NULL;
esp_mqtt_client_handle_t client = NULL;

static void mqtt_app_start(void);

unsigned long millis()
{
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t wifi_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        ESP_LOGI(TAG, "Trying to connect with Wi-Fi\n");
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "Wi-Fi connected\n");
        wifi_status = true;
        break;

    case IP_EVENT_STA_GOT_IP:
        ESP_LOGI(TAG, "got ip: startibg MQTT Client\n");
        mqtt_app_start();
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG, "disconnected: Retrying Wi-Fi\n");
        if (retry_cnt++ < MAX_RETRY)
        {
            esp_wifi_connect();
        }
        else
            ESP_LOGI(TAG, "Max Retry Failed: Wi-Fi Connection\n");
        break;

    default:
        break;
    }
    return ESP_OK;
}

void wifi_init(void)
{
    printf("[wifi_init]\n");

    esp_event_loop_create_default();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_netif_init();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    // ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        mqtt_connected = true;

        msg_id = esp_mqtt_client_subscribe(client, "/emanuele_topic/#", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        mqtt_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    ESP_LOGI(TAG, "STARTING MQTT");

    const esp_mqtt_client_config_t mqttConfig = {
        .broker = {
            .address.uri = "mqtt://broker.hivemq.com:1883",
        },
    };

    client = esp_mqtt_client_init(&mqttConfig);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

void wifi_stop(void)
{
    printf("[wifi_stop]\n");

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    esp_netif_deinit();

    esp_mqtt_client_stop(client);

    wifi_status = false;
}

void publisher_task(void *params)
{
    while (true)
    {
        if (!wifi_status && conn_flag_on)
        {
            wifi_init();
        }
        else if (wifi_status && !conn_flag_on)
        {
            wifi_stop();

            // Go to sleep now
            printf("Going to sleep now\n");
            esp_deep_sleep_start();
            printf("This will never be printed\n");
        }
        else if (wifi_status && mqtt_connected)
        {
            char pub_str[100];
            int max_len = sizeof(pub_str);
            snprintf(pub_str, max_len, "hello world banana %d", (int)millis());
            printf("sending %s\n", pub_str);

            esp_mqtt_client_publish(client, "/emanuele_topic/test3/", pub_str, 0, 0, 0);
        }
        else
        {
            printf("doing nothing %d %d %d %d\n", wifi_status, conn_flag_on, mqtt_connected, (int)millis());
        }

        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    conn_flag_on = !conn_flag_on;
}

void print_wakeup_reason()
{
    esp_sleep_wakeup_cause_t wakeup_reason;

    wakeup_reason = esp_sleep_get_wakeup_cause();

    switch (wakeup_reason)
    {
    case ESP_SLEEP_WAKEUP_EXT0:
        printf("Wakeup caused by external signal using RTC_IO\n");
        break;
    case ESP_SLEEP_WAKEUP_EXT1:
        printf("Wakeup caused by external signal using RTC_CNTL\n");
        break;
    case ESP_SLEEP_WAKEUP_TIMER:
        printf("Wakeup caused by timer\n");
        break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
        printf("Wakeup caused by touchpad\n");
        break;
    case ESP_SLEEP_WAKEUP_ULP:
        printf("Wakeup caused by ULP program\n");
        break;
    default:
        printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason);
        break;
    }
}

void app_main(void)
{
    esp_timer_early_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Print the wakeup reason for ESP32
    print_wakeup_reason();
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_34, 0); // 1 = High, 0 = Low

    // attach isr on button press
    gpio_pad_select_gpio(OLIMEX_BUT_PIN);
    gpio_set_direction(OLIMEX_BUT_PIN, GPIO_MODE_INPUT);
    gpio_set_intr_type(OLIMEX_BUT_PIN, GPIO_INTR_POSEDGE);
    gpio_install_isr_service(ESP_INTR_FLAG_LOWMED);
    gpio_isr_handler_add(OLIMEX_BUT_PIN, gpio_isr_handler, 0);

    xTaskCreate(publisher_task, "publisher_task", 1024 * 5, NULL, 5, &publisher_task_handle);
}