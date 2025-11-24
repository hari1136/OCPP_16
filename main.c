#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_spiffs.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "ocpp.h"
#include <stdint.h>

#define MAX_URL_LEN 128
#define MAX_ENDPOINT_LEN 64
#define MAX_NAME_LEN 64
#define MAX_SSID_LENT 64
#define MAX_PS_LEN 64
#define ENC_BUF_SIZE 192

#define DEFAULT_SSID "Tucker"
#define DEFAULT_PASSWORD "12345678"
#define MIN_PASSWORD_LENGTH 8

#define TAG "Main"

#define MAX_RETRIES 3
static int retry_count = 0;

#define WDT_TIMEOUT_S 5 //

static esp_timer_handle_t timeout_timer;

// connector_state_t connector1_state;
static void retry_config_fetch_if_needed();

char device_info_url[128] = "ws://15.207.37.132:9081/tuckermotors";
char device_info_endpoint[64] = "ChargePoint690";
char device_info_name[64] = "FTCT000690";
char device_info_ssid[64] = "Tucker Jio 4G";
char device_info_ps[64] = "Tucker#123";
char mac_str[18];
int device_info_net = 1;
float def_values_uv, def_values_ov, def_values_oc, def_values_max_temp;
// int def_values_Phase;
float def_values_max_current;
uint16_t power;
bool profile_expired;
void start_websocket_task(void *pvParameters);

char charger1_last_status[64] = {0};
char charger_last_status[64] = {0};
int gun1_status_value;
int gun2_status_value;
char charger1_status[64] = {0};
char charger2_status[64] = {0};

bool send_D_flag[NUM_CONNECTORS];
bool send_F_flag[NUM_CONNECTORS];
bool send_I_flag[NUM_CONNECTORS];
bool send_I2_flag[NUM_CONNECTORS];
bool send_M_flag[NUM_CONNECTORS];

typedef struct {
  const char *data_ptr;
  size_t data_len;
} websocket_event_data_t;

bool url_encode_safe(const char *src, char *dst, size_t dst_size) {
  if (!src || !dst || dst_size == 0)
    return false;

  size_t len = strlen(src);
  size_t pos = 0;
  for (size_t i = 0; i < len && pos < dst_size - 1; i++) {
    unsigned char c = (unsigned char)src[i];
    if (isalnum(c)) {
      dst[pos++] = c;
    } else {
      if (pos + 3 >= dst_size)
        break;
      snprintf(&dst[pos], 4, "%%%02X", c);
      pos += 3;
    }
  }
  dst[pos] = '\0';
  return true;
}

void mount_spiffs(void) {
  esp_vfs_spiffs_conf_t conf = {.base_path = "/spiffs",
                                .partition_label = "storage",
                                .max_files = 5,
                                .format_if_mount_failed = true};
  esp_err_t ret = esp_vfs_spiffs_register(&conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount or format SPIFFS (%s)",
             esp_err_to_name(ret));
  } else {
    size_t total = 0, used = 0;
    esp_spiffs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "SPIFFS total: %d, used: %d", total, used);
  }
}

void send_update_request(bool should_restart) {
  char enc_url[ENC_BUF_SIZE];
  char enc_id[ENC_BUF_SIZE];
  char enc_qr[ENC_BUF_SIZE];
  char enc_info[ENC_BUF_SIZE];
  char enc_ps[ENC_BUF_SIZE];
  char enc_ssid[ENC_BUF_SIZE];

  if (!url_encode_safe(device_info_url, enc_url, ENC_BUF_SIZE) ||
      !url_encode_safe(device_info_endpoint, enc_id, ENC_BUF_SIZE) ||
      !url_encode_safe(device_info_name, enc_qr, ENC_BUF_SIZE) ||
      !url_encode_safe(device_info_net == 0 || device_info_net == 3
                           ? "SIM info"
                           : device_info_ssid,
                       enc_info, ENC_BUF_SIZE) ||
      !url_encode_safe(device_info_ps, enc_ps, ENC_BUF_SIZE) ||
      !url_encode_safe(device_info_ssid, enc_ssid, ENC_BUF_SIZE)) {
    ESP_LOGE(TAG, "URL encoding failed");
    return;
  }

  // Construct URL (ensure full_url buffer size adequate)
  char full_url[2048];
  snprintf(full_url, sizeof(full_url),
           "http://star.tuckermotors.com/ftdtocpp/"
           "ftdt_update.php?mac=%s&url=%s&id=%s&qr=%s&info=%s&ver=1.1.0&ps=%s&"
           "ssid=%s&net=%d",
           mac_str, enc_url, enc_id, enc_qr, enc_info, enc_ps, enc_ssid,
           device_info_net);

  ESP_LOGI(TAG, "[UPDATE] %s", full_url);

  esp_http_client_config_t config = {
      .url = full_url,
      .method = HTTP_METHOD_GET,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_perform(client);

  if (err == ESP_OK) {
    int status = esp_http_client_get_status_code(client);
    if (status == 200) {
      char *response_buf = malloc(512);
      if (response_buf) {
        int len = esp_http_client_read_response(client, response_buf, 512);
        response_buf[len] = 0;
        ESP_LOGI(TAG, "[RESPONSE] %s", response_buf);
        free(response_buf);
      }

      if (should_restart) {
        ESP_LOGI(TAG, "Restarting device...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "Calling esp_restart() now...");
        esp_restart();
      }
    } else {
      ESP_LOGE(TAG, "Update failed: HTTP %d", status);
    }
  } else {
    ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
  }

  esp_http_client_cleanup(client);
  xTaskCreate(&start_websocket_task, "websocket_task", 8192, NULL, 5, NULL);
  send_B_flag = true;
}

void saveConfigToSPIFFS() {
  // Create a new cJSON root object
  cJSON *root = cJSON_CreateObject();

  if (root == NULL) {
    ESP_LOGE("CONFIG", "Failed to create cJSON root object");
    return;
  }

  // Add string values
  cJSON_AddStringToObject(root, "ssid", device_info_ssid);
  cJSON_AddStringToObject(root, "ps", device_info_ps);
  cJSON_AddStringToObject(root, "url", device_info_url);
  cJSON_AddStringToObject(root, "endpoint", device_info_endpoint);
  cJSON_AddStringToObject(root, "name", device_info_name);

  // Add numeric values
  cJSON_AddNumberToObject(root, "net", device_info_net);
  cJSON_AddNumberToObject(root, "uv", def_values_uv);
  cJSON_AddNumberToObject(root, "ov", def_values_ov);
  cJSON_AddNumberToObject(root, "oc", def_values_oc);
  cJSON_AddNumberToObject(root, "max_temp", def_values_max_temp);
  //    cJSON_AddNumberToObject(root, "phase", def_values_Phase);
  cJSON_AddNumberToObject(root, "max_current", def_values_max_current);

  // Convert JSON object to string
  char *json_str = cJSON_PrintUnformatted(root);
  if (json_str == NULL) {
    ESP_LOGE("CONFIG", "Failed to print JSON to string");
    cJSON_Delete(root);
    return;
  }

  // Write to SPIFFS
  FILE *file = fopen("/spiffs/config.json", "w");
  if (file == NULL) {
    ESP_LOGE("CONFIG", "Failed to open file for writing");
    cJSON_free(json_str);
    cJSON_Delete(root);
    return;
  }

  fwrite(json_str, 1, strlen(json_str), file);
  fclose(file);

  ESP_LOGI("CONFIG", "Config saved to SPIFFS");

  // Clean up
  cJSON_free(json_str);
  cJSON_Delete(root);
  send_update_request(true);
}

bool load_config_from_spiffs() {
  FILE *file = fopen("/spiffs/config.json", "r");
  if (!file) {
    ESP_LOGE(TAG, "Failed to open config.json");
    return false;
  }

  fseek(file, 0, SEEK_END);
  long length = ftell(file);
  rewind(file);

  char *json_data = malloc(length + 1);
  if (!json_data) {
    fclose(file);
    ESP_LOGE(TAG, "Failed to allocate memory for config.json");
    return false;
  }

  fread(json_data, 1, length, file);
  json_data[length] = '\0';
  fclose(file);

  cJSON *root = cJSON_Parse(json_data);
  free(json_data);

  if (!root) {
    ESP_LOGE(TAG, "Failed to parse config.json");
    return false;
  }

  cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
  ESP_LOGI(TAG, "SSID: %s", ssid->valuestring);
  if (cJSON_IsString(ssid) && (ssid->valuestring != NULL)) {
    safe_strncpy(device_info_ssid, ssid->valuestring, MAX_SSID_LEN);
  } else {
    ESP_LOGW(TAG,
             "SSID field missing or invalid in config JSON, using default");
    safe_strncpy(device_info_ssid, DEFAULT_SSID, MAX_SSID_LEN);
  }

  cJSON *password = cJSON_GetObjectItem(root, "ps");
  ESP_LOGI(TAG, "ps: %s", password->valuestring);
  if (cJSON_IsString(password) && (password->valuestring != NULL)) {
    safe_strncpy(device_info_ps, password->valuestring, MAX_PS_LEN);
  } else {
    ESP_LOGW(TAG,
             "Password field missing or invalid in config JSON, using default");
    safe_strncpy(device_info_ps, DEFAULT_PASSWORD, MAX_PS_LEN);
  }
  strcpy(device_info_url, cJSON_GetObjectItem(root, "url")->valuestring);
  strcpy(device_info_endpoint,
         cJSON_GetObjectItem(root, "endpoint")->valuestring);
  strcpy(device_info_name, cJSON_GetObjectItem(root, "name")->valuestring);
  device_info_net = cJSON_GetObjectItem(root, "net")->valueint;

  def_values_uv = cJSON_GetObjectItem(root, "uv")->valuedouble;
  def_values_ov = cJSON_GetObjectItem(root, "ov")->valuedouble;
  def_values_oc = cJSON_GetObjectItem(root, "oc")->valuedouble;
  def_values_max_temp = cJSON_GetObjectItem(root, "max_temp")->valuedouble;
  //    def_values_Phase = cJSON_GetObjectItem(root, "phase")->valueint;
  def_values_max_current =
      cJSON_GetObjectItem(root, "max_current")->valuedouble;

  cJSON_Delete(root);
  ESP_LOGI(TAG, "Config loaded from SPIFFS");

  //  ESP_LOGI(TAG, "Loaded Configuration:");
  //  ESP_LOGI(TAG, "SSID: %s", device_info_ssid);
  //  ESP_LOGI(TAG, "Password: %s", device_info_ps);
  //  ESP_LOGI(TAG, "URL: %s", device_info_url);
  //  ESP_LOGI(TAG, "Endpoint: %s", device_info_endpoint);
  //  ESP_LOGI(TAG, "Device Name: %s", device_info_name);
  //  ESP_LOGI(TAG, "Network Type: %d", device_info_net);
  //  ESP_LOGI(TAG, "UV: %.2f", def_values_uv);
  //  ESP_LOGI(TAG, "OV: %.2f", def_values_ov);
  //  ESP_LOGI(TAG, "OC: %.2f", def_values_oc);
  //  ESP_LOGI(TAG, "Max Temp: %.2f", def_values_max_temp);
  //  ESP_LOGI(TAG, "Max Current: %.2f", def_values_max_current);

  return true;
}

// ========== JSON PARSING FUNCTION ==========

static void parse_json_response(const char *json_str) {
  if (json_str == NULL || json_str[0] != '{') {
    ESP_LOGE(TAG, "Invalid JSON response: %s", json_str);
    retry_config_fetch_if_needed();
    return;
  }
  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    ESP_LOGE(TAG, "Failed to parse JSON");
    retry_config_fetch_if_needed();
    return;
  }

  cJSON *status = cJSON_GetObjectItem(root, "status");
  if (cJSON_IsString(status) && strcmp(status->valuestring, "true") == 0) {
    cJSON *data = cJSON_GetObjectItem(root, "Data");
    if (data) {
      cJSON *url = cJSON_GetObjectItem(data, "url");
      if (cJSON_IsString(url)) {
        safe_strncpy(device_info_url, url->valuestring, MAX_URL_LEN);
      }

      cJSON *id = cJSON_GetObjectItem(data, "id");
      if (cJSON_IsString(id)) {
        safe_strncpy(device_info_endpoint, id->valuestring, MAX_ENDPOINT_LEN);
      }

      cJSON *qr = cJSON_GetObjectItem(data, "qr");
      if (cJSON_IsString(qr)) {
        safe_strncpy(device_info_name, qr->valuestring, MAX_NAME_LEN);
      }

      cJSON *ps = cJSON_GetObjectItem(data, "pass");

      if (cJSON_IsString(ps) && (ps->valuestring != NULL) &&
          strlen(ps->valuestring) >= MIN_PASSWORD_LENGTH) {
        ESP_LOGI(TAG, "password: %s", ps->valuestring);
        safe_strncpy(device_info_ps, ps->valuestring, MAX_PS_LEN);
      } else {
        ESP_LOGW(TAG, "Password field missing, invalid, or too short in config "
                      "JSON, using default");
        safe_strncpy(device_info_ps, DEFAULT_PASSWORD, MAX_PS_LEN);
      }
      cJSON *ssid = cJSON_GetObjectItem(data, "ssid");
      ESP_LOGI(TAG, "SSID: %s", ssid->valuestring);
      if (cJSON_IsString(ssid) && (ssid->valuestring != NULL)) {
        safe_strncpy(device_info_ssid, ssid->valuestring, MAX_SSID_LEN);
      } else {
        ESP_LOGW(TAG,
                 "SSID field missing or invalid in config JSON, using default");
        safe_strncpy(device_info_ssid, DEFAULT_SSID, MAX_SSID_LEN);
      }

      cJSON *net = cJSON_GetObjectItem(data, "net");
      if (cJSON_IsString(net)) {
        // Assuming net is a string number, convert to int if needed
        int net_val = atoi(net->valuestring);
        ESP_LOGI(TAG, "NET: %d", net_val);
        // store net_val to your net variable if you have one
      }

      ESP_LOGI(TAG, "URL: %s", device_info_url);
      ESP_LOGI(TAG, "ID: %s", device_info_endpoint);
      ESP_LOGI(TAG, "QR: %s", device_info_name);
      ESP_LOGI(TAG, "PS: %s", device_info_ps);
      ESP_LOGI(TAG, "SSID: %s", device_info_ssid);

      // Optionally parse inner object "1"
      cJSON *inner = cJSON_GetObjectItem(data, "1");
      if (inner) {
        for (int i = 1; i <= 6; i++) {
          char key[2];
          snprintf(key, sizeof(key), "%d", i);
          cJSON *val = cJSON_GetObjectItem(inner, key);
          if (val && cJSON_IsString(val)) {
            ESP_LOGI(TAG, "Param %s: %s", key, val->valuestring);
          }
        }
      }
    }
    saveConfigToSPIFFS();

  } else {
    ESP_LOGW(TAG, "Status is not true or missing.");
    send_update_request(false);
  }
  retry_count = 0;
  cJSON_Delete(root);
}
// ========== HTTP EVENT HANDLER ==========

static char *output_buffer = NULL; // Buffer to store response
static int output_len = 0;

static void cleanup_http_response_buffer() {
  if (output_buffer) {
    free(output_buffer);
    output_buffer = NULL;
    output_len = 0;
  }
}

#define MAX_RESPONSE_SIZE 1024 // 1 KB max buffer size

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
  case HTTP_EVENT_ON_DATA:
    if (evt->data_len > 0) {
      if (output_len + evt->data_len > MAX_RESPONSE_SIZE) {
        ESP_LOGE(TAG, "Response too large, aborting reception");
        cleanup_http_response_buffer(); // Free buffer before aborting
        return ESP_FAIL;                // Abort HTTP client
      }
      char *new_buffer = realloc(output_buffer, output_len + evt->data_len + 1);
      if (new_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for response buffer");
        cleanup_http_response_buffer();
        return ESP_FAIL;
      }
      output_buffer = new_buffer;
      memcpy(output_buffer + output_len, evt->data, evt->data_len);
      output_len += evt->data_len;
      output_buffer[output_len] = '\0'; // Null-terminate
    }
    break;

  case HTTP_EVENT_ON_FINISH:
    if (output_buffer) {
      ESP_LOGI(TAG, "HTTP Response:\n%s", output_buffer);
      parse_json_response(output_buffer);
      cleanup_http_response_buffer();
    }
    break;

  case HTTP_EVENT_DISCONNECTED:
    cleanup_http_response_buffer();
    break;

  default:
    break;
  }

  return ESP_OK;
}

// ========== HTTP GET TASK ==========
void http_get_task(void *pvParameters) {
  // Step 1: Get MAC address
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  // 12 hex chars + null terminator
  snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);

  // Step 2: Construct full URL
  char full_url[256];
  snprintf(full_url, sizeof(full_url),
           "http://star.tuckermotors.com/ftdtocpp/ftdt_get.php?mac=%s",
           mac_str);

  // Step 3: Create config with full_url
  esp_http_client_config_t config = {
      .url = full_url,
      .event_handler = _http_event_handler,
  };

  // Step 4: Perform HTTP GET
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_perform(client);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %" PRId64,
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));
  } else {
    ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
  }

  // Step 5: Cleanup
  esp_http_client_cleanup(client);
  vTaskDelete(NULL);
}

static void retry_config_fetch_if_needed() {
  if (retry_count < MAX_RETRIES) {
    retry_count++;
    ESP_LOGW(TAG, "Retrying config fetch... Attempt %d/%d", retry_count,
             MAX_RETRIES);
    vTaskDelay(pdMS_TO_TICKS(2000));
    xTaskCreate(http_get_task, "http_get_task", 8192, NULL, 5, NULL);
  } else {
    ESP_LOGE(TAG, "Max retries reached. Aborting config fetch.");
    retry_count = 0;            // Reset retry counter
    send_update_request(false); // Optional fallback after max retries
  }
}

void websocket_reconnect_task(void *arg) {
  int retry_attempts = 0;
  const int max_retry_attempts = 6;
  int max_delay_ms = 1000; // Max 60 seconds between retries
  max_delay_ms *= websocket_ping;
  while (true) {
    // Check connection status
    if (!esp_websocket_client_is_connected(client)) {
      // Exponential backoff delay calculation
      int delay = (1 << retry_attempts) * 1000; // 1s, 2s, 4s, ...
      if (delay > max_delay_ms) {
        delay = max_delay_ms;
      }

      // Add jitter +/- 20% of delay
      int jitter_range = delay / 5;
      int jitter =
          esp_random() % (jitter_range * 2 + 1) - jitter_range; // -20% to +20%
      delay += jitter;

      ESP_LOGW(TAG,
               "WebSocket disconnected. Reconnecting in %d ms (attempt %d)...",
               delay, retry_attempts);

      if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        // Only stop if connected
        if (esp_websocket_client_is_connected(client)) {
          esp_websocket_client_stop(client);
          vTaskDelay(pdMS_TO_TICKS(1000)); // Give some time for stop to settle
        }

        // Only start if we have retry attempts left and still not connected
        if (retry_attempts < max_retry_attempts &&
            !esp_websocket_client_is_connected(client)) {
          esp_websocket_client_start(client);
        }

        xSemaphoreGive(client_mutex);
      } else {
        ESP_LOGW(TAG, "Failed to take mutex for WebSocket reconnect");
      }

      retry_attempts++;
      vTaskDelay(pdMS_TO_TICKS(delay));
    } else {
      // Connected - reset retry attempts and check again later
      retry_attempts = 0;
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data) {
  switch (event_id) {
  case WEBSOCKET_EVENT_CONNECTED:
    ESP_LOGI(TAG, "WebSocket connected");
    set_index_value(3, 2);
    build_frame_json("A", a_frame_values, ARRAY_SIZE(a_frame_values));
    xTaskCreate(websocket_reconnect_task, "websocket_reconnect_task", 4096,
                NULL, 5, NULL);
    xTaskCreate(send_bootnotification_task, "send_boot_notification", 4096,
                NULL, 3, NULL);

    break;

  case WEBSOCKET_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "WebSocket disconnected");
    break;

  case WEBSOCKET_EVENT_DATA: {
    websocket_event_data_t *data = (websocket_event_data_t *)event_data;

    // Allocate memory dynamically, add 1 byte for null-terminator
    char *message = (char *)malloc(data->data_len + 1);
    if (!message) {
      ESP_LOGE(TAG, "Failed to allocate memory for WebSocket message");
      break;
    }

    memcpy(message, data->data_ptr, data->data_len);
    message[data->data_len] = '\0'; // Null-terminate

    ESP_LOGI(TAG, "Received WebSocket data: %s", message);

    handle_ocpp_websocket_message(message);

    // Free the allocated memory after use
    free(message);
    break;
  }

  case WEBSOCKET_EVENT_ERROR:
    ESP_LOGE(TAG, "WebSocket error");
    break;
  }
}

void start_websocket_task(void *pvParameters) {
  char websocket_url[256];
  snprintf(websocket_url, sizeof(websocket_url), "%s/%s", device_info_url,
           device_info_endpoint);

  esp_websocket_client_config_t websocket_cfg = {
      .uri = websocket_url,
      .subprotocol = "ocpp1.6",
      .headers = "Connection: Upgrade\r\n"
                 "Upgrade: websocket\r\n",
      .buffer_size = 2048, // Increase if large messages
  };

  client = esp_websocket_client_init(&websocket_cfg);
  esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                websocket_event_handler, (void *)client);
  esp_websocket_client_start(client);

  vTaskDelay(3000 / portTICK_PERIOD_MS);

  vTaskDelete(NULL);
}

// ========== WIFI EVENT HANDLER ==========
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *event =
        (wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGW(TAG, "Disconnected from Wi-Fi, reason: %d", event->reason);

    // Auto reconnect
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ESP_LOGI(TAG, "Got IP. Starting HTTP task...");
    set_index_value(3, 1); // Changes A[2] to 9
    build_frame_json("A", a_frame_values, ARRAY_SIZE(a_frame_values));
    xTaskCreate(&http_get_task, "http_get_task", 8192, NULL, 5, NULL);
  }
}

// ========== WIFI INIT ==========
void wifi_init_sta(void) {
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                      &wifi_event_handler, NULL, NULL);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                      &wifi_event_handler, NULL, NULL);

  wifi_config_t wifi_config = {0}; // Zero-initialize

  // Copy dynamic SSID and password into config
  safe_strncpy((char *)wifi_config.sta.ssid, device_info_ssid,
               sizeof(wifi_config.sta.ssid));
  ESP_LOGI(TAG, "Current Wi-Fi password: %s", device_info_ps);

  safe_strncpy((char *)wifi_config.sta.password, device_info_ps,
               sizeof(wifi_config.sta.password));
  wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
  esp_wifi_start();

  ESP_LOGI(TAG, "Wi-Fi init finished. Connecting to %s", device_info_ssid);
}

void get_meter_values(int i) {
  if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    char payload[32];
    snprintf(payload, sizeof(payload), "{\"M\":[%d]}", i + 1);
    if (i2c_send(payload) == ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(500));
      if (i2c_receive(response, MAX_RESPONSE_LEN) == ESP_OK) {
        parse_json_frame(response);
        //        printf("Received M: %s\n", response);
      }
    } else {
      //      printf("Failed to send frame M\n");
    }
    xSemaphoreGive(i2c_mutex);
  } else {
    ESP_LOGW(I2CTAG, "Could not acquire I2C mutex for meter values");
  }
}
// void get_meter_values(void) {
//     if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
//         if (i2c_send(frame_M1) == ESP_OK) {
//             vTaskDelay(pdMS_TO_TICKS(500));
//             if (i2c_receive(response, MAX_RESPONSE_LEN) == ESP_OK) {
//                 parse_json_frame(response); // Assume this fills some temp
//                 array, e.g., tempMeterValues
//
//                 // Copy values into each connector's MeterValues
//                 for (int i = 0; i < NUM_CONNECTORS; i++) {
//                     for (int j = 0; j < 14; j++) {
//                         // Use safe copy to avoid buffer overflow
//                         safe_strncpy(connectors[i].MeterValues[j],
//                         guns[i].MeterValues[j], 15);
//                     }
//                 }
//
//                 // Optional: log for debugging
//                 // for (int i = 0; i < NUM_CONNECTORS; i++) {
//                 //     ESP_LOGI(I2CTAG, "Connector %d Meter[0]: %s", i + 1,
//                 connectors[i].MeterValues[0]);
//                 // }
//             } else {
//                 ESP_LOGW(I2CTAG, "Failed to receive meter values from I2C");
//             }
//         } else {
//             ESP_LOGW(I2CTAG, "Failed to send meter frame M");
//         }
//         xSemaphoreGive(i2c_mutex);
//     } else {
//         ESP_LOGW(I2CTAG, "Could not acquire I2C mutex for meter values");
//     }
// }

void get_measurement_interval(void *arg) {
  while (1) {
    for (int i = 0; i < NUM_CONNECTORS; i++) {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      get_meter_values(i);
    }
    //    for (int i = 0; i < 14; i++) {
    //      safe_strncpy(meterValues[i], MeterValues[i], 15);
    //    }
  }
}

static TickType_t last_frame_sent_tick = 0;
#define ACK_TIMEOUT pdMS_TO_TICKS(10000)
// void i2c_task(void *arg) {
//   ESP_ERROR_CHECK(i2c_init());
//   ESP_LOGI(I2CTAG, "I2C Initialized");
//
//   // First A frame (ping)
//   set_index_value(1, 9);
//   build_frame_json("A", a_frame_values, ARRAY_SIZE(a_frame_values));
//   send_frame_with_tag(frame_buffer, "\"A\""); // Waits for ACK + response
//
//   while (1) {
//     TickType_t i2c_now = xTaskGetTickCount();
//     if (!waiting_for_ack || (i2c_now - last_frame_sent_tick) > ACK_TIMEOUT) {
//       bool frame_sent_this_loop = false;
//       set_index_value(1, gun1_status_value);
//       set_index_value(2, stop_error_code);
//
//       // === Send B Frame ===
//       if (!frame_sent_this_loop && send_B_flag) {
//         build_frame_json("B", b_frame_values, ARRAY_SIZE(b_frame_values));
//         if (send_frame_with_tag(frame_buffer, "\"B\"")) {
//           send_B_flag = false;
//           send_C_flag = true;
//           waiting_for_ack = true;
//           frame_sent_this_loop = true;
//         }
//       }
//
//       if (!frame_sent_this_loop && send_P_flag) {
//         build_frame_json("P", p_frame_values, ARRAY_SIZE(p_frame_values));
//         if (send_frame_with_tag(frame_buffer, "\"P\"")) {
//           send_P_flag = false;
//           waiting_for_ack = true;
//           frame_sent_this_loop = true;
//         }
//       }
//
//       // === Send C Frame ===
//       if (!frame_sent_this_loop && send_C_flag) { // fill this if needed
//         if (send_frame_with_tag(frame_C, "\"C\"")) {
//           send_C_flag = false;
//           waiting_for_ack = true;
//           frame_sent_this_loop = true;
//         }
//       }
//
//       //       // === Send D Frame ===
//       //       if (!frame_sent_this_loop && send_D1_flag) {
//       //         if (send_frame_with_tag(frame_D1, "\"D\"")) {
//       //           send_D1_flag = false;
//       //           waiting_for_ack = true;
//       //           frame_sent_this_loop = true;
//       //         }
//       //       }
//       //       if (!frame_sent_this_loop && send_D2_flag) {
//       //         if (send_frame_with_tag(frame_D2, "\"D\"")) {
//       //           send_D2_flag = false;
//       //           waiting_for_ack = true;
//       //           frame_sent_this_loop = true;
//       //         }
//       //       }
//       //       // === Send F Frame ===
//       //       if (!frame_sent_this_loop && send_F1_flag) {
//       //         static uint8_t a = 0;
//       //         a++;
//       //         if (a > 3) {
//       //           if (send_frame_with_tag(frame_F1, "\"F\"")) {
//       //             send_F1_flag = false;
//       //             waiting_for_ack = true;
//       //             frame_sent_this_loop = true;
//       //             a = 0;
//       //           }
//       //         }
//       //       }
//       //       if (!frame_sent_this_loop && send_F2_flag) {
//       //         static uint8_t a = 0;
//       //         a++;
//       //         if (a > 3) {
//       //           if (send_frame_with_tag(frame_F2, "\"F\"")) {
//       //             send_F2_flag = false;
//       //             waiting_for_ack = true;
//       //             frame_sent_this_loop = true;
//       //             a = 0;
//       //           }
//       //         }
//       //       }
//       //
//       //       // === Send I Frame ===
//       //       if (!frame_sent_this_loop && send_I1_flag) {
//       //         if (send_frame_with_tag(frame_I1, "\"I\"")) {
//       //           send_I1_flag = false;
//       //           waiting_for_ack = true;
//       //           frame_sent_this_loop = true;
//       //         }
//       //       }
//       //      if (!frame_sent_this_loop && send_I2_flag) {
//       //        if (send_frame_with_tag(frame_I2, "\"I\"")) {
//       //          send_I2_flag = false;
//       //          waiting_for_ack = true;
//       //          frame_sent_this_loop = true;
//       //        }
//       //      }
//
//       if (!frame_sent_this_loop && send_O_flag) {
//         if (send_frame_with_tag(frame_O, "\"O\"")) {
//           send_O_flag = false;
//           waiting_for_ack = true;
//           frame_sent_this_loop = true;
//         }
//       }
//
//       //      // === Send I3 Frame ===
//       //      if (!frame_sent_this_loop && send_I3_flag) {
//       //        if (send_frame_with_tag(frame_I3, "\"I\"")) {
//       //          send_I3_flag = false;
//       //          waiting_for_ack = true;
//       //          frame_sent_this_loop = true;
//       //        }
//       //      }
//       if (!frame_sent_this_loop && send_V_flag) {
//         if (send_frame_with_tag(frame_V, "\"V\"")) {
//           send_V_flag = false;
//           waiting_for_ack = true;
//           frame_sent_this_loop = true;
//         }
//       }
//       // === Send G Frame (time-based) ===
//       if (!frame_sent_this_loop && time_synced) {
//         TickType_t now = xTaskGetTickCount();
//         if ((now - last_G_sent) > G_interval) {
//           if (send_frame_with_tag(display_time, "\"G\"")) {
//             last_G_sent = now;
//             waiting_for_ack = true;
//             frame_sent_this_loop = true;
//           }
//         }
//       }
//
//       // === Send A Frame (default fallback) ===
//       if (!frame_sent_this_loop) {
//         build_frame_json("A", a_frame_values, ARRAY_SIZE(a_frame_values));
//         send_frame_with_tag(frame_buffer, "\"A\"");
//         waiting_for_ack = true;
//         frame_sent_this_loop = true;
//         // Do not update flags here; "A" is like a heartbeat/ping
//       }
//     }
//     vTaskDelay(pdMS_TO_TICKS(100)); // Adjust loop speed as needed
//
//     switch (vendorErrorCode) {
//     case 0:
//       safe_strncpy(errorCode, "NoError", sizeof(errorCode));
//       break;
//     case 1:
//       safe_strncpy(errorCode, "ConnectorLockFailure", sizeof(errorCode));
//       break;
//     case 2:
//       safe_strncpy(errorCode, "EVCommunicationError", sizeof(errorCode));
//       break;
//     case 3:
//       safe_strncpy(errorCode, "GroundFailure", sizeof(errorCode));
//       break;
//     case 4:
//       safe_strncpy(errorCode, "HighTemperature", sizeof(errorCode));
//       break;
//     case 5:
//       safe_strncpy(errorCode, "InternalError", sizeof(errorCode));
//       break;
//     case 6:
//       safe_strncpy(errorCode, "LocalListConflict", sizeof(errorCode));
//       break;
//     case 7:
//       safe_strncpy(errorCode, "OverCurrentFailure", sizeof(errorCode));
//       break;
//     case 8:
//       safe_strncpy(errorCode, "OverVoltage", sizeof(errorCode));
//       break;
//     case 9:
//       safe_strncpy(errorCode, "UnderVoltage", sizeof(errorCode));
//       break;
//     case 10:
//       safe_strncpy(errorCode, "PowerMeterFailure", sizeof(errorCode));
//       break;
//     case 11:
//       safe_strncpy(errorCode, "PowerSwitchFailure", sizeof(errorCode));
//       break;
//     case 12:
//       safe_strncpy(errorCode, "ReaderFailure", sizeof(errorCode));
//       break;
//     case 13:
//       safe_strncpy(errorCode, "ResetFailure", sizeof(errorCode));
//       break;
//     case 14:
//       safe_strncpy(errorCode, "WeakSignal", sizeof(errorCode));
//       break;
//     default:
//       safe_strncpy(errorCode, "OtherError", sizeof(errorCode));
//       break;
//     }
//     ESP_LOGE(I2CTAG, "gun_state_value: %d", gun_state_value);
//     switch (gun1_state_value) {
////    case 0:
////      if (charger1_state != CHARGER_AVAILABLE)
////        charger1_state = CHARGER_AVAILABLE;
////      safe_strncpy(charger1_status, "Available",
/// sizeof(charger1_status)); /      stop_error_code = 0; /      break; /
/// case 1: /      //      gun_state = GUN_STATUS_PREPARING; /      if
///(charger1_state != CHARGER_PREPARING) /        charger1_state =
/// CHARGER_PREPARING; /      safe_strncpy(charger1_status, "Preparing",
/// sizeof(charger1_status)); /      break; /    case 2: /      // gun_state
///= GUN_STATUS_CHARGING; /      if (charger1_state != CHARGER_CHARGE) /
/// charger1_state = CHARGER_CHARGE; /      safe_strncpy(charger1_status,
///"Charging", sizeof(charger1_status)); /      break;
//    case 3:
//      //      gun_state = GUN_STATUS_SUSPENDED_EVSE;
//      safe_strncpy(charger1_status, "SuspendedEVSE",
//      sizeof(charger1_status)); break;
//    case 4:
//      //      gun_state = GUN_STATUS_SUSPENDED_EV;
//      safe_strncpy(charger1_status, "SuspendedEV",
//      sizeof(charger1_status)); break;
//    case 5:
//      //      gun_state = GUN_STATUS_FINISHING;
//      safe_strncpy(charger1_status, "Finishing", sizeof(charger1_status));
//      break;
//    case 6:
//      //      gun_state = GUN_STATUS_RESERVED;
//      safe_strncpy(charger1_status, "Reserved", sizeof(charger1_status));
//      break;
//    case 7:
//      //      gun_state = GUN_STATUS_UNAVAILABLE;
//      safe_strncpy(charger1_status, "Unavailable",
//      sizeof(charger1_status)); break;
//    case 8:
//      //      gun_state = GUN_STATUS_FAULTED;
//      safe_strncpy(charger1_status, "Faulted", sizeof(charger1_status));
//      break;
//      //    case 9:
//      //      //      gun_state = GUN_STATUS_RESET;
//      //      break;
//      //    case 10:
//      //      gun_state = GUN_STATUS_RESET;
//    default:
//      break;
//    }

//    if (station_state != STATION_BOOTING) {
//      if (strncmp(charger1_last_status, charger1_status,
//                  sizeof(charger1_last_status)) != 0) {
//        trigger_status_notification(1, charger1_status, errorCode,
//                                    VE_code); // ← create task
//        //                                    here
//        snprintf(charger1_last_status, sizeof(charger1_last_status), "%s",
//                 charger1_status);
//      }
//    }
//    switch (gun2_state_value) {
////    case 0:
////      if (charger2_state != CHARGER_AVAILABLE)
////        charger2_state = CHARGER_AVAILABLE;
////      safe_strncpy(charger2_status, "Available",
/// sizeof(charger2_status)); /      stop_error_code = 0; /      break; /
/// case 1: /      //      gun_state = GUN_STATUS_PREPARING; /      if
///(charger2_state != CHARGER_PREPARING) /        charger2_state =
/// CHARGER_PREPARING; /      safe_strncpy(charger2_status, "Preparing",
/// sizeof(charger2_status)); /      break; /    case 2: /      // gun_state
///= GUN_STATUS_CHARGING; /      if (charger2_state != CHARGER_CHARGE) /
/// charger2_state = CHARGER_CHARGE; /      safe_strncpy(charger2_status,
///"Charging", sizeof(charger2_status)); /      break;
//    case 3:
//      //      gun_state = GUN_STATUS_SUSPENDED_EVSE;
//      safe_strncpy(charger2_status, "SuspendedEVSE",
//      sizeof(charger2_status)); break;
//    case 4:
//      //      gun_state = GUN_STATUS_SUSPENDED_EV;
//      safe_strncpy(charger2_status, "SuspendedEV",
//      sizeof(charger2_status)); break;
//    case 5:
//      //      gun_state = GUN_STATUS_FINISHING;
//      safe_strncpy(charger2_status, "Finishing", sizeof(charger2_status));
//      break;
//    case 6:
//      //      gun_state = GUN_STATUS_RESERVED;
//      safe_strncpy(charger2_status, "Reserved", sizeof(charger2_status));
//      break;
//    case 7:
//      //      gun_state = GUN_STATUS_UNAVAILABLE;
//      safe_strncpy(charger2_status, "Unavailable",
//      sizeof(charger2_status)); break;
//    case 8:
//      //      gun_state = GUN_STATUS_FAULTED;
//      safe_strncpy(charger2_status, "Faulted", sizeof(charger2_status));
//      break;
//      //    case 9:
//      //      //      gun_state = GUN_STATUS_RESET;
//      //      break;
//      //    case 10:
//      //      gun_state = GUN_STATUS_RESET;
//    default:
//      break;
//    }
//    ESP_LOGI(I2CTAG, "Previous status: %s", charger_last_status);
//    ESP_LOGI(I2CTAG, "Current status: %s", charger_status);
//    if (station_state != STATION_BOOTING) {
//      if (strncmp(charger2_last_status, charger2_status,
//                  sizeof(charger2_last_status)) != 0) {
//        trigger_status_notification(2, charger2_status, errorCode,
//                                    VE_code); // ← create task
//        //                                    here
//        snprintf(charger2_last_status, sizeof(charger2_last_status), "%s",
//                 charger2_status);
//      }
//    }
//  }
//}

void i2c_task(void *arg) {
  ESP_ERROR_CHECK(i2c_init());
  ESP_LOGI(I2CTAG, "I2C Initialized");

  // First A frame (ping)
  set_index_value(1, guns[0].gun_state_value);
  build_frame_json("A", a_frame_values, ARRAY_SIZE(a_frame_values));
  send_frame_with_tag(frame_buffer, "\"A\""); // Waits for ACK + response

  while (1) {
    TickType_t now_tick = xTaskGetTickCount();

    if (!waiting_for_ack || (now_tick - last_frame_sent_tick) > ACK_TIMEOUT) {
      bool frame_sent_this_loop = false;

      // Update B-frame values dynamically for each connector

      // --- Send frames based on flags ---
      if (!frame_sent_this_loop && send_B_flag) {
        build_frame_json("B", b_frame_values, ARRAY_SIZE(b_frame_values));
        if (send_frame_with_tag(frame_buffer, "\"B\"")) {
          send_B_flag = false;
          send_C_flag = true;
          waiting_for_ack = true;
          frame_sent_this_loop = true;
        }
      }

      if (!frame_sent_this_loop && send_P_flag) {
        build_frame_json("P", p_frame_values, ARRAY_SIZE(p_frame_values));
        if (send_frame_with_tag(frame_buffer, "\"P\"")) {
          send_P_flag = false;
          waiting_for_ack = true;
          frame_sent_this_loop = true;
        }
      }
      // === Send C Frame ===
      if (!frame_sent_this_loop && send_C_flag) { // fill this if

        if (send_frame_with_tag(frame_C, "\"C\"")) {
          send_C_flag = false;
          waiting_for_ack = true;
          frame_sent_this_loop = true;
        }
      }

      for (int i = 0; i < NUM_CONNECTORS; i++) {
        if (!frame_sent_this_loop && send_D_flag[i]) {

          char payload[32];
          snprintf(payload, sizeof(payload), "{\"D\":[%d]}", i + 1);

          if (send_frame_with_tag(payload, "\"D\"")) {
            send_D_flag[i] = false;
            waiting_for_ack = true;
            frame_sent_this_loop = true;
          }
        }
        if (!frame_sent_this_loop && send_F_flag[i]) {

          char payload[32];
          snprintf(payload, sizeof(payload), "{\"F\":[%d]}", i + 1);

          if (send_frame_with_tag(payload, "\"F\"")) {
            send_F_flag[i] = false;
            waiting_for_ack = true;
            frame_sent_this_loop = true;
          }
        }
        if (!frame_sent_this_loop && send_I_flag[i]) {

          char payload[32];
          snprintf(payload, sizeof(payload), "{\"I\":[%d]}", i + 1);

          if (send_frame_with_tag(payload, "\"I\"")) {
            send_I_flag[i] = false;
            waiting_for_ack = true;
            frame_sent_this_loop = true;
          }
        }
      }

      //       ... repeat similar logic for C, D, F, I, O, I3, V, G, A frames
      //       ...
      for (int i = 0; i < NUM_CONNECTORS; i++) {
        set_index_value(1, guns[i].gun_state_value);
        set_index_value(2, guns[i].vendorErrorCode);

        // Default heartbeat A-frame if nothing else sent
        if (!frame_sent_this_loop) {
          build_frame_json("A", a_frame_values, ARRAY_SIZE(a_frame_values));
          send_frame_with_tag(frame_buffer, "\"A\"");
          waiting_for_ack = true;
          frame_sent_this_loop = true;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100)); // Adjust loop speed

    // --- Update connector states dynamically ---
    for (int i = 0; i < NUM_CONNECTORS; i++) {
      connector_t *gun = &guns[i];
      Connector *conn = &connectors[i];
      char *status_str;

      // Map gun_state_value -> charger state & status string
      switch (gun->gun_state_value) {
      case 0:
        gun->charger_state = CHARGER_AVAILABLE;
        status_str = "Available";
        break;
      case 1:
        gun->charger_state = CHARGER_PREPARING;
        status_str = "Preparing";
        break;
      case 2:
        gun->charger_state = CHARGER_CHARGE;
        status_str = "Charging";
        break;
      case 3:
        status_str = "SuspendedEVSE";
        break;
      case 4:
        status_str = "SuspendedEV";
        break;
      case 5:
        gun->charger_state = CHARGER_STOP;
        status_str = "Finishing";
        break;
      case 6:
        status_str = "Reserved";
        break;
      case 7:
        status_str = "Unavailable";
        break;
      case 8:
        status_str = "Faulted";
        break;
      default:
        status_str = "Unknown";
        break;
      }

      // Map vendorErrorCode -> error string
      switch (gun->vendorErrorCode) {
      case 0:
        safe_strncpy(errorCode, "NoError", sizeof(errorCode));
        break;
      case 1:
        safe_strncpy(errorCode, "ConnectorLockFailure", sizeof(errorCode));
        break;
      case 2:
        safe_strncpy(errorCode, "EVCommunicationError", sizeof(errorCode));
        break;
      case 3:
        safe_strncpy(errorCode, "GroundFailure", sizeof(errorCode));
        break;
      case 4:
        safe_strncpy(errorCode, "HighTemperature", sizeof(errorCode));
        break;
      case 5:
        safe_strncpy(errorCode, "InternalError", sizeof(errorCode));
        break;
      case 6:
        safe_strncpy(errorCode, "LocalListConflict", sizeof(errorCode));
        break;
      case 7:
        safe_strncpy(errorCode, "OverCurrentFailure", sizeof(errorCode));
        break;
      case 8:
        safe_strncpy(errorCode, "OverVoltage", sizeof(errorCode));
        break;
      case 9:
        safe_strncpy(errorCode, "UnderVoltage", sizeof(errorCode));
        break;
      case 10:
        safe_strncpy(errorCode, "PowerMeterFailure", sizeof(errorCode));
        break;
      case 11:
        safe_strncpy(errorCode, "PowerSwitchFailure", sizeof(errorCode));
        break;
      case 12:
        safe_strncpy(errorCode, "ReaderFailure", sizeof(errorCode));
        break;
      case 13:
        safe_strncpy(errorCode, "ResetFailure", sizeof(errorCode));
        break;
      case 14:
        safe_strncpy(errorCode, "WeakSignal", sizeof(errorCode));
        break;
      default:
        safe_strncpy(errorCode, "OtherError", sizeof(errorCode));
        break;
      }
      // TODO:
      //  Trigger status notification if state changed
      if (strcmp(conn->charger_current_status, status_str) != 0) {
        trigger_status_notification(i + 1, status_str, errorCode, VE_code);

        snprintf(conn->charger_current_status,
                 sizeof(conn->charger_current_status), "%s", status_str);
      }
    }
  }
}

void ocpp_data(void *args) {
  while (1) {
    for (int i = 0; i < NUM_CONNECTORS; i++) {
      connector_t *gun = &guns[i];
      Connector *conn = &connectors[i];

      switch (conn->state) {
      case CONNECTOR_BOOTING:
        gun->gun_state_value = 10;
        break;
      case CONNECTOR_AVAILABLE:
        gun->gun_state_value = 0;
        break;
      case CONNECTOR_PREPARING:
        gun->gun_state_value = 1;
        break;
      case CONNECTOR_CHARGING:
        gun->gun_state_value = 2;
        break;
      case CONNECTOR_RESERVED:
        gun->gun_state_value = 6;
        break;
      case CONNECTOR_UNAVAILABLE:
        gun->gun_state_value = 7;
        break;
      case CONNECTOR_AUTHORIZED: {
        Connector *conn = &connectors[i];

        // Only start the timer if not already running
        if (!conn->prep_timeout_started) {
          esp_timer_start_once(conn->prep_timeout_timer,
                               connection_timeout * 1000000);
          conn->prep_timeout_started = true;
          conn->prep_timeout_expired = false;
        }
        if (gun->charger_state == CHARGER_PREPARING) {

          // If timer is active, stop it and fire D-flag
          if (conn->prep_timeout_started &&
              esp_timer_is_active(conn->prep_timeout_timer)) {
            esp_timer_stop(conn->prep_timeout_timer);
            conn->prep_timeout_started = false;
            send_D_flag[i] = true;
          }
          // If timer already expired, EV plugged in late
          else if (conn->prep_timeout_expired) {
            // Optionally handle late plug-in
            send_D_flag[i] = false;             // or handle as failed session
            conn->prep_timeout_expired = false; // reset flag
          }
        }
      } break;
      case CONNECTOR_FINISHING:
        gun->gun_state_value = 5;
        if (gun->charger_state != CHARGER_STOP) {
          send_F_flag[i] = true;
        }
        break;

      case CONNECTOR_AUTHORIZE_FAILED:
        gun->charger_state = CHARGER_CARD_AUTHORIZE_FAIL;
        if (conn->transaction_active)
          conn->state = CONNECTOR_CHARGING;
        else
          conn->state = CONNECTOR_AVAILABLE;
        break;
      default:
        break;
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// void ocpp_data(void *args) {
//   while (1) {
//     //	  ESP_LOGE(TAG, "connector1_state%u",connector1_state);
//     VE_code = vendorErrorCode;
//     switch (connector1_state) {
//     case CONNECTOR_BOOTING:
//       gun1_status_value = 10;
//       break;
//     case CONNECTOR_AVAILABLE:
//       gun1_status_value = 0;
//       break;
//
//     case CONNECTOR_PREPARING:
//       gun1_status_value = 1;
//       break;
//
//     case CONNECTOR_CHARGING:
//       gun1_status_value = 2;
//       break;
//
//     case CONNECTOR_RESERVED:
//       gun1_status_value = 6;
//       break;
//     case CONNECTOR_UNAVAILABLE:
//       gun1_status_value = 7;
//       break;
//
//     case CONNECTOR_AUTHORIZED:
//       esp_timer_start_once(timeout_timer, connection_timeout * 1000000);
//       if (charger1_state == CHARGER_PREPARING) {
//
//         bool timer_active = esp_timer_is_active(timeout_timer);
//         ESP_LOGI(TAG, "Timer active status: %s\n",
//                  timer_active ? "true" : "false");
//         if (esp_timer_is_active(timeout_timer)) {
//           esp_timer_stop(timeout_timer);
//           send_D1_flag = true;
//         }
//       }
//       break;
//
//     case CONNECTOR_FINISHING:
//       gun1_status_value = 5;
//       if (charger1_state != CHARGER_STOP) {
//
//         send_F1_flag = true;
//       }
//       //      connector1_state = CONNECTOR_STOP_TRANSACTION;
//       break;
//     case CONNECTOR_UPDATE:
//       //      gun_status_value = 5;
//       vTaskDelay(1000 / portTICK_PERIOD_MS);
//       send_O_flag = true;
//
//       connector1_state = CONNECTOR_AVAILABLE;
//       break;
//     case CONNECTOR_AUTHORIZE_FAILED:
//       charger1_state = CHARGER_CARD_AUTHORIZE_FAIL;
//       if (gun1_transaction_active == true)
//         connector1_state = CONNECTOR_CHARGING;
//       else
//         connector1_state = CONNECTOR_AVAILABLE;
//       break;
//
//     default:
//       //      gun_status_value = 10; // Optional: Handle unknown state
//       break;
//     }
//     switch (connector2_state) {
//     case CONNECTOR_BOOTING:
//       gun2_status_value = 10;
//       break;
//     case CONNECTOR_AVAILABLE:
//       gun2_status_value = 0;
//       break;
//
//     case CONNECTOR_PREPARING:
//       gun2_status_value = 1;
//       break;
//
//     case CONNECTOR_CHARGING:
//       gun2_status_value = 2;
//       break;
//
//     case CONNECTOR_RESERVED:
//       gun2_status_value = 6;
//       break;
//     case CONNECTOR_UNAVAILABLE:
//       gun2_status_value = 7;
//       break;
//
//     case CONNECTOR_AUTHORIZED:
//       esp_timer_start_once(timeout_timer, connection_timeout * 1000000);
//       if (charger2_state == CHARGER_PREPARING) {
//
//         bool timer_active = esp_timer_is_active(timeout_timer);
//         ESP_LOGI(TAG, "Timer active status: %s\n",
//                  timer_active ? "true" : "false");
//         if (esp_timer_is_active(timeout_timer)) {
//           esp_timer_stop(timeout_timer);
//           send_D2_flag = true;
//         }
//       }
//       break;
//
//     case CONNECTOR_FINISHING:
//       gun2_status_value = 5;
//       if (charger2_state != CHARGER_STOP) {
//
//         send_F2_flag = true;
//       }
//       //      connector1_state = CONNECTOR_STOP_TRANSACTION;
//       break;
//     case CONNECTOR_UPDATE:
//       //      gun_status_value = 5;
//       vTaskDelay(1000 / portTICK_PERIOD_MS);
//       send_O_flag = true;
//
//       connector2_state = CONNECTOR_AVAILABLE;
//       break;
//     case CONNECTOR_AUTHORIZE_FAILED:
//       charger2_state = CHARGER_CARD_AUTHORIZE_FAIL;
//       if (gun2_transaction_active == true)
//         connector2_state = CONNECTOR_CHARGING;
//       else
//         connector2_state = CONNECTOR_AVAILABLE;
//       break;
//
//     default:
//       //      gun_status_value = 10; // Optional: Handle unknown state
//       break;
//     }
//     vTaskDelay(100 / portTICK_PERIOD_MS);
//   }
// }

void i2c_data(void *args) {
  while (1) {
    for (int i = 0; i < NUM_CONNECTORS; i++) {
      connector_t *gun = &guns[i];
      Connector *conn = &connectors[i];

      switch (gun->charger_state) {
      case CHARGER_FAULTED:
        if (conn->transaction_active)
          conn->state = CONNECTOR_FINISHING;
        break;

      case CHARGER_CARD_DETECTED:
        send_I_flag[i] = true;
        break;

      case CHARGER_CHARGE:
        if (gun->gun_state_value == CONNECTOR_AUTHORIZED) {
          ocpp_send_start_transaction(gun->meter_start, gun->connectorCode,
                                      conn->trans_id_tag);
          conn->state = CONNECTOR_START_TRANSACTION;
        }
        break;

      case CHARGER_STOP:
        if (gun->gun_state_value == CONNECTOR_FINISHING) {
          ocpp_send_stop_transaction(gun->connectorCode, gun->meter_stop,
                                     reason);
          conn->state = CONNECTOR_STOP_TRANSACTION;
        }
        break;

      case CHARGER_CARD_AUTHORIZE:
        if (gun->gun_state_value < CONNECTOR_AUTHORIZE_REQ) {
          if (conn->transaction_active) {
            if (strcasecmp(conn->trans_id_tag, gun->id_tag) == 0) {
              stop_error_code = 5;
              ocpp_send_authorize(gun->connectorCode, gun->id_tag);
              gun->charger_state = CHARGER_CARD_AUTHORIZE_REQ;
              conn->state = CONNECTOR_AUTHORIZE_REQ;
            } else {
              gun->charger_state = CHARGER_CARD_AUTHORIZE_FAIL;
            }
          } else if (conn->reserved_expiry_time != 0) {
            if (strcasecmp(conn->reserved_id_tag, gun->id_tag) != 0) {
              ESP_LOGE(TAG, "Diff_Card: reserved %s, idTag %s",
                       conn->reserved_id_tag, gun->id_tag);
              gun->charger_state = CHARGER_CARD_AUTHORIZE_FAIL;
            } else {
              ocpp_send_authorize(gun->connectorCode, gun->id_tag);
              conn->state = CONNECTOR_AUTHORIZE_REQ;
              gun->charger_state = CHARGER_CARD_AUTHORIZE_REQ;
            }
          } else {
            if (gun->gun_state_value == CONNECTOR_AVAILABLE ||
                gun->gun_state_value == CONNECTOR_RESERVED)
              ocpp_send_authorize(gun->connectorCode, gun->id_tag);
            conn->state = CONNECTOR_AUTHORIZE_REQ;
            gun->charger_state = CHARGER_CARD_AUTHORIZE_REQ;
          }
        }
        break;

      case CHARGER_CARD_AUTHORIZE_FAIL:
        send_I2_flag[i] = true;
        if (conn->transaction_active)
          gun->charger_state = CHARGER_CHARGE;
        else
          gun->charger_state = CHARGER_AVAILABLE;
        break;

      default:
        break;
      }
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// void i2c_data(void *args) {
//   while (1) {
//
//     for (int i = 0; i < NUM_CONNECTORS; i++) {
//       connector_t *gun = &guns[i];
//       Connector *conn = &connectors[i];
//
//       switch (gun->charger_state) {
//       case CHARGER_FAULTED:
//         if (conn->transaction_active)
//           gun->gun_state_value = CONNECTOR_FINISHING;
//         break;
//
//       case CHARGER_CARD_DETECTED:
//         send_I_flag[i] = true;
//         break;
//
//       case CHARGER_CHARGE:
//         if (connector1_state == CONNECTOR_AUTHORIZED) {
//           ocpp_send_start_transaction(meter_start, 1, gun1_trans_id_tag);
//           connector1_state = CONNECTOR_START_TRANSACTION;
//         }
//         break;
//
//       case CHARGER_STOP:
//         if (connector1_state == CONNECTOR_FINISHING) {
//           ocpp_send_stop_transaction(1, meter_stop, reason);
//           connector1_state = CONNECTOR_STOP_TRANSACTION;
//         }
//         break;
//
//       case CHARGER_CARD_AUTHORIZE:
//         if (connector1_state < 7) {
//           if (gun1_transaction_active == true) {
//             if (strcasecmp(gun1_trans_id_tag, gun1_id_tag) == 0) {
//               stop_error_code = 5;
//               ocpp_send_authorize(1, gun1_id_tag);
//               charger1_state = CHARGER_CARD_AUTHORIZE_REQ;
//               connector1_state = CONNECTOR_AUTHORIZE_REQ;
//             } else
//               charger1_state = CHARGER_CARD_AUTHORIZE_FAIL;
//           } else if (reserved_expiry_time != 0) {
//             if (strcasecmp(reserved_id_tag, gun1_id_tag) != 0) {
//               ESP_LOGE(TAG, "reserved_id_tag%s", reserved_id_tag);
//               ESP_LOGE(TAG, "idTag%s", gun1_id_tag);
//               ESP_LOGE(TAG, "Diff_Card");
//               charger1_state = CHARGER_CARD_AUTHORIZE_FAIL;
//             } else {
//               ocpp_send_authorize(1, gun1_id_tag);
//               connector1_state = CONNECTOR_AUTHORIZE_REQ;
//               charger1_state = CHARGER_CARD_AUTHORIZE_REQ;
//             }
//           } else {
//             if (connector1_state == CONNECTOR_AVAILABLE ||
//                 connector1_state == CONNECTOR_RESERVED)
//               ocpp_send_authorize(1, gun1_id_tag);
//             connector1_state = CONNECTOR_AUTHORIZE_REQ;
//             charger1_state = CHARGER_CARD_AUTHORIZE_REQ;
//           }
//         }
//         break;
//
//       case CHARGER_CARD_AUTHORIZE_FAIL:
//         send_I3_flag = true;
//         if (gun1_transaction_active == true)
//           charger1_state = CHARGER_CHARGE;
//         else
//           charger1_state = CHARGER_AVAILABLE;
//         break;
//         //    case CHARGER_AVAILABLE:
//         //      gun_status_value = 2;
//         //      break;
//
//       default:
//         //      gun_status_value = 10; // Optional: Handle unknown state
//         break;
//       }
//
//       switch (charger2_state) {
//       case CHARGER_FAULTED:
//         if (gun2_transaction_active)
//           connector2_state = CONNECTOR_FINISHING;
//         break;
//
//       case CHARGER_CARD_DETECTED:
//         send_I2_flag = true;
//         break;
//
//       case CHARGER_CHARGE:
//         if (connector2_state == CONNECTOR_AUTHORIZED) {
//           ocpp_send_start_transaction(meter_start, 2, gun2_trans_id_tag);
//           connector2_state = CONNECTOR_START_TRANSACTION;
//         }
//         break;
//
//       case CHARGER_STOP:
//         if (connector2_state == CONNECTOR_FINISHING) {
//           ocpp_send_stop_transaction(2, meter_stop, reason);
//           connector2_state = CONNECTOR_STOP_TRANSACTION;
//         }
//         break;
//
//       case CHARGER_CARD_AUTHORIZE:
//         if (connector2_state < 7) {
//           if (gun2_transaction_active == true) {
//             if (strcasecmp(gun2_trans_id_tag, gun2_id_tag) == 0) {
//               stop_error_code = 5;
//               ocpp_send_authorize(2, gun2_id_tag);
//               charger2_state = CHARGER_CARD_AUTHORIZE_REQ;
//               connector2_state = CONNECTOR_AUTHORIZE_REQ;
//             } else
//               charger2_state = CHARGER_CARD_AUTHORIZE_FAIL;
//           } else if (reserved_expiry_time != 0) {
//             if (strcasecmp(reserved_id_tag, gun2_id_tag) != 0) {
//               ESP_LOGE(TAG, "reserved_id_tag%s", reserved_id_tag);
//               ESP_LOGE(TAG, "idTag%s", gun2_id_tag);
//               ESP_LOGE(TAG, "Diff_Card");
//               charger2_state = CHARGER_CARD_AUTHORIZE_FAIL;
//             } else {
//               ocpp_send_authorize(2, gun2_id_tag);
//               connector2_state = CONNECTOR_AUTHORIZE_REQ;
//               charger2_state = CHARGER_CARD_AUTHORIZE_REQ;
//             }
//           } else {
//             if (connector2_state == CONNECTOR_AVAILABLE ||
//                 connector2_state == CONNECTOR_RESERVED)
//               ocpp_send_authorize(2, gun2_id_tag);
//             connector2_state = CONNECTOR_AUTHORIZE_REQ;
//             charger2_state = CHARGER_CARD_AUTHORIZE_REQ;
//           }
//         }
//         break;
//
//       case CHARGER_CARD_AUTHORIZE_FAIL:
//         send_I3_flag = true;
//         if (gun2_transaction_active == true)
//           charger2_state = CHARGER_CHARGE;
//         else
//           charger2_state = CHARGER_AVAILABLE;
//         break;
//         //    case CHARGER_AVAILABLE:
//         //      gun_status_value = 2;
//         //      break;
//
//       default:
//         //      gun_status_value = 10; // Optional: Handle unknown state
//         break;
//       }
//       vTaskDelay(50 / portTICK_PERIOD_MS);
//     }
//   }
// }
const char *connector_state_to_string(connector_state_t state) {
  switch (state) {
  case CONNECTOR_BOOTING:
    return "CONNECTOR_BOOTING";
  case CONNECTOR_AVAILABLE:
    return "CONNECTOR_AVAILABLE";
  case CONNECTOR_PREPARING:
    return "CONNECTOR_PREPARING";
  case CONNECTOR_CHARGING:
    return "CONNECTOR_CHARGING";
  case CONNECTOR_SUSPENDED:
    return "CONNECTOR_SUSPENDED";
  case CONNECTOR_AUTHORIZE_REQ:
    return "CONNECTOR_AUTHORIZE_REQ";
  case CONNECTOR_RESERVED:
    return "CONNECTOR_RESERVED";
  case CONNECTOR_AUTHORIZED:
    return "CONNECTOR_AUTHORIZED";
  case CONNECTOR_AUTHORIZE_FAILED:
    return "CONNECTOR_AUTHORIZE_FAILED";
  case CONNECTOR_START_TRANSACTION:
    return "CONNECTOR_START_TRANSACTION";
  case CONNECTOR_STOP_TRANSACTION:
    return "CONNECTOR_STOP_TRANSACTION";
  case CONNECTOR_FINISHING:
    return "CONNECTOR_FINISHING";
  case CONNECTOR_UNAVAILABLE:
    return "CONNECTOR_UNAVAILABLE";
    //  case CONNECTOR_UPDATE:
    //    return "CONNECTOR_UPDATE";
  case CONNECTOR_FAULTED:
    return "CONNECTOR_FAULTED";
  default:
    return "UNKNOWN_STATE";
  }
}

const char *charger_state_to_string(charger_state_t state) {
  switch (state) {
  case CHARGER_BOOTING:
    return "CHARGER_BOOTING";
  case CHARGER_AVAILABLE:
    return "CHARGER_AVAILABLE";
  case CHARGER_PREPARING:
    return "CHARGER_PREPARING";
  case CHARGER_CHARGE_INIT:
    return "CHARGER_CHARGE_INIT";
  case CHARGER_CHARGE:
    return "CHARGER_CHARGE";
  case CHARGER_SUSPENDED:
    return "CHARGER_SUSPENDED";
  case CHARGER_CARD_DETECTED:
    return "CHARGER_CARD_DETECTED";
  case CHARGER_CARD_AUTHORIZE:
    return "CHARGER_CARD_AUTHORIZE";
  case CHARGER_CARD_AUTHORIZE_REQ:
    return "CHARGER_CARD_AUTHORIZE_REQ";
  case CHARGER_CARD_AUTHORIZE_FAIL:
    return "CHARGER_CARD_AUTHORIZE_FAIL";
  case CHARGER_STOP:
    return "CHARGER_STOP";
  case CHARGER_FAULTED:
    return "CHARGER_FAULTED";
  default:
    return "UNKNOWN_CHARGER_STATE";
  }
}

// void connection_timeout_cb(void *arg) {
//   //	TODO:
//   //  connector1_state = CONNECTOR_AVAILABLE;
// }
void connection_timeout_cb(void *arg) {
  Connector *conn = (Connector *)arg;
  if (!conn)
    return;

  // Timeout fired
  conn->prep_timeout_expired = true;
  conn->prep_timeout_started = false;

  // Optionally reset connector state
  conn->state = CONNECTOR_AVAILABLE; // or your equivalent available state
                                     //  snprintf(conn->charger_current_status,
                                     //  sizeof(conn->charger_current_status),
                                     //           "Available");

  printf("Connector %d: timeout occurred, EV did not plug in.\n",
         conn->connector_id);
}

void create_connector_timers() {
  for (int i = 0; i < NUM_CONNECTORS; i++) {
    esp_timer_create_args_t timer_args = {
        .callback = &connection_timeout_cb,
        .arg = &connectors[i], // pass connector as argument
        .name = "prep_timeout_timer"};

    esp_err_t err =
        esp_timer_create(&timer_args, &connectors[i].prep_timeout_timer);
    if (err != ESP_OK) {
      printf("Failed to create timer for connector %d: %d\n",
             connectors[i].connector_id, err);
    }
  }
}
// void setup_timeout_timer() {
//   esp_timer_create_args_t timer_args = {.callback = &connection_timeout_cb,
//                                         .arg = NULL,
//                                         .dispatch_method = ESP_TIMER_TASK,
//                                         .name = "timeout_timer"};
//   ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timeout_timer));
// }

static inline void set_all_connectors_state(Connector connectors[], int state) {
  for (int i = 0; i < NUM_CONNECTORS; i++) {
    connectors[i].state = state;
  }
}

void app_main(void) {
  send_V_flag = true;
  send_P_flag = true;
  ESP_ERROR_CHECK(nvs_flash_init());
  mount_spiffs();
  load_config_from_spiffs();
  wifi_init_sta();
  client_mutex = xSemaphoreCreateMutex();
  if (client_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create WebSocket mutex");
    abort(); // or handle more gracefully
  }
  i2c_mutex = xSemaphoreCreateMutex();
  if (i2c_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create i2c_mutex");
    abort(); // or handle error
  }
  ocpp_send_mutex = xSemaphoreCreateMutex();
  if (ocpp_send_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create ocpp_mutex");
    abort(); // or handle error
  }
  create_connector_timers();
  //  uart_init();
  xTaskCreate(i2c_task, "i2c_task", 4096, NULL, 5, NULL);

  xTaskCreate(get_measurement_interval, "get_measurement_task", 4096, NULL, 1,
              NULL);

  xTaskCreate(ocpp_data, "ocpp_data_task", 4096, NULL, 5, NULL);
  xTaskCreate(i2c_data, "i2c_data_task", 4096, NULL, 5, NULL);
  xTaskCreate(get_profile, "get_profile_task", 4096, NULL, 5, NULL);

  esp_task_wdt_add(NULL);
  while (1) {
    //    ESP_LOGI(TAG, "Feeding watchdog...");
    esp_task_wdt_reset(); // Feed the watchdog
    if (currentprofile.purpose[0] != '\0') {
      profile_expired = false;
      if (power != max_power) {

        power = max_power;

        if (max_power > 0)
          p_frame_values[1] = max_power;
        else
          p_frame_values[1] = -1;
        send_P_flag = true;
      }
    } else {
      if (profile_expired == false) {
        p_frame_values[1] = -1;
        send_P_flag = true;
        profile_expired = true;
      }
    }

    switch (station_state) {

    case STATION_BOOTING:
      set_all_connectors_state(connectors, CONNECTOR_BOOTING);
      break;

    case STATION_UNAVAILABLE:
      set_all_connectors_state(connectors, CONNECTOR_UNAVAILABLE);
      break;

    case STATION_UPDATE:
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      send_O_flag = true;
      set_all_connectors_state(connectors, CONNECTOR_UNAVAILABLE);
      break;

    default:
      break;
    }
    /*    ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes",
       esp_get_free_heap_size()); if (!heap_caps_check_integrity_all(true)) {
          ESP_LOGE(TAG, "Heap corruption detected!");
        }*/
    /*TaskStatus_t *pxTaskStatusArray;
    UBaseType_t uxArraySize = uxTaskGetNumberOfTasks();

    pxTaskStatusArray = pvPortMCMactivealloc(uxArraySize *
    sizeof(TaskStatus_t));

    if (pxTaskStatusArray != NULL) {
      uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize,
    NULL); for (UBaseType_t i = 0; i < uxArraySize; i++) { ESP_LOGI(TAG,
    "%-20s | Stack HW Mark: %" PRIu32, pxTaskStatusArray[i].pcTaskName,
                 pxTaskStatusArray[i].usStackHighWaterMark);

        if (pxTaskStatusArray[i].usStackHighWaterMark < 512) {
          ESP_LOGW(TAG, "Task: %s has LOW stack: %" PRIu32,
                   pxTaskStatusArray[i].pcTaskName,
                   pxTaskStatusArray[i].usStackHighWaterMark);
        }
      }
      vPortFree(pxTaskStatusArray);
    }*/

    for (int i = 0; i < NUM_CONNECTORS; i++) {
      connector_t *gun = &guns[i];
      Connector *conn = &connectors[i];

      ESP_LOGI(TAG, "Current charger %d state :  %s\n", i,
               charger_state_to_string(gun->charger_state));
      //    connector_state_t connector1_state3 = CONNECTOR_CHARGING;
      ESP_LOGI(TAG, "State:%d %s\n", i, connector_state_to_string(conn->state));
      vTaskDelay(pdMS_TO_TICKS(1000)); // Normal operation
    }
  }
}