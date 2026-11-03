// apps/atc/atc_catalog.h —— 航空波段频道表(网络直播流,非无线电接收)。
// 频道覆盖玩法「网络航空波段收音机」的代表性机场塔台。
// 注意:radio_player 的声道目录上限是 kMaxStations(10)——它的电台表与 HLS
// 工作区共用 10KB 辅助内存(6000B 给 HLS,剩余才是电台表),超出部分会被
// install_stations() 静默截断,所以这里必须控制在 10 条以内。
#pragma once

#include <cstddef>

struct AtcChannel {
    const char *name;       // 展示名(机场+席位)
    const char *detail;     // 副标题(英文席位名)
    const char *url;        // MP3 直播流地址
};

static const AtcChannel kAtcChannels[] = {
    {"纽约肯尼迪 塔台", "JFK Tower", "http://audio.liveatc.net:8019/kjfk_twr.mp3"},
    {"洛杉矶 塔台", "LAX Tower", "http://audio.liveatc.net:8019/klax_twr.mp3"},
    {"旧金山 塔台", "SFO Tower", "http://audio.liveatc.net:8019/ksfo_twr.mp3"},
    {"芝加哥奥黑尔 塔台", "ORD Tower", "http://audio.liveatc.net:8019/kord_twr.mp3"},
    {"伦敦希思罗 塔台", "LHR Tower", "http://audio.liveatc.net:8019/egll_twr.mp3"},
    {"法兰克福 塔台", "FRA Tower", "http://audio.liveatc.net:8019/eddf_twr.mp3"},
    {"迪拜 塔台", "DXB Tower", "http://audio.liveatc.net:8019/omdb_twr.mp3"},
    {"香港 塔台", "HKG Tower", "http://audio.liveatc.net:8019/vhhh_twr.mp3"},
    {"东京成田 塔台", "NRT Tower", "http://audio.liveatc.net:8019/rjaa_twr.mp3"},
    {"悉尼 塔台", "SYD Tower", "http://audio.liveatc.net:8019/yssy_twr.mp3"},
};

static constexpr size_t kAtcChannelCount = sizeof(kAtcChannels) / sizeof(kAtcChannels[0]);
static_assert(kAtcChannelCount <= 10, "radio_player 的电台目录上限是 10 条");
