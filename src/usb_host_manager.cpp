#include "usb_host_manager.h"

#include <Arduino.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"

namespace {

TaskHandle_t usbHostTaskHandle = nullptr;
volatile esp_err_t installResult = ESP_FAIL;
volatile esp_err_t powerResult = ESP_FAIL;

#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
bool allowUsbDeviceEnumeration(const usb_device_desc_t*, uint8_t*) {
    return true;
}
#endif

void usbHostLibraryTask(void* notifyTask) {
    const usb_host_config_t config{
        .skip_phy_setup = false,
        .root_port_unpowered = true,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
        .enum_filter_cb = allowUsbDeviceEnumeration
#endif
    };

    installResult = usb_host_install(&config);
    if (installResult == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        powerResult = usb_host_lib_set_root_port_power(true);
    }
    xTaskNotifyGive(static_cast<TaskHandle_t>(notifyTask));

    if (installResult != ESP_OK || powerResult != ESP_OK) {
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        uint32_t eventFlags = 0;
        const esp_err_t result =
            usb_host_lib_handle_events(portMAX_DELAY, &eventFlags);
        if (result == ESP_OK &&
            (eventFlags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0) {
            usb_host_device_free_all();
        }
    }
}

}  // namespace

bool usbHostManagerBegin() {
    if (usbHostTaskHandle != nullptr) {
        return installResult == ESP_OK && powerResult == ESP_OK;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        usbHostLibraryTask,
        "usb_host_events",
        4096,
        xTaskGetCurrentTaskHandle(),
        2,
        &usbHostTaskHandle,
        0
    );
    if (created != pdPASS ||
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 0) {
        return false;
    }
    return installResult == ESP_OK && powerResult == ESP_OK;
}
