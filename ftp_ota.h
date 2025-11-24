// ftp_ota.h

#ifndef FTP_OTA_H
#define FTP_OTA_H

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/sha256.h"
//#include <ctype.h>
//#include <errno.h>
//#include <stdbool.h>
//#include <stddef.h>
//#include <stdio.h>
//#include <stdlib.h>

typedef enum {
  Downloaded,
  DownloadFailed,
  Downloading,
  Idle,
  InstallationFailed,
  Installing,
  Installed
} firmware_status_t;

extern firmware_status_t firmware_status;

#ifdef __cplusplus
extern "C" {
#endif

extern time_t ist;
// Start the FTP OTA update task
void start_ftp_ota_update(const char *url, const char *sha256_hex,uint8_t retry,uint16_t interval,timer_t update_time);
esp_err_t send_partition_over_uart(const esp_partition_t *partition,
                                   size_t firmware_size);
void uart_init();
void some_function();

//// Parse the OTA update info from a JSON string
// bool parse_update_info(const char *json_payload, char *out_ftp_url, size_t
// url_size, char *out_sha256, size_t sha_size);
//
//// Decode chunked HTTP response
// int decode_chunked_response(const char *chunked_data, size_t chunked_len,
// char *out_data, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif // FTP_OTA_H
