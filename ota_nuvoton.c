/*
 * ota_nuvoton.c
 *
 *  Created on: 04-Oct-2025
 *      Author: Admin
 */



#include "ftp_ota.h"
#include "driver/uart.h"
#include <sys/param.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "ota_nuvoton";

#define UART_NUM UART_NUM_1
#define UART_TXD 17 // Example GPIO
#define UART_RXD 16 // Example GPIO
uint16_t counter;



// extern bool call_somefunction;

esp_err_t erase_partition(const esp_partition_t *partition) {
  if (partition == NULL) {
    ESP_LOGE(TAG, "Partition is NULL");
    return ESP_ERR_INVALID_ARG;
  }
  ESP_LOGI(TAG,
           "Erasing partition at address 0x%" PRIx32 ", size %" PRIu32 " bytes",
           partition->address, partition->size);
  esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to erase partition (0x%x)", err);
  }
  return err;
}

// Get inactive OTA partition (the one NOT currently running)
const esp_partition_t *get_inactive_ota_partition() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  ESP_LOGI(TAG, "Running partition type %d subtype %d address 0x%" PRIx32,
           running->type, running->subtype, running->address);

  // Find next OTA partition different from running one
  const esp_partition_t *ota_0 = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  const esp_partition_t *ota_1 = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);

  if (ota_0 == NULL || ota_1 == NULL) {
    ESP_LOGE(TAG, "OTA partitions not found!");
    return NULL;
  }

  if (running->address == ota_0->address) {
    return ota_1;
  } else {
    return ota_0;
  }
}

static void build_header(uint8_t *header, uint32_t firmware_size) {
  // Fixed header pattern
  header[0] = 0xA0;
  header[1] = 0x00;
  header[2] = 0x00;
  header[3] = 0x00;
  header[4] = 0x03;
  header[5] = 0x00;
  header[6] = 0x00;
  header[7] = 0x00;
  header[8] = 0x00;
  header[9] = 0x00;
  header[10] = 0x00;
  header[11] = 0x00;

  // Last 4 bytes = firmware size in little endian
  header[12] = (uint8_t)(firmware_size & 0xFF);
  header[13] = (uint8_t)((firmware_size >> 8) & 0xFF);
  header[14] = (uint8_t)((firmware_size >> 16) & 0xFF);
  header[15] = (uint8_t)((firmware_size >> 24) & 0xFF);
}
int nuvo_send_simple_cmd_and_read(uint8_t cmd, uint8_t byte4, uint8_t byte5,
                                  uint8_t *response, size_t response_len) {
  uint8_t pkt[64] = {0};
  pkt[0] = cmd;
  pkt[4] = byte4;
  pkt[5] = byte5;

  // Send the command
  int sent = uart_write_bytes(UART_NUM, (const char *)pkt, 64);
  uart_wait_tx_done(UART_NUM, pdMS_TO_TICKS(100));
  ESP_LOGI(TAG,"send\n");
  esp_log_buffer_hex(TAG, pkt, sent);

  // Wait and read the response
  int len = uart_read_bytes(UART_NUM, response, response_len,
                            pdMS_TO_TICKS(200)); // 200ms timeout
                                                 //  printf("received\n");
  //  esp_log_buffer_hex(TAG, response, len);
  return len; // returns number of bytes read (or -1 on error)
}
void nuvo_send_cmd_connect(uint8_t cmd, uint8_t len) {
  uint8_t connect_pkt[64] = {0};

  connect_pkt[0] = cmd; // CMD_CONNECT
  connect_pkt[4] = len; // Packet length (bytes)

  uart_write_bytes(UART_NUM, (const char *)connect_pkt, sizeof(connect_pkt));
  uart_wait_tx_done(UART_NUM, pdMS_TO_TICKS(100));

  ESP_LOGI(TAG, "Sent CMD_CONNECT (64 bytes)");
}

esp_err_t send_partition_over_uart(const esp_partition_t *partition,
                                          size_t firmware_size) {
#define HEADER_SIZE 16
#define CHUNK_SIZE 56
#define UART_MIN_CHUNK 64 // Minimum UART packet size

  uint8_t header[HEADER_SIZE] = {/* your header data */};
  //  uint8_t data_buffer[CHUNK_SIZE]; // for remaining chunks
  uint8_t first_tx_buffer[HEADER_SIZE +
                          CHUNK_SIZE]; // buffer for header + first chunk
  build_header(header, firmware_size);
  size_t offset = 0;

  // 1️⃣ Read the first data chunk
  size_t first_chunk_size = UART_MIN_CHUNK - HEADER_SIZE; // At least 48 bytes
  if (firmware_size < first_chunk_size)
    first_chunk_size = firmware_size;

  esp_err_t err = esp_partition_read(
      partition, offset, first_tx_buffer + HEADER_SIZE, first_chunk_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Flash read failed at offset 0x%" PRIx32, (uint32_t)offset);
    return err;
  }

  // 2️⃣ Copy the header into the beginning
  memcpy(first_tx_buffer, header, HEADER_SIZE);

  // 3️⃣ Send header + first chunk
  int sent = uart_write_bytes(UART_NUM, (const char *)first_tx_buffer,
                              HEADER_SIZE + first_chunk_size);
  ESP_LOGI(TAG, "UART write (header + first chunk): requested=%d, sent=%d",
           HEADER_SIZE + first_chunk_size, sent);
  esp_log_buffer_hex(TAG, first_tx_buffer, sent);
  uint8_t response[64] = {0}; // Adjust size based on expected response
  int len =
      uart_read_bytes(UART_NUM, response, sizeof(response), pdMS_TO_TICKS(200));

  if (len > 0) {
    ESP_LOGI(TAG, "Received %d bytes from UART:", len);
    esp_log_buffer_hex(TAG, response, len);
  } else {
    ESP_LOGW(TAG, "No response received or timeout.");
  }

  if (sent != HEADER_SIZE + first_chunk_size) {
    ESP_LOGE(TAG, "Failed to send header + first chunk");
    return ESP_FAIL;
  }

  offset += first_chunk_size;

  // 4️⃣ Continue sending remaining firmware
#define CHUNK_SIZE 56
#define headerA_SIZE 8
#define TOTAL_CHUNK_SIZE (headerA_SIZE + CHUNK_SIZE)

  uint8_t dataChunk[TOTAL_CHUNK_SIZE];
  uint8_t headerA[headerA_SIZE] = {0x00, 0x00, 0x00, 0x00,
                                   0x05, 0x00, 0x00, 0x00};

  while (offset < firmware_size) {
    size_t bytes_to_read = MIN(CHUNK_SIZE, firmware_size - offset);

    // Read firmware into the payload portion (after header)
    err = esp_partition_read(partition, offset, dataChunk + headerA_SIZE,
                             bytes_to_read);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Flash read failed at offset 0x%" PRIx32, (uint32_t)offset);
      return err;
    }

    // Copy header at the beginning
    memcpy(dataChunk, headerA, headerA_SIZE);

    // Pad any unused payload bytes with zeroes
    if (bytes_to_read < CHUNK_SIZE) {
      memset(dataChunk + headerA_SIZE + bytes_to_read, 0,
             CHUNK_SIZE - bytes_to_read);
    }

    // Send entire chunk (header + fixed payload size)
    size_t total_to_send = headerA_SIZE + CHUNK_SIZE;
    sent = uart_write_bytes(UART_NUM, (const char *)dataChunk, total_to_send);

    ESP_LOGI(TAG, "UART write: requested=%d, sent=%d", total_to_send, sent);
    esp_log_buffer_hex(TAG, dataChunk, sent);

    if (sent != total_to_send) {
      ESP_LOGE(TAG, "UART write failed or incomplete (sent=%d)", sent);
      return ESP_FAIL;
    }

    // Read UART response (ACK, etc.)
    uint8_t response[64] = {0};
    int resp_len = uart_read_bytes(UART_NUM, response, sizeof(response),
                                   pdMS_TO_TICKS(200));

    if (resp_len > 0) {
      ESP_LOGI(TAG, "Received %d bytes from UART:", resp_len);
      esp_log_buffer_hex(TAG, response, resp_len);
      // TODO: Validate ACK if needed
    } else {
      ESP_LOGW(TAG, "No response received or timeout after chunk send.");
      // TODO: Handle retry if needed
    }
    counter = (headerA[5] << 8) | headerA[4];
    // Increment by 2
    counter += 2;

    // Split back into bytes (little endian)
    headerA[4] = counter & 0xFF;        // low byte
    headerA[5] = (counter >> 8) & 0xFF; // high byt
    offset += bytes_to_read;
  }
  ESP_LOGI(TAG, "Firmware transmission to Nuvoton complete.");
  ESP_LOGI(TAG, "Firmware transfer complete. Sending footer...");
  nuvo_send_simple_cmd_and_read(0xAB, counter & 0xFF, (counter >> 8) & 0xFF,
                                response, sizeof(response));
  //  nuvo_send_cmd_connect(0xAB, 0x8D);

  ESP_LOGI(TAG, "Footer sent successfully.");
  return ESP_OK;
}

void erase_storage_partition() {
  const esp_partition_t *inactive = get_inactive_ota_partition();
  if (!inactive) {
    ESP_LOGE(TAG, "Inactive OTA partition not found");
    return;
  }

  esp_err_t err = erase_partition(inactive);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Inactive OTA partition erased successfully");
  } else {
    ESP_LOGE(TAG, "Erase failed");
  }
}

void uart_init() {
  const uart_config_t uart_config = {.baud_rate = 115200,
                                     .data_bits = UART_DATA_8_BITS,
                                     .parity = UART_PARITY_DISABLE,
                                     .stop_bits = UART_STOP_BITS_1,
                                     .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};
  uart_param_config(UART_NUM, &uart_config);
  uart_set_pin(UART_NUM, UART_TXD, UART_RXD, UART_PIN_NO_CHANGE,
               UART_PIN_NO_CHANGE);
  uart_driver_install(UART_NUM, 2048, 2048, 0, NULL, 0);
}

void some_function() {
  uint8_t resp[64] = {0}; // Buffer to hold the response
  while (true) {
    // Send command 0x01 and read response into resp
    int resp_len =
        nuvo_send_simple_cmd_and_read(0xAE, 0x09, 0x00, resp, sizeof(resp));

    if (resp_len > 0) {
      ESP_LOGI(TAG,"Received %d bytes:\n", resp_len);
      for (int i = 0; i < resp_len; i++) {
        ESP_LOGI(TAG,"%02X ", resp[i]);
      }
      printf("\n");
    } else {
      ESP_LOGI(TAG,"No response or timeout\n");
    }
    if (resp[4] == 0x02)
      break;
  }
}
