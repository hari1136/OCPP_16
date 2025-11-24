/*
 * smart_charging.c
 *
 *  Created on: 09-Oct-2025
 *      Author: Admin
 */
#include "cJSON.h"
#include "ocpp.h"
#include "utils.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TAG "Smart"

static ChargingProfile profileStore[MAX_PROFILES];
ChargingProfile currentprofile;
static int profileCount = 0;
int max_power;
void print_all_profiles() {
  for (int i = 0; i < profileCount; i++) {
    ChargingProfile *p = &profileStore[i];
    ESP_LOGI(
        TAG, "Profile #%d: ID=%d, Connector=%d, StackLevel=%d\n,Purpose=%s\n",
        i, p->chargingProfileId, p->connectorId, p->stackLevel, p->purpose);
  }
}

int get_seconds_since_midnight(time_t timestamp) {
  struct tm tm_time;
  gmtime_r(&timestamp, &tm_time); // Convert to UTC time

  int seconds_since_midnight =
      tm_time.tm_hour * 3600 + tm_time.tm_min * 60 + tm_time.tm_sec;
  return seconds_since_midnight;
}

void handle_set_charging_profile(cJSON *payload, const char *uniqueId) {
  cJSON *connectorIdItem = cJSON_GetObjectItem(payload, "connectorId");
  int connectorId = (connectorIdItem && cJSON_IsNumber(connectorIdItem))
                        ? connectorIdItem->valueint
                        : -1;

  cJSON *csChargingProfiles =
      cJSON_GetObjectItem(payload, "csChargingProfiles");
  if (!csChargingProfiles || !cJSON_IsObject(csChargingProfiles)) {
    ESP_LOGE(TAG, "Error: csChargingProfiles missing or invalid\n");
    send_call_result_with_status(uniqueId, "Rejected",
                                 "Invalid csChargingProfiles");
    return;
  }

  cJSON *chargingProfileIdItem =
      cJSON_GetObjectItem(csChargingProfiles, "chargingProfileId");
  int chargingProfileId =
      (chargingProfileIdItem && cJSON_IsNumber(chargingProfileIdItem))
          ? chargingProfileIdItem->valueint
          : -1;

  cJSON *stackLevelItem = cJSON_GetObjectItem(csChargingProfiles, "stackLevel");
  int stackLevel = (stackLevelItem && cJSON_IsNumber(stackLevelItem))
                       ? stackLevelItem->valueint
                       : -1;

  cJSON *purposeItem =
      cJSON_GetObjectItem(csChargingProfiles, "chargingProfilePurpose");
  const char *purpose = (purposeItem && cJSON_IsString(purposeItem))
                            ? purposeItem->valuestring
                            : "Unknown";

  cJSON *kindItem =
      cJSON_GetObjectItem(csChargingProfiles, "chargingProfileKind");
  const char *kind = (kindItem && cJSON_IsString(kindItem))
                         ? kindItem->valuestring
                         : "Unknown";

  cJSON *recurrKindItem =
      cJSON_GetObjectItem(csChargingProfiles, "recurrencyKind");
  const char *recurrKind = (recurrKindItem && cJSON_IsString(recurrKindItem))
                               ? recurrKindItem->valuestring
                               : "Unknown";
  cJSON *validFromItem = cJSON_GetObjectItem(csChargingProfiles, "validFrom");
  const char *validFrom = (validFromItem && cJSON_IsString(validFromItem))
                              ? validFromItem->valuestring
                              : NULL;

  cJSON *validToItem = cJSON_GetObjectItem(csChargingProfiles, "validTo");
  const char *validTo = (validToItem && cJSON_IsString(validToItem))
                            ? validToItem->valuestring
                            : NULL;

  cJSON *schedule = cJSON_GetObjectItem(csChargingProfiles, "chargingSchedule");
  if (!schedule || !cJSON_IsObject(schedule)) {
    ESP_LOGE(TAG,"Error: chargingSchedule missing or invalid\n");
    send_call_result_with_status(uniqueId, "Rejected",
                                 "Invalid chargingSchedule");
    return;
  }

  cJSON *durationItem = cJSON_GetObjectItem(schedule, "duration");
  int duration = (durationItem && cJSON_IsNumber(durationItem))
                     ? durationItem->valueint
                     : -1;

  cJSON *startScheduleItem = cJSON_GetObjectItem(schedule, "startSchedule");
  const char *startSchedule =
      (startScheduleItem && cJSON_IsString(startScheduleItem))
          ? startScheduleItem->valuestring
          : NULL;

  cJSON *chargingRateUnitItem =
      cJSON_GetObjectItem(schedule, "chargingRateUnit");
  const char *chargingRateUnit =
      (chargingRateUnitItem && cJSON_IsString(chargingRateUnitItem))
          ? chargingRateUnitItem->valuestring
          : NULL;

  cJSON *minChargingRateItem = cJSON_GetObjectItem(schedule, "minChargingRate");
  int minChargingRate =
      (minChargingRateItem && cJSON_IsNumber(minChargingRateItem))
          ? minChargingRateItem->valueint
          : -1;

  cJSON *periods = cJSON_GetObjectItem(schedule, "chargingSchedulePeriod");
  if (!periods || !cJSON_IsArray(periods)) {
    ESP_LOGE(TAG,"Error: chargingSchedulePeriod missing or invalid\n");
    send_call_result_with_status(uniqueId, "Rejected",
                                 "Invalid chargingSchedulePeriod");
    return;
  }

  int periodCount = cJSON_GetArraySize(periods);

  // Create and populate a new profile
  ChargingProfile newProfile;
  memset(&newProfile, 0, sizeof(newProfile));

  newProfile.connectorId = connectorId;
  newProfile.chargingProfileId = chargingProfileId;
  newProfile.stackLevel = stackLevel;
  safe_strncpy(newProfile.purpose, purpose, sizeof(newProfile.purpose));
  safe_strncpy(newProfile.kind, kind, sizeof(newProfile.kind));
  safe_strncpy(newProfile.recurrKind, recurrKind,
               sizeof(newProfile.recurrKind));

  if (validFrom) {
    newProfile.validFrom = parse_iso8601(validFrom);
  }
  if (validTo) {
    newProfile.validTo = parse_iso8601(validTo);
  }
  newProfile.duration = duration;

  if (startSchedule) {
    newProfile.startSchedule = parse_iso8601(startSchedule);
    //    newProfile.startSchedule += 19800;
  }

  if (chargingRateUnit)
    safe_strncpy(newProfile.chargingRateUnit, chargingRateUnit,
                 sizeof(newProfile.chargingRateUnit));

  newProfile.minChargingRate = minChargingRate;

  newProfile.periodCount = 0;
  for (int i = 0; i < periodCount && i < MAX_PERIODS; i++) {
    cJSON *period = cJSON_GetArrayItem(periods, i);
    if (!period || !cJSON_IsObject(period))
      continue;

    cJSON *startPeriodItem = cJSON_GetObjectItem(period, "startPeriod");
    cJSON *limitItem = cJSON_GetObjectItem(period, "limit");
    cJSON *numberPhasesItem = cJSON_GetObjectItem(period, "numberPhases");

    ChargingSchedulePeriod p;
    p.startPeriod = (startPeriodItem && cJSON_IsNumber(startPeriodItem))
                        ? startPeriodItem->valueint
                        : -1;
    p.limit =
        (limitItem && cJSON_IsNumber(limitItem)) ? limitItem->valueint : -1;
    p.numberPhases = (numberPhasesItem && cJSON_IsNumber(numberPhasesItem))
                         ? numberPhasesItem->valueint
                         : -1;

    newProfile.periods[newProfile.periodCount++] = p;
  }

  if ((strcmp(newProfile.purpose, "TxProfile") == 0) &&
      (!(gun1_state_value == 1 || gun1_transaction_active))) {
    send_call_result_with_status(uniqueId, "Rejected", "Profile store full");
    return;
  }

  bool profileUpdated = false;

  for (int i = 0; i < profileCount; i++) {
    if (profileStore[i].chargingProfileId == newProfile.chargingProfileId &&
        profileStore[i].connectorId == newProfile.connectorId) {
      profileStore[i] = newProfile; // Update existing profile
      profileUpdated = true;
      break;
    } else if (profileStore[i].stackLevel == newProfile.stackLevel &&
               strcmp(profileStore[i].purpose, newProfile.purpose) == 0) {
      profileStore[i] = newProfile; // Update existing profile
      profileUpdated = true;
      break;
    }
  }

  if (!profileUpdated) {
    if (profileCount < MAX_PROFILES) {
      profileStore[profileCount++] = newProfile; // Add new profile
    } else {
      ESP_LOGI(TAG,"Profile store full, cannot save more profiles.\n");
      send_call_result_with_status(uniqueId, "Rejected", "Profile store full");
      return;
    }
  }

  // Save the profile

  ESP_LOGI(TAG,"Stored new charging profile with ID %d for connector %d.\n",
         chargingProfileId, connectorId);

  send_call_result_with_status(uniqueId, "Accepted", "SetChargingProfile");

  print_all_profiles();
}

void clear_profile(int i, const char *uniqueId) {

  for (int j = i; j < profileCount - 1; j++) {
    profileStore[j] = profileStore[j + 1];
  }
  profileCount--;
  memset(&profileStore[profileCount], 0, sizeof(profileStore[profileCount]));
  send_call_result_with_status(uniqueId, "Accepted", "ClearChargingProfile");
}

void handle_clear_charging_profile(cJSON *payload, const char *uniqueId) {
  cJSON *profile_id = cJSON_GetObjectItem(payload, "id");

  cJSON *profile = cJSON_GetObjectItem(payload, "chargingProfilePurpose");
  cJSON *stack = cJSON_GetObjectItem(payload, "stackLevel");

  if (cJSON_IsNumber(profile_id)) {

    int id = profile_id->valueint;

    for (int i = 0; i < profileCount; i++) {
      if (profileStore[i].chargingProfileId == id) {
        // Clear the profile by shifting remaining entries left
        clear_profile(i, uniqueId);
        return;
      }
    }
  } else if (cJSON_IsNumber(stack) && cJSON_IsString(profile)) {

    int st_level = stack->valueint;
    char profile_purpose[30];
    safe_strncpy(profile_purpose, profile->valuestring,
                 sizeof(profile_purpose));
    for (int i = 0; i < profileCount; i++) {
      if (strcmp(profileStore[i].purpose, profile_purpose) == 0 &&
          profileStore[i].stackLevel == st_level) {
        clear_profile(i, uniqueId);
        return;
      }
    }
  }

  send_call_result_with_status(uniqueId, "Unknown", "ClearChargingProfile");
} // If no match was found

void get_profile(void *arg) {
  while (true) {
    ChargingProfile *CMactive = get_active_profile("ChargePointMaxProfile");
    ChargingProfile *TxDefaultactive = get_active_profile("TxDefaultProfile");
    ChargingProfile *Txactive = get_active_profile("TxProfile");

    //    if (CMactive) {
    //      printf("Active ChargePointMaxProfile: stackLevel=%d, maxPower=%d
    //      A\n",
    //             CMactive->stackLevel, CMactive->periods->limit);
    //    } else {
    //      printf("No active ChargePointMaxProfile found.\n");
    //    }
    //
    //    if (TxDefaultactive) {
    //      printf("Active TxDefaultProfile: stackLevel=%d, maxPower=%d A\n",
    //             TxDefaultactive->stackLevel,
    //             TxDefaultactive->periods->limit);
    //    } else {
    //      printf("No active TxDefaultProfile found.\n");
    //    }
    //
    //    if (Txactive) {
    //      printf("Active TxProfile: stackLevel=%d, maxPower=%d A\n",
    //             Txactive->stackLevel, Txactive->periods->limit);
    //    } else {
    //      printf("No active TxProfile found.\n");
    //    }

    // Set currentprofile with safety
    if (Txactive)
      currentprofile = *Txactive;
    else if (TxDefaultactive)
      currentprofile = *TxDefaultactive;
    else if (CMactive)
      currentprofile = *CMactive;
    else {
      ESP_LOGI(TAG,"No profile available to set as currentprofile.\n");
      memset(&currentprofile, 0, sizeof(currentprofile));
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

int get_power_limit(int current_time, int start_time,
                    ChargingSchedulePeriod *periods, int periodCount) {
  int max_power = 0;

  for (int i = 0; i < periodCount; i++) {
    int period_start_time = start_time + periods[i].startPeriod;

    if (current_time >= period_start_time) {
      max_power = periods[i].limit;
    } else {
      // Since periods are usually ordered, we can break early
      break;
    }
  }
  if (max_power >= 0)
    return max_power;
  else
    return 0;
}

bool is_time_in_window(int current_time, int start_time, int duration) {

  int end_time = start_time + duration;

  if (end_time <= 86400) {
    // Normal case
    return current_time >= start_time && current_time <= end_time;
  } else {
    // Midnight wrap-around
    int wrapped_end = end_time - 86400;
    return current_time >= start_time || current_time <= wrapped_end;
  }
}

ChargingProfile *get_active_profile(const char *targetPurpose) {
  ChargingProfile *selectedProfile = NULL;
  int ist_time = get_seconds_since_midnight(ist_now);

  struct tm ist_tm;
  localtime_r(&ist_now, &ist_tm);
  int today = ist_tm.tm_wday;

  for (int i = 0; i < profileCount; i++) {
    ChargingProfile *profile = &profileStore[i];

    // Skip unmatched purposes
    if (strcmp(profile->purpose, targetPurpose) != 0)
      continue;

    // Skip profiles not valid yet or expired (unless it's a TxProfile)
    if (strcmp("TxProfile", targetPurpose) != 0) {
      if ((profile->validFrom != 0 && ist_now < profile->validFrom) ||
          (profile->validTo != 0 && ist_now > profile->validTo)) {
        continue;
      }
    }

    int schedule_time = get_seconds_since_midnight(profile->startSchedule);

    struct tm profile_tm;
    localtime_r(&profile->startSchedule, &profile_tm);
    int profile_day = profile_tm.tm_wday;

    bool is_active = false;

    if (strcmp(profile->kind, "Absolute") == 0) {
      if ((profile->startSchedule == 0 || ist_now >= profile->startSchedule) &&
          (profile->duration == 0 ||
           ist_now <= profile->startSchedule + profile->duration)) {
        is_active = true;
      }

    } else if (strcmp(profile->kind, "Recurring") == 0) {
      if (strcmp(profile->recurrKind, "Daily") == 0) {
        is_active =
            is_time_in_window(ist_time, schedule_time, profile->duration);

      } else if (strcmp(profile->recurrKind, "Weekly") == 0) {
        if (today == profile_day) {
          is_active =
              is_time_in_window(ist_time, schedule_time, profile->duration);
        }
      }
    }

    if (!is_active)
      continue;

    // Select the profile with the highest stack level
    if (selectedProfile == NULL ||
        profile->stackLevel > selectedProfile->stackLevel) {
      selectedProfile = profile;
      max_power = get_power_limit(ist_time, schedule_time, profile->periods,
                                  profile->periodCount);
    }
  }

  return selectedProfile;
}

void handle_get_composite_schedule(cJSON *payload, const char *uniqueId) {
  cJSON *id = cJSON_GetObjectItem(payload, "connectorId");
  cJSON *duration = cJSON_GetObjectItem(payload, "duration");

  // Assuming currentprofile is a global or accessible ChargingProfile variable

  if (currentprofile.purpose[0] != '\0') {
    char scheduletime[20];

    struct tm st_tm;
    gmtime_r(&currentprofile.startSchedule, &st_tm);
    strftime(scheduletime, sizeof(scheduletime), "%Y-%m-%dT%H:%M:%S", &st_tm);
    ESP_LOGI(TAG,"ChargingProfile periods (count: %d):\n",
           currentprofile.periodCount);

    cJSON *response_payload = cJSON_CreateObject();

    // Add simple fields
    cJSON_AddItemToObject(response_payload, "connectorId",
                          cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(response_payload, "status",
                          cJSON_CreateString("Accepted"));
    cJSON_AddItemToObject(response_payload, "duration",
                          cJSON_Duplicate(duration, 1));
    cJSON_AddItemToObject(response_payload, "scheduleStart",
                          cJSON_CreateString(scheduletime));

    // === chargingSchedule object ===
    cJSON *chargingSchedule = cJSON_CreateObject();
    cJSON_AddItemToObject(chargingSchedule, "chargingRateUnit",
                          cJSON_CreateString("W"));
    cJSON_AddItemToObject(chargingSchedule, "minChargingRate",
                          cJSON_CreateNumber(1000));

    // === chargingSchedulePeriod array ===
    cJSON *chargingSchedulePeriodArray = cJSON_CreateArray();

    for (int i = 0; i < currentprofile.periodCount; i++) {
      ChargingSchedulePeriod *period = &currentprofile.periods[i];

      // Debug print
      ESP_LOGI(TAG,"Period %d: start=%ld duration=%d\n", i, (long)period->startPeriod,
             period->limit);

      // Create JSON object for this period
      cJSON *periodObj = cJSON_CreateObject();
      cJSON_AddItemToObject(periodObj, "startPeriod",
                            cJSON_CreateNumber((long)period->startPeriod));
      cJSON_AddItemToObject(periodObj, "limit",
                            cJSON_CreateNumber((double)period->limit));
      cJSON_AddItemToObject(periodObj, "numberPhases", cJSON_CreateNumber(3));

      // Add to array
      cJSON_AddItemToArray(chargingSchedulePeriodArray, periodObj);
    }

    // Add array to chargingSchedule
    cJSON_AddItemToObject(chargingSchedule, "chargingSchedulePeriod",
                          chargingSchedulePeriodArray);

    // Add chargingSchedule to main response
    cJSON_AddItemToObject(response_payload, "chargingSchedule",
                          chargingSchedule);

    send_call_result_with_payload(uniqueId, response_payload,
                                  "GetConfiguration");
    cJSON_Delete(response_payload);
    return;
  }

  send_call_result_with_status(uniqueId, "Rejected", "GetCompositeSchedule");
}
