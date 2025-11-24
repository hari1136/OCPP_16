/*
 * ocpp.c
 *
 *  Created on: 16-Sep-2025
 *      Author: Admin
 */

#include "ocpp.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_random.h"
#include "esp_system.h" // for esp_restart()
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdint.h>
#include <string.h>
#include <time.h>

#define TAG "OCPP_Con"

esp_websocket_client_handle_t client = NULL;

static int reserved_id = -1;
time_t reserved_expiry_time = 0;

// SemaphoreHandle_t ocpp_mutex;
SemaphoreHandle_t ocpp_send_mutex;
SemaphoreHandle_t client_mutex;

bool gun1_transaction_active = 0;
bool gun2_transaction_active = 0;

char errorCode[32];

// int connector_id;
char display_time[32];
uint8_t stop_error_code, gun1_state_value, gun2_state_value, VE_code = 0;
bool time_synced = false;

// connector_state_t connector1_state =
//     CONNECTOR_BOOTING; // define the variable here
// connector_state_t connector2_state = CONNECTOR_BOOTING;
station_state_t station_state = STATION_BOOTING;

// int gun1_transaction_id;
// int gun2_transaction_id;
// char gun1_id_tag[20];
// char gun2_id_tag[20];
// char gun1_trans_id_tag[20] = {0};
// char gun2_trans_id_tag[20] = {0};
// char reserved_id_tag[20] = {0};
bool restart = false;
bool obtain_time = false;

time_t ist_now;

uint16_t heartbeat_interval, metervalues_interval = 30;

Connector connectors[NUM_CONNECTORS];

static SemaphoreHandle_t ocpp_con_mutex;

#define MAX_REQUESTS 15
RequestMapEntry request_map[MAX_REQUESTS];
static int next_insert_index = 0;

// static TaskHandle_t metervalues_task_handle = NULL;

void ocpp_init_connectors(void) {
  ocpp_con_mutex = xSemaphoreCreateMutex();

  for (int i = 0; i < NUM_CONNECTORS; i++) {
    connectors[i].connector_id = i + 1;
    connectors[i].transaction_active = false;
    connectors[i].transaction_id = -1;
    connectors[i].state = CONNECTOR_BOOTING;
    connectors[i].id_tag[0] = '\0';
    connectors[i].trans_id_tag[0] = '\0';
    connectors[i].metervalues_task = NULL;
  }
}

void delete_oldest_requests(int count) {
  for (int i = 0; i < count; ++i) {
    int index = (next_insert_index + i) % MAX_REQUESTS;

    if (request_map[index].uuid[0] != '\0') {
      ESP_LOGW(TAG, "Deleting oldest UUID: %s at index %d",
               request_map[index].uuid, index);
      request_map[index].uuid[0] = '\0';
      request_map[index].action[0] = '\0';
    } else {
      ESP_LOGI(TAG, "Slot at index %d is already empty", index);
    }
  }
}

void remember_ocpp_request(const char *uuid, const char *action,
                           int connector_id) {
  // Count how many active requests
  int active_requests = 0;
  for (int i = 0; i < MAX_REQUESTS; ++i) {
    if (request_map[i].uuid[0] != '\0') {
      active_requests++;
    }
  }

  // Optional: delete 5 oldest if full
  if (active_requests >= MAX_REQUESTS) {
    ESP_LOGW(TAG, "Too many active requests (%d). Deleting 5 oldest.",
             active_requests);
    delete_oldest_requests(5);
  }

  // Overwrite the entry at the next insert index
  if (request_map[next_insert_index].uuid[0] != '\0') {
    ESP_LOGW(TAG, "Overwriting UUID: %s at index %d",
             request_map[next_insert_index].uuid, next_insert_index);
  }

  // Store new UUID and action
  safe_strncpy(request_map[next_insert_index].uuid, uuid,
               sizeof(request_map[next_insert_index].uuid));

  safe_strncpy(request_map[next_insert_index].action, action,
               sizeof(request_map[next_insert_index].action));
  request_map[next_insert_index].connector_id = connector_id;
  ESP_LOGI(TAG, "Remembered UUID: %s for action: %s at index %d", uuid, action,
           next_insert_index);

  // Advance the circular pointer
  next_insert_index = (next_insert_index + 1) % MAX_REQUESTS;
}

const char *find_action_for_uuid(const char *uuid) {
  for (int i = 0; i < MAX_REQUESTS; ++i) {
    if (strcmp(request_map[i].uuid, uuid) == 0) {
      return request_map[i].action;
    }
  }
  return NULL; // not found
}

int find_connector_id_for_unique_id(const char *uuid) {
  for (int i = 0; i < MAX_REQUESTS; ++i) {
    if (strcmp(request_map[i].uuid, uuid) == 0) {
      return request_map[i].connector_id;
    }
  }
  return -1; // Not found
}

void remove_ocpp_request(const char *uuid) {
  for (int i = 0; i < MAX_REQUESTS; ++i) {
    if (strcmp(request_map[i].uuid, uuid) == 0) {
      ESP_LOGI(TAG, "Removing request UUID: %s (action=%s, connector=%d)",
               request_map[i].uuid, request_map[i].action,
               request_map[i].connector_id);
      memset(&request_map[i], 0, sizeof(request_map[i]));
      return; // exit immediately
    }
  }
  ESP_LOGW(TAG, "UUID not found in request_map: %s", uuid);
}

void log_request_map(void) {
  ESP_LOGI(TAG, "Current request_map:");
  for (int i = 0; i < MAX_REQUESTS; ++i) {
    if (request_map[i].uuid[0] != '\0') {
      ESP_LOGI(TAG, "[%d] UUID: %s | Action: %s", i, request_map[i].uuid,
               request_map[i].action);
    } else {
      ESP_LOGI(TAG, "[%d] Empty", i);
    }
  }
}

bool save_transaction_id_to_nvs_indexed(int connector_index, int32_t tx_id) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK)
    return false;

  char key[32];
  snprintf(key, sizeof(key), "tx_id_%d", connector_index + 1);

  err = nvs_set_i32(nvs_handle, key, tx_id);
  if (err == ESP_OK)
    err = nvs_commit(nvs_handle);

  nvs_close(nvs_handle);
  return (err == ESP_OK);
}

// Build CALLRESULT

bool load_transaction_id_from_nvs_indexed(int connector_index,
                                          int32_t *out_id) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK)
    return false;

  // Build unique key name for each connector
  char key[32];
  snprintf(key, sizeof(key), "tx_id_%d", connector_index + 1);

  err = nvs_get_i32(nvs_handle, key, out_id);
  nvs_close(nvs_handle);
  return (err == ESP_OK);
}

void delete_transaction_id_from_nvs_indexed(int connector_index) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS for deletion (err=0x%x)", err);
    return;
  }

  // Key based on connector index
  char key[32];
  snprintf(key, sizeof(key), "tx_id_%d", connector_index + 1);

  err = nvs_erase_key(nvs_handle, key);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "%s not found in NVS — nothing to delete.", key);
  } else if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to erase %s from NVS (err=0x%x)", key, err);
    nvs_close(nvs_handle);
    return;
  }

  err = nvs_commit(nvs_handle);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Deleted transaction for connector %d from NVS",
             connector_index + 1);
  } else {
    ESP_LOGE(TAG, "Failed to commit deletion of %s (err=0x%x)", key, err);
  }

  nvs_close(nvs_handle);
}

void trigger_status_notification(int connectorId, const char *status,
                                 const char *errorCode, int vendorErrorCode) {
  cJSON *payload = cJSON_CreateObject();
  if (!payload) {
    ESP_LOGE(TAG, "Failed to create JSON object for StatusNotification");
    return;
  }

  cJSON_AddNumberToObject(payload, "connectorId", connectorId);
  cJSON_AddStringToObject(payload, "status", status);

  // Use "NoError" if errorCode is NULL
  cJSON_AddStringToObject(payload, "errorCode",
                          errorCode ? errorCode : "NoError");

  cJSON_AddStringToObject(payload, "timestamp",
                          timestamp); // Make sure timestamp is set
  cJSON_AddNumberToObject(payload, "vendorErrorCode", vendorErrorCode);

  send_ocpp_request("StatusNotification", payload,
                    connectorId); // Ownership transferred
}

void ocpp_send_authorize(int connector_id, const char *idTag) {
  Connector *conn = &connectors[connector_id - 1];
  strcpy(conn->id_tag, idTag);
  cJSON *payload = cJSON_CreateObject();
  if (!payload) {
    ESP_LOGE(TAG, "JSON creation failed");
    return;
  }
  cJSON_AddStringToObject(payload, "idTag", idTag);
  send_ocpp_request("Authorize", payload, connector_id);
}

void ocpp_send_start_transaction(int connector_id, double meter_start,
                                 const char *idTag) {
  char local_timestamp[32];
  int local_reserved_id = 0;
  bool has_reservation = false;

  xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);
  safe_strncpy(local_timestamp, timestamp, sizeof(local_timestamp));
  if (reserved_expiry_time != 0) {
    has_reservation = true;
    local_reserved_id = reserved_id;
  }
  xSemaphoreGive(ocpp_send_mutex);

  cJSON *payload = cJSON_CreateObject();
  if (!payload) {
    ESP_LOGE(TAG, "JSON creation failed");
    return;
  }
  cJSON_AddNumberToObject(payload, "connectorId", connector_id);
  cJSON_AddStringToObject(payload, "idTag", idTag);
  cJSON_AddNumberToObject(payload, "meterStart", meter_start);
  cJSON_AddStringToObject(payload, "timestamp", local_timestamp);
  if (has_reservation) {
    cJSON_AddNumberToObject(payload, "reservationId", local_reserved_id);
  }

  send_ocpp_request("StartTransaction", payload, connector_id);
}

// Helper to create a sampledValue JSON object and add to array
// Reusable helper to create a sampledValue and add to array
static void add_sampled_value(cJSON *array, const char *value,
                              const char *context, const char *format,
                              const char *measurand, const char *location,
                              const char *unit, const char *phase) {
  if (!array)
    return;

  cJSON *sv = cJSON_CreateObject();
  if (!sv)
    return;

  cJSON_AddStringToObject(sv, "value", value);
  cJSON_AddStringToObject(sv, "context", context);
  cJSON_AddStringToObject(sv, "format", format);
  cJSON_AddStringToObject(sv, "measurand", measurand);
  cJSON_AddStringToObject(sv, "location", location);
  if (unit)
    cJSON_AddStringToObject(sv, "unit", unit);
  if (phase)
    cJSON_AddStringToObject(sv, "phase", phase);

  cJSON_AddItemToArray(array, sv);
}

// Main function to send MeterValues
void ocpp_send_meter_values(int connector_id) {
  const char *ctx = "Sample.Periodic";
  const char *fmt = "Raw";

  cJSON *payload = cJSON_CreateObject();
  if (!payload)
    return;
  if (connector_id < 0 || connector_id >= NUM_CONNECTORS)
    return;
  Connector *conn = &connectors[connector_id];
  cJSON_AddNumberToObject(payload, "connectorId", connector_id);

  // Copy shared data under mutex
  int local_transaction_id = -1;
  char local_timestamp[32];
  char local_meter_values[14][15]; // Snapshot buffer

  xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);
  for (int i = 0; i < 14; ++i) {
    safe_strncpy(local_meter_values[i], conn->MeterValues[i],
                 sizeof(local_meter_values[i]));
  }
  safe_strncpy(local_timestamp, timestamp, sizeof(local_timestamp));
  local_transaction_id = conn->transaction_id;
  xSemaphoreGive(ocpp_send_mutex);

  if (local_transaction_id > 0) {
    cJSON_AddNumberToObject(payload, "transactionId", local_transaction_id);
  }

  cJSON *meter_value_array = cJSON_CreateArray();
  cJSON *meter_value_obj = cJSON_CreateObject();
  cJSON_AddStringToObject(meter_value_obj, "timestamp", local_timestamp);

  cJSON *sampled_value_array = cJSON_CreateArray();

  // Add Sampled Values from snapshot
  add_sampled_value(sampled_value_array, local_meter_values[0], ctx, fmt,
                    "Voltage", "Inlet", "V", "L1");
  add_sampled_value(sampled_value_array, local_meter_values[1], ctx, fmt,
                    "Voltage", "Inlet", "V", "L2");
  add_sampled_value(sampled_value_array, local_meter_values[2], ctx, fmt,
                    "Voltage", "Inlet", "V", "L3");

  add_sampled_value(sampled_value_array, local_meter_values[3], ctx, fmt,
                    "Current.Import", "Inlet", "A", "L1");
  add_sampled_value(sampled_value_array, local_meter_values[4], ctx, fmt,
                    "Current.Import", "Inlet", "A", "L2");
  add_sampled_value(sampled_value_array, local_meter_values[5], ctx, fmt,
                    "Current.Import", "Inlet", "A", "L3");

  add_sampled_value(sampled_value_array, local_meter_values[6], ctx, fmt,
                    "Power.Active.Import", "Inlet", "kW", NULL);
  add_sampled_value(sampled_value_array, local_meter_values[7], ctx, fmt,
                    "Temperature", "Inlet", "Celsius", NULL);

  add_sampled_value(sampled_value_array, local_meter_values[8], ctx, fmt,
                    "Temperature", "Cable", "Celsius", NULL);
  add_sampled_value(sampled_value_array, local_meter_values[9], ctx, fmt,
                    "Voltage", "Outlet", "V", NULL);
  add_sampled_value(sampled_value_array, local_meter_values[10], ctx, fmt,
                    "Current.Import", "Outlet", "A", NULL);
  add_sampled_value(sampled_value_array, local_meter_values[11], ctx, fmt,
                    "Power.Active.Import", "Outlet", "kW", NULL);

  add_sampled_value(sampled_value_array, local_meter_values[12], ctx, fmt,
                    "Energy.Active.Import.Register", "Outlet", "Wh", NULL);
  add_sampled_value(sampled_value_array, local_meter_values[13], ctx, fmt,
                    "SoC", "EV", "Percent", NULL);

  // Final payload structure
  cJSON_AddItemToObject(meter_value_obj, "sampledValue", sampled_value_array);
  cJSON_AddItemToArray(meter_value_array, meter_value_obj);
  cJSON_AddItemToObject(payload, "meterValue", meter_value_array);

  send_ocpp_request("MeterValues", payload, connector_id);

  // Optionally free cJSON if send_ocpp_request doesn't
  // cJSON_Delete(payload);
}

void metervalues(void *arg) {
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(metervalues_interval * 1000));
    for (int i = 0; i < NUM_CONNECTORS; i++) {
      if (connectors[i].transaction_active) {
        ocpp_send_meter_values(connectors[i].connector_id);
      }
    }
  }
}

void ocpp_send_stop_transaction(int connector_id, double meter_stop,
                                const char *reason) {

  if (connector_id < 1 || connector_id > NUM_CONNECTORS) {
    ESP_LOGW(TAG, "Invalid connector_id: %d", connector_id);
    return;
  }

  Connector *conn = &connectors[connector_id - 1];

  if (!conn) {
    ESP_LOGW(TAG, "Connector pointer is NULL");
    return;
  }

  xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);
  if (!conn->transaction_active || conn->transaction_id <= 0) {
    ESP_LOGW(TAG, "No active transaction on connector %d", conn->connector_id);
    xSemaphoreGive(ocpp_send_mutex);
    return;
  }
  int local_transaction_id = conn->transaction_id;
  xSemaphoreGive(ocpp_send_mutex);
  // Create JSON payload
  cJSON *payload = cJSON_CreateObject();
  if (!payload) {
    ESP_LOGE(TAG, "Failed to allocate StopTransaction JSON payload");
    return;
  }
  cJSON_AddNumberToObject(payload, "transactionId", local_transaction_id);
  cJSON_AddNumberToObject(payload, "meterStop", meter_stop);
  cJSON_AddStringToObject(payload, "timestamp", timestamp);
  cJSON_AddStringToObject(payload, "reason",
                          reason && *reason ? reason : "Local");
  cJSON_AddStringToObject(payload, "idTag", conn->id_tag);

  ESP_LOGI(TAG, "StopTransaction: conn=%d, tx=%d, meter=%.2f, reason=%s",
           conn->connector_id, local_transaction_id, meter_stop,
           reason && *reason ? reason : "Local");

  send_ocpp_request("StopTransaction", payload, conn->connector_id);
  cJSON_Delete(payload);
}

// Helper to build and send CALLRESULT with a status field
void handle_remote_stop_transaction(cJSON *call_payload, const char *uniqueId) {
  if (!call_payload || !uniqueId) {
    ESP_LOGE(TAG, "Invalid input to handle_remote_stop_transaction");
    return;
  }

  // Extract transactionId
  cJSON *tx_item = cJSON_GetObjectItem(call_payload, "transactionId");
  if (!cJSON_IsNumber(tx_item)) {
    ESP_LOGW(TAG, "RemoteStopTransaction missing or invalid transactionId");
    send_call_result_with_status(uniqueId, "Rejected", "RemoteStopTransaction");
    return;
  }

  int requested_tx_id = tx_item->valueint;
  ESP_LOGI(TAG, "RemoteStopTransaction received: transactionId=%d",
           requested_tx_id);

  bool accepted = false;

  xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);

  // Find which connector has this transaction
  for (int i = 0; i < NUM_CONNECTORS; i++) {
    if (connectors[i].transaction_active &&
        connectors[i].transaction_id == requested_tx_id) {
      connectors[i].state = CONNECTOR_FINISHING;
      ESP_LOGI(TAG, "Stopping connector %d (transaction ID %d)",
               connectors[i].connector_id, requested_tx_id);
      accepted = true;
      break;
    }
  }

  xSemaphoreGive(ocpp_send_mutex);

  // Respond to CSMS
  send_call_result_with_status(uniqueId, accepted ? "Accepted" : "Rejected",
                               "RemoteStopTransaction");
}

void handle_remote_start_transaction(cJSON *call_payload,
                                     const char *uniqueId) {
  cJSON *connectorId_item = cJSON_GetObjectItem(call_payload, "connectorId");
  cJSON *idTag_item = cJSON_GetObjectItem(call_payload, "idTag");

  if (!connectorId_item || !cJSON_IsNumber(connectorId_item) || !idTag_item ||
      !cJSON_IsString(idTag_item)) {
    ESP_LOGW(TAG,
             "RemoteStartTransaction missing or invalid connectorId/idTag");
    send_call_result_with_status(uniqueId, "Rejected",
                                 "RemoteStartTransaction");
    return;
  }
  int connectorId = connectorId_item->valueint;
  const char *idTag = idTag_item->valuestring;
  bool accepted = false;
  xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);
  // Find the requested connector
  Connector *target = NULL;
  for (int i = 0; i < NUM_CONNECTORS; i++) {
    if (connectors[i].connector_id == connectorId) {
      target = &connectors[i];
      break;
    }
  }

  if (target) {
    accepted = (target->state == CONNECTOR_AVAILABLE);
  } else {
    ESP_LOGW(TAG, "ConnectorId %d not found", connectorId);
  }

  xSemaphoreGive(ocpp_send_mutex);

  ESP_LOGI(TAG, "RemoteStartTransaction received: connectorId=%d, idTag=%s",
           connectorId, idTag);

  send_call_result_with_status(uniqueId, accepted ? "Accepted" : "Rejected",
                               "RemoteStartTransaction");
  connector_t *gun = &guns[connectorId];

  if (accepted && target) {
    if (authorize_remote) {
      ocpp_send_authorize(connectorId, idTag);
      safe_strncpy(gun->id_tag, idTag, sizeof(gun->id_tag));
    } else {
      xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);
      target->state = CONNECTOR_AUTHORIZED;
      safe_strncpy(target->trans_id_tag, idTag, sizeof(target->trans_id_tag));
      xSemaphoreGive(ocpp_send_mutex);
    }
  } else if (!accepted) {
    ESP_LOGW(TAG,
             "RemoteStartTransaction rejected for connectorId=%d, state=%d",
             connectorId, target ? target->state : -1);
  }
}

void handle_reset_request(cJSON *payload, const char *uniqueId) {
  cJSON *typeItem = cJSON_GetObjectItem(payload, "type");
  if (!typeItem || !cJSON_IsString(typeItem)) {
    ESP_LOGW(TAG, "Reset request missing or invalid type");
    send_call_result_with_status(uniqueId, "Rejected", "Reset");
    return;
  }
  const char *type = typeItem->valuestring;
  send_call_result_with_status(uniqueId, "Accepted", "Reset");
  vTaskDelay(pdMS_TO_TICKS(1000));
  ESP_LOGI(TAG, "Received Reset request: type=%s", type);
  xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);
  for (int i = 0; i < NUM_CONNECTORS; i++) {
    if (connectors[i].transaction_active) {
      if (strcmp(type, "Soft") == 0) {
        stop_error_code = 9;
        connectors[i].state = CONNECTOR_FINISHING;
        ESP_LOGI(TAG, "Soft reset: connector %d finishing active transaction",
                 connectors[i].connector_id);
      } else if (strcmp(type, "Hard") == 0) {
        stop_error_code = 4;
        connectors[i].state = CONNECTOR_FINISHING;
        restart = true;
        ESP_LOGI(TAG, "Hard reset: connector %d finishing and restarting",
                 connectors[i].connector_id);
      }
    }
  }

  xSemaphoreGive(ocpp_send_mutex);
  if (strcmp(type, "Hard") == 0 && !restart) {
    ESP_LOGI(TAG, "Performing hard reset now");
    esp_restart();
  }
}

void handle_reserve_now(cJSON *payload, const char *uniqueId) {
  cJSON *connectorIdItem = cJSON_GetObjectItem(payload, "connectorId");
  cJSON *expiryDateItem = cJSON_GetObjectItem(payload, "expiryDate");
  cJSON *idTagItem = cJSON_GetObjectItem(payload, "idTag");
  cJSON *reservationIdItem = cJSON_GetObjectItem(payload, "reservationId");

  if (!connectorIdItem || !cJSON_IsNumber(connectorIdItem) || !expiryDateItem ||
      !cJSON_IsString(expiryDateItem) || !idTagItem ||
      !cJSON_IsString(idTagItem) || !reservationIdItem ||
      !cJSON_IsNumber(reservationIdItem)) {
    ESP_LOGW(TAG, "Invalid ReserveNow payload");
    send_call_result_with_status(uniqueId, "Rejected", "ReserveNow");
    return;
  }

  int connectorId = connectorIdItem->valueint;
  const char *expiry = expiryDateItem->valuestring;
  const char *idTag = idTagItem->valuestring;
  int reservationId = reservationIdItem->valueint;
  const char *status = "Rejected"; // default fallback

  // Find matching connector
  Connector *target = NULL;
  for (int i = 0; i < NUM_CONNECTORS; i++) {
    if (connectors[i].connector_id == connectorId) {
      target = &connectors[i];
      break;
    }
  }

  if (!target) {
    ESP_LOGW(TAG, "ReserveNow: Unknown connectorId=%d", connectorId);
    send_call_result_with_status(uniqueId, "Rejected", "ReserveNow");
    return;
  }

  // Decide status based on connector state
  switch (target->state) {
  case CONNECTOR_AVAILABLE:
    status = "Accepted";
    break;
  case CONNECTOR_FAULTED:
    status = "Faulted";
    break;
  case CONNECTOR_CHARGING:
  case CONNECTOR_RESERVED:
  case CONNECTOR_PREPARING:
    status = "Occupied";
    break;
  case CONNECTOR_UNAVAILABLE:
    status = "Unavailable";
    break;
  default:
    status = "Rejected";
    break;
  }

  ESP_LOGI(TAG,
           "ReserveNow: reservationId=%d, connectorId=%d, idTag=%s, expiry=%s, "
           "status=%s",
           reservationId, connectorId, idTag, expiry, status);

  send_call_result_with_status(uniqueId, status, "ReserveNow");

  // Save reservation details if accepted
  if (strcmp(status, "Accepted") == 0) {
    xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);
    safe_strncpy(target->reserved_id_tag, idTag,
                 sizeof(target->reserved_id_tag));
    target->reserved_expiry_time = parse_iso8601(expiry);
    target->reserved_id = reservationId;
    target->state = CONNECTOR_RESERVED;
    xSemaphoreGive(ocpp_send_mutex);
  }
}

void handle_cancel_reservation(cJSON *payload, const char *uniqueId) {
  cJSON *reservationIdItem = cJSON_GetObjectItem(payload, "reservationId");

  if (!reservationIdItem || !cJSON_IsNumber(reservationIdItem)) {
    ESP_LOGW(TAG, "CancelReservation: Invalid payload");
    send_call_result_with_status(uniqueId, "Rejected", "CancelReservation");
    return;
  }

  int reservationId = reservationIdItem->valueint;

  ESP_LOGI(TAG, "CancelReservation received: reservationId=%d", reservationId);

  // If cancellation is successful:
  xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);
  for (int i = 0; i < NUM_CONNECTORS; i++) {
    Connector *conn = &connectors[i];
    if (conn->reserved_id == reservationId) {
      conn->reserved_id = 0;
      conn->reserved_expiry_time = 0;
      conn->state = CONNECTOR_AVAILABLE;
      send_call_result_with_status(uniqueId, "Accepted", "CancelReservation");
      xSemaphoreGive(ocpp_send_mutex);
      return;
    }
  }
  send_call_result_with_status(uniqueId, "Rejected", "CancelReservation");
  xSemaphoreGive(ocpp_send_mutex);
  // Optionally update internal reservation state
  // e.g., clear reservation data, reset connector state, etc.
}

void handle_change_availability(cJSON *payload, const char *uniqueId) {
  cJSON *connector_item = cJSON_GetObjectItem(payload, "connectorId");
  cJSON *type_item = cJSON_GetObjectItem(payload, "type");

  if (!connector_item || !cJSON_IsNumber(connector_item) || !type_item ||
      !cJSON_IsString(type_item)) {
    ESP_LOGW(TAG, "Invalid ChangeAvailability payload");
    send_call_result_with_status(uniqueId, "Rejected", "ChangeAvailability");
    return;
  }

  int connectorId = connector_item->valueint;
  const char *type = type_item->valuestring;
  bool is_inoperative = (strcmp(type, "Inoperative") == 0);
  const char *status = "Accepted";

  ESP_LOGI(TAG, "ChangeAvailability received: connectorId=%d, type=%s",
           connectorId, type);

  // If 0, apply to all connectors
  if (connectorId == 0) {
    for (int i = 0; i < NUM_CONNECTORS; i++) {
      Connector *conn = &connectors[i];
      if (conn->transaction_active) {
        status = "Scheduled";
        ESP_LOGI(TAG, "Connector %d busy — scheduling availability change",
                 conn->connector_id);
        // You can mark a flag to apply after transaction ends
        continue;
      }

      if (is_inoperative) {
        if (conn->state != CONNECTOR_UNAVAILABLE) {
          conn->state = CONNECTOR_UNAVAILABLE;
          ESP_LOGI(TAG, "Connector %d set to UNAVAILABLE", conn->connector_id);
        } else {
          status = "Rejected";
        }
      } else { // Operative
        if (conn->state == CONNECTOR_UNAVAILABLE) {
          conn->state = CONNECTOR_AVAILABLE;
          ESP_LOGI(TAG, "Connector %d set to AVAILABLE", conn->connector_id);
        } else {
          status = "Rejected";
        }
      }
    }
  } else {
    // Find the matching connector
    Connector *conn = NULL;
    for (int i = 0; i < NUM_CONNECTORS; i++) {
      if (connectors[i].connector_id == connectorId) {
        conn = &connectors[i];
        break;
      }
    }
    if (!conn) {
      ESP_LOGW(TAG, "Invalid connectorId: %d", connectorId);
      status = "Rejected";
    } else if (conn->transaction_active) {
      status = "Scheduled";
      ESP_LOGI(TAG, "Transaction active on connector %d — scheduling change",
               connectorId);
    } else {
      if (is_inoperative) {
        if (conn->state != CONNECTOR_UNAVAILABLE) {
          conn->state = CONNECTOR_UNAVAILABLE;
        } else {
          status = "Rejected";
        }
      } else { // Operative
        if (conn->state == CONNECTOR_UNAVAILABLE) {
          conn->state = CONNECTOR_AVAILABLE;
        } else {
          status = "Rejected";
        }
      }
    }
  }
  send_call_result_with_status(uniqueId, status, "ChangeAvailability");
}

void handle_get_locallistVersion(cJSON *payload, const char *uniqueId) {
  int local_list_version =
      -1; // <-- Replace with your actual version logic if needed

  cJSON *response = cJSON_CreateObject();
  if (!response) {
    ESP_LOGE(TAG, "GetLocalListVersion: Failed to create response object");
    send_call_error(uniqueId, "InternalError", "Failed to create response",
                    NULL);
    return;
  }
  cJSON_AddNumberToObject(response, "listVersion", local_list_version);

  send_call_result_with_payload(uniqueId, response, "GetLocalListVersion");
  // Allow client to receive response before reset
}

void handle_ocpp_call_message(cJSON *root) {
  const char *uniqueId = cJSON_GetArrayItem(root, 1)->valuestring;
  const char *action = cJSON_GetArrayItem(root, 2)->valuestring;
  cJSON *call_payload = cJSON_GetArrayItem(root, 3);

  if (!uniqueId || !action || !call_payload) {
    ESP_LOGE(TAG, "Missing fields in CALL message");
    return;
  }

  if (strcmp(action, "RemoteStartTransaction") == 0) {
    handle_remote_start_transaction(call_payload, uniqueId);
  } else if (strcmp(action, "RemoteStopTransaction") == 0) {
    handle_remote_stop_transaction(call_payload, uniqueId);
  } else if (strcmp(action, "ReserveNow") == 0) {
    handle_reserve_now(call_payload, uniqueId);
  } else if (strcmp(action, "CancelReservation") == 0) {
    handle_cancel_reservation(call_payload, uniqueId);
  } else if (strcmp(action, "Reset") == 0) {
    handle_reset_request(call_payload, uniqueId);
  } else if (strcmp(action, "UnlockConnector") == 0) {
    send_call_result_with_status(uniqueId, "NotSupported", "UnlockConnector");
  } else if (strcmp(action, "ChangeAvailability") == 0) {
    handle_change_availability(call_payload, uniqueId);
  } else if (strcmp(action, "UpdateFirmware") == 0) {
    handle_update_firmware(call_payload, uniqueId);
  } else if (strcmp(action, "TriggerMessage") == 0) {
    handle_trigger_message(call_payload, uniqueId);
  } else if (strcmp(action, "GetConfiguration") == 0) {
    handle_get_configuration(call_payload, uniqueId);
  } else if (strcmp(action, "ChangeConfiguration") == 0) {
    handle_change_configuration(call_payload, uniqueId);
  } else if (strcmp(action, "GetLocalListVersion") == 0) {
    handle_get_locallistVersion(call_payload, uniqueId);
  } else if (strcmp(action, "ClearCache") == 0) {
    send_call_result_with_status(uniqueId, "Rejected", "ClearCache");
  } else if (strcmp(action, "SendLocalList") == 0) {
    send_call_result_with_status(uniqueId, "NotSupported", "SendLocalList");
  } else if (strcmp(action, "GetCompositeSchedule") == 0) {
    handle_get_composite_schedule(call_payload, uniqueId);
  } else if (strcmp(action, "ClearChargingProfile") == 0) {
    handle_clear_charging_profile(call_payload, uniqueId);
  } else if (strcmp(action, "SetChargingProfile") == 0) {
    handle_set_charging_profile(call_payload, uniqueId);
  } else {
    ESP_LOGW(TAG, "Unhandled CALL action: %s", action);
  }

  remove_ocpp_request(uniqueId);
  log_request_map();
}

void handle_boot_notification_response(cJSON *payload) {
  if (!payload) {
    ESP_LOGE(TAG, "BootNotification response payload is NULL");
    return;
  }

  cJSON *statusItem = cJSON_GetObjectItem(payload, "status");
  if (!cJSON_IsString(statusItem)) {
    ESP_LOGW(TAG, "BootNotification response missing or invalid status");
    return;
  }

  ESP_LOGI(TAG, "BootNotification response: %s", statusItem->valuestring);

  // If not accepted, stop here
  if (strcmp(statusItem->valuestring, "Accepted") != 0) {
    ESP_LOGW(TAG, "BootNotification rejected");
    return;
  }

  // --- Handle accepted BootNotification ---
  cJSON *timeItem = cJSON_GetObjectItem(payload, "currentTime");
  if (cJSON_IsString(timeItem)) {
    ESP_LOGI(TAG, "BootNotification current time: %s", timeItem->valuestring);
    set_time_from_string(timeItem->valuestring);
    time_synced = true;

    if (!obtain_time) {
      xTaskCreate(get_current_iso8601_time, "get_timestamp", 4096, NULL, 5,
                  NULL);
      obtain_time = true;
    }
  }

  cJSON *intervalItem = cJSON_GetObjectItem(payload, "interval");
  if (cJSON_IsNumber(intervalItem)) {
    heartbeat_interval = intervalItem->valueint;
    ESP_LOGI(TAG, "Heartbeat interval set to %d seconds", heartbeat_interval);
  }

  // Update system states
  station_state = STATION_AVAILABLE;
  xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);
  for (int i = 0; i < NUM_CONNECTORS; i++) {
    connectors[i].state = CONNECTOR_AVAILABLE;
    connectors[i].transaction_active = false;
    connectors[i].transaction_id = 0;
  }
  //  firmware_status = Idle;
  xSemaphoreGive(ocpp_send_mutex);
  // Try to restore transactions (if they exist)
  for (int i = 0; i < NUM_CONNECTORS; i++) {
    int32_t tx_id;
    if (load_transaction_id_from_nvs_indexed(i, &tx_id)) {
      xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);
      connectors[i].transaction_id = tx_id;
      connectors[i].transaction_active = true;
      connectors[i].state = CONNECTOR_FINISHING;
      stop_error_code = 6;
      xSemaphoreGive(ocpp_send_mutex);

      ESP_LOGI(TAG, "Restored transaction %" PRId32 " on connector %d", tx_id,
               i + 1);
    }
  }

  // Start heartbeat task
  xTaskCreate(send_heartbeat, "heartbeat_task", 4096, NULL, 1, NULL);
}

void handle_authorize_response(cJSON *payload, int connector_id) {
  Connector *conn = &connectors[connector_id];
  connector_t *gun = &guns[connector_id];
  cJSON *idTagInfo = cJSON_GetObjectItem(payload, "idTagInfo");

  if (!idTagInfo) {
    ESP_LOGW(TAG, "Authorize rejected (no idTagInfo) for connector %d",
             conn->connector_id);
    conn->state = CONNECTOR_AUTHORIZE_FAILED;
    return;
  }

  cJSON *statusItem = cJSON_GetObjectItem(idTagInfo, "status");
  if (!statusItem || !cJSON_IsString(statusItem)) {
    ESP_LOGW(TAG, "Authorize rejected (invalid status) for connector %d",
             conn->connector_id);
    conn->state = CONNECTOR_AUTHORIZE_FAILED;
    return;
  }

  ESP_LOGI(TAG, "Authorize response for connector %d: %s", conn->connector_id,
           statusItem->valuestring);

  if (strcmp(statusItem->valuestring, "Accepted") == 0) {
    if (!conn->transaction_active) {
      conn->state = CONNECTOR_AUTHORIZED;
      xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);
      safe_strncpy(conn->trans_id_tag, gun->id_tag, sizeof(conn->trans_id_tag));
      xSemaphoreGive(ocpp_send_mutex);
    } else {
      conn->state = CONNECTOR_FINISHING;
    }
  } else {
    ESP_LOGW(TAG, "Authorize rejected for connector %d", conn->connector_id);
    conn->state = CONNECTOR_AUTHORIZE_FAILED;
  }
}

void handle_start_transaction_response(cJSON *payload, int connector_id) {
  Connector *conn = &connectors[connector_id - 1];
  cJSON *tid = cJSON_GetObjectItem(payload, "transactionId");

  if (!tid || !cJSON_IsNumber(tid)) {
    ESP_LOGW(TAG, "StartTransaction response invalid for connector %d",
             conn->connector_id);
    return;
  }

  xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);

  conn->transaction_id = tid->valueint;
  conn->transaction_active = true;
  conn->state = CONNECTOR_CHARGING;

  // Clear reservation info if any
  reserved_expiry_time = 0;
  reserved_id = 0;

  ESP_LOGI(TAG, "Transaction started on connector %d, ID: %d",
           conn->connector_id, conn->transaction_id);

  save_transaction_id_to_nvs_indexed(conn->connector_id, conn->transaction_id);

  xSemaphoreGive(ocpp_send_mutex);

  // Start metervalues task
  xTaskCreate(metervalues, "metervalues_task", 4096, conn, 3,
              &conn->metervalues_task);
}

void handle_stop_transaction_response(cJSON *payload, int connector_id) {
  Connector *conn = &connectors[connector_id - 1];
  ESP_LOGI(TAG, "Transaction stopped on connector %d", conn->connector_id);
  xSemaphoreTake(ocpp_send_mutex, portMAX_DELAY);
  // Reset transaction info
  delete_transaction_id_from_nvs_indexed(conn->connector_id);
  conn->transaction_active = false;
  conn->transaction_id = -1;
  memset(conn->id_tag, 0, sizeof(conn->id_tag));
  memset(conn->trans_id_tag, 0, sizeof(conn->trans_id_tag));
  conn->state = CONNECTOR_AVAILABLE;
  xSemaphoreGive(ocpp_send_mutex);
  // Delete metervalues task if running
  if (conn->metervalues_task != NULL) {
    vTaskDelete(conn->metervalues_task);
    conn->metervalues_task = NULL;
  }

  if (restart == true) {
    esp_restart();
  }
}

void handle_ocpp_call_result(cJSON *root) {
  const char *uniqueId = cJSON_GetArrayItem(root, 1)->valuestring;
  cJSON *payload = cJSON_GetArrayItem(root, 2);

  if (!uniqueId || !payload) {
    ESP_LOGE(TAG, "Missing fields in CALLRESULT message");
    return;
  }

  const char *action = find_action_for_uuid(uniqueId);
  if (!action) {
    ESP_LOGW(TAG, "CALLRESULT received but no matching action found");
    return;
  }
  int connector_id = find_connector_id_for_unique_id(uniqueId);

  if (strcmp(action, "BootNotification") == 0) {
    handle_boot_notification_response(payload);
  } else if (strcmp(action, "Authorize") == 0) {
    handle_authorize_response(payload, connector_id);
  } else if (strcmp(action, "StartTransaction") == 0) {
    handle_start_transaction_response(payload, connector_id);
  } else if (strcmp(action, "StopTransaction") == 0) {
    handle_stop_transaction_response(payload, connector_id);
  } else if (strcmp(action, "Heartbeat") == 0) {
    handle_heartbeat_response(payload);
  } else if (strcmp(action, "StatusNotification") == 0) {
    ESP_LOGI(TAG, "StatusNotification acknowledged");
  } else if (strcmp(action, "MeterValues") == 0) {
    ESP_LOGI(TAG, "MeterValues acknowledged");
  } else {
    ESP_LOGW(TAG, "Unknown action in CALLRESULT: %s", action);
  }

  remove_ocpp_request(uniqueId);
  log_request_map();
}
