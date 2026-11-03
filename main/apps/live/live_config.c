// main/app_config.c —— 配置默认值、校验与桥接地址解析（纯逻辑）。
#include "live_config.h"

#include <stdio.h>
#include <string.h>

void app_config_defaults(app_config_t *cfg)
{
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->path, sizeof(cfg->path), "%s", APP_CFG_DEFAULT_PATH);
    cfg->port = APP_CFG_DEFAULT_PORT;
    cfg->tls = false;
    cfg->volume = 70;
    cfg->greet_enter = true;
    cfg->thank_gift = true;
    cfg->thank_follow = true;
    cfg->thank_like = true;
    cfg->answer_comment = true;
}

bool app_config_ready(const app_config_t *cfg, char *why, size_t why_cap)
{
    if (why && why_cap) why[0] = '\0';
    if (cfg == NULL) return false;

    const char *missing = NULL;
    if (cfg->ssid[0] == '\0') missing = "未配置 Wi-Fi";
    else if (cfg->host[0] == '\0') missing = "未配置桥接地址";
    else if (cfg->port == 0) missing = "桥接端口无效";

    if (missing && why && why_cap) snprintf(why, why_cap, "%s", missing);
    return missing == NULL;
}

// 端口解析：拒绝 0 与越界值，不接受前导空格之后的杂字符。
static bool parse_port(const char *text, size_t len, uint16_t *port)
{
    if (len == 0 || len > 5) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + (uint32_t)(text[i] - '0');
    }
    if (value == 0 || value > 65535) return false;
    *port = (uint16_t)value;
    return true;
}

bool app_config_url_parse(const char *url, char *host, size_t host_cap, uint16_t *port,
                          char *path, size_t path_cap, bool *tls)
{
    if (url == NULL || host == NULL || port == NULL || path == NULL) return false;

    const char *p = url;
    while (*p == ' ') p++;

    bool secure = false;
    if (strncmp(p, "ws://", 5) == 0) p += 5;
    else if (strncmp(p, "wss://", 6) == 0) { p += 6; secure = true; }
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) { p += 8; secure = true; }

    // 主机段：到 ':'、'/' 或结尾为止。
    const char *host_start = p;
    while (*p != '\0' && *p != ':' && *p != '/') p++;
    const size_t host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len >= host_cap) return false;
    for (size_t i = 0; i < host_len; i++) {
        const char c = host_start[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }

    uint16_t parsed_port = APP_CFG_DEFAULT_PORT;
    if (*p == ':') {
        const char *port_start = ++p;
        while (*p >= '0' && *p <= '9') p++;
        if (!parse_port(port_start, (size_t)(p - port_start), &parsed_port)) return false;
    }

    const char *path_start = (*p == '/') ? p : APP_CFG_DEFAULT_PATH;
    size_t path_len = (*p == '/') ? strlen(p) : strlen(APP_CFG_DEFAULT_PATH);
    if (path_len >= path_cap) return false;

    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    memcpy(path, path_start, path_len);
    path[path_len] = '\0';
    *port = parsed_port;
    if (tls) *tls = secure;
    return true;
}

bool app_config_set_bridge(app_config_t *cfg, const char *url)
{
    if (cfg == NULL || url == NULL) return false;

    char host[APP_CFG_HOST_MAX];
    char path[APP_CFG_PATH_MAX];
    uint16_t port = 0;
    bool tls = false;
    if (!app_config_url_parse(url, host, sizeof(host), &port, path, sizeof(path), &tls)) {
        return false;
    }

    snprintf(cfg->host, sizeof(cfg->host), "%s", host);
    snprintf(cfg->path, sizeof(cfg->path), "%s", path);
    cfg->port = port;
    cfg->tls = tls;
    return true;
}

size_t app_config_build_uri(const app_config_t *cfg, char *buf, size_t cap)
{
    if (cfg == NULL || buf == NULL || cap == 0) return 0;
    const int written = snprintf(buf, cap, "%s://%s:%u%s",
                                 cfg->tls ? "wss" : "ws", cfg->host,
                                 (unsigned)cfg->port, cfg->path);
    if (written < 0 || (size_t)written >= cap) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)written;
}
