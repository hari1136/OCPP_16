
#include "ocpp.h"

#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 100000
#define I2C_MASTER_TX_BUF_DISABLE 0
#define I2C_MASTER_RX_BUF_DISABLE 0
#define SLAVE_ADDR 0x08

SemaphoreHandle_t i2c_mutex;

connector_t guns[NUM_CONNECTORS];

static TaskHandle_t connectorStatus_handle = NULL;
char MeterValues[15][MAX_STRING_LENGTH];

bool i2c_comm_failed = false;
uint8_t vendorErrorCode = 0;
uint8_t connectorCode = 0;
char reason[32];

// charger_state_t charger1_state;
// charger_state_t charger2_state;

#define NUM_B_FRAME_VALUES 5

int meter_start, meter_stop;
int a_frame_values[4] = {1, 10, 0, 2};
int b_frame_values[NUM_B_FRAME_VALUES] = {260, 170, 33, 1, 16};
int p_frame_values[2] = {1, 30};
char frame_buffer[256] = {0};
char response[MAX_RESPONSE_LEN] = {0};
TickType_t last_G_sent = 0;
TickType_t G_interval = pdMS_TO_TICKS(5000);
char frame_G[64];
int g_temp = 30;

int connector_id;

bool switch_connector;
// bool send_F2_flag, send_D2_flag;
bool send_B_flag, send_C_flag, send_O_flag, send_P_flag, send_V_flag;
const char *frame_C = "{\"C\":[]}";
// const char *frame_D1 = "{\"D\":[1]}";
// const char *frame_D2 = "{\"D\":[2]}";
// const char *frame_F1 = "{\"F\":[1]}";
// const char *frame_F2 = "{\"F\":[2]}";
// const char *frame_I1 = "{\"I\":[1]}";
////const char *frame_I2 = "{\"I\":[2]}";
// const char *frame_I3 = "{\"I\":[3]}";
const char *frame_M1 = "{\"M\":[1]}";
//const char *frame_M2 = "{\"M\":[2]}";
const char *frame_O = "{\"O\":[0]}";
const char *frame_V = "{\"V\":[0]}";

typedef enum {
  KEY_UNKNOWN = 0,
  KEY_A,
  KEY_C,
  KEY_D,
  KEY_G,
  KEY_I,
  KEY_F,
  KEY_M,
  KEY_V
} KeyType;

bool waiting_for_ack = false;

esp_err_t i2c_init() {
  i2c_config_t conf = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = I2C_MASTER_SDA_IO,
      .scl_io_num = I2C_MASTER_SCL_IO,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master.clk_speed = I2C_MASTER_FREQ_HZ,
  };
  ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
  return i2c_driver_install(I2C_MASTER_NUM, conf.mode,
                            I2C_MASTER_RX_BUF_DISABLE,
                            I2C_MASTER_TX_BUF_DISABLE, 0);
}

esp_err_t i2c_send(const char *json) {
  size_t len = strlen(json);
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (SLAVE_ADDR << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write(cmd, (const uint8_t *)json, len, true);
  i2c_master_stop(cmd);

  esp_err_t ret =
      i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
  i2c_cmd_link_delete(cmd);
  return ret;
}

esp_err_t i2c_receive(char *buffer, size_t max_len) {
  memset(buffer, 0, max_len);

  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (SLAVE_ADDR << 1) | I2C_MASTER_READ, true);

  if (max_len > 1) {
    i2c_master_read(cmd, (uint8_t *)buffer, max_len - 1, I2C_MASTER_ACK);
  }
  i2c_master_read_byte(cmd, (uint8_t *)&buffer[max_len - 1], I2C_MASTER_NACK);

  i2c_master_stop(cmd);

  esp_err_t ret =
      i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
  i2c_cmd_link_delete(cmd);

  // Truncate at closing brace to remove garbage
  char *end = strchr(buffer, '}');
  if (end) {
    *(end + 1) = '\0'; // Null-terminate at end of JSON
  }

  return ret;
}

void set_index_value(int index, int value) {
  if (index >= 0 && index < 4) {
    a_frame_values[index] = value;
    //    ESP_LOGI("I2C_JSON", "[%d] set to %d", index, value);
  } else {
    ESP_LOGW("I2C_JSON", "Invalid index: %d", index);
  }
}

void build_frame_json(const char *Key, int *value, int length) {
  cJSON *root = cJSON_CreateObject();
  cJSON *array = cJSON_CreateIntArray(value, length);
  cJSON_AddItemToObject(root, Key, array);

  memset(frame_buffer, 0, sizeof(frame_buffer));
  if (!cJSON_PrintPreallocated(root, frame_buffer, sizeof(frame_buffer),
                               false)) {
    ESP_LOGE(I2CTAG, "Failed to build JSON into preallocated buffer");
    // Optional: handle error (fallback, reset, etc.)
  }

  cJSON_Delete(root);
}

static KeyType get_key_type(const char *key) {
  if (strcmp(key, "A") == 0)
    return KEY_A;
  if (strcmp(key, "C") == 0)
    return KEY_C;
  if (strcmp(key, "D") == 0)
    return KEY_D;
  if (strcmp(key, "G") == 0)
    return KEY_G;
  if (strcmp(key, "I") == 0)
    return KEY_I;
  if (strcmp(key, "F") == 0)
    return KEY_F;
  if (strcmp(key, "M") == 0)
    return KEY_M;
  if (strcmp(key, "V") == 0)
    return KEY_V;
  return KEY_UNKNOWN;
}

void connectorStatus(void *arg) {
  while (1) {
    if (switch_connector) {
      set_index_value(0, 1);
      switch_connector = 0;
    } else {
      set_index_value(0, 2);
      switch_connector = 1;
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

static void handle_key_A(cJSON *item) {
  int index = 0;
  cJSON *val;
  cJSON_ArrayForEach(val, item) {
    if (!cJSON_IsNumber(val))
      continue;
    int v = val->valueint;
    int idx =
        connector_id > 0 ? connector_id - 1 : 0; // Default to first connector
    switch (index) {
    case 0:
      connector_id = v;
      idx = connector_id - 1;
      break;
    case 1:
      guns[idx].gun_state_value = v;
      break;
    case 2:
      if (v == 1)
        guns[idx].charger_state = CHARGER_FAULTED;
      else if (v == 2)
        guns[idx].charger_state = CHARGER_CARD_DETECTED;
      break;
    case 3:
      guns[idx].vendorErrorCode = v;
      break;
    case 4:
      connectorCode = val->valueint;
      switch (connectorCode) {
      case 0:
      case 1:
      case 2:
        set_index_value(0, connectorCode);
        if (connectorStatus_handle != NULL) {
          vTaskDelete(connectorStatus_handle);
          connectorStatus_handle = NULL;
        }
        break;
      case 3:
        xTaskCreate(connectorStatus, "connector_task", 2048, NULL, 3,
                    &connectorStatus_handle);

        break;
      }
      break;
    }
    index++;
  }
}
static void handle_key_C(cJSON *item) {
  int index = 0;
  cJSON *val;

  cJSON_ArrayForEach(val, item) {
    if (!cJSON_IsNumber(val))
      continue;

    if (index >= NUM_B_FRAME_VALUES) {
      ESP_LOGW(I2CTAG, "Extra value at index %d: %d (no reference to compare)",
               index, val->valueint);
      index++;
      continue;
    }

    int json_value = val->valueint;
    int expected_value = b_frame_values[index];

    if (json_value == expected_value) {
      ESP_LOGI(I2CTAG, "C[%d] OK: %d", index, json_value);
    } else {
      ESP_LOGE(I2CTAG, "C[%d] MISMATCH: expected %d, got %d", index,
               expected_value, json_value);
      // You can handle mismatch here (set error flag, change state, etc.)
    }

    index++;
  }

  // Optional: check if fewer values were received
  if (index < NUM_B_FRAME_VALUES) {
    ESP_LOGW(I2CTAG, "C array incomplete: expected %d values, got %d",
             NUM_B_FRAME_VALUES, index);
  }
}

static void handle_key_D(cJSON *item) {
  int index = 0;
  cJSON *val;
  int connector_id_local = 0; // to avoid confusion with global

  cJSON_ArrayForEach(val, item) {
    if (!cJSON_IsNumber(val))
      continue;

    switch (index) {
    case 0:
      // First element = connector id
      connector_id_local = val->valueint;
      break;

    case 1:
      // Second element = meter start value
      if (connector_id_local < 1 || connector_id_local > NUM_CONNECTORS) {
        ESP_LOGW(I2CTAG, "Invalid connector_id %d in D frame",
                 connector_id_local);
        break;
      }

      int idx = connector_id_local - 1;
      guns[idx].meter_start = val->valueint;
      guns[idx].charger_state = CHARGER_CHARGE;

      ESP_LOGI(I2CTAG, "Connector %d: meter_start=%d, state=CHARGER_CHARGE",
               connector_id_local, guns[idx].meter_start);
      break;

    default:
      // Ignore extra fields
      break;
    }

    index++;
  }
}

static void handle_key_G(cJSON *item) {
  cJSON *val = cJSON_GetArrayItem(item, 0);
  if (val && cJSON_IsNumber(val)) {
    g_temp = val->valueint;
    ESP_LOGI(I2CTAG, "Stored G[0] as int: %d", g_temp);
  }
}

static void handle_key_I(cJSON *item) {
  int index = 0;
  cJSON *val;
  cJSON_ArrayForEach(val, item) {
    if (!val)
      continue;
    int idx = connector_id > 0 ? connector_id - 1 : 0;
    switch (index) {
    case 0:
      if (cJSON_IsNumber(val))
        connector_id = val->valueint;
      idx = connector_id - 1;
      break;
      break;
    case 1: // Second element
      if (cJSON_IsString(val)) {
        guns[idx].charger_state = CHARGER_CARD_AUTHORIZE;
        safe_strncpy(guns[idx].id_tag, val->valuestring,
                     sizeof(guns[idx].id_tag));
      }
      break;
    }
    index++;
  }
}

static void handle_key_F(cJSON *item) {
  int index = 0;
  cJSON *val;
  cJSON_ArrayForEach(val, item) {
    if (!cJSON_IsNumber(val))
      continue;
    int idx = connector_id > 0 ? connector_id - 1 : 0;
    switch (index) {
    case 0:
      connector_id = val->valueint;
      idx = connector_id - 1;
      break;
    case 1:
      guns[idx].meter_stop = val->valueint;
      guns[idx].charger_state = CHARGER_STOP;
      break;

    case 2: {
      const char *reason_str = NULL;
      switch (val->valueint) {
      case 1:
        reason_str = "EVDisconnected";
        break;
      case 2:
        reason_str = "Reboot";
        break;
      case 3:
        reason_str = "DeAuthorized";
        break;
      case 4:
        reason_str = "HardReset";
        break;
      case 5:
        reason_str = "Local";
        break;
      case 6:
        reason_str = "PowerLoss";
        break;
      case 7:
        reason_str = "UnlockCommand";
        break;
      case 8:
        reason_str = "EmergencyStop";
        break;
      case 9:
        reason_str = "SoftReset";
        break;
      case 10:
        reason_str = "Remote";
        break;
      default:
        reason_str = "Other";
        break;
      }
      safe_strncpy(reason, reason_str, sizeof(reason));
      guns[idx].charger_state = CHARGER_STOP;
      ESP_LOGI(I2CTAG, "send_stop_transaction for connector %d", connector_id);
      break;
      // ocpp_send_stop_transaction(3.45, reason);
    }
    }
    index++;
  }
}

static void handle_key_M(cJSON *item) {
  int index = 0;
  cJSON *val;
  cJSON_ArrayForEach(val, item) {
    if (!val)
      continue;
    int idx = connector_id > 0 ? connector_id - 1 : 0;
    switch (index) {
    case 0:
      // Expecting connector_id as number at index 0
      if (cJSON_IsNumber(val)) {
        connector_id = val->valueint;
        idx = connector_id - 1;
      }
      break;

    case 1 ... 14: // GCC extension; or use multiple case statements if needed
      if (cJSON_IsString(val)) {
        safe_strncpy(connectors[idx].MeterValues[index - 1], val->valuestring,
                     MAX_STRING_LENGTH);
      }
      break;

    // You can add more cases here if needed
    default:
      // Ignore anything beyond index 14 or unknown formats
      break;
    }

    index++;
  }
}
static void handle_key_V(cJSON *item) {
  cJSON *val = cJSON_GetArrayItem(item, 0);
  if (val && cJSON_IsString(val)) {
    safe_strncpy(Nuv_ver, val->valuestring, sizeof(Nuv_ver));
    ESP_LOGI(I2CTAG, "Nuv_ver: %s", Nuv_ver);
  }
}
static void handle_generic_key(const char *key, cJSON *item) {
  int index = 0;
  cJSON *val;
  cJSON_ArrayForEach(val, item) {
    if (cJSON_IsNumber(val)) {
      ESP_LOGI(I2CTAG, "%s[%d] = %d", key, index, val->valueint);
    } else if (cJSON_IsString(val)) {
      ESP_LOGI(I2CTAG, "%s[%d] = %s", key, index, val->valuestring);
    }
    index++;
  }
}

void parse_json_frame(const char *frame) {
  cJSON *root = cJSON_Parse(frame);
  if (!root) {
    ESP_LOGE(I2CTAG, "Failed to parse JSON");
    return;
  }

  cJSON *item = NULL;
  cJSON_ArrayForEach(item, root) {
    if (!cJSON_IsArray(item))
      continue;
    const char *key = item->string;
    if (!key)
      continue;

    //    ESP_LOGI(I2CTAG, "Tag: %s", key);

    KeyType keyType = get_key_type(key);
    switch (keyType) {
    case KEY_A:
      handle_key_A(item);
      break;
    case KEY_C:
      handle_key_C(item);
      break;
    case KEY_D:
      handle_key_D(item);
      break;
    case KEY_G:
      handle_key_G(item);
      break;
    case KEY_I:
      handle_key_I(item);
      break;
    case KEY_F:
      handle_key_F(item);
      break;
    case KEY_M:
      handle_key_M(item);
      break;
    case KEY_V:
      handle_key_V(item);
      break;
    default:
      handle_generic_key(key, item);
      break;
    }
  }

  cJSON_Delete(root);
}

// void set_A_values(int count, ...) {
//   if (count < 1 || count > 4) {
//     ESP_LOGW("I2C_JSON", "Invalid count for A values: %d", count);
//     return;
//   }
//
//   va_list args;
//   va_start(args, count);
//
//   for (int i = 0; i < count; i++) {
//     int value = va_arg(args, int);
//     a_frame_values[i] = value;
//     //    ESP_LOGI("I2C_JSON", "A[%d] set to %d", i, value);
//   }
//
//   va_end(args);
// }

bool send_frame_with_tag(const char *frame, const char *expected_tag) {
  bool success = false;

  if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    ESP_LOGI(I2CTAG, "Sending frame: %s", frame);

    if (i2c_send(frame) == ESP_OK) {
      // Give slave time to respond
      vTaskDelay(pdMS_TO_TICKS(100)); // Increase if needed

      if (i2c_receive(response, MAX_RESPONSE_LEN) == ESP_OK) {
        ESP_LOGI(I2CTAG, "Received: %s", response);

        // Check if the expected tag is in the response
        if (strstr(response, expected_tag)) {
          parse_json_frame(response);
          success = true;
          waiting_for_ack = false;
        } else {
          ESP_LOGW(I2CTAG, "Unexpected response: expected tag '%s' not found",
                   expected_tag);
        }
      } else {
        ESP_LOGW(I2CTAG, "No response received from slave");
      }
    } else {
      //      ESP_LOGE(I2CTAG, "Failed to send frame");
    }

    // Optional: reset I2C on failure
    if (!success) {
      i2c_driver_delete(I2C_MASTER_NUM);
      i2c_init();
    }

    xSemaphoreGive(i2c_mutex);
  } else {
    ESP_LOGW(I2CTAG, "Could not acquire I2C mutex");
  }

  return success;
}
