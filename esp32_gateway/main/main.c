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
#include "esp_https_ota.h"
#include "esp_spiffs.h"
#include "config.h"

static int retry_cnt = 0;

bool conn_flag_on = false;
bool wifi_status = false;
bool mqtt_connected = false;
TaskHandle_t publisher_task_handle = NULL;
esp_mqtt_client_handle_t client = NULL;

char index_html[4096];
char rcv_buffer[200];

// esp_http_client event handler
esp_err_t _http_event_handler(esp_http_client_event_t *evt) {

	switch (evt->event_id) {
	case HTTP_EVENT_ERROR:
		break;
	case HTTP_EVENT_ON_CONNECTED:
		break;
	case HTTP_EVENT_HEADER_SENT:
		break;
	case HTTP_EVENT_ON_HEADER:
		break;
	case HTTP_EVENT_ON_DATA:
		if (!esp_http_client_is_chunked_response(evt->client)) {
			strncpy(rcv_buffer, (char*) evt->data, evt->data_len);
		}
		break;
	case HTTP_EVENT_ON_FINISH:
		break;
	case HTTP_EVENT_DISCONNECTED:
		break;
	}
	return ESP_OK;
}

static void initi_web_page_buffer(void) {
	esp_vfs_spiffs_conf_t conf = { .base_path = "/spiffs", .partition_label =
	NULL, .max_files = 5, .format_if_mount_failed = true };

	ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

	memset((void*) index_html, 0, sizeof(index_html));
	struct stat st;
	if (stat(INDEX_HTML_PATH, &st)) {
		ESP_LOGE(TAG, "index.html not found");
		return;
	}

	FILE *fp = fopen(INDEX_HTML_PATH, "r");
	if (fread(index_html, st.st_size, 1, fp) == 0) {
		ESP_LOGE(TAG, "fread failed");
	}

	printf("html page:\n");
	printf("%s\n", index_html);

	fclose(fp);
}

unsigned long millis() {
	return (unsigned long) (esp_timer_get_time() / 1000ULL);
}

void start_ota_update(char *ota_uri_bin) {
	esp_http_client_config_t config = { .url = ota_uri_bin, .cert_pem =
			(char*) OTA_SERVER_ROOT_CA, .skip_cert_common_name_check = false };
	esp_https_ota_config_t ota_config = { .http_config = &config, };

	esp_err_t ret = esp_https_ota(&ota_config);
	if (ret == ESP_OK) {
		printf("OTA OK, restarting...\n");
		esp_restart();
	} else {
		printf("OTA failed...\n");
	}
}

static esp_err_t wifi_event_handler(void *arg, esp_event_base_t event_base,
		int32_t event_id, void *event_data) {
	switch (event_id) {
	case WIFI_EVENT_STA_START:
		esp_wifi_connect();
		ESP_LOGI(TAG, "Trying to connect with Wi-Fi\n");
		break;

	case WIFI_EVENT_STA_CONNECTED:
		ESP_LOGI(TAG, "Wi-Fi connected\n");
		wifi_status = true;
		break;

	case IP_EVENT_STA_GOT_IP:
		ESP_LOGI(TAG, "Wi-Fi got ip\n");
		break;

	case WIFI_EVENT_STA_DISCONNECTED:
		ESP_LOGI(TAG, "disconnected: Retrying Wi-Fi\n");
		if (retry_cnt++ < MAX_RETRY) {
			esp_wifi_connect();
		} else
			ESP_LOGI(TAG, "Max Retry Failed: Wi-Fi Connection\n");
		break;

	default:
		break;
	}
	return ESP_OK;
}

void wifi_init(void) {
	printf("[wifi_init]\n");

	esp_event_loop_create_default();
	esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
			&wifi_event_handler, NULL);
	esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
			&wifi_event_handler, NULL);

	wifi_config_t wifi_config = { .sta = { .ssid = EXAMPLE_ESP_WIFI_SSID,
			.password = EXAMPLE_ESP_WIFI_PASS, .threshold.authmode =
					WIFI_AUTH_WPA2_PSK, }, };
	esp_netif_init();
	esp_netif_create_default_wifi_sta();
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_wifi_init(&cfg);
	esp_wifi_set_mode(WIFI_MODE_STA);
	esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
	esp_wifi_start();
}

char* ota_get_json() {
	ESP_LOGI(TAG, "ota_get_json\n");

	// configure the esp_http_client
	esp_http_client_config_t config = { .url = OTA_URI_JSON, .event_handler =
			_http_event_handler, };
	esp_http_client_handle_t client = esp_http_client_init(&config);

	// download json file (fw version and bin uri)
	esp_err_t err = esp_http_client_perform(client);

	if (err == ESP_OK) {
		cJSON *json = cJSON_Parse(rcv_buffer);
		if (json == NULL) {
			ESP_LOGE(TAG, "cannot parse downloaded json file. abort\n");
			return NULL;
		} else {
			cJSON *version = cJSON_GetObjectItemCaseSensitive(json, "version");
			cJSON *file = cJSON_GetObjectItemCaseSensitive(json, "file");

			// check the version
			if (!cJSON_IsNumber(version)) {
				ESP_LOGE(TAG, "cannot read version number. abort\n");
				return NULL;
			} else {
				double new_version = version->valuedouble;
				if (new_version > FIRMWARE_VERSION) {
					ESP_LOGI(TAG,
							"current firmware version (%.1f) is lower than the available one (%.1f), upgrading...\n",
							FIRMWARE_VERSION, new_version);
					if (cJSON_IsString(file) && (file->valuestring != NULL)) {
						ESP_LOGI(TAG, "firmware uri: %s\n", file->valuestring);
						return (char*) file->valuestring;
					}
				}
			}
		}
	}

	esp_http_client_cleanup(client);

	ESP_LOGE(TAG, "unable to download json file\n");
	return NULL;
}

void app_main(void) {
	esp_timer_early_init();
	initi_web_page_buffer();

	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	wifi_init();

	vTaskDelay(5000 / portTICK_PERIOD_MS);

	char* ota_uri_bin = ota_get_json();
	start_ota_update(ota_uri_bin);
}
