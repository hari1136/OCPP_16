/*
 * utils.c
 *
 *  Created on: 04-Oct-2025
 *      Author: Admin
 */

#include "utils.h"

 void safe_strncpy(char *dest, const char *src, size_t len) {
  if (len == 0)
    return;
  strncpy(dest, src, len - 1);
  dest[len - 1] = '\0'; // Always null-terminate
}