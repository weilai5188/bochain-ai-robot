#ifndef _BOCHAIN_BYPASS_CLIENT_H_
#define _BOCHAIN_BYPASS_CLIENT_H_

#include <memory>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <web_socket.h>

class BochainBypassClient {
public:
    static BochainBypassClient& GetInstance();

    void Start();
    void Stop();
    bool IsRunning() const { return running_; }
    bool IsConnected() const;

private:
    BochainBypassClient() = default;
    ~BochainBypassClient() = default;
    BochainBypassClient(const BochainBypassClient&) = delete;
    BochainBypassClient& operator=(const BochainBypassClient&) = delete;

    static void TaskEntry(void* arg);
    void Run();
    bool ConnectOnce(const std::string& url);
    void SendHello();
    void HandleTextMessage(const char* data, size_t len);
    void HandleSpeakText(const std::string& text);
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
};

#endif // _BOCHAIN_BYPASS_CLIENT_H_
