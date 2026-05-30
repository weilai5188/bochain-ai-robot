#ifndef _BOCHAIN_BYPASS_CLIENT_H_
#define _BOCHAIN_BYPASS_CLIENT_H_

#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <web_socket.h>
#include <cJSON.h>

class BochainBypassClient {
public:
    static BochainBypassClient& GetInstance();

    void Start();
    void Stop();
    bool IsRunning() const { return running_; }
    bool IsConnected() const;

    // 从小智官方 STT 文本里本地拦截“查询绑定码”等命令，避免继续交给小智云端回答“找不到”。
    bool TryHandleVoiceCommand(const std::string& text);

    // 拦截到本地命令后，短时间内忽略小智云端返回的 TTS/LLM，避免两边同时说话。
    bool ShouldSuppressXiaozhiResponse() const;

private:
    BochainBypassClient() = default;
    ~BochainBypassClient() = default;
    BochainBypassClient(const BochainBypassClient&) = delete;
    BochainBypassClient& operator=(const BochainBypassClient&) = delete;

    static void TaskEntry(void* arg);
    void Run();
    bool ConnectOnce(const std::string& url);
    void SendHello();
    void SendPong();
    bool RegisterWithLiveConsole();
    std::string BuildAuthenticatedWsUrl(const std::string& base_url) const;
    void MaybeRepeatBindCodePrompt();
    void RestartBindCodePromptWindow();
    void StopBindCodePrompt(const char* source);
    void InterruptXiaozhiForBochainPush(const char* source);
	void HandleTextMessage(const char* data, size_t len);
	void HandleBinaryMessage(const char* data, size_t len);
	void HandleBindCodeMessage(cJSON* root);
	void HandleQueryBindCodeMessage(cJSON* root);
	void HandleTtsMessage(cJSON* root);
	void HandlePlayAudioMessage(cJSON* root);
	void HandleStopAudioMessage(cJSON* root);
	void HandleSpeakText(const std::string& text);
    void DisplayBypassText(const std::string& text, int duration_ms);
    void SpeakBindCodeDigits(const std::string& code, const std::string& display_text);
    void ShowBindCode(bool speak, const char* source);

    // 发送普通确认消息给铂链服务器
    void SendAck(const char* event, const char* status, const std::string& message);

    // 发送音频解码队列状态给铂链服务器，用于服务器自适应降速
    void SendAudioStatus(bool queue_full, int drop_count, size_t last_packet_len);

    // BoChain V7：设备主动告诉服务器还能接收多少帧，服务器按 credit 发包。
    void SendAudioReady(bool force = false, size_t last_packet_len = 0);

    void LoadSettings();
    std::vector<std::string> BuildCandidateUrls() const;

    TaskHandle_t task_handle_ = nullptr;
    std::unique_ptr<WebSocket> websocket_;
    bool running_ = false;
    bool stop_requested_ = false;
    std::string configured_url_;
    std::string register_url_;
    std::string current_url_;
    std::string device_id_;
    std::string token_;
    std::string latest_bind_code_;
    std::string latest_bind_prompt_;
	bool speak_bind_code_ = true;
    int bind_status_ = 0;
    int64_t last_bind_prompt_us_ = 0;
    int64_t bind_prompt_window_start_us_ = 0;
    int64_t last_bind_status_refresh_us_ = 0;
	bool bochain_tts_active_ = false;
	int64_t suppress_xiaozhi_until_us_ = 0;

    // 旁路音频参数：由服务端每轮 TTS start 下发；没有下发时保持旧版 24k/60ms。
    int current_audio_sample_rate_ = 24000;
    int current_audio_frame_duration_ms_ = 60;

    // 音频队列满状态上报限频，避免队列满时疯狂发 JSON
    int64_t last_audio_status_us_ = 0;
    int audio_drop_report_count_ = 0;

    // BoChain V7 audio_ready credit flow control.
    int64_t last_audio_ready_us_ = 0;
    int last_audio_ready_credits_ = -1;
};

#endif // _BOCHAIN_BYPASS_CLIENT_H_
