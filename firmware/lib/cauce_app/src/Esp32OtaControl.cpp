#include "cauce/app/Esp32OtaControl.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace cauce::app {

Esp32OtaControl::Esp32OtaControl() = default;

bool Esp32OtaControl::isPendingVerify() const {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr) return false;
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
  return state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool Esp32OtaControl::markAppValid() {
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

bool Esp32OtaControl::requestRollback() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr) return false;
  const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
  if (target == nullptr) return false;
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
  if (state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) return false;
    return true;
  }
  return esp_ota_set_boot_partition(target) == ESP_OK;
}

bool Esp32OtaControl::selfTestPassed() const { return selfTestPassed_; }

const char* Esp32OtaControl::runningPartitionLabel() const {
  const esp_partition_t* running = esp_ota_get_running_partition();
  return running != nullptr ? running->label : "";
}

uint32_t Esp32OtaControl::runningPartitionSize() const {
  const esp_partition_t* running = esp_ota_get_running_partition();
  return running != nullptr ? running->size : 0;
}

}  // namespace cauce::app

#endif
#endif
