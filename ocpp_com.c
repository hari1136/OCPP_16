/*
 * ocpp_com.c
 *
 *  Created on: 31-Oct-2025
 *      Author: Admin
 */

#include "ftp_ota.h"
#include "ocpp.h"

#define TAG "OCPP"

time_t now;
char timestamp[32];
char Esp_ver[30] = "E_1.0.5 ";
char Nuv_ver[15];

void generate_uuid_v4(char *uuid_str) {
  uint8_t uuid[16];
  for (int i = 0; i < 16; i++) {
    uuid[i] = esp_random() & 0xFF;
  }

  // Set version (4)
  uuid[6] = (uuid[6] & 0x0F) | 0x40;

  // Set variant (10xxxxxx)
  uuid[8] = (uuid[8] & 0x3F) | 0x80;

  snprintf(
      uuid_str, 37, // 36 chars + null terminator
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
      uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14],
      uuid[15]);
}

int get_time_unix_timestamp(const char *datetime_str) {
  int hour, min, sec;
  // Parse the time part of the string (assuming fixed format)
  // Example input: "2025-10-14T09:30:15Z"
  sscanf(datetime_str, "%*4d-%*2d-%*2dT%d:%d:%dZ", &hour, &min, &sec);

  // Convert to seconds since midnight
  int seconds_since_midnight = hour * 3600 + min * 60 + sec;

  return seconds_since_midnight;
}

time_t timegm_fallback(struct tm *tm) {
  char *tz = getenv("TZ");
  setenv("TZ", "", 1); // Use UTC
  tzset();
  time_t t = mktime(tm); // Now mktime interprets tm as UTC
  if (tz)
    setenv("TZ", tz, 1);
  else
    unsetenv("TZ");
  tzset();
  return t;
}
void set_time_from_string(const char *iso8601_time_str) {
  if (!iso8601_time_str) {
    ESP_LOGE(TAG, "ISO8601 time string is NULL");
    return;
  }
  struct tm tm_time = {0};
  // Ignore milliseconds and timezone, parse up to seconds
  char trimmed_time[32];
  safe_strncpy(trimmed_time, iso8601_time_str, sizeof(trimmed_time));

  // Remove milliseconds if present (split on '.')
  char *dot = strchr(trimmed_time, '.');
  if (dot) {
    *dot = '\0'; // Cut off at milliseconds
  }

  if (strptime(trimmed_time, "%Y-%m-%dT%H:%M:%S", &tm_time) == NULL) {
    ESP_LOGE(TAG, "Failed to parse ISO8601 time: %s", trimmed_time);
    return;
  }
  // Convert to time_t and set system time
  time_t epoch = timegm_fallback(&tm_time);
  if (epoch == -1) {
    ESP_LOGE(TAG, "Failed to convert to epoch time");
    return;
  }
  struct timeval now = {.tv_sec = epoch, .tv_usec = 0};
  if (settimeofday(&now, NULL) != 0) {
    ESP_LOGE(TAG, "Failed to set system time");
  } else {
    ESP_LOGI(TAG, "System time set to: %s", trimmed_time);
  }
}
// Fallback if timegm() is not available

time_t parse_iso8601(const char *datetime) {
  if (!datetime)
    return (time_t)-1;

  struct tm tm = {0};
  char trimmed_time[32];

  // Copy input safely
  safe_strncpy(trimmed_time, datetime, sizeof(trimmed_time));

  // Remove milliseconds: find '.' and truncate
  char *dot = strchr(trimmed_time, '.');
  if (dot) {
    *dot = '\0';
  }

  // Also remove trailing 'Z' if present
  char *z = strchr(trimmed_time, 'Z');
  if (z) {
    *z = '\0';
  }

  // Now parse using strptime
  if (strptime(trimmed_time, "%Y-%m-%dT%H:%M:%S", &tm) == NULL) {
    ESP_LOGW(TAG, "parse_iso8601: Failed to parse datetime: %s", trimmed_time);
    return (time_t)-1;
  }

  return timegm_fallback(&tm); // Treat parsed time as UTC
}

void get_current_iso8601_time(void *arg) {
  struct tm utc_tm, ist_tm;
  time_t now, ist;

  while (1) {
    time(&now); // Get current time

    // UTC timestamp (ISO8601)
    gmtime_r(&now, &utc_tm);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);

    // IST time
    ist = now + 19800; // IST = UTC + 5h30m
    gmtime_r(&ist, &ist_tm);

    char datetime[20];
    strftime(datetime, sizeof(datetime), "%Y-%m-%dT%H:%M:%S", &ist_tm);
    snprintf(display_time, sizeof(display_time), "{\"G\":[\"%s\"]}", datetime);

    // Check reservations for all connectors
    for (int i = 0; i < NUM_CONNECTORS; i++) {
      if (connectors[i].reserved_expiry_time != 0) {
        if (ist > connectors[i].reserved_expiry_time) {
          // Reservation expired
          struct tm expiry_tm;
          char expiry_str[32];
          gmtime_r(&connectors[i].reserved_expiry_time, &expiry_tm);
          strftime(expiry_str, sizeof(expiry_str), "%Y-%m-%dT%H:%M:%SZ",
                   &expiry_tm);

          xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);
          connectors[i].state = CONNECTOR_AVAILABLE;
          connectors[i].reserved_expiry_time = 0;
          xSemaphoreGive(ocpp_send_mutex);

          ESP_LOGI(TAG, "Connector %d reservation expired at: %s", i + 1,
                   expiry_str);
        } else {
          if (connectors[i].state == CONNECTOR_AVAILABLE)
            connectors[i].state = CONNECTOR_RESERVED;

          // Optional: Log remaining time
          // double seconds_left = difftime(connectors[i].reserved_expiry_time,
          // ist); ESP_LOGI(TAG, "Connector %d reservation valid. Time left:
          // %.0f seconds", i + 1, seconds_left);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void ocpp_message(const char *msg) {
  esp_websocket_client_send_text(client, msg, strlen(msg), portMAX_DELAY);
}

void send_call_error(const char *uniqueId, const char *errorCode,
                     const char *errorDescription, cJSON *errorDetails) {
  if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (esp_websocket_client_is_connected(client)) {
      cJSON *response = cJSON_CreateArray();
      cJSON_AddItemToArray(response, cJSON_CreateNumber(4)); // CALLERROR
      cJSON_AddItemToArray(response, cJSON_CreateString(uniqueId));
      cJSON_AddItemToArray(response, cJSON_CreateString(errorCode));
      cJSON_AddItemToArray(response, cJSON_CreateString(errorDescription));

      // If errorDetails is NULL, create empty object
      if (errorDetails == NULL) {
        errorDetails = cJSON_CreateObject();
      }
      cJSON_AddItemToArray(response, errorDetails); // takes ownership

      char *resp_str = cJSON_PrintUnformatted(response);
      if (resp_str) {
        ocpp_message(resp_str); // your WebSocket sending function
        ESP_LOGI(TAG, "Sent CALLERROR: %s", resp_str);
        free(resp_str);
      }

      cJSON_Delete(response);
    } else {
      ESP_LOGW(TAG, "WebSocket not connected, cannot send CALLERROR");
    }
    xSemaphoreGive(client_mutex);
  } else {
    ESP_LOGW(TAG, "Failed to take mutex while sending CALLERROR");
  }
}
void send_call_result_with_status(const char *uniqueId, const char *status_str,
                                  const char *action) {
  if (!uniqueId || !status_str || !action) {
    ESP_LOGW(TAG, "Invalid input argument(s) to send_call_result_with_status");
    return;
  }

  if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (esp_websocket_client_is_connected(client)) {
      cJSON *response = cJSON_CreateArray();
      if (!response) {
        ESP_LOGE(TAG, "Failed to create cJSON array");
        xSemaphoreGive(client_mutex);
        return;
      }

      if (!cJSON_AddItemToArray(response,
                                cJSON_CreateNumber(3)) || // CALLRESULT
          !cJSON_AddItemToArray(response, cJSON_CreateString(uniqueId))) {
        ESP_LOGE(TAG, "Failed to add CALLRESULT elements");
        cJSON_Delete(response);
        xSemaphoreGive(client_mutex);
        return;
      }

      cJSON *payload = cJSON_CreateObject();
      if (!payload) {
        ESP_LOGE(TAG, "Failed to create cJSON payload object");
        cJSON_Delete(response);
        xSemaphoreGive(client_mutex);
        return;
      }
      if (!cJSON_AddStringToObject(payload, "status", status_str)) {
        ESP_LOGE(TAG, "Failed to add status to payload");
        cJSON_Delete(payload);
        cJSON_Delete(response);
        xSemaphoreGive(client_mutex);
        return;
      }

      cJSON_AddItemToArray(response, payload);

      char *resp_str = cJSON_PrintUnformatted(response);
      if (resp_str) {
        ocpp_message(resp_str);
        ESP_LOGI(TAG, "Sent CALLRESULT for %s: %s", action, resp_str);
        free(resp_str);
      } else {
        ESP_LOGE(TAG, "Failed to print cJSON response");
      }

      cJSON_Delete(response);
    } else {
      ESP_LOGW(TAG, "WebSocket not connected, cannot send %s", action);
    }
    xSemaphoreGive(client_mutex);
  } else {
    ESP_LOGW(TAG, "Failed to take mutex while sending OCPP request");
  }
}
void send_ocpp_request(const char *action, cJSON *payload, int connectorID) {
  if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (esp_websocket_client_is_connected(client)) {
      char uuid[37];
      generate_uuid_v4(uuid);
      remember_ocpp_request(uuid, action, connectorID);

      cJSON *msg = cJSON_CreateArray();
      cJSON_AddItemToArray(msg, cJSON_CreateNumber(2));
      cJSON_AddItemToArray(msg, cJSON_CreateString(uuid));
      cJSON_AddItemToArray(msg, cJSON_CreateString(action));
      cJSON_AddItemToArray(msg, payload); // ownership transferred

      char *msg_str = cJSON_PrintUnformatted(msg);
      if (msg_str) {
        ocpp_message(msg_str);
        ESP_LOGI(TAG, "Sent %s: %s", action, msg_str);
        free(msg_str);
      }
      cJSON_Delete(msg);
    } else {
      ESP_LOGW(TAG, "WebSocket not connected, cannot send %s", action);
    }
    xSemaphoreGive(client_mutex);
  } else {
    ESP_LOGW(TAG, "Failed to take mutex while sending OCPP request");
  }
}

void send_firmware_status_notification() {
  const char *status_str = NULL;

  switch (firmware_status) {
  case Downloading:
    status_str = "Downloading";
    break;
  case DownloadFailed:
    status_str = "DownloadFailed";
    break;
  case Downloaded:
    status_str = "Downloaded";
    break;
  case Installing:
    status_str = "Installing";
    break;
  case Installed:
    status_str = "Installed";
    break;
  case InstallationFailed:
    status_str = "InstallationFailed";
    break;
  case Idle:
  default:
    status_str = "Idle";
    break;
  }

  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "status", status_str);

  send_ocpp_request("FirmwareStatusNotification", payload, 0);
}

void handle_update_firmware(cJSON *payload, const char *uniqueId) {

  cJSON *location = cJSON_GetObjectItem(payload, "location");
  if (!cJSON_IsString(location)) {
    ESP_LOGE(TAG, "UpdateFirmware: missing or invalid 'location'");
    send_call_error(uniqueId, "ProtocolError", "Missing or invalid 'location'",
                    NULL);
    return;
  }

  cJSON *json_datetime = cJSON_GetObjectItem(payload, "retrieveDate");
  if (!(cJSON_IsString(json_datetime) &&
        (json_datetime->valuestring != NULL))) {
    return;
    //    ESP_LOGE(
    //        TAG,
    //        "UpdateFirmware: missing or invalid 'retries' or
    //        'retryInterval': %lld", (long long)retrieve_time);
  }

  //  if (now >= retrieve_time) {
  //    printf("Retrieve date reached or passed, proceed with firmware
  //    update.\n");
  //  } else {
  //    double diff = difftime(retrieve_time, now);
  //    printf("Retrieve date not reached yet. Wait %.0f seconds.\n", diff);
  //  }
  cJSON *retries = cJSON_GetObjectItem(payload, "retries");
  cJSON *retryInterval = cJSON_GetObjectItem(payload, "retryInterval");
  if (!(cJSON_IsNumber(retries) && cJSON_IsNumber(retryInterval))) {
    ESP_LOGE(TAG,
             "UpdateFirmware: missing or invalid 'retries'or 'retryInterval'");
    send_call_error(uniqueId, "ProtocolError",
                    "Missing or invalid 'retries'or 'retryInterval", NULL);
    return;
  }
  //  const char *retrieveDate = "2025-10-06T14:50:00.000Z";
  const char *location_str = location->valuestring;
  const char *last_slash = strrchr(location_str, '/');

  if (!last_slash || strlen(last_slash + 1) != 64) {
    ESP_LOGE(TAG, "UpdateFirmware: missing or invalid SHA256 "
                  "checksum in URL");
    send_call_error(uniqueId, "ProtocolError",
                    "Missing or invalid SHA256 checksum", NULL);
    return;
  }

  char ftp_url[FTP_URL_MAX_LEN] = {0};
  char sha256_checksum[HASH_STR_LEN] = {0};
  uint8_t number_of_retry = retries->valueint;
  uint16_t retry_interval = retryInterval->valueint;
  time_t retrieve_time = parse_iso8601(json_datetime->valuestring);

  // Extract FTP URL (everything before last '/')
  size_t url_len = last_slash - location_str;
  if (url_len >= FTP_URL_MAX_LEN)
    url_len = FTP_URL_MAX_LEN - 1;
  safe_strncpy(ftp_url, location_str, url_len + 1);
  // Extract SHA256 checksum (after last '/')
  safe_strncpy(sha256_checksum, last_slash + 1, HASH_STR_LEN);

  ESP_LOGI(TAG, "UpdateFirmware: Parsed FTP URL: %s", ftp_url);
  ESP_LOGI(TAG, "UpdateFirmware: Parsed SHA256: %s", sha256_checksum);

  // Send CALLRESULT to acknowledge UpdateFirmware request
  send_call_result_with_status(uniqueId, "Accepted", "UpdateFirmware");
//TODO:
  station_state = STATION_UPDATE;
  // Start OTA update
  start_ftp_ota_update(ftp_url, sha256_checksum, number_of_retry,
                       retry_interval, retrieve_time);
}

void send_bootnotification() {
  strcat(Esp_ver, Nuv_ver);
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "chargePointVendor", "TuckerMotors");
  cJSON_AddStringToObject(payload, "chargePointModel", "CCS2");
  cJSON_AddStringToObject(payload, "chargePointSerialNumber",
                          device_info_endpoint);
  cJSON_AddStringToObject(payload, "firmwareVersion", Esp_ver);
  cJSON_AddStringToObject(payload, "iccid", "89314404000012345678");
  cJSON_AddStringToObject(payload, "imsi", "310150123456789");
  cJSON_AddStringToObject(payload, "meterType", "ABB B24 112-100");
  cJSON_AddStringToObject(payload, "meterSerialNumber", "MTSN-000982347561");
  if (payload != NULL) {
    send_ocpp_request("BootNotification", payload, 0);
    //          cJSON_Delete(payload);
  }
}

void send_bootnotification_task(void *arg) {
  while (1) {
    if (station_state != STATION_BOOTING) {
      vTaskDelete(NULL);
    }
    send_bootnotification();
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}

void send_heartbeat(void *arg) {
  while (1) {
    cJSON *payload = cJSON_CreateObject();
    if (payload != NULL) {
      send_ocpp_request("Heartbeat", payload, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(heartbeat_interval * 1000));
  }
}
void handle_heartbeat_response(cJSON *payload) {
  cJSON *timeItem = cJSON_GetObjectItem(payload, "currentTime");
  if (timeItem && cJSON_IsString(timeItem)) {
    ESP_LOGI(TAG, "Heartbeat response received. Current time: %s",
             timeItem->valuestring);
    set_time_from_string(timeItem->valuestring);

    if (!obtain_time) {
      xTaskCreate(get_current_iso8601_time, "get_timestamp", 2048, NULL, 5,
                  NULL);
      obtain_time = true;
    }
    time_synced = true;
  }
}
void handle_trigger_message(cJSON *payload, const char *uniqueId) {
  cJSON *msg_item = cJSON_GetObjectItem(payload, "requestedMessage");

  if (!msg_item || !cJSON_IsString(msg_item)) {
    ESP_LOGW(TAG, "TriggerMessage: Invalid or missing 'requestedMessage'");
    send_call_result_with_status(uniqueId, "Rejected", "TriggerMessage");
    return;
  }

  const char *requestedMessage = msg_item->valuestring;
  ESP_LOGI(TAG, "TriggerMessage received: %s", requestedMessage);

  if (strcmp(requestedMessage, "DiagnosticsStatusNotification") == 0) {
    send_call_result_with_status(uniqueId, "NotImplemented", "TriggerMessage");
    return;
  }

  send_call_result_with_status(uniqueId, "Accepted", "TriggerMessage");

  if (strcmp(requestedMessage, "FirmwareStatusNotification") == 0) {
    send_firmware_status_notification();
  } else if (strcmp(requestedMessage, "BootNotification") == 0) {
    send_bootnotification();
  } else if (strcmp(requestedMessage, "StatusNotification") == 0) {
    int connectorId = 1;
    cJSON *connector_item = cJSON_GetObjectItem(payload, "connectorId");
    if (connector_item && cJSON_IsNumber(connector_item)) {
      connectorId = connector_item->valueint;
    }
//    char status[32];
//    if (connectorId == 1)
//      safe_strncpy(status, charger1_status, sizeof(status));
//    else if (connectorId == 2)
//      safe_strncpy(status, charger2_status, sizeof(status));
//    // TODO
    trigger_status_notification(connectorId, connectors[connectorId].charger_current_status, errorCode, VE_code);
  } else if (strcmp(requestedMessage, "Heartbeat") == 0) {
    cJSON *heartbeat_payload = cJSON_CreateObject();
    if (heartbeat_payload != NULL) {
      send_ocpp_request("Heartbeat", heartbeat_payload, 1);
    } else {
      ESP_LOGW(TAG, "Failed to create payload for Heartbeat");
    }
  } else if (strcmp(requestedMessage, "MeterValues") == 0) {
    ocpp_send_meter_values(1);
  } else {
    ESP_LOGW(TAG, "TriggerMessage: Unknown requestedMessage: %s",
             requestedMessage);
  }
}

void handle_ocpp_websocket_message(const char *json_str) {
  cJSON *root = cJSON_Parse(json_str);
  if (!root || !cJSON_IsArray(root)) {
    ESP_LOGE(TAG, "Invalid JSON message format");
    if (root)
      cJSON_Delete(root);
    return;
  }

  cJSON *messageTypeItem = cJSON_GetArrayItem(root, 0);
  if (!cJSON_IsNumber(messageTypeItem)) {
    ESP_LOGE(TAG, "Invalid message type");
    cJSON_Delete(root);
    return;
  }

  int messageTypeId = messageTypeItem->valueint;

  if (messageTypeId == 2) {
    handle_ocpp_call_message(root);
  } else if (messageTypeId == 3) {
    handle_ocpp_call_result(root);
  } else {
    ESP_LOGW(TAG, "Unsupported MessageTypeId: %d", messageTypeId);
  }

  cJSON_Delete(root);
}
