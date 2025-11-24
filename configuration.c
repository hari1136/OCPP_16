/*
 * configuration.c
 *
 *  Created on: 07-Oct-2025
 *      Author: Admin
 */

#include "cJSON.h"
#include "ocpp.h"
#include <stdbool.h>
#include <stdint.h>
#define TAG "configure"

uint16_t max_charge_profile = 23, charging_schedule_max_period = 5,
         number_of_connectors = 2, transaction_retry_interval = 120,
         transaction_attempts = 10, getconfiguration_maxkeys = 45,
         resetretries = 3;
uint16_t charge_profile_max_stack = 4, local_list_max = 20, auth_list_max = 20,
         connection_timeout = 120, websocket_ping = 60;
char AllowedChargingRateUnit = 'W';

char *SupportedFeatureProfiles =
         "Core,FirmwareManagement,Reservation,RemoteTrigger,Smart Charging",
     *meterdata = "Energy.Active.Import.Register,Current.Import,Voltage,Power."
                  "Active.Import,SoC";

bool authEnabled = true, authDisabled = false;
bool authorize_remote = true, local_authorize_offline = true,
     local_pre_authorize = true, stop_on_invalidId = true,
     local_list_enabled = true;

typedef enum {
  CONFIG_VALUE_TYPE_NUMBER,
  CONFIG_VALUE_TYPE_BOOL,
  CONFIG_VALUE_TYPE_STRING
} config_value_type_t;

typedef enum { CONFIG_TYPE_BOOL, CONFIG_TYPE_INT } ConfigType;

typedef struct {
  const char *key;
  void *target;
  ConfigType type;
} ConfigEntry;

ConfigEntry config_registry[] = {
    {"AuthorizeRemoteTxRequests", &authorize_remote, CONFIG_TYPE_BOOL},
    {"LocalAuthorizeOffline", &local_authorize_offline, CONFIG_TYPE_BOOL},
    {"LocalPreAuthorize", &local_pre_authorize, CONFIG_TYPE_BOOL},
    {"StopTransactionOnInvalidId", &stop_on_invalidId, CONFIG_TYPE_BOOL},
    {"LocalAuthListEnabled", &local_list_enabled, CONFIG_TYPE_BOOL},
    {"HeartbeatInterval", &heartbeat_interval, CONFIG_TYPE_INT},
    {"ConnectionTimeOut", &connection_timeout, CONFIG_TYPE_INT},
    {"MeterValueSampleInterval", &metervalues_interval, CONFIG_TYPE_INT},
    {"NumberOfConnectors", &number_of_connectors, CONFIG_TYPE_INT},
    {"ResetRetries", &resetretries, CONFIG_TYPE_INT},
    {"TransactionMessageAttempts", &transaction_attempts, CONFIG_TYPE_INT},
    {"TransactionMessageRetryInterval", &transaction_retry_interval,
     CONFIG_TYPE_INT},
    {"WebSocketPingInterval", &websocket_ping, CONFIG_TYPE_INT},
};

void send_call_result_with_payload(const char *uniqueId, cJSON *payload,
                                   const char *action) {
  if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (esp_websocket_client_is_connected(client)) {
      cJSON *response = cJSON_CreateArray();
      cJSON_AddItemToArray(response, cJSON_CreateNumber(3)); // CALLRESULT
      cJSON_AddItemToArray(response, cJSON_CreateString(uniqueId));
      cJSON_AddItemToArray(response,
                           cJSON_Duplicate(payload, 1)); // deep copy payload

      char *resp_str = cJSON_PrintUnformatted(response);
      if (resp_str) {
        ocpp_message(resp_str);
        ESP_LOGI(TAG, "Sent CALLRESULT for %s: %s", action, resp_str);
        free(resp_str);
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

cJSON *create_config_item(const char *key, void *value,
                          config_value_type_t type, bool readonly) {
  cJSON *configItem = cJSON_CreateObject();
  if (!configItem)
    return NULL;

  cJSON_AddStringToObject(configItem, "key", key);

  switch (type) {
  case CONFIG_VALUE_TYPE_NUMBER:
    cJSON_AddNumberToObject(configItem, "value", *((uint16_t *)value));
    break;
  case CONFIG_VALUE_TYPE_BOOL:
    cJSON_AddBoolToObject(configItem, "value", *((bool *)value));
    break;
  case CONFIG_VALUE_TYPE_STRING:
    cJSON_AddStringToObject(configItem, "value", (const char *)value);
    break;
  default:
    cJSON_Delete(configItem);
    return NULL;
  }

  cJSON_AddBoolToObject(configItem, "readonly", readonly);
  return configItem;
}

void handle_get_configuration(cJSON *payload, const char *uniqueId) {
  cJSON *keyArray = cJSON_GetObjectItem(payload, "key");
  cJSON *configKeyArray = cJSON_CreateArray();
  cJSON *unknownKeyArray = cJSON_CreateArray();

  if (!keyArray || !cJSON_IsArray(keyArray)) {
    ESP_LOGW(TAG, "GetConfiguration: No 'key' array provided, return all "
                  "config keys (not implemented)");
    send_call_error(uniqueId, "NotSupported",
                    "Full config listing not supported yet", NULL);
    cJSON_Delete(configKeyArray);
    cJSON_Delete(unknownKeyArray);
    return;
  }

  cJSON *keyItem = NULL;
  cJSON_ArrayForEach(keyItem, keyArray) {
    if (!cJSON_IsString(keyItem)) {
      continue;
    }

    const char *keyName = keyItem->valuestring;
    cJSON *item = NULL;
    if (strcmp(keyName, "AuthorizeRemoteTxRequests") == 0) {
      item = create_config_item("AuthorizeRemoteTxRequests", &authorize_remote,
                                CONFIG_VALUE_TYPE_BOOL, false);
    } else if (strcmp(keyName, "ClockAlignedDataInterval") == 0) {
      item = create_config_item("ClockAlignedDataInterval", &authDisabled,
                                CONFIG_VALUE_TYPE_NUMBER, true);
    } else if (strcmp(keyName, "ConnectionTimeOut") == 0) {
      item = create_config_item("ConnectionTimeOut", &connection_timeout,
                                CONFIG_VALUE_TYPE_NUMBER, false);
    } else if (strcmp(keyName, "GetConfigurationMaxKeys") == 0) {
      item = create_config_item("GetConfigurationMaxKeys",
                                &getconfiguration_maxkeys,
                                CONFIG_VALUE_TYPE_NUMBER, true);
    } else if (strcmp(keyName, "HeartbeatInterval") == 0) {
      item = create_config_item("HeartbeatInterval", &heartbeat_interval,
                                CONFIG_VALUE_TYPE_NUMBER, false);
    } else if (strcmp(keyName, "LocalAuthorizeOffline") == 0) {
      item = create_config_item("LocalAuthorizeOffline", &authDisabled,
                                CONFIG_VALUE_TYPE_BOOL, false);
    } else if (strcmp(keyName, "LocalPreAuthorize") == 0) {
      item = create_config_item("LocalPreAuthorize", &authDisabled,
                                CONFIG_VALUE_TYPE_BOOL, false);
    } else if (strcmp(keyName, "MeterValuesAlignedData") == 0) {
      item = create_config_item("MeterValuesAlignedData", &authDisabled,
                                CONFIG_VALUE_TYPE_NUMBER, true);
    } else if (strcmp(keyName, "MeterValuesSampledData") == 0) {
      item = create_config_item("MeterValuesSampledData", meterdata,
                                CONFIG_VALUE_TYPE_STRING, true);
    } else if (strcmp(keyName, "MeterValueSampleInterval") == 0) {
      item =
          create_config_item("MeterValueSampleInterval", &metervalues_interval,
                             CONFIG_VALUE_TYPE_NUMBER, false);
    } else if (strcmp(keyName, "NumberOfConnectors") == 0) {
      item = create_config_item("NumberOfConnectors", &number_of_connectors,
                                CONFIG_VALUE_TYPE_NUMBER, true);
    } else if (strcmp(keyName, "ResetRetries") == 0) {
      item = create_config_item("ResetRetries", &resetretries,
                                CONFIG_VALUE_TYPE_NUMBER, false);
    } else if (strcmp(keyName, "ConnectorPhaseRotation") == 0) {
      item = create_config_item("ConnectorPhaseRotation", "NotApplicable",
                                CONFIG_VALUE_TYPE_STRING, true);
    } else if (strcmp(keyName, "StopTransactionOnEVSideDisconnect") == 0) {
      item = create_config_item("StopTransactionOnEVSideDisconnect",
                                &authEnabled, CONFIG_VALUE_TYPE_BOOL, true);
    } else if (strcmp(keyName, "StopTransactionOnInvalidId") == 0) {
      item = create_config_item("StopTransactionOnInvalidId", &authDisabled,
                                CONFIG_VALUE_TYPE_BOOL, false);
    } else if (strcmp(keyName, "StopTxnAlignedData") == 0) {
      item = create_config_item("StopTxnAlignedData", &authDisabled,
                                CONFIG_VALUE_TYPE_NUMBER, true);
    } else if (strcmp(keyName, "StopTxnSampledData") == 0) {
      item = create_config_item("StopTxnSampledData",
                                "Energy.Active.Import.Register",
                                CONFIG_VALUE_TYPE_STRING, true);
    } else if (strcmp(keyName, "SupportedFeatureProfiles") == 0) {
      item = create_config_item("SupportedFeatureProfiles",
                                SupportedFeatureProfiles,
                                CONFIG_VALUE_TYPE_STRING, true);
    } else if (strcmp(keyName, "TransactionMessageAttempts") == 0) {
      item = create_config_item("TransactionMessageAttempts",
                                &transaction_attempts, CONFIG_VALUE_TYPE_NUMBER,
                                false);
    } else if (strcmp(keyName, "TransactionMessageRetryInterval") == 0) {
      item = create_config_item("TransactionMessageRetryInterval",
                                &transaction_retry_interval,
                                CONFIG_VALUE_TYPE_NUMBER, false);
    } else if (strcmp(keyName, "UnlockConnectorOnEVSideDisconnect") == 0) {
      item = create_config_item("UnlockConnectorOnEVSideDisconnect",
                                &authDisabled, CONFIG_VALUE_TYPE_BOOL, true);
    } else if (strcmp(keyName, "WebSocketPingInterval") == 0) {
      item = create_config_item("WebSocketPingInterval", &websocket_ping,
                                CONFIG_VALUE_TYPE_NUMBER, false);
    } else if (strcmp(keyName, "LocalAuthListEnabled") == 0) {
      item = create_config_item("LocalAuthListEnabled", &authDisabled,
                                CONFIG_VALUE_TYPE_BOOL, false);
    } else if (strcmp(keyName, "LocalAuthListMaxLength") == 0) {
      item = create_config_item("LocalAuthListMaxLength", &auth_list_max,
                                CONFIG_VALUE_TYPE_NUMBER, true);
    } else if (strcmp(keyName, "SendLocalListMaxLength") == 0) {
      item = create_config_item("SendLocalListMaxLength", &local_list_max,
                                CONFIG_VALUE_TYPE_NUMBER, true);
    } else if (strcmp(keyName, "ChargeProfileMaxStackLevel") == 0) {
      item = create_config_item("ChargeProfileMaxStackLevel",
                                &charge_profile_max_stack,
                                CONFIG_VALUE_TYPE_NUMBER, true);
    } else if (strcmp(keyName, "ChargingScheduleAllowedChargingRateUnit") ==
               0) {
      item = create_config_item("ChargingScheduleAllowedChargingRateUnit",
                                &AllowedChargingRateUnit,
                                CONFIG_VALUE_TYPE_STRING, true);

    } else if (strcmp(keyName, "ChargingScheduleMaxPeriods") == 0) {
      item = create_config_item("ChargingScheduleMaxPeriods",
                                &charging_schedule_max_period,
                                CONFIG_VALUE_TYPE_NUMBER, true);

    } else if (strcmp(keyName, "MaxChargingProfilesInstalled") == 0) {
      item = create_config_item("MaxChargingProfilesInstalled",
                                &max_charge_profile, CONFIG_VALUE_TYPE_NUMBER,
                                true);
    } else {
      cJSON_AddItemToArray(unknownKeyArray, cJSON_CreateString(keyName));
      continue;
    }

    if (item) {
      cJSON_AddItemToArray(configKeyArray, item);
    }
  }

  cJSON *response_payload = cJSON_CreateObject();
  cJSON_AddItemToObject(response_payload, "configurationKey", configKeyArray);

  if (cJSON_GetArraySize(unknownKeyArray) > 0) {
    cJSON_AddItemToObject(response_payload, "unknownKey", unknownKeyArray);
  } else {
    cJSON_Delete(unknownKeyArray); // Clean up unused array
  }

  send_call_result_with_payload(uniqueId, response_payload, "GetConfiguration");
  cJSON_Delete(response_payload);
}

static bool parse_bool_value(const char *value, bool *result) {
  if (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0) {
    *result = true;
    return true;
  } else if (strcasecmp(value, "false") == 0 || strcmp(value, "0") == 0) {
    *result = false;
    return true;
  }
  return false;
}

void handle_change_configuration(cJSON *payload, const char *uniqueId) {
  cJSON *keyItem = cJSON_GetObjectItem(payload, "key");
  cJSON *valueItem = cJSON_GetObjectItem(payload, "value");

  if (!cJSON_IsString(keyItem) || !cJSON_IsString(valueItem)) {
    ESP_LOGW(TAG, "ChangeConfiguration: Invalid or missing 'key' or 'value'");
    send_call_result_with_status(uniqueId, "Rejected", "ChangeConfiguration");
    return;
  }

  const char *key = keyItem->valuestring;
  const char *value = valueItem->valuestring;
  const char *status = "NotSupported"; // Default status

  for (size_t i = 0; i < sizeof(config_registry) / sizeof(ConfigEntry); ++i) {
    if (strcmp(config_registry[i].key, key) == 0) {

      if (config_registry[i].type == CONFIG_TYPE_BOOL) {
        bool parsed_value;
        if (parse_bool_value(value, &parsed_value)) {
          *(bool *)config_registry[i].target = parsed_value;
          status = "Accepted";
        } else {
          status = "Rejected";
        }

      } else if (config_registry[i].type == CONFIG_TYPE_INT) {
        char *endptr = NULL;
        long val = strtol(value, &endptr, 10);
        if (*endptr == '\0' && val >= 0) { // valid integer and non-negative
          *(int *)config_registry[i].target = (int)val;
          status = "Accepted";
        } else {
          status = "Rejected";
        }
      }

      break;
    }
  }
  //  if (!found) {
  //    ESP_LOGW(TAG, "Unknown ChangeConfiguration key: %s", key);
  //  }

  send_call_result_with_status(uniqueId, status, "ChangeConfiguration");
}
