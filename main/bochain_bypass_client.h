#ifndef _BOCHAIN_BYPASS_CLIENT_H_
#define _BOCHAIN_BYPASS_CLIENT_H_

#include <memory>
#include <string>
#include <vector>
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
    void HandleTextMessage(const char* data, size_t len);
    void HandleBindCodeMessage(cJSON* root);
    void HandleQueryBindCodeMessage(cJSON* root);
    void HandlePlayAudioMessage(cJSON* root);
    void HandleStopAudioMessage(cJSON* root);
    void HandleSpeakText(const std::string& text);
    void DisplayBypassText(const std::string& text, int duration_ms);
    void SpeakBindCodeDigits(const std::string& code, const std::string& display_text);
    void ShowBindCode(bool speak, const char* source);
    void SendAck(const char* event, const char* status, const std::string& message);
    void LoadSettings();
    std::vector<std::string> BuildCandidateUrls() const;

    TaskHandle_t task_handle_ = nullptr;
    std::unique_ptr<WebSocket> websocket_;
    bool running_ = false;
    bool stop_requested_ = false;
    std::string configured_url_;
    std::string current_url_;
    std::string device_id_;
    std::string token_;
    std::string latest_bind_code_;
    std::string latest_bind_prompt_;
    bool speak_bind_code_ = false;
    int64_t suppress_xiaozhi_until_us_ = 0;
};

#endif // _BOCHAIN_BYPASS_CLIENT_H_
