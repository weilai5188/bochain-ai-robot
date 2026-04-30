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

#define TAG "BochainBypass"

// 小智官方聊天走 OTA 下发的官方 websocket；这里是铂链直播旁路通道。
#define DEFAULT_BOCHAIN_BYPASS_WS_URL "ws://robot.blsx.com:8011/danmaku/"
#define DEFAULT_BOCHAIN_DEVICE_ID_PREFIX "BL-ESP32-"
#define DEFAULT_BOCHAIN_TOKEN "mr.fu875188"

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
    token_ = settings.GetString("token", DEFAULT_BOCHAIN_TOKEN);
    device_id_ = settings.GetString("device_id", "");
    speak_bind_code_ = settings.GetBool("speak_bind_code", false);
    if (device_id_.empty()) {
        device_id_ = std::string(DEFAULT_BOCHAIN_DEVICE_ID_PREFIX) + SystemInfo::GetMacAddress();
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
    if (!configured_url_.empty()) {
        urls.push_back(configured_url_);
    }

    // 一次构建多种候选地址，方便远程打包后真机直接多试几个路径。
    const char* defaults[] = {
        "ws://robot.blsx.com:8011/danmaku/",
        "ws://robot.blsx.com:8011/danmaku",
        "ws://robot.blsx.com:8011/",
        "ws://robot.blsx.com:8011"
    };
    for (auto url : defaults) {
        bool exists = false;
        for (auto& item : urls) {
            if (item == url) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            urls.push_back(url);
        }
    }
    return urls;
}

void BochainBypassClient::TaskEntry(void* arg) {
    auto* self = static_cast<BochainBypassClient*>(arg);
    self->Run();
    self->task_handle_ = nullptr;
    self->running_ = false;
    vTaskDelete(nullptr);
}

void BochainBypassClient::Run() {
    ESP_LOGI(TAG, "Task started, configured_url=%s, device_id=%s", configured_url_.c_str(), device_id_.c_str());

    while (!stop_requested_) {
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
            ESP_LOGW(TAG, "All candidate urls failed, retry in 5 seconds");
            for (int i = 0; i < 50 && !stop_requested_; ++i) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        ESP_LOGI(TAG, "Connected: %s", current_url_.c_str());
        SendHello();

        while (!stop_requested_ && websocket_ != nullptr && websocket_->IsConnected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
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
            ESP_LOGW(TAG, "Ignore binary message, len=%u", static_cast<unsigned>(len));
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
    cJSON_AddStringToObject(root, "version", "1.0.1-bind-code");
    cJSON_AddStringToObject(root, "board_uuid", Board::GetInstance().GetUuid().c_str());
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "bypass_url", current_url_.c_str());
    cJSON_AddBoolToObject(root, "support_bind_code", true);
    cJSON_AddBoolToObject(root, "support_display", true);
    cJSON_AddBoolToObject(root, "support_digit_voice", true);

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

    if (IsType(message_type, "bind_code")) {
        HandleBindCodeMessage(root);
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
    } else if (IsType(message_type, "bind_success")) {
        latest_bind_code_.clear();
        latest_bind_prompt_.clear();
        const char* success_keys[] = {"text", "message", "display_text"};
        std::string success_text = FirstJsonString(root, success_keys, sizeof(success_keys) / sizeof(success_keys[0]));
        if (success_text.empty()) {
            success_text = "设备绑定成功，已经归属到当前账号。";
        }
        HandleSpeakText(success_text);
    } else if (IsType(message_type, "audio")) {
        cJSON* url = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(url)) {
            ESP_LOGI(TAG, "audio url received, reserved for next version: %s", url->valuestring);
        }
    } else if (IsType(message_type, "ping")) {
        SendPong();
    } else if (IsType(message_type, "pong")) {
        ESP_LOGD(TAG, "pong");
    } else {
        ESP_LOGW(TAG, "Unhandled message type: %s", message_type);
    }

    cJSON_Delete(root);
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

    if (code.empty()) {
        ESP_LOGW(TAG, "bind_code message missing code");
        return;
    }

    latest_bind_code_ = code;

    std::string display_text = "铂链绑定码：" + code;
    const char* display_from_json = GetJsonString(root, "display_text");
    if (display_from_json && strlen(display_from_json) > 0) {
        display_text = display_from_json;

        // 防止屏幕上只显示“绑定码”，和小智官方激活码混淆
        if (display_text.find("铂链") == std::string::npos) {
            display_text = "铂链" + display_text;
        }
    }

    ESP_LOGI(TAG, "BoChain bind code cached: %s", code.c_str());

    // 关键优化：
    // 1. 不立即播报，避免和小智官方激活码混在一起
    // 2. 不立即抢屏，避免覆盖小智官方激活码
    // 3. 延迟显示铂链绑定码，只显示，不自动读
    xTaskCreate(
        [](void* arg) {
            std::string* text = static_cast<std::string*>(arg);

            // 给小智官方激活码留出播报/显示时间
            vTaskDelay(pdMS_TO_TICKS(45000));

            Application::GetInstance().Schedule([message = *text]() {
                auto display = Board::GetInstance().GetDisplay();
                if (display) {
                    display->SetChatMessage("system", message.c_str());
                    display->ShowNotification(message.c_str(), 10000);
                }
            });

            delete text;
            vTaskDelete(nullptr);
        },
        "bochain_bind_delay",
        4096,
        new std::string(display_text),
        3,
        nullptr
    );

    if (speak_bind_code_) {
        ESP_LOGI(TAG, "speak_bind_code is enabled, but auto speak is suppressed during boot bind flow");
    }
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

        // 不走小智官方 TTS，不影响官方对话链路。这里复用固件内置数字音频，保证无屏幕版本也能听到绑定码。
        for (char digit : code) {
            PlayDigitSound(digit);
        }
    });
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
