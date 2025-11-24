/*
 * ocpp.h
 *
 *  Created on: 24-Sep-2025
 *      Author: Admin
 */

#ifndef MAIN_OCPP_H_
#define MAIN_OCPP_H_

#include "cJSON.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "utils.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

// -------- ENUMS --------

typedef enum {
  CHARGER_BOOTING,
  CHARGER_AVAILABLE,
  CHARGER_PREPARING,
  CHARGER_CHARGE_INIT,
  CHARGER_CHARGE,
  CHARGER_SUSPENDED,
  CHARGER_CARD_DETECTED,
  CHARGER_CARD_AUTHORIZE,
  CHARGER_CARD_AUTHORIZE_REQ,
  CHARGER_CARD_AUTHORIZE_FAIL,
  CHARGER_STOP,
  CHARGER_FAULTED,
} charger_state_t;

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))


extern bool send_B_flag, send_C_flag,send_O_flag, send_P_flag, send_V_flag;
extern const char *frame_C;
extern const char *frame_O;
extern const char *frame_V;

#define I2CTAG "I2CTAG"
// #define TAG "I2c"

#define MAX_STRING_LENGTH 15
#define MAX_RESPONSE_LEN 128

// Globals (should ideally be private but declared as extern here)
extern SemaphoreHandle_t i2c_mutex;
extern bool waiting_for_ack;

extern char MeterValues[15][MAX_STRING_LENGTH];
extern char device_info_endpoint[64];

extern bool i2c_comm_failed;
extern uint8_t vendorErrorCode;
extern char reason[32];

extern char timestamp[32];
extern char charger1_last_status[64];
extern char charger2_last_status[64];
extern char errorCode[32];
//

extern uint8_t connectorCode;
// extern int gun1_status_value;
// extern int gun2_status_value;
// extern uint8_t gun2_state_value;
extern int meter_start, meter_stop;
extern int a_frame_values[4];
extern int b_frame_values[5];
extern int p_frame_values[2];
extern char frame_buffer[256];
extern char response[MAX_RESPONSE_LEN];
extern TickType_t last_G_sent;
extern TickType_t G_interval;
extern char frame_G[64];
extern int g_temp;
extern bool obtain_time;

esp_err_t i2c_init();
esp_err_t i2c_send(const char *json);
esp_err_t i2c_receive(char *buffer, size_t max_len);
bool send_frame_with_tag(const char *frame, const char *expected_tag);
void set_index_value(int index, int value);
void build_frame_json(const char *Key, int *value, int length);
void parse_json_frame(const char *frame);
// void set_A_values(int count, ...);

typedef enum {
  STATION_BOOTING,
  STATION_AVAILABLE,
  STATION_UNAVAILABLE,
  STATION_UPDATE,
  STATION_FAULTED
} station_state_t;

typedef enum {
  CONNECTOR_BOOTING,
  CONNECTOR_AVAILABLE,
  CONNECTOR_PREPARING,
  CONNECTOR_CHARGING,
  CONNECTOR_SUSPENDED,
  CONNECTOR_AUTHORIZE_REQ,
  CONNECTOR_RESERVED,
  CONNECTOR_AUTHORIZED,
  CONNECTOR_AUTHORIZE_FAILED,
  CONNECTOR_START_TRANSACTION,
  CONNECTOR_STOP_TRANSACTION,
  CONNECTOR_FINISHING,
  CONNECTOR_UNAVAILABLE,  
  CONNECTOR_FAULTED,
} connector_state_t;

// -------- STRUCTS --------

typedef struct {
  char uuid[40];
  char action[32];
  int connector_id;
} RequestMapEntry;

#define MAX_PROFILES 10
#define MAX_PERIODS 24
typedef struct {
  int startPeriod;
  int limit;
  int numberPhases;
} ChargingSchedulePeriod;

typedef struct {
  int connectorId;
  int chargingProfileId;
  int stackLevel;
  char purpose[32];
  char kind[32];
  char recurrKind[15];
  time_t validFrom;
  time_t validTo;
  int duration;
  time_t startSchedule;
  char chargingRateUnit[16];
  int minChargingRate;

  ChargingSchedulePeriod periods[MAX_PERIODS];
  int periodCount;
} ChargingProfile;
// -------- CONSTANTS --------

#define FTP_URL_MAX_LEN 256
#define HASH_LEN 64
#define HASH_STR_LEN (HASH_LEN + 1) // +1 for null terminator

// extern charger_state_t charger1_state;
// extern charger_state_t charger2_state;

extern ChargingProfile currentprofile;
// -------- EXTERNAL VARIABLES --------
extern esp_websocket_client_handle_t client;
// extern SemaphoreHandle_t ocpp_mutex;
extern SemaphoreHandle_t client_mutex;
extern SemaphoreHandle_t ocpp_send_mutex;
extern int max_power;

// extern connector_state_t connector1_state;
// extern connector_state_t connector2_state;
extern station_state_t station_state;

extern uint16_t websocket_ping;
extern char charger_last_status[64];
extern char charger2_status[64];
extern char errorCode[32];
extern char display_time[32];
extern char Nuv_ver[15];
extern int connector_id;
// extern bool authorize_req = false;
extern char gun1_id_tag[20];
extern char gun2_id_tag[20];
extern char gun1_trans_id_tag[20];
extern char gun2_trans_id_tag[20];
extern char reserved_id_tag[20];
extern time_t reserved_expiry_time;

extern time_t ist_now;
extern uint8_t stop_error_code, gun1_state_value, VE_code;

typedef struct {
  int connector_id;
  bool transaction_active;
  int transaction_id;
  char id_tag[32];
  char trans_id_tag[32];
  char MeterValues[14][15];
  TaskHandle_t metervalues_task;
  connector_state_t state;
  char charger_current_status[64];
  char reserved_id_tag[32];
  int reserved_id;
  time_t reserved_expiry_time;
  esp_timer_handle_t prep_timeout_timer; // one timer per connector
  bool prep_timeout_started;             // true if timer is running
  bool prep_timeout_expired;
} Connector;

#define NUM_CONNECTORS 1
// Charger States

typedef struct {
  int gun_state_value;
  charger_state_t charger_state;
  char id_tag[32];
  int meter_start;
  int meter_stop;
  int vendorErrorCode;
  int connectorCode;
} connector_t;

extern connector_t guns[NUM_CONNECTORS];
extern Connector connectors[NUM_CONNECTORS];
// extern char reason[32];
// extern char errorCode[32];
// extern int transaction_id;
extern bool gun1_transaction_active;
extern bool gun2_transaction_active;
extern uint16_t heartbeat_interval;
extern uint16_t metervalues_interval;
extern bool authorize_remote;
extern uint16_t connection_timeout;
// extern bool restart;

// extern char timestamp[32];
extern bool time_synced;

// extern bool  send_F_flag, send_D_flag, send_I3_flag;

// -------- FUNCTION DECLARATIONS --------

// Request map management
void remember_ocpp_request(const char *uuid, const char *action,
                           int connector_id);
const char *find_action_for_uuid(const char *uuid);
void remove_ocpp_request(const char *uuid);
void log_request_map(void);

// OCPP communication
void ocpp_message(const char *msg);
void send_ocpp_request(const char *action, cJSON *payload, int connectorID);

// Task functions
void send_bootnotification_task(void *arg);
void send_heartbeat(void *arg);
void metervalues(void *arg);
void trigger_status_notification(int connectorId, const char *status,
                                 const char *errorCode, int vendorErrorCode);

void get_current_iso8601_time(void *arg);
void generate_uuid_v4(char *uuid_str);

// Message handling
void handle_ocpp_websocket_message(const char *json_str);
void send_call_result_with_status(const char *uniqueId, const char *status_str,
                                  const char *action);
void send_call_error(const char *uniqueId, const char *errorCode,
                     const char *errorDescription, cJSON *errorDetails);
void send_call_result_with_payload(const char *uniqueId, cJSON *payload,
                                   const char *action);

// OCPP actions
void ocpp_send_authorize(int connector_id, const char *idTag);
void ocpp_send_start_transaction(int connector_id, double meter_start,
                                 const char *idTag);
void ocpp_send_meter_values(int connector_id);
void ocpp_send_stop_transaction(int connectorID, double meter_stop,
                                const char *reason);

// Specific message handlers
void handle_remote_stop_transaction(cJSON *call_payload, const char *uniqueId);
void handle_remote_start_transaction(cJSON *call_payload, const char *uniqueId);
void handle_reset_request(cJSON *payload, const char *uniqueId);
void handle_change_availability(cJSON *payload, const char *uniqueId);
void handle_update_firmware(cJSON *payload, const char *uniqueId);
void handle_trigger_message(cJSON *payload, const char *uniqueId);
void handle_get_configuration(cJSON *payload, const char *uniqueId);
void handle_change_configuration(cJSON *payload, const char *uniqueId);
// Time & UUID helpers

void send_firmware_status_notification();
void handle_set_charging_profile(cJSON *payload, const char *uniqueId);
void handle_clear_charging_profile(cJSON *payload, const char *uniqueId);
void handle_get_composite_schedule(cJSON *payload, const char *uniqueId);
void get_profile(void *arg);
ChargingProfile *get_active_profile(const char *targetPurpose);

void set_time_from_string(const char *iso8601_time_str);
time_t parse_iso8601(const char *datetime);
// NVS storage functions
void save_transaction_id_to_nvs(int32_t tx_id);
bool load_transaction_id_from_nvs(int32_t *out_id);
void handle_heartbeat_response(cJSON *payload);

void handle_ocpp_call_result(cJSON *root);
void handle_ocpp_call_message(cJSON *root);

// OTA update (from FTP)
// void start_ftp_ota_update(const char *ftp_url, const char *sha256);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_OCPP_H_ */
