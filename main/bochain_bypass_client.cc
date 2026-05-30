#include "bochain_bypass_client.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display/display.h"
#include "settings.h"
#include "system_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cJSON.h>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include "esp_heap_caps.h"
#include <esp_system.h>
#include <algorithm>
#include <cstdio>

#define TAG "BochainBypass"

// 小智官方聊天走 OTA 下发的官方 websocket；这里是铂链直播旁路通道。
#define DEFAULT_BOCHAIN_BYPASS_WS_URL "wss://live.blsx.com/ws/live-device"
#define DEFAULT_BOCHAIN_REGISTER_URL "https://live.blsx.com/api/live-console/device/register"
#define DEFAULT_BOCHAIN_DEVICE_ID_PREFIX "BL-ESP32-"
#define DEFAULT_BOCHAIN_TOKEN ""
#define LEGACY_BOCHAIN_TOKEN "mr.fu875188"

namespace {
const char* GetJsonString(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

std::string FirstJsonString(cJSON* root, const char* const* keys, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const char* value = GetJsonString(root, keys[i]);
        if (value != nullptr && value[0] != '\0') {
            return value;
        }
    }
    return "";
}

bool IsType(const char* actual, const char* expected) {
    return actual != nullptr && strcmp(actual, expected) == 0;
}


std::string UrlEncode(const std::string& value) {
    std::string out;
    char buf[4];
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

bool LooksLikeLiveConsoleUrl(const std::string& url) {
    return url.find("live.blsx.com/ws/live-device") != std::string::npos ||
           url.find("live.blsx.com/ws/device") != std::string::npos ||
           url.find("live.blsx.com/ws/bochain-device") != std::string::npos;
}

void PlayDigitSound(char digit) {
    auto& audio_service = Application::GetInstance().GetAudioService();
    switch (digit) {
        case '0': audio_service.PlaySound(Lang::Sounds::OGG_0); break;
        case '1': audio_service.PlaySound(Lang::Sounds::OGG_1); break;
        case '2': audio_service.PlaySound(Lang::Sounds::OGG_2); break;
        case '3': audio_service.PlaySound(Lang::Sounds::OGG_3); break;
        case '4': audio_service.PlaySound(Lang::Sounds::OGG_4); break;
        case '5': audio_service.PlaySound(Lang::Sounds::OGG_5); break;
        case '6': audio_service.PlaySound(Lang::Sounds::OGG_6); break;
        case '7': audio_service.PlaySound(Lang::Sounds::OGG_7); break;
        case '8': audio_service.PlaySound(Lang::Sounds::OGG_8); break;
        case '9': audio_service.PlaySound(Lang::Sounds::OGG_9); break;
        default: break;
    }
}
}  // namespace

BochainBypassClient& BochainBypassClient::GetInstance() {
    static BochainBypassClient instance;
    return instance;
}

void BochainBypassClient::LoadSettings() {
    Settings settings("bochain", false);
    configured_url_ = settings.GetString("url", DEFAULT_BOCHAIN_BYPASS_WS_URL);
    register_url_ = settings.GetString("register_url", DEFAULT_BOCHAIN_REGISTER_URL);
    token_ = settings.GetString("token", DEFAULT_BOCHAIN_TOKEN);
    device_id_ = settings.GetString("device_id", "");
    speak_bind_code_ = settings.GetBool("speak_bind_code", true);

    // 旧版小皮/旁路固定 token 不再作为直播中控台认证 token 使用。
    // 发现旧 token 时清空，让设备重新向我们的后台注册并拿 live_devices.device_token。
    if (token_ == LEGACY_BOCHAIN_TOKEN) {
        token_.clear();
    }

    if (device_id_.empty()) {
        device_id_ = std::string(DEFAULT_BOCHAIN_DEVICE_ID_PREFIX) + SystemInfo::GetMacAddress();
    }

    if (!LooksLikeLiveConsoleUrl(configured_url_)) {
        ESP_LOGW(TAG, "Configured bypass url is legacy, switch to live-console default: %s", configured_url_.c_str());
        configured_url_ = DEFAULT_BOCHAIN_BYPASS_WS_URL;
    }
}

void BochainBypassClient::Start() {
    if (running_ || task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Already running");
        return;
    }

    Settings settings("bochain", false);
    bool enabled = settings.GetBool("enabled", true);
    if (!enabled) {
        ESP_LOGW(TAG, "Disabled by settings");
        return;
    }

    LoadSettings();
    stop_requested_ = false;
    running_ = true;

    BaseType_t ok = xTaskCreate(
        &BochainBypassClient::TaskEntry,
        "bochain_ws",
        4096 * 2,
        this,
        4,
        &task_handle_
    );

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create bochain bypass task");
        task_handle_ = nullptr;
        running_ = false;
    }
}

void BochainBypassClient::Stop() {
    stop_requested_ = true;
    websocket_.reset();
}

bool BochainBypassClient::IsConnected() const {
    return websocket_ != nullptr && websocket_->IsConnected();
}

std::vector<std::string> BochainBypassClient::BuildCandidateUrls() const {
    std::vector<std::string> urls;

    // 直播中控台设备入口优先，不再依赖小皮/旧 robot:8011 通道。
    const char* defaults[] = {
        configured_url_.empty() ? DEFAULT_BOCHAIN_BYPASS_WS_URL : configured_url_.c_str(),
        "wss://live.blsx.com/ws/live-device",
        "wss://live.blsx.com/ws/device",
        "wss://live.blsx.com/ws/bochain-device"
    };

    for (auto url : defaults) {
        if (url == nullptr || url[0] == '\0') continue;
        std::string full = BuildAuthenticatedWsUrl(url);
        bool exists = false;
        for (auto& item : urls) {
            if (item == full) {
                exists = true;
                break;
            }
        }
        if (!exists) urls.push_back(full);
    }
    return urls;
}

std::string BochainBypassClient::BuildAuthenticatedWsUrl(const std::string& base_url) const {
    std::string url = base_url;
    // 如果配置里已经带了参数，不重复追加。
    if (url.find("device_id=") != std::string::npos && url.find("token=") != std::string::npos) {
        return url;
    }
    url += (url.find('?') == std::string::npos) ? "?" : "&";
    url += "device_id=" + UrlEncode(device_id_);
    url += "&token=" + UrlEncode(token_);
    return url;
}


bool BochainBypassClient::RegisterWithLiveConsole() {
    if (!token_.empty() && token_ != LEGACY_BOCHAIN_TOKEN) {
        return true;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "Network is null, cannot register live device");
        return false;
    }

    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create http client for live device register");
        return false;
    }

    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
    http->SetHeader("Device-Id", device_id_.c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(root, "device_name", "铂链ESP32语音终端");
    cJSON_AddStringToObject(root, "device_type", "xiaozhi");

    cJSON* caps = cJSON_CreateObject();
    cJSON_AddBoolToObject(caps, "screen_display", true);
    cJSON_AddBoolToObject(caps, "system_tts", true);
    cJSON_AddBoolToObject(caps, "bochain_stream_tts", true);
    cJSON_AddBoolToObject(caps, "support_bind_code", true);
    cJSON_AddBoolToObject(caps, "support_audio_ready", true);
    // 当前固件还没有 MP3/URL 解码管线，远程 URL 先不声明为可用，避免后台误判。
    cJSON_AddBoolToObject(caps, "remote_audio_url", false);
    cJSON_AddItemToObject(root, "capabilities", caps);

    char* body = cJSON_PrintUnformatted(root);
    if (body == nullptr) {
        cJSON_Delete(root);
        return false;
    }
    http->SetContent(std::string(body));
    cJSON_free(body);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Register live device: %s", register_url_.c_str());
    if (!http->Open("POST", register_url_)) {
        ESP_LOGE(TAG, "Open register http failed, code=0x%x", http->GetLastError());
        return false;
    }

    int status = http->GetStatusCode();
    std::string resp = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "Register response status=%d body=%s", status, resp.c_str());
    if (status < 200 || status >= 300) {
        return false;
    }

    cJSON* json = cJSON_Parse(resp.c_str());
    if (json == nullptr) {
        return false;
    }

    const char* token_keys[] = {"device_token", "token"};
    std::string new_token = FirstJsonString(json, token_keys, sizeof(token_keys) / sizeof(token_keys[0]));
    cJSON* device = cJSON_GetObjectItem(json, "device");
    if (new_token.empty() && cJSON_IsObject(device)) {
        new_token = FirstJsonString(device, token_keys, sizeof(token_keys) / sizeof(token_keys[0]));
    }

    const char* code_keys[] = {"bind_code", "bindCode", "code"};
    std::string bind_code = FirstJsonString(json, code_keys, sizeof(code_keys) / sizeof(code_keys[0]));
    if (bind_code.empty() && cJSON_IsObject(device)) {
        bind_code = FirstJsonString(device, code_keys, sizeof(code_keys) / sizeof(code_keys[0]));
    }

    int bind_status = 0;
    cJSON* bind_status_item = cJSON_GetObjectItem(json, "bind_status");
    if (!cJSON_IsNumber(bind_status_item) && cJSON_IsObject(device)) {
        bind_status_item = cJSON_GetObjectItem(device, "bind_status");
    }
    if (cJSON_IsNumber(bind_status_item)) {
        bind_status = bind_status_item->valueint;
    }

    cJSON_Delete(json);

    if (new_token.empty()) {
        ESP_LOGE(TAG, "Register ok but device_token missing");
        return false;
    }

    token_ = new_token;

    // 正确顺序：
    // 1. 配网后如果拿到铂链 bind_code，先缓存，不抢小智官方绑定码播报。
    // 2. 等小智绑定/激活状态 bind_status != 0 后，再启动铂链绑定码播报窗口。
    // 3. 只要接口仍返回 bind_code，就说明铂链设备还未绑定，不能停止播报。
    // 4. 接口不再返回 bind_code 时，才认为铂链后台已绑定，停止播报。
    if (!bind_code.empty()) {
        bool is_new_bind_code = latest_bind_code_ != bind_code;
        latest_bind_code_ = bind_code;
        latest_bind_prompt_ = "铂链直播助手绑定码是" + bind_code;

        if (bind_status != 0) {
            bind_status_ = 0;  // 铂链仍未绑定，继续播我们的码
            if (is_new_bind_code || bind_prompt_window_start_us_ == 0) {
                RestartBindCodePromptWindow();
                ShowBindCode(speak_bind_code_, "register_after_xiaozhi_bound");
            }
        } else {
            // 小智还没绑定/激活，只缓存铂链码，不播报，避免和小智官方码打架。
            bind_status_ = 0;
            bind_prompt_window_start_us_ = 0;
            last_bind_prompt_us_ = 0;
            last_bind_status_refresh_us_ = 0;
            ESP_LOGI(TAG, "BoChain bind code cached, wait Xiaozhi bound before speaking");
        }
    } else {
        // 如果之前已经拿到过铂链绑定码，但本次 register 不再返回 bind_code，
        // 说明铂链后台大概率已完成绑定或已清除待绑定码，必须停止播报。
        if (!latest_bind_code_.empty()) {
            StopBindCodePrompt("register_no_bind_code_stop");
        } else {
            bind_status_ = bind_status;
            if (bind_status_ != 0) {
                StopBindCodePrompt("register_bound_status_no_bind_code");
            }
        }
    }

    {
        Settings settings("bochain", true);
        settings.SetString("device_id", device_id_);
        settings.SetString("token", token_);
        settings.SetString("url", configured_url_);
        settings.SetString("register_url", register_url_);
    }

    ESP_LOGI(TAG, "Live device registered, bind_status=%d, token_tail=%s", bind_status_, token_.size() > 6 ? token_.substr(token_.size() - 6).c_str() : token_.c_str());
    return true;
}

void BochainBypassClient::TaskEntry(void* arg) {
    auto* self = static_cast<BochainBypassClient*>(arg);
    self->Run();
    self->task_handle_ = nullptr;
    self->running_ = false;
    vTaskDelete(nullptr);
}

void BochainBypassClient::Run() {
    ESP_LOGI(TAG, "Task started, configured_url=%s, register_url=%s, device_id=%s", configured_url_.c_str(), register_url_.c_str(), device_id_.c_str());

    if (!RegisterWithLiveConsole()) {
        ESP_LOGW(TAG, "Initial live-console register failed, will retry before reconnect");
    }

    while (!stop_requested_) {
        if (token_.empty() && !RegisterWithLiveConsole()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        bool connected = false;
        auto urls = BuildCandidateUrls();
        for (const auto& url : urls) {
            if (stop_requested_) {
                break;
            }
            ESP_LOGI(TAG, "Try connect: %s", url.c_str());
            if (ConnectOnce(url)) {
                current_url_ = url;
                connected = true;
                break;
            }
            websocket_.reset();
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (!connected) {
            ESP_LOGW(TAG, "All live-console candidate urls failed, retry in 5 seconds");
            // 可能是服务器重装/数据库清空导致旧 token 失效，清空后下轮重新注册。
            if (!token_.empty()) {
                token_.clear();
                Settings settings("bochain", true);
                settings.EraseKey("token");
            }
            for (int i = 0; i < 50 && !stop_requested_; ++i) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        ESP_LOGI(TAG, "Connected: %s", current_url_.c_str());
        SendHello();

        while (!stop_requested_ && websocket_ != nullptr && websocket_->IsConnected()) {
            // BoChain V7: while TTS is active, periodically publish receive credits.
            // This lets the server send only when the device has real capacity.
            if (bochain_tts_active_) {
                SendAudioReady(false, 0);
            }
            MaybeRepeatBindCodePrompt();
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        ESP_LOGW(TAG, "Disconnected, will reconnect");
        websocket_.reset();
        for (int i = 0; i < 30 && !stop_requested_; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    websocket_.reset();
    ESP_LOGI(TAG, "Task stopped");
}

bool BochainBypassClient::ConnectOnce(const std::string& url) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "Network is null");
        return false;
    }

    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    websocket_->SetHeader("Device-Id", device_id_.c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    websocket_->SetHeader("Bochain-Token", token_.c_str());
    websocket_->SetHeader("Bochain-Role", "speaker");

	websocket_->OnData([this](const char* data, size_t len, bool binary) {
		if (binary) {
			HandleBinaryMessage(data, len);
			return;
		}
		HandleTextMessage(data, len);
	});

    websocket_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "WebSocket disconnected");
    });

    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Connect failed, url=%s, code=%d", url.c_str(), websocket_->GetLastError());
        return false;
    }

    return true;
}

void BochainBypassClient::SendHello() {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "role", "speaker");
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(root, "token", token_.c_str());
    cJSON_AddStringToObject(root, "version", "1.0.4-live-console-direct-v1");
    cJSON_AddStringToObject(root, "board_uuid", Board::GetInstance().GetUuid().c_str());
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "bypass_url", current_url_.c_str());
    cJSON_AddBoolToObject(root, "support_bind_code", true);
    cJSON_AddBoolToObject(root, "support_display", true);
    cJSON_AddBoolToObject(root, "support_digit_voice", true);
    cJSON_AddBoolToObject(root, "support_voice_query_bind_code", true);
    cJSON_AddBoolToObject(root, "support_play_audio_command", true);
    cJSON_AddBoolToObject(root, "support_live_console_action", true);
    cJSON_AddBoolToObject(root, "remote_audio_url", false);
    cJSON_AddBoolToObject(root, "bochain_stream_tts", true);

    // 告诉铂链服务器：本固件支持音频队列状态反馈
    cJSON_AddBoolToObject(root, "support_audio_status", true);
    cJSON_AddBoolToObject(root, "support_audio_params", true);
    cJSON_AddBoolToObject(root, "support_audio_watermark", true);
    cJSON_AddBoolToObject(root, "support_audio_ready", true);
    cJSON_AddStringToObject(root, "audio_flow_control", "credit_v7");

    // 告诉服务器当前旁路音频默认入队参数；后续每轮 TTS start 可动态覆盖。
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", current_audio_sample_rate_);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", current_audio_frame_duration_ms_);
    cJSON_AddItemToObject(root, "audio_params", audio_params);

    char* json = cJSON_PrintUnformatted(root);
    if (json != nullptr) {
        ESP_LOGI(TAG, "Send hello: %s", json);
        websocket_->Send(json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

void BochainBypassClient::HandleTextMessage(const char* data, size_t len) {
    std::string payload(data, len);
    ESP_LOGI(TAG, "recv: %s", payload.c_str());

    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Invalid JSON message");
        return;
    }

    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        ESP_LOGW(TAG, "Missing type in message");
        cJSON_Delete(root);
        return;
    }

    const char* message_type = type->valuestring;

    // 兼容直播中控台设备通道：{ type:"action", action:{ actionType:"..." } }
    if (IsType(message_type, "action")) {
        cJSON* action = cJSON_GetObjectItem(root, "action");
        if (cJSON_IsObject(action)) {
            const char* action_keys[] = {"actionType", "action_type", "type", "command", "cmd"};
            std::string action_type = FirstJsonString(action, action_keys, sizeof(action_keys) / sizeof(action_keys[0]));
            if (action_type == "play_audio_url" || action_type == "play_remote_audio" || action_type == "play_audio") {
                HandlePlayAudioMessage(action);
            } else if (action_type == "stop_audio" || action_type == "stop_current_audio" || action_type == "clear_audio_queue") {
                HandleStopAudioMessage(action);
            } else if (action_type == "clear_play_status") {
                DisplayBypassText("播放状态已清除", 3000);
                SendAck("clear_play_status", "ok", "status cleared");
            } else if (action_type == "speak" || action_type == "display" || action_type == "text" || action_type == "banner") {
                const char* keys[] = {"text", "display_text", "speak_text", "message", "content", "title"};
                std::string text = FirstJsonString(action, keys, sizeof(keys) / sizeof(keys[0]));
                if (!text.empty()) HandleSpeakText(text);
                SendAck(action_type.c_str(), "ok", text);
            } else {
                ESP_LOGW(TAG, "Unhandled live-console action: %s", action_type.c_str());
                SendAck(action_type.empty() ? "action" : action_type.c_str(), "ignored", "unsupported action");
            }
        }
        cJSON_Delete(root);
        return;
    }

    if (IsType(message_type, "hello")) {
        cJSON* bs = cJSON_GetObjectItem(root, "bind_status");
        if (cJSON_IsNumber(bs)) {
            int xiaozhi_bind_status = bs->valueint;
            ESP_LOGI(TAG, "Live-console hello ok, xiaozhi_bind_status=%d", xiaozhi_bind_status);

            // 小智绑定/激活后，如果本地已经缓存了铂链绑定码，就开始播报铂链码。
            if (xiaozhi_bind_status != 0 && !latest_bind_code_.empty() && bind_prompt_window_start_us_ == 0) {
                bind_status_ = 0;
                RestartBindCodePromptWindow();
                ShowBindCode(speak_bind_code_, "hello_after_xiaozhi_bound");
            }
        }
        cJSON_Delete(root);
        return;
    }

	if (IsType(message_type, "bind_code")) {
		HandleBindCodeMessage(root);
	} else if (IsType(message_type, "query_bind_code")) {
		HandleQueryBindCodeMessage(root);
	} else if (IsType(message_type, "tts")) {
		HandleTtsMessage(root);
	} else if (
		IsType(message_type, "play_audio") ||
        IsType(message_type, "music") ||
        IsType(message_type, "audio_play")
    ) {
        HandlePlayAudioMessage(root);
    } else if (IsType(message_type, "stop_audio")) {
        HandleStopAudioMessage(root);
    } else if (
        IsType(message_type, "speak") ||
        IsType(message_type, "display") ||
        IsType(message_type, "banner") ||
        IsType(message_type, "text")
    ) {
        const char* keys[] = {"text", "display_text", "speak_text", "message", "content"};
        std::string text = FirstJsonString(root, keys, sizeof(keys) / sizeof(keys[0]));
        if (!text.empty()) {
            HandleSpeakText(text);
        } else {
            ESP_LOGW(TAG, "%s message missing text", message_type);
        }
    } else if (IsType(message_type, "bind_success") || IsType(message_type, "bound")) {
        StopBindCodePrompt("bind_success_message");
        const char* success_keys[] = {"text", "message", "display_text"};
        std::string success_text = FirstJsonString(root, success_keys, sizeof(success_keys) / sizeof(success_keys[0]));
        if (success_text.empty()) {
            success_text = "设备绑定成功，已经归属到当前账号。";
        }
        HandleSpeakText(success_text);
    } else if (IsType(message_type, "audio")) {
        HandlePlayAudioMessage(root);
    } else if (IsType(message_type, "ping")) {
        SendPong();
    } else if (IsType(message_type, "pong")) {
        ESP_LOGD(TAG, "pong");
    } else {
        ESP_LOGW(TAG, "Unhandled message type: %s", message_type);
    }

    cJSON_Delete(root);
}


bool BochainBypassClient::TryHandleVoiceCommand(const std::string& text) {
    if (text.empty()) {
        return false;
    }

    // 只在已经拿到铂链绑定码后拦截，避免误伤小智官方激活流程。
    if (latest_bind_code_.empty()) {
        return false;
    }

    bool looks_like_bind_query =
        text.find("绑定码") != std::string::npos ||
        text.find("设备码") != std::string::npos ||
        text.find("配对码") != std::string::npos ||
        text.find("后台码") != std::string::npos;

    bool looks_like_query =
        text.find("查询") != std::string::npos ||
        text.find("查看") != std::string::npos ||
        text.find("多少") != std::string::npos ||
        text.find("是什么") != std::string::npos ||
        text.find("说一下") != std::string::npos ||
        text.find("播报") != std::string::npos;

    if (!looks_like_bind_query || !looks_like_query) {
        return false;
    }

    ESP_LOGI(TAG, "Voice query bind code intercepted: %s", text.c_str());

    // 15 秒内抑制小智云端随后返回的“不知道/找不到”等回答。
    suppress_xiaozhi_until_us_ = esp_timer_get_time() + 15LL * 1000 * 1000;
    ShowBindCode(true, "voice");
    SendAck("query_bind_code", "ok", "voice query handled locally");
    return true;
}

bool BochainBypassClient::ShouldSuppressXiaozhiResponse() const {
    return suppress_xiaozhi_until_us_ > 0 && esp_timer_get_time() < suppress_xiaozhi_until_us_;
}

void BochainBypassClient::HandleQueryBindCodeMessage(cJSON* root) {
    bool speak = false;
    cJSON* speak_item = cJSON_GetObjectItem(root, "speak");
    if (cJSON_IsBool(speak_item)) {
        speak = cJSON_IsTrue(speak_item);
    }

    // 后台主动查询默认只显示；传 speak:true 时才读数字，避免和小智官方播报混音。
    ShowBindCode(speak, "server");
    SendAck("query_bind_code", latest_bind_code_.empty() ? "empty" : "ok", latest_bind_code_.empty() ? "no bind code cached" : "bind code displayed");
}
void BochainBypassClient::HandleTtsMessage(cJSON* root) {
    cJSON* state = cJSON_GetObjectItem(root, "state");
    if (!cJSON_IsString(state)) {
        ESP_LOGW(TAG, "tts message missing state");
        return;
    }

    const char* state_value = state->valuestring;

if (strcmp(state_value, "start") == 0) {
      ESP_LOGI(TAG, "BoChain TTS start");
      InterruptXiaozhiForBochainPush("bochain_tts_start");

      // 服务端每轮 TTS start 下发真实音频参数，固件按参数入队，不再写死 24k。
      cJSON* audio_params = cJSON_GetObjectItem(root, "audio_params");
      if (cJSON_IsObject(audio_params)) {
          cJSON* sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
          cJSON* frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");

          if (cJSON_IsNumber(sample_rate) && sample_rate->valueint >= 8000 && sample_rate->valueint <= 48000) {
              current_audio_sample_rate_ = sample_rate->valueint;
          }
          if (cJSON_IsNumber(frame_duration) && frame_duration->valueint > 0 && frame_duration->valueint <= 120) {
              current_audio_frame_duration_ms_ = frame_duration->valueint;
          }
      }

      ESP_LOGI(TAG, "BoChain audio params: sample_rate=%d, frame_duration=%dms",
               current_audio_sample_rate_, current_audio_frame_duration_ms_);

      bochain_tts_active_ = true;

      // V8 smooth：BoChain 旁路 TTS 播放期间关闭 WiFi 省电，减少 WebSocket 音频抖动。
      esp_wifi_set_ps(WIFI_PS_NONE);

      // 新一轮旁路 TTS 开始，重置音频状态上报计数
      // 避免上一轮累计的 drop_count 影响下一轮自适应流控
      audio_drop_report_count_ = 0;
      last_audio_status_us_ = 0;
      last_audio_ready_us_ = 0;
      last_audio_ready_credits_ = -1;

      auto& app = Application::GetInstance();
      app.GetAudioService().ResetDecoder();
      SendAudioReady(true, 0);

      app.Schedule([]() {
          auto& app = Application::GetInstance();
          auto state = app.GetDeviceState();

          // V8 smooth：官方激活阶段不要强行切 speaking，避免 activating -> speaking 非法状态。
          if (state == kDeviceStateActivating) {
              ESP_LOGW(TAG, "Skip BoChain speaking state while activating");
              return;
          }

          if (state != kDeviceStateSpeaking) {
              app.SetDeviceState(kDeviceStateSpeaking);
          }
      });

      SendAck("tts", "start", "bochain tts started");
      return;
  }

    if (strcmp(state_value, "sentence_start") == 0) {
        cJSON* text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            ESP_LOGI(TAG, "BoChain TTS text: %s", text->valuestring);

            auto& app = Application::GetInstance();
            app.Schedule([message = std::string(text->valuestring)]() {
                auto display = Board::GetInstance().GetDisplay();
                if (display != nullptr) {
                    display->SetChatMessage("assistant", message.c_str());
                    display->ShowNotification(message.c_str(), 6000);
                }
            });
        }
        return;
    }

    if (
        strcmp(state_value, "stop") == 0 ||
        strcmp(state_value, "end") == 0 ||
        strcmp(state_value, "sentence_end") == 0
    ) {
        ESP_LOGI(TAG, "BoChain TTS stop");
        bochain_tts_active_ = false;

        // V8 smooth：测试阶段不立刻恢复 WiFi 省电，优先保证旁路流式音频稳定。
        esp_wifi_set_ps(WIFI_PS_NONE);

        auto& app = Application::GetInstance();
        app.Schedule([]() {
            if (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
                Application::GetInstance().SetDeviceState(kDeviceStateIdle);
            }
        });

        SendAck("tts", "stop", "bochain tts stopped");
        return;
    }

    ESP_LOGW(TAG, "Unhandled tts state: %s", state_value);
}

void BochainBypassClient::HandleBinaryMessage(const char* data, size_t len) {
    if (!bochain_tts_active_) {
        ESP_LOGW(TAG, "Ignore binary message because bochain tts is not active, len=%u", static_cast<unsigned>(len));
        return;
    }

    if (data == nullptr || len == 0) {
        ESP_LOGW(TAG, "Empty binary audio message");
        return;
    }

    auto& audio_service = Application::GetInstance().GetAudioService();

    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = current_audio_sample_rate_;
    packet->frame_duration = current_audio_frame_duration_ms_;
    packet->timestamp = 0;
    packet->payload.assign(
        reinterpret_cast<const uint8_t*>(data),
        reinterpret_cast<const uint8_t*>(data) + len
    );

    /*
     * 第一轮：非阻塞入队。
     * 如果队列没满，直接成功，正常播放。
     */
    bool ok = audio_service.PushPacketToDecodeQueue(std::move(packet), false);
    if (ok) {
        // BoChain V7: report current receive credits after every accepted frame, rate-limited inside.
        SendAudioReady(false, len);

        // Keep V6 status report for compatibility with existing server-side logs.
        int qsize = audio_service.GetDownlinkQueueSize();
        int qcap = audio_service.GetDownlinkQueueCapacity();
        if (qcap > 0 && qsize * 100 >= qcap * 60) {
            SendAudioStatus(false, 0, len);
        }
        return;
    }

    /*
     * BoChain V7: queue is full. Do NOT block the WebSocket receive callback.
     * Blocking here creates TCP backlog and makes the next seconds even more choppy.
     * We report zero/low credits immediately and drop this frame; the V7 server should stop
     * sending until audio_ready advertises capacity again.
     */
    ESP_LOGW(TAG, "Audio decode queue full, drop current frame and report credits, binary audio len=%u", static_cast<unsigned>(len));
    SendAudioStatus(true, 1, len);
    SendAudioReady(true, len);

    // Give decoder/output task a tiny slice without blocking the socket for a full frame.
    vTaskDelay(pdMS_TO_TICKS(2));
    return;
}
void BochainBypassClient::HandlePlayAudioMessage(cJSON* root) {
    InterruptXiaozhiForBochainPush("play_audio_command");
    const char* url_keys[] = {"audioUrl", "audio_url", "url", "src", "mp3_url", "fileUrl", "file_url", "playUrl", "play_url"};
    std::string url = FirstJsonString(root, url_keys, sizeof(url_keys) / sizeof(url_keys[0]));
    const char* title_keys[] = {"title", "audioTitle", "audio_title", "name", "text", "message"};
    std::string title = FirstJsonString(root, title_keys, sizeof(title_keys) / sizeof(title_keys[0]));
    if (title.empty()) {
        title = "音频播放测试";
    }

    std::string message;
    if (url.empty()) {
        message = "收到音频播放指令，但没有音频地址";
        ESP_LOGW(TAG, "%s", message.c_str());
        DisplayBypassText(message, 5000);
        SendAck("play_audio_url", "error", message);
        return;
    }

    // 直播中控台已经可以把命令推到 ESP32。当前固件音频管线支持的是“服务端按 Opus 帧推流”模式；
    // 对 MP3/URL 直放暂不声明 remote_audio_url，避免后台误以为已经能直接解码网络 MP3。
    message = "收到播报任务：" + title;
    ESP_LOGI(TAG, "play_audio_url command received, title=%s, url=%s", title.c_str(), url.c_str());
    DisplayBypassText(message, 6000);
    Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
    SendAck("play_audio_url", "received", url);
}

void BochainBypassClient::HandleStopAudioMessage(cJSON* root) {
    (void)root;
    DisplayBypassText("收到停止播放指令", 4000);
    Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
    SendAck("stop_audio", "received", "stop command received");
}

void BochainBypassClient::ShowBindCode(bool speak, const char* source) {
    std::string display_text;
    if (latest_bind_code_.empty()) {
        display_text = "当前没有铂链绑定码";
        DisplayBypassText(display_text, 6000);
        ESP_LOGW(TAG, "No BoChain bind code cached, source=%s", source ? source : "unknown");
        return;
    }

    display_text = "铂链直播助手绑定码是" + latest_bind_code_;
    DisplayBypassText(display_text, 20000);
    ESP_LOGI(TAG, "Show BoChain bind code, source=%s, speak=%d", source ? source : "unknown", speak ? 1 : 0);

    if (speak) {
        last_bind_prompt_us_ = esp_timer_get_time();
        SpeakBindCodeDigits(latest_bind_code_, display_text);
    }
}

void BochainBypassClient::SendAck(const char* event, const char* status, const std::string& message) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddStringToObject(root, "event", event ? event : "unknown");
    cJSON_AddStringToObject(root, "status", status ? status : "ok");
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(root, "message", message.c_str());

    char* json = cJSON_PrintUnformatted(root);
    if (json != nullptr) {
        websocket_->Send(json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

void BochainBypassClient::SendAudioReady(bool force, size_t last_packet_len) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return;
    }

    auto& audio_service = Application::GetInstance().GetAudioService();
    int decode_queue_size = audio_service.GetDecodeQueueSize();
    int decode_queue_capacity = audio_service.GetDecodeQueueCapacity();
    int playback_queue_size = audio_service.GetPlaybackQueueSize();
    int playback_queue_capacity = audio_service.GetPlaybackQueueCapacity();
    int queue_size = audio_service.GetDownlinkQueueSize();
    int queue_capacity = audio_service.GetDownlinkQueueCapacity();

    if (queue_capacity <= 0) {
        return;
    }

    int free_slots = queue_capacity - queue_size;
    if (free_slots < 0) {
        free_slots = 0;
    }

    // Keep a small safety reserve so the server does not fill the device to the brim.
    // credits is an absolute current allowance, not a cumulative counter.
    int credits = free_slots - 4;
    if (credits < 0) {
        credits = 0;
    }
    if (credits > 8) {
        credits = 8;
    }

    int64_t now_us = esp_timer_get_time();
    if (!force && last_audio_ready_us_ > 0 && (now_us - last_audio_ready_us_) < 200000 && credits == last_audio_ready_credits_) {
        return;
    }

    last_audio_ready_us_ = now_us;
    last_audio_ready_credits_ = credits;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "audio_ready");
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(root, "flow_control", "credit_v7");
    cJSON_AddNumberToObject(root, "credits", credits);
    cJSON_AddNumberToObject(root, "free_slots", free_slots);
    cJSON_AddNumberToObject(root, "queue_size", queue_size);
    cJSON_AddNumberToObject(root, "queue_capacity", queue_capacity);
    cJSON_AddNumberToObject(root, "decode_queue_size", decode_queue_size);
    cJSON_AddNumberToObject(root, "decode_queue_capacity", decode_queue_capacity);
    cJSON_AddNumberToObject(root, "playback_queue_size", playback_queue_size);
    cJSON_AddNumberToObject(root, "playback_queue_capacity", playback_queue_capacity);
    cJSON_AddNumberToObject(root, "last_packet_len", static_cast<int>(last_packet_len));
    cJSON_AddNumberToObject(root, "sample_rate", current_audio_sample_rate_);
    cJSON_AddNumberToObject(root, "frame_duration", current_audio_frame_duration_ms_);

    char* json = cJSON_PrintUnformatted(root);
    if (json != nullptr) {
        ESP_LOGD(TAG, "Send audio_ready: %s", json);
        websocket_->Send(json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

void BochainBypassClient::SendAudioStatus(bool queue_full, int drop_count, size_t last_packet_len) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return;
    }

    /*
     * 队列满时可能连续触发很多次。
     * 这里做限频：500ms 内最多上报一次，避免状态 JSON 反过来挤占音频通道。
     */
    int64_t now_us = esp_timer_get_time();
    audio_drop_report_count_ += drop_count > 0 ? drop_count : 0;

    if (last_audio_status_us_ > 0) {
        int64_t min_interval_us = queue_full ? 500000 : 300000;
        if ((now_us - last_audio_status_us_) < min_interval_us) {
            return;
        }
    }

    last_audio_status_us_ = now_us;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "audio_status");
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddBoolToObject(root, "queue_full", queue_full);
    cJSON_AddNumberToObject(root, "drop_count", audio_drop_report_count_ > 0 ? audio_drop_report_count_ : drop_count);
    cJSON_AddNumberToObject(root, "last_packet_len", static_cast<int>(last_packet_len));

    auto& audio_service = Application::GetInstance().GetAudioService();
    int decode_queue_size = audio_service.GetDecodeQueueSize();
    int decode_queue_capacity = audio_service.GetDecodeQueueCapacity();
    int playback_queue_size = audio_service.GetPlaybackQueueSize();
    int playback_queue_capacity = audio_service.GetPlaybackQueueCapacity();
    int queue_size = audio_service.GetDownlinkQueueSize();
    int queue_capacity = audio_service.GetDownlinkQueueCapacity();

    // V8.1 anti-tremble：高水位上报不要过于敏感。
    // playback_queue=2/2 在正常播放时也可能长期满，单独用它触发 queue_full
    // 会导致服务器频繁 pause，听感就是轻微“颤抖”。
    bool high_watermark = false;

    // 解码队列真正接近满时才认为需要服务端让速。
    if (decode_queue_capacity > 0 && decode_queue_size >= (decode_queue_capacity * 4 / 5)) {
        high_watermark = true;
    }

    // 下行队列明显积压时才认为网络/播放链路吃紧。
    if (queue_capacity > 0 && queue_size >= (queue_capacity * 5 / 6)) {
        high_watermark = true;
    }

    // playback 队列满只有在 decode 队列也偏高时才触发，避免正常满队列误报。
    if (
        playback_queue_capacity > 0 &&
        playback_queue_size >= playback_queue_capacity &&
        decode_queue_capacity > 0 &&
        decode_queue_size >= (decode_queue_capacity * 3 / 4)
    ) {
        high_watermark = true;
    }

    if (high_watermark) {
        queue_full = true;
    }

    cJSON_ReplaceItemInObject(root, "queue_full", cJSON_CreateBool(queue_full));

    cJSON_AddNumberToObject(root, "queue_size", queue_size);
    cJSON_AddNumberToObject(root, "queue_capacity", queue_capacity);
    cJSON_AddNumberToObject(root, "decode_queue_size", decode_queue_size);
    cJSON_AddNumberToObject(root, "decode_queue_capacity", decode_queue_capacity);
    cJSON_AddNumberToObject(root, "playback_queue_size", playback_queue_size);
    cJSON_AddNumberToObject(root, "playback_queue_capacity", playback_queue_capacity);

    cJSON_AddNumberToObject(root, "free_sram", static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(root, "sample_rate", current_audio_sample_rate_);
    cJSON_AddNumberToObject(root, "frame_duration", current_audio_frame_duration_ms_);

    char* json = cJSON_PrintUnformatted(root);
    if (json != nullptr) {
        ESP_LOGD(TAG, "Send audio_status: %s", json);
        websocket_->Send(json);
        cJSON_free(json);
    }

    cJSON_Delete(root);
    audio_drop_report_count_ = 0;
}

void BochainBypassClient::HandleBindCodeMessage(cJSON* root) {
    const char* code_keys[] = {"code", "bind_code"};
    std::string code;

    for (auto key : code_keys) {
        const char* value = GetJsonString(root, key);
        if (value && strlen(value) > 0) {
            code = value;
            break;
        }
    }

    // 后台如果推空 code，说明绑定码已清除或设备已绑定，立即停止播报。
    if (code.empty()) {
        ESP_LOGW(TAG, "bind_code message missing code, stop bind prompt");
        StopBindCodePrompt("bind_code_message_empty_code");
        return;
    }

    // 后台如果 bind_code 消息里明确带已绑定状态，也立即停止播报。
    cJSON* bs = cJSON_GetObjectItem(root, "bind_status");
    if (cJSON_IsNumber(bs) && bs->valueint != 0) {
        StopBindCodePrompt("bind_code_message_bound_status");
        return;
    }

    bind_status_ = 0;
    latest_bind_code_ = code;

    std::string display_text = "铂链直播助手绑定码是" + code;
    const char* display_from_json = GetJsonString(root, "display_text");
    if (display_from_json && strlen(display_from_json) > 0) {
        display_text = display_from_json;

        // 防止屏幕上只显示“绑定码”，和小智官方激活码混淆
        if (display_text.find("铂链") == std::string::npos) {
            display_text = "铂链" + display_text;
        }
    }

    latest_bind_prompt_ = display_text;
    ESP_LOGI(TAG, "BoChain bind code cached: %s, wait Xiaozhi bound before speaking", code.c_str());

    // 关键：收到铂链绑定码时只缓存，不立即播报，避免抢小智官方绑定码。
    // 等 hello/register 判断小智已绑定后，再 RestartBindCodePromptWindow + ShowBindCode。
    bind_prompt_window_start_us_ = 0;
    last_bind_prompt_us_ = 0;
    last_bind_status_refresh_us_ = 0;
}

void BochainBypassClient::HandleSpeakText(const std::string& text) {
    ESP_LOGI(TAG, "speak/display text: %s", text.c_str());
    DisplayBypassText(text, 5000);
}

void BochainBypassClient::DisplayBypassText(const std::string& text, int duration_ms) {
    auto& app = Application::GetInstance();
    app.Schedule([message = text, duration_ms]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->SetChatMessage("system", message.c_str());
            display->ShowNotification(message.c_str(), duration_ms);
        }
    });
}

void BochainBypassClient::SpeakBindCodeDigits(const std::string& code, const std::string& display_text) {
    auto& app = Application::GetInstance();
    app.Schedule([code, display_text]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->SetChatMessage("system", display_text.c_str());
        }

        // 固件当前只有内置数字音频资源；文字完整显示为“铂链直播助手绑定码是xxxx”，
        // 无屏幕设备至少能稳定听到绑定码数字，不依赖小智云端 TTS。
        auto& audio_service = Application::GetInstance().GetAudioService();
        audio_service.PlaySound(Lang::Sounds::OGG_POPUP);
        for (char digit : code) {
            PlayDigitSound(digit);
        }
    });
}

void BochainBypassClient::RestartBindCodePromptWindow() {
    int64_t now = esp_timer_get_time();
    bind_prompt_window_start_us_ = now;
    last_bind_prompt_us_ = 0;
    last_bind_status_refresh_us_ = 0;
}

void BochainBypassClient::StopBindCodePrompt(const char* source) {
    ESP_LOGI(TAG, "Stop BoChain bind code prompt, source=%s", source ? source : "unknown");
    bind_status_ = 1;
    latest_bind_code_.clear();
    latest_bind_prompt_.clear();
    bind_prompt_window_start_us_ = 0;
    last_bind_prompt_us_ = 0;
    last_bind_status_refresh_us_ = 0;
}

void BochainBypassClient::InterruptXiaozhiForBochainPush(const char* source) {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        ESP_LOGI(TAG, "Interrupt Xiaozhi for BoChain push, source=%s, state=%d", source ? source : "unknown", static_cast<int>(state));
        app.AbortSpeaking(kAbortReasonNone);
        app.GetAudioService().ResetDecoder();
        suppress_xiaozhi_until_us_ = esp_timer_get_time() + 10LL * 1000 * 1000;
    }
}

void BochainBypassClient::MaybeRepeatBindCodePrompt() {
    if (bind_status_ != 0 || latest_bind_code_.empty() || !speak_bind_code_) {
        return;
    }

    int64_t now = esp_timer_get_time();
    const int64_t interval_us = 5LL * 1000 * 1000;
    const int64_t status_refresh_interval_us = 10LL * 1000 * 1000;

    if (bind_prompt_window_start_us_ == 0) {
        // 还没等到小智绑定/激活，不启动铂链码播报窗口。
        // 但允许每 10 秒刷新一次 register，发现小智已绑定后由 RegisterWithLiveConsole 启动播报。
        if (last_bind_status_refresh_us_ == 0 || (now - last_bind_status_refresh_us_) >= status_refresh_interval_us) {
            last_bind_status_refresh_us_ = now;
            RegisterWithLiveConsole();
        }
        return;
    }

    // 3 分钟窗口到期后不再因为窗口结束而永久停止；
    // 只要铂链后台还没绑定，继续每 5 秒播报，直到 register 不再返回 bind_code 或收到 bind_success。

    // 关键保险：后台绑定成功但 WebSocket 成功消息没推到设备时，设备要主动核验注册接口。
    // 只要接口返回 bind_status != 0，StopBindCodePrompt 会立刻清空绑定码并停止后续播报。
    if (last_bind_status_refresh_us_ == 0 || (now - last_bind_status_refresh_us_) >= status_refresh_interval_us) {
        last_bind_status_refresh_us_ = now;
        RegisterWithLiveConsole();
        if (bind_status_ != 0 || latest_bind_code_.empty()) {
            return;
        }
    }

    if (last_bind_prompt_us_ != 0 && (now - last_bind_prompt_us_) < interval_us) {
        return;
    }

    if (Application::GetInstance().GetDeviceState() == kDeviceStateActivating) {
        return;
    }

    last_bind_prompt_us_ = now;
    ShowBindCode(true, "auto_repeat_3min");
}

void BochainBypassClient::SendPong() {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "pong");
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(root, "url", current_url_.c_str());

    char* json = cJSON_PrintUnformatted(root);
    if (json != nullptr) {
        websocket_->Send(json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}
