#include "opus_decoder_storage.h"
#include "audio_workspace.h"
#include "esp_log.h"

// Keep esp_opus_enc's configuration and frame API unchanged. Only its libopus
// factory uses caller storage, through the same pinned public API as decoding.
// AudioService closes the old codec while its worker is idle before reusing it.
extern "C" OpusEncoder *__real_opus_encoder_create(int32_t, int, int, int *);
extern "C" void __real_opus_encoder_destroy(OpusEncoder *);
extern "C" OpusEncoder *__wrap_opus_encoder_create(int32_t rate, int channels,
                                                  int application, int *error) {
    // 对讲机与小智共用同一编码配置(16kHz/单声道/60ms/complexity 0),同样放进
    // 共享静态存储:近场⇄联网运行时切换后,WS 兜底 TLS 已把堆碎片化,真实
    // malloc 需要的大块连续内存不再可得(largest~18KB),编码器必 NO_MEM,
    // 表现为「切回联网永远失败退回近场」。编码器与解码器半双工互斥,
    // 与小智共享同一 aux 切片是安全的。
    const auto o = audio_workspace::owner.load();
    if (o != audio_workspace::Owner::Xiaozhi && o != audio_workspace::Owner::Talkie)
        return __real_opus_encoder_create(rate,channels,application,error);
    const int bytes=opus_encoder_get_size(channels);
    if (bytes<=0 || static_cast<size_t>(bytes)>audio_workspace::kAuxBytes) {
        if(error)*error=-7; // OPUS_ALLOC_FAIL; fail before touching the storage.
        return nullptr;
    }
    auto *state=reinterpret_cast<OpusEncoder *>(audio_workspace::aux());
    const int result=opus_encoder_init(state,rate,channels,application);
    if(error)*error=result;
    if(result!=0)return nullptr;
    ESP_LOGI("xiaozhi_audio", "Opus encoder in shared storage: %d bytes",bytes);
    return state;
}
extern "C" void __wrap_opus_encoder_destroy(OpusEncoder *state) {
    if (reinterpret_cast<void *>(state)!=audio_workspace::aux())
        __real_opus_encoder_destroy(state);
}
