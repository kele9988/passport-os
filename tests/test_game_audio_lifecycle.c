#include "platform.h"
#include <stdio.h>
#include <string.h>
#ifdef TEST_SFX
#include "../main/apps/game/game_sfx.c"
#define start_worker game_sfx_start
#define prepare_worker game_sfx_prepare_stop
#else
#include "../main/apps/game/game_mic.c"
#define start_worker game_mic_start
#define prepare_worker game_mic_prepare_stop
#endif
static void (*task)(void *);
static bool fail_create;
static int io_count,exits;
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned prio,TaskHandle_t *out){
 (void)name;(void)stack;(void)arg;(void)prio;if(fail_create)return 0;task=fn;*out=(void*)1;return pdPASS;
}
void vTaskDelete(TaskHandle_t handle){assert(handle==NULL);++exits;}
void vTaskDelay(unsigned ms){(void)ms;}
unsigned ulTaskNotifyTake(int clear,unsigned ticks){(void)clear;(void)ticks;return 1;}
void xTaskNotifyGive(TaskHandle_t handle){assert(handle==(void*)1);}
int bsp_audio_init(void){return ESP_OK;}
int bsp_audio_set_format(unsigned rate,uint8_t bits,uint8_t channels){assert(rate==16000&&bits==16&&channels==1);return ESP_OK;}
int bsp_audio_write(const void *data,size_t count){assert(data&&count);++io_count;assert(!prepare_worker());return ESP_OK;}
int bsp_audio_read(void *data,size_t count){memset(data,0,count);++io_count;assert(!prepare_worker());return ESP_OK;}
int main(void){
 assert(prepare_worker());
 fail_create=true;start_worker();assert(prepare_worker());fail_create=false;
 for(int i=0;i<30;++i){
  start_worker();assert(atomic_load(&s_running));
#ifdef TEST_SFX
  game_sfx_play(GS_START);
#endif
  int before=io_count;task(NULL);assert(io_count==before+1);
  assert(prepare_worker()&&s_task==NULL);
 }
 assert(exits==30);
 start_worker();assert(!prepare_worker());int before=io_count;task(NULL);
 assert(io_count==before&&prepare_worker());
 puts("Game audio: stop during real worker IO, idle cancellation, create failure and 30 repeated sessions PASS");
}
