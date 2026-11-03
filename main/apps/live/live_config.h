// main/app_config.h —— 设备运行配置（Wi-Fi、桥接地址、播报开关）。
//
// 与 NVS 读写分离：本文件只做结构与校验，app_store.c 负责落盘。URL 解析放在
// 这里是为了能在主机上测试——配网页写错一个端口不应该等到刷机后才发现。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_CFG_SSID_MAX  33
#define APP_CFG_PASS_MAX  65
#define APP_CFG_HOST_MAX  64
#define APP_CFG_PATH_MAX  48
#define APP_CFG_TOKEN_MAX 64
#define APP_CFG_DEFAULT_PORT 8787
#define APP_CFG_DEFAULT_PATH "/ws"

typedef struct {
    char ssid[APP_CFG_SSID_MAX];
    char pass[APP_CFG_PASS_MAX];
    char host[APP_CFG_HOST_MAX];
    char path[APP_CFG_PATH_MAX];
    char token[APP_CFG_TOKEN_MAX];
    uint16_t port;
    bool tls;
    uint8_t volume;              // 0..100
    bool greet_enter;
    bool thank_gift;
    bool thank_follow;
    bool thank_like;
    bool answer_comment;
} app_config_t;

void app_config_defaults(app_config_t *cfg);

// 是否具备联网播报的最低条件。why 用于在界面/日志上说明缺什么（可为 NULL）。
bool app_config_ready(const app_config_t *cfg, char *why, size_t why_cap);

// 解析 ws://host:port/path（也接受 wss/http/https 与省略协议的 host:port）。
// 成功后 host/port/path/tls 均为可用值，未写的端口与路径取默认值。
bool app_config_url_parse(const char *url, char *host, size_t host_cap, uint16_t *port,
                          char *path, size_t path_cap, bool *tls);

// 把 URL 写入配置；解析失败返回 false 且不改动 cfg。
bool app_config_set_bridge(app_config_t *cfg, const char *url);

// 生成 ws:// 或 wss:// 形式的目标地址（不含 token）。返回写入字节数。
size_t app_config_build_uri(const app_config_t *cfg, char *buf, size_t cap);
#ifdef __cplusplus
}
#endif
