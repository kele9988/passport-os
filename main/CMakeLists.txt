idf_component_register(
    EMBED_TXTFILES "apps/home/phone_setup.html"
    EMBED_FILES "apps/home/cities.json.gz"
    SRCS
        "main.cc"
        "apps/xiaozhi/opus_encoder_storage.cc"
        "boot_chime.c"
        "ui_sfx.c"
        "ui_sfx_tone.c"
        "boot_splash.c"
        "launcher/launcher.cc"
        "launcher/app_registry.cc"
        "launcher/wifi_setup.cc"
        "launcher/wifi_picker.cc"
        "launcher/network_diagnostics.cc"
        "launcher/settings.cc"
        "launcher/power.cc"
        "launcher/system_overlay.cc"
        "fonts/radio_font.c"
        "fonts/radio_font_title.c"
        "fonts/buddy_font_16.c"
        "fonts/voice_font_20.c"
        "fonts/ui_font_20.c"
        "fonts/home_font_12.c"
        "apps/radio/app_radio.cc"
        "apps/radio/radio_catalog.cc"
        "apps/radio/radio_display_settings.cc"
        "apps/radio/radio_player.cc"
        "apps/radio/radio_source.cc"
        "apps/radio/radio_sleep_timer.cc"
        "apps/radio/radio_ui.cc"
        "apps/voice/time_sync.c"
        "apps/vokie/app_vokie.cc"
        "apps/vokie/vokie_ble.c"
        "apps/vokie/ui_status.c"
        "apps/wbconsole/app_wbconsole.cc"
        "apps/wbconsole/wb_ble.c"
        "apps/wbconsole/wb_wifi_link.c"
        "apps/wbconsole/wb_voice.c"
        # 抖音直播助手(apps/live)已于 2026-11-03 按用户要求摘除:源码保留,不参与编译。
        "apps/talkie/app_talkie.cc"
        "apps/talkie/talkie_link.cc"
        "apps/talkie/talkie_local.cc"
        "apps/talkie/walkie/protocol.cpp"
        "apps/talkie/walkie/adpcm.cpp"
        "apps/talkie/walkie/audio_jitter.cpp"
        "apps/talkie/walkie/talk_controller.cpp"
        "apps/talkie/walkie/presence.cpp"
        "apps/talkie/walkie/espnow_transport.cc"
        "ui/ui_pixel.c"
        "ui/ui_capture.c"
        "apps/xiaozhi/xiaozhi_protocol.cc"
        "apps/xiaozhi/xiaozhi_audio.cc"
        "apps/xiaozhi/xiaozhi_ui.cc"
        "apps/xiaozhi/xiaozhi_store.cc"
        "apps/xiaozhi/xiaozhi_ota.cc"
        "apps/xiaozhi/app_xiaozhi.cc"
        "apps/home/home_profile.c"
        "apps/home/home_clock.c"
        "apps/home/home_weather.c"
        "apps/home/band_art.c"
        "apps/home/home_upload.cc"
        "apps/home/app_home.cc"
        # 三个小游戏(《你在狗叫什么》《跳一跳》《俄罗斯方块》)已于 2026-11-01 下架:
        # 源文件不再参与编译,链接器随之裁掉游戏核心/麦克风/音效模块,腾出 flash 与堆。
    INCLUDE_DIRS
        "."
        "launcher"
        "apps"
        "apps/radio"
        "apps/xiaozhi"
        "apps/home"
        "apps/game"
        "apps/wbconsole"
        "apps/talkie"
        "ui"
        "fonts"
    REQUIRES
        bsp
        bt
        esp-wifi-connect
        esp_wifi
        nvs_flash
        esp_http_client
        esp_websocket_client
        json
        mbedtls
        esp_codec_dev
        esp_audio_codec
        esp_timer
        esp_pm
        spi_flash
        esp_driver_usb_serial_jtag
        esp_http_server
        spiffs
        lwip
)
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--undefined=__wrap_opus_encoder_create,--undefined=__wrap_opus_encoder_destroy"
    "-Wl,--wrap=opus_encoder_create,--wrap=opus_encoder_destroy")

if(PASSPORT_RADIO_HLS_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PASSPORT_RADIO_HLS_TEST=1)
endif()

# 公开发行版：对讲机不预置作者私有中转主机（见 talkie_link.cc kDefaultHost）。
# 私享分发版：-DPASSPORT_DEFAULT_HOST=<域名或IP> [-DPASSPORT_DEFAULT_ROOM=<房间>]
# 注入默认中转主机/房间（仅当 PASSPORT_PUBLIC_RELEASE=ON 时生效），刷完即连。
if(PASSPORT_PUBLIC_RELEASE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TALKIE_PUBLIC_RELEASE=1)
    if(PASSPORT_DEFAULT_HOST)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE
            TALKIE_DEFAULT_HOST="${PASSPORT_DEFAULT_HOST}")
    endif()
endif()
if(PASSPORT_DEFAULT_ROOM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        TALKIE_DEFAULT_ROOM="${PASSPORT_DEFAULT_ROOM}")
endif()
