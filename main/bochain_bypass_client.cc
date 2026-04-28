#include "bochain_bypass_client.h"

#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <cstring>
#include <esp_log.h>

#define TAG "BochainBypass"

// 小智官方聊天走 OTA 下发的官方 websocket；这里是铂链直播旁路通道。
#define DEFAULT_BOCHAIN_BYPASS_WS_URL "ws://robot.blsx.com:8011/danmaku/"
#define DEFAULT_BOCHAIN_DEVICE_ID_PREFIX "BL-ESP32-"
#define DEFAULT_BOCHAIN_TOKEN "mr.fu875188"

BochainBypassClient& BochainBypassClient::GetInstance() {
    static BochainBypassClient instance;
    return instance;
}

void BochainBypassClient::LoadSettings() {
    Settings settings("bochain", false);
    configured_url_ = settings.GetString("url", DEFAULT_BOCHAIN_BYPASS_WS_URL);
    token_ = settings.GetString("token", DEFAULT_BOCHAIN_TOKEN);
    device_id_ = settings.GetString("device_id", "");
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
    cJSON_AddStringToObject(root, "version", "1.0.0");
    cJSON_AddStringToObject(root, "board_uuid", Board::GetInstance().GetUuid().c_str());
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "bypass_url", current_url_.c_str());

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

    if (strcmp(type->valuestring, "speak") == 0) {
        cJSON* text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            HandleSpeakText(text->valuestring);
        } else {
            ESP_LOGW(TAG, "speak message missing text");
        }
    } else if (strcmp(type->valuestring, "audio") == 0) {
        cJSON* url = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(url)) {
            ESP_LOGI(TAG, "audio url received, reserved for next version: %s", url->valuestring);
        }
    } else if (strcmp(type->valuestring, "ping") == 0) {
        if (websocket_ && websocket_->IsConnected()) {
            websocket_->Send("{\"type\":\"pong\"}");
        }
    } else if (strcmp(type->valuestring, "pong") == 0) {
        ESP_LOGD(TAG, "pong");
    } else {
        ESP_LOGW(TAG, "Unhandled message type: %s", type->valuestring);
    }

    cJSON_Delete(root);
}

void BochainBypassClient::HandleSpeakText(const std::string& text) {
    // 第一版先只打印，确认真机旁路链路跑通后，再接入音频播放/TTS逻辑。
    ESP_LOGI(TAG, "speak text: %s", text.c_str());
}
