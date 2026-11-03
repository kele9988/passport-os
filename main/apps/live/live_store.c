// main/app_store.c —— NVS 持久化实现。
#include "live_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "app_store";

#define APP_STORE_NAMESPACE "aipasprt"
#define APP_STORE_KEY       "cfg"
#define APP_STORE_MAGIC     0x41495031u   // "AIP1"
#define APP_STORE_VERSION   1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    app_config_t config;
} app_store_blob_t;

esp_err_t app_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 只在这个明确的、无法继续使用的前提下擦除 NVS，属于恢复动作而非日常路径。
        ESP_LOGW(TAG, "NVS 需要重新初始化: %s", esp_err_to_name(err));
        esp_err_t erase_err = nvs_flash_erase();
        if (erase_err != ESP_OK) {
            ESP_LOGE(TAG, "NVS 擦除失败: %s", esp_err_to_name(erase_err));
            return erase_err;
        }
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t app_store_load(app_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    app_config_defaults(cfg);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_STORE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;

    app_store_blob_t blob;
    size_t length = sizeof(blob);
    err = nvs_get_blob(handle, APP_STORE_KEY, &blob, &length);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    if (length != sizeof(blob) || blob.magic != APP_STORE_MAGIC ||
        blob.version != APP_STORE_VERSION || blob.size != sizeof(app_config_t)) {
        ESP_LOGW(TAG, "配置版本不匹配(ver=%u size=%u)，按未配置处理",
                 (unsigned)blob.version, (unsigned)blob.size);
        return ESP_ERR_INVALID_VERSION;
    }

    *cfg = blob.config;
    // 兜底：结构来自旧固件时可能是 0，避免音量被静音成 0 或端口失效。
    if (cfg->port == 0) cfg->port = APP_CFG_DEFAULT_PORT;
    if (cfg->path[0] != '/') {
        strncpy(cfg->path, APP_CFG_DEFAULT_PATH, sizeof(cfg->path) - 1);
        cfg->path[sizeof(cfg->path) - 1] = '\0';
    }
    if (cfg->volume > 100) cfg->volume = 70;
    return ESP_OK;
}

esp_err_t app_store_save(const app_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;

    app_store_blob_t blob = {
        .magic = APP_STORE_MAGIC,
        .version = APP_STORE_VERSION,
        .size = sizeof(app_config_t),
    };
    blob.config = *cfg;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, APP_STORE_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) ESP_LOGE(TAG, "保存配置失败: %s", esp_err_to_name(err));
    return err;
}

esp_err_t app_store_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
