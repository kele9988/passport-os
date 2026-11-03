// main/app_store.h —— 配置在 NVS 中的持久化。
//
// NVS 写入是块级操作，掉电时可能只写了一部分，因此存的是"魔数 + 版本 + 长度"
// 的自描述块：版本或长度不符一律当作没有配置，走配网流程，而不是拿半截数据
// 去连一个不存在的网络。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include "live_config.h"
#include "esp_err.h"

#include <stdbool.h>

// 打开 NVS 分区（幂等）。失败通常意味着分区表或 Flash 有问题。
esp_err_t app_store_init(void);

// 读取配置。没有已保存配置时返回 ESP_ERR_NOT_FOUND 并填入默认值。
esp_err_t app_store_load(app_config_t *cfg);

esp_err_t app_store_save(const app_config_t *cfg);

// 清除配置（出厂/重新配网时使用）。
esp_err_t app_store_erase(void);
#ifdef __cplusplus
}
#endif
