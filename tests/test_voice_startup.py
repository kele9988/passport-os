"""Execute the production transport startup and user-facing Muse error mapper."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
voice = (root / 'main/apps/voice/app_voice.cc').read_text()
startup = voice[voice.index('static bool start_voice_transport(void)'):voice.index('void voice_app_start(void)')]
muse = (root / 'components/passport_muse/passport_muse.cc').read_text()
mapper = muse[muse.index('const char *voice_error_detail('):muse.index('void pairing_view(')]
code = r'''
#include <cassert>
#include <cstring>
#include <vector>
constexpr int ESP_OK=0;
static int udp_result, audio_result;
static std::vector<int> calls;
static int udp_audio_init() {calls.push_back(1);return udp_result;}
static int audio_streamer_init() {calls.push_back(2);return audio_result;}
static bool audio_streamer_deinit() {calls.push_back(3);return true;}
static int udp_audio_deinit() {calls.push_back(4);return 0;}
'''+startup+mapper+r'''
int main() {
    udp_result=-1; assert(!start_voice_transport()); assert((calls==std::vector<int>{1,4}));
    calls.clear(); udp_result=0; audio_result=-1;
    assert(!start_voice_transport()); assert((calls==std::vector<int>{1,2,3,4}));
    calls.clear(); audio_result=0;
    assert(start_voice_transport()); assert((calls==std::vector<int>{1,2}));
    assert(!strcmp(voice_error_detail("REPLY OUT OF MEMORY"),"接收内存不足，请返回重试"));
    assert(!strcmp(voice_error_detail("OUT OF MEMORY"),"录音内存不足，松开上键后重试"));
    assert(!strcmp(voice_error_detail("REPLY EVENT TOO LARGE"),"回复数据过大，请在手机查看"));
    assert(!strcmp(voice_error_detail("REPLY BUFFER LIMIT - TRY AGAIN"),"回复缓存已满，请在手机查看"));
}
'''
with tempfile.TemporaryDirectory() as temp:
    source=Path(temp)/'startup.cc'; binary=Path(temp)/'startup'
    source.write_text(code)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('Voice startup rollback/retry and Muse failure attribution: PASS')
