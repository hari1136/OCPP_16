/*
 * ftp_ota.c
 *
 *  Created on: 25-Sep-2025
 *      Author: Admin
 */

#include "ftp_ota.h"
#include "utils.h"
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#define TAG "FTP_OTA"

#define FTP_DEFAULT_PORT 21
#define FTP_URL_MAX_LEN 130
#define FTP_USER_MAX_LEN 32
#define FTP_PASS_MAX_LEN 64
#define FTP_HOST_MAX_LEN 64
#define FTP_FILE_MAX_LEN 128
#define HASH_LEN 64
#define HASH_STR_LEN (HASH_LEN + 1)

firmware_status_t firmware_status;

time_t ist, ota_start_time;
uint8_t MAX_OTA_RETRIES = 1;
uint16_t RETRY_INTERVAL_MS = 1;
static char ftp_url[FTP_URL_MAX_LEN] = {0};
static char expected_hash[HASH_STR_LEN] = {0};
static TaskHandle_t ftp_ota_task_handle = NULL;

static void decode_url(char *dst, const char *src) {
  while (*src) {
    if (*src == '%' && isxdigit((unsigned char)src[1]) &&
        isxdigit((unsigned char)src[2])) {
      char hex[3] = {src[1], src[2], 0};
      *dst++ = (char)strtol(hex, NULL, 16);
      src += 3;
    } else if (*src == '+') {
      *dst++ = ' ';
      src++;
    } else {
      *dst++ = *src++;
    }
  }
  *dst = '\0';
}

static bool parse_ftp_url(const char *url, char *user, char *pass, char *host,
                          int *port, char *filepath) {
  if (strncmp(url, "ftp://", 6) != 0) {
    ESP_LOGE(TAG, "Invalid FTP URL: Missing ftp://");
    return false;
  }

  const char *ptr = url + 6;
  const char *at = strchr(ptr, '@');
  if (!at) {
    ESP_LOGE(TAG, "FTP URL missing '@' separator");
    return false;
  }

  // Extract user:pass
  char userinfo[FTP_USER_MAX_LEN + FTP_PASS_MAX_LEN] = {0};
  int userinfo_len = at - ptr;
  if (userinfo_len <= 0 || userinfo_len >= (int)sizeof(userinfo)) {
    ESP_LOGE(TAG, "Invalid userinfo length");
    return false;
  }

  safe_strncpy(userinfo, ptr, userinfo_len + 1);

  char *colon = strchr(userinfo, ':');
  if (!colon) {
    ESP_LOGE(TAG, "User info missing ':'");
    return false;
  }
  *colon = '\0';
  safe_strncpy(user, userinfo, FTP_USER_MAX_LEN);
  decode_url(pass, colon + 1);

  // Host and path
  ptr = at + 1;
  const char *slash = strchr(ptr, '/');
  if (!slash) {
    ESP_LOGE(TAG, "FTP URL missing file path");
    return false;
  }

  // Extract host:port
  char hostinfo[FTP_HOST_MAX_LEN + 6] = {0}; // 6 extra for port
  int host_len = slash - ptr;
  if (host_len <= 0 || host_len >= (int)sizeof(hostinfo)) {
    ESP_LOGE(TAG, "Invalid host info length");
    return false;
  }
  safe_strncpy(hostinfo, ptr, host_len + 1);

  char *port_sep = strchr(hostinfo, ':');
  if (port_sep) {
    *port_sep = '\0';
    *port = atoi(port_sep + 1);
    if (*port <= 0)
      *port = FTP_DEFAULT_PORT;
  } else {
    *port = FTP_DEFAULT_PORT;
  }
  safe_strncpy(host, hostinfo, FTP_HOST_MAX_LEN);
  safe_strncpy(filepath, slash, FTP_FILE_MAX_LEN);

  ESP_LOGI(TAG, "Parsed FTP filepath: %s", filepath);
  if (strstr(filepath, "/Nuvoton/") != NULL) {
    ESP_LOGI(TAG, "File path contains Nuvoton");
    uart_init();
    some_function();
  } else if (strstr(filepath, "/ESP32/") != NULL) {
    ESP_LOGI(TAG, "File path contains ESP32");
  } else {
    firmware_status = DownloadFailed;
    ESP_LOGI(TAG, "Unknown File path");
    //    return false;
  }
  return true;
}

static int ftp_send_command(int sock, const char *cmd) {
  ESP_LOGI(TAG, "FTP CMD: %s", cmd);
  return send(sock, cmd, strlen(cmd), 0);
}

static bool ftp_recv_and_check(int sock, char *buf, size_t buf_size,
                               int expected_code_prefix) {
  int total_len = 0;
  int code = 0;
  bool multiline = false;

  while (1) {
    int len = recv(sock, buf + total_len, buf_size - total_len - 1, 0);
    if (len <= 0) {
      ESP_LOGE(TAG, "FTP recv error: %d, errno: %d", len, errno);
      return false;
    }
    total_len += len;
    buf[total_len] = '\0';

    if (total_len < 4)
      continue;

    if (code == 0) {
      if (!isdigit((unsigned char)buf[0]) || !isdigit((unsigned char)buf[1]) ||
          !isdigit((unsigned char)buf[2])) {
        ESP_LOGE(TAG, "Invalid FTP response code format");
        return false;
      }
      code = (buf[0] - '0') * 100 + (buf[1] - '0') * 10 + (buf[2] - '0');
      if (code / 100 != expected_code_prefix) {
        ESP_LOGE(TAG, "Unexpected FTP code: %d", code);
        return false;
      }
      multiline = (buf[3] == '-');
      if (!multiline) {
        ESP_LOGI(TAG, "FTP Response: %s", buf);
        return true;
      }
    }
    if (multiline) {
      char *line = buf;
      while ((line = strstr(line, "\r\n")) != NULL) {
        line += 2;
        if (strncmp(line, buf, 3) == 0 && line[3] == ' ') {
          ESP_LOGI(TAG, "FTP Multiline Response: %s", buf);
          return true;
        }
      }
    }
    if (total_len >= (int)buf_size - 1) {
      ESP_LOGE(TAG, "FTP response too long");
      return false;
    }
  }
}

static void ftp_ota_task(void *param) {

  bool start = false;

  while (!start) {
    ESP_LOGE(TAG, "IST TIME %lld", (long long)ist);
    ESP_LOGE(TAG, "ota_start_time %lld", (long long)ota_start_time);
    if (ist >= ota_start_time) {
      start = true;
      break;
    }
    // Sleep for some time before checking again (e.g. 1 second)
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  char ftp_user[FTP_USER_MAX_LEN] = {0};
  char ftp_pass[FTP_PASS_MAX_LEN] = {0};
  char ftp_host[FTP_HOST_MAX_LEN] = {0};
  char ftp_file[FTP_FILE_MAX_LEN] = {0};
  int ftp_port = FTP_DEFAULT_PORT;

  if (!parse_ftp_url(ftp_url, ftp_user, ftp_pass, ftp_host, &ftp_port,
                     ftp_file)) {
    firmware_status = DownloadFailed;
    ESP_LOGE(TAG, "Failed to parse FTP URL");
    goto cleanup;
  }

  struct hostent *he = gethostbyname(ftp_host);
  if (!he) {
    ESP_LOGE(TAG, "DNS resolution failed for %s", ftp_host);
    goto cleanup;
  }

  int retry = 0;
  bool ota_done = false;
  firmware_status = Downloading;
  while (retry < MAX_OTA_RETRIES && !ota_done) {
    ESP_LOGW(TAG, "OTA attempt %d of %d", retry + 1, MAX_OTA_RETRIES);

    int control_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (control_sock < 0) {
      ESP_LOGE(TAG, "Failed to create control socket");
      retry++;
      vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
      continue;
    }

    struct timeval tv = {10, 0};
    setsockopt(control_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(ftp_port);
    memcpy(&server_addr.sin_addr, he->h_addr, he->h_length);

    if (connect(control_sock, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) != 0) {
      ESP_LOGE(TAG, "Failed to connect to FTP server");
      close(control_sock);
      retry++;
      vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
      continue;
    }

    char recv_buf[1024] = {0};
    char cmd_buf[256] = {0};

    if (!ftp_recv_and_check(control_sock, recv_buf, sizeof(recv_buf), 2)) {
      close(control_sock);
      retry++;
      vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
      continue;
    }

    snprintf(cmd_buf, sizeof(cmd_buf), "USER %s\r\n", ftp_user);
    ftp_send_command(control_sock, cmd_buf);
    if (!ftp_recv_and_check(control_sock, recv_buf, sizeof(recv_buf), 3)) {
      close(control_sock);
      retry++;
      vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
      continue;
    }

    snprintf(cmd_buf, sizeof(cmd_buf), "PASS %s\r\n", ftp_pass);
    ftp_send_command(control_sock, cmd_buf);
    if (!ftp_recv_and_check(control_sock, recv_buf, sizeof(recv_buf), 2)) {
      close(control_sock);
      retry++;
      vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
      continue;
    }

    ftp_send_command(control_sock, "TYPE I\r\n");
    if (!ftp_recv_and_check(control_sock, recv_buf, sizeof(recv_buf), 2)) {
      close(control_sock);
      retry++;
      vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
      continue;
    }

    ftp_send_command(control_sock, "PASV\r\n");
    if (!ftp_recv_and_check(control_sock, recv_buf, sizeof(recv_buf), 2)) {
      close(control_sock);
      retry++;
      vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
      continue;
    }

    int ip1, ip2, ip3, ip4, p1, p2;
    char *p_start = strchr(recv_buf, '(');
    char *p_end = strchr(recv_buf, ')');
    if (!p_start || !p_end ||
        sscanf(p_start + 1, "%d,%d,%d,%d,%d,%d", &ip1, &ip2, &ip3, &ip4, &p1,
               &p2) != 6) {
      ESP_LOGE(TAG, "Invalid PASV response");
      close(control_sock);
      retry++;
      vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
      continue;
    }

    char data_ip[16];
    snprintf(data_ip, sizeof(data_ip), "%d.%d.%d.%d", ip1, ip2, ip3, ip4);
    int data_port = p1 * 256 + p2;

    int data_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (data_sock < 0) {
      ESP_LOGE(TAG, "Failed to create data socket");
      close(control_sock);
      retry++;
      vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
      continue;
    }
    setsockopt(data_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in data_addr = {0};
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(data_port);
    inet_pton(AF_INET, data_ip, &data_addr.sin_addr);

    if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) !=
        0) {
      ESP_LOGE(TAG, "Failed to connect data socket");
      close(data_sock);
      close(control_sock);
      retry++;
      vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS * 1000));
      continue;
    }

    snprintf(cmd_buf, sizeof(cmd_buf), "RETR %s\r\n", ftp_file);
    ftp_send_command(control_sock, cmd_buf);
    if (!ftp_recv_and_check(control_sock, recv_buf, sizeof(recv_buf), 1)) {
      close(data_sock);
      close(control_sock);
      retry++;
      vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS * 1000));
      continue;
    }

    bool is_nuvoton = strstr(ftp_file, "/Nuvoton/") != NULL;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *ota_partition =
        esp_ota_get_next_update_partition(NULL);
    size_t write_offset = 0;

    if (!ota_partition) {
      ESP_LOGE(TAG, "Failed to find OTA partition");
      close(data_sock);
      close(control_sock);
      retry++;
      vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS * 1000));
      continue;
    }

    if (is_nuvoton) {
      ESP_LOGI(TAG, "Erasing OTA partition for Nuvoton binary...");
      esp_err_t err =
          esp_partition_erase_range(ota_partition, 0, ota_partition->size);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase OTA partition: %s",
                 esp_err_to_name(err));
        close(data_sock);
        close(control_sock);
        retry++;
        vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS * 1000));
        continue;
      }
    } else {
      esp_err_t err =
          esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        close(data_sock);
        close(control_sock);
        retry++;
        vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS * 1000));
        continue;
      }
    }

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    bool ota_failed = false;
    int read_bytes;
    do {
      read_bytes = recv(data_sock, recv_buf, sizeof(recv_buf), 0);
      if (read_bytes < 0) {
        ESP_LOGE(TAG, "Data recv error");
        ota_failed = true;
        break;
      }
      if (read_bytes > 0) {
        mbedtls_sha256_update(&sha_ctx, (const unsigned char *)recv_buf,
                              read_bytes);

        if (is_nuvoton) {
          if (write_offset + read_bytes > ota_partition->size) {
            ESP_LOGE(TAG, "Nuvoton binary too large for OTA partition");
            ota_failed = true;
            break;
          }
          esp_err_t err = esp_partition_write(ota_partition, write_offset,
                                              recv_buf, read_bytes);
          if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_partition_write failed at 0x%x: %s",
                     write_offset, esp_err_to_name(err));
            ota_failed = true;
            break;
          }
          write_offset += read_bytes;
        } else {
          if (esp_ota_write(ota_handle, recv_buf, read_bytes) != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed");
            ota_failed = true;
            break;
          }
        }
      }
    } while (read_bytes > 0);

    close(data_sock);
    close(control_sock);

    if (ota_failed) {
      if (!is_nuvoton) {
        esp_ota_end(ota_handle);
      }
      retry++;
      vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS * 1000));
      firmware_status = DownloadFailed;
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    // Finish SHA256 hash
    unsigned char sha_result[32];
    mbedtls_sha256_finish(&sha_ctx, sha_result);
    mbedtls_sha256_free(&sha_ctx);

    // Convert hash to hex string
    char calculated_hash[HASH_STR_LEN] = {0};
    for (int i = 0; i < 32; i++) {
      sprintf(&calculated_hash[i * 2], "%02x", sha_result[i]);
    }
    firmware_status = Downloaded;
    ESP_LOGI(TAG, "Calculated SHA256: %s", calculated_hash);
    ESP_LOGI(TAG, "Expected SHA256:   %s", expected_hash);

    if (strcasecmp(calculated_hash, expected_hash) != 0) {
      ESP_LOGE(TAG, "SHA256 mismatch");
      if (!is_nuvoton) {
        esp_ota_end(ota_handle);
      }
      retry++;
      firmware_status = InstallationFailed;
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    if (!is_nuvoton) {
      if (esp_ota_end(ota_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed");
        retry++;
        firmware_status = InstallationFailed;
        vTaskDelay(pdMS_TO_TICKS(3000));
        continue;
      }

      if (esp_ota_set_boot_partition(ota_partition) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed");
        retry++;
        firmware_status = InstallationFailed;
        vTaskDelay(pdMS_TO_TICKS(3000));
        continue;
      }
      firmware_status = Installed;
      ESP_LOGI(TAG, "ESP32 firmware OTA successful, rebooting...");
      ota_done = true;
      ftp_ota_task_handle = NULL; // Self delete and reboot after
      esp_restart();
    } else {

      ESP_LOGI(TAG, "Nuvoton firmware stored to OTA partition.");
      ESP_LOGI(TAG, "Sending to Nuvoton over UART...");
      firmware_status = Installing;
      esp_err_t err = send_partition_over_uart(ota_partition, write_offset);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART transfer to Nuvoton failed");
        firmware_status = InstallationFailed;
        retry++;
        vTaskDelay(pdMS_TO_TICKS(3000));
        continue;
      }

      ESP_LOGI(TAG, "Nuvoton update complete.");
      ota_done = true;
      firmware_status = Installed;
    }
  }

cleanup:
  if (ftp_ota_task_handle) {
    ftp_ota_task_handle = NULL;
  }
  ESP_LOGI(TAG, "OTA task ending");
  vTaskDelete(NULL);
}

void start_ftp_ota_update(const char *url, const char *sha256_hex,
                          uint8_t retry, uint16_t interval,
                          timer_t update_time) {
  if (!url || !sha256_hex) {
    ESP_LOGE(TAG, "Invalid OTA URL or hash");
    return;
  }

  if (ftp_ota_task_handle) {
    ESP_LOGW(TAG, "OTA update already in progress");
    return;
  }

  safe_strncpy(ftp_url, url, FTP_URL_MAX_LEN);
  safe_strncpy(expected_hash, sha256_hex, HASH_STR_LEN);
  MAX_OTA_RETRIES = retry;
  RETRY_INTERVAL_MS = interval;
  ota_start_time = update_time;
  BaseType_t result = xTaskCreate(&ftp_ota_task, "ftp_ota_task", 8192, NULL, 5,
                                  &ftp_ota_task_handle);
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Failed to create OTA task");
    ftp_ota_task_handle = NULL;
  }
}
