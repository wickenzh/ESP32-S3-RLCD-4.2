// 实现小智 WebSocket 上的轻量 MCP JSON-RPC 工具服务。
#include "xiaozhi_mcp.h"
#include "xiaozhi_mcp_arguments.h"
#include "xiaozhi_mcp_json.h"
#include "xiaozhi_mcp_request_parser.h"
#include "xiaozhi_mcp_schema.h"
#include "xiaozhi_json_owner.h"

#ifdef XIAOZHI_MCP_HOST_TEST
#include "xiaozhi_mcp_host_port.h"
#else
#include "app_state.h"
#include "audio_services.h"
#include "network_services.h"
#include "sensor_services.h"
#endif

#include <atomic>
#include <cstdio>
#include <string.h>

namespace {
constexpr const char *kMcpType = "mcp";
constexpr const char *kMcpJsonRpcVersion = "2.0";
constexpr const char *kMcpInitializeMethod = "initialize";
constexpr const char *kMcpToolsListMethod = "tools/list";
constexpr const char *kMcpToolsCallMethod = "tools/call";
constexpr const char *kMcpNotificationPrefix = "notifications";
constexpr const char *kJsonFieldType = "type";
constexpr const char *kJsonFieldPayload = "payload";
constexpr int kJsonRpcInvalidRequest = -32600;
constexpr int kJsonRpcMethodNotFound = -32601;
constexpr int kJsonRpcInvalidParams = -32602;
constexpr int kJsonRpcInternalError = -32603;
constexpr size_t kToolResultTextLen = 256;

using xiaozhi_mcp_schema::kDeviceStatusTool;
using xiaozhi_mcp_schema::kDisableAlarmTool;
using xiaozhi_mcp_schema::kPomodoroControlTool;
using xiaozhi_mcp_schema::kSetAlarmTool;
using xiaozhi_mcp_schema::kSetCountdownTool;
using xiaozhi_mcp_schema::kSetVolumeTool;
using xiaozhi_mcp_schema::kSetWeatherCityTool;
using xiaozhi_mcp_json::add_owned_item_to_array;
using xiaozhi_mcp_json::add_owned_item_to_object;
using xiaozhi_mcp_json::add_string;

std::atomic<bool> s_volume_save_pending{false};
XiaozhiMcpAlarmHandler s_alarm_handler = nullptr;
XiaozhiMcpAlarmDisableHandler s_alarm_disable_handler = nullptr;
XiaozhiMcpCountdownHandler s_countdown_handler = nullptr;
XiaozhiMcpPomodoroHandler s_pomodoro_handler = nullptr;
XiaozhiMcpWeatherCityHandler s_weather_city_handler = nullptr;

cJSON *create_tool_content(const char *text, bool is_error)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    if (!result || !content || !item ||
        !add_string(item, "type", "text") ||
        !add_string(item, "text", text ? text : "")) {
        cJSON_Delete(result);
        cJSON_Delete(content);
        cJSON_Delete(item);
        return nullptr;
    }
    if (!add_owned_item_to_array(content, item)) {
        cJSON_Delete(result);
        cJSON_Delete(content);
        return nullptr;
    }
    if (!add_owned_item_to_object(result, "content", content) ||
        !cJSON_AddBoolToObject(result, "isError", is_error)) {
        cJSON_Delete(result);
        return nullptr;
    }
    return result;
}

cJSON *create_device_status_result()
{
    float temperature = 0.0f;
    float humidity = 0.0f;
    bool sensor_available = get_local_sensor_snapshot(&temperature, &humidity, nullptr, nullptr);
    XiaozhiJsonOwner status{cJSON_CreateObject()};
    if (!status) {
        return nullptr;
    }
    cJSON_AddBoolToObject(status.get(), "sensor_available", sensor_available);
    if (sensor_available) {
        cJSON_AddNumberToObject(status.get(), "temperature_c", temperature);
        cJSON_AddNumberToObject(status.get(), "humidity_percent", humidity);
    } else {
        cJSON_AddNullToObject(status.get(), "temperature_c");
        cJSON_AddNullToObject(status.get(), "humidity_percent");
    }
    BatteryRuntimeSnapshot battery;
    battery_runtime_snapshot_load(&battery);
    bool battery_available = battery.percent >= 0 && battery.percent <= 100;
    cJSON_AddBoolToObject(status.get(), "battery_available", battery_available);
    if (battery_available) {
        cJSON_AddNumberToObject(status.get(), "battery_percent", battery.percent);
    } else {
        cJSON_AddNullToObject(status.get(), "battery_percent");
    }
    cJSON_AddNumberToObject(status.get(),
                            "volume_percent",
                            static_cast<int>(g_chime_volume_percent));
    char *status_text = cJSON_PrintUnformatted(status.get());
    if (!status_text) {
        return nullptr;
    }
    cJSON *result = create_tool_content(status_text, false);
    cJSON_free(status_text);
    return result;
}

cJSON *call_set_volume(const cJSON *arguments)
{
    int volume = 0;
    if (!xiaozhi_mcp_arguments::parse_volume(arguments, &volume)) {
        return nullptr;
    }
    if (g_chime_volume_percent != volume) {
        g_chime_volume_percent = volume;
        s_volume_save_pending.store(true);
        apply_xiaozhi_speaker_volume(volume);
    }
    char result[kToolResultTextLen] = {};
    snprintf(result,
             sizeof(result),
             "{\"volume_percent\":%d}",
             static_cast<int>(g_chime_volume_percent));
    return create_tool_content(result, false);
}

cJSON *call_alarm(const cJSON *arguments)
{
    if (!s_alarm_handler) {
        return nullptr;
    }
    XiaozhiMcpAlarmRequest request = {};
    if (!xiaozhi_mcp_arguments::parse_alarm(arguments, &request)) {
        return nullptr;
    }
    char result[kToolResultTextLen] = {};
    return s_alarm_handler(request, result, sizeof(result))
               ? create_tool_content(result, false)
               : create_tool_content(result[0] ? result : "alarm rejected", true);
}

cJSON *call_disable_alarm()
{
    if (!s_alarm_disable_handler) {
        return nullptr;
    }
    char result[kToolResultTextLen] = {};
    return s_alarm_disable_handler(result, sizeof(result))
               ? create_tool_content(result, false)
               : create_tool_content(result[0] ? result : "alarm disable rejected", true);
}

cJSON *call_countdown(const cJSON *arguments)
{
    if (!s_countdown_handler) {
        return nullptr;
    }
    XiaozhiMcpCountdownRequest request = {};
    if (!xiaozhi_mcp_arguments::parse_countdown(arguments, &request)) {
        return nullptr;
    }
    char result[kToolResultTextLen] = {};
    return s_countdown_handler(request, result, sizeof(result))
               ? create_tool_content(result, false)
               : create_tool_content(result[0] ? result : "countdown rejected", true);
}

cJSON *call_pomodoro(const cJSON *arguments)
{
    if (!s_pomodoro_handler) {
        return nullptr;
    }
    XiaozhiMcpPomodoroRequest request = {};
    if (!xiaozhi_mcp_arguments::parse_pomodoro(arguments, &request)) {
        return nullptr;
    }
    char result[kToolResultTextLen] = {};
    return s_pomodoro_handler(request, result, sizeof(result))
               ? create_tool_content(result, false)
               : create_tool_content(result[0] ? result : "pomodoro request rejected", true);
}

cJSON *call_weather_city(const cJSON *arguments)
{
    if (!s_weather_city_handler) {
        return nullptr;
    }
    XiaozhiMcpWeatherCityRequest request = {};
    if (!xiaozhi_mcp_arguments::parse_weather_city(arguments, &request)) {
        return nullptr;
    }
    char result[kToolResultTextLen] = {};
    return s_weather_city_handler(request, result, sizeof(result))
               ? create_tool_content(result, false)
               : create_tool_content(result[0] ? result : "weather city request rejected", true);
}

cJSON *create_tool_call_result(const cJSON *params,
                               bool allow_alarm_disable,
                               int *error_code,
                               const char **error_message)
{
    const cJSON *name = cJSON_IsObject(params) ? cJSON_GetObjectItem(params, "name") : nullptr;
    const cJSON *arguments = cJSON_IsObject(params) ? cJSON_GetObjectItem(params, "arguments") : nullptr;
    if (!cJSON_IsString(name) || (arguments && !cJSON_IsObject(arguments))) {
        *error_code = kJsonRpcInvalidParams;
        *error_message = "Invalid tool parameters";
        return nullptr;
    }
    if (strcmp(name->valuestring, kDeviceStatusTool) == 0) {
        return create_device_status_result();
    }
    if (strcmp(name->valuestring, kSetVolumeTool) == 0) {
        cJSON *result = call_set_volume(arguments);
        if (!result) {
            *error_code = kJsonRpcInvalidParams;
            *error_message = "volume must be an integer from 0 to 100";
        }
        return result;
    }
    if (strcmp(name->valuestring, kSetAlarmTool) == 0 && s_alarm_handler) {
        cJSON *result = call_alarm(arguments);
        if (!result) {
            *error_code = kJsonRpcInvalidParams;
            *error_message = "Invalid alarm parameters";
        }
        return result;
    }
    if (strcmp(name->valuestring, kDisableAlarmTool) == 0 && s_alarm_disable_handler) {
        if (!allow_alarm_disable) {
            return create_tool_content("alarm unchanged while exiting Xiaozhi", false);
        }
        return call_disable_alarm();
    }
    if (strcmp(name->valuestring, kSetCountdownTool) == 0 && s_countdown_handler) {
        cJSON *result = call_countdown(arguments);
        if (!result) {
            *error_code = kJsonRpcInvalidParams;
            *error_message = "Invalid countdown parameters";
        }
        return result;
    }
    if (strcmp(name->valuestring, kPomodoroControlTool) == 0 && s_pomodoro_handler) {
        cJSON *result = call_pomodoro(arguments);
        if (!result) {
            *error_code = kJsonRpcInvalidParams;
            *error_message = "Invalid pomodoro parameters";
        }
        return result;
    }
    if (strcmp(name->valuestring, kSetWeatherCityTool) == 0 && s_weather_city_handler) {
        cJSON *result = call_weather_city(arguments);
        if (!result) {
            *error_code = kJsonRpcInvalidParams;
            *error_message = "city must be a non-empty string";
        }
        return result;
    }
    *error_code = kJsonRpcMethodNotFound;
    *error_message = "Unknown tool";
    return nullptr;
}

bool write_response(const char *session_id,
                    const cJSON *request_id,
                    cJSON *result,
                    int error_code,
                    const char *error_message,
                    char *response,
                    size_t response_len)
{
    XiaozhiJsonOwner root{cJSON_CreateObject()};
    cJSON *payload = cJSON_CreateObject();
    if (!root || !payload || !response || response_len == 0 ||
        !add_string(root.get(), "session_id", session_id ? session_id : "") ||
        !add_string(root.get(), kJsonFieldType, kMcpType) ||
        !add_string(payload, "jsonrpc", kMcpJsonRpcVersion)) {
        cJSON_Delete(payload);
        cJSON_Delete(result);
        return false;
    }
    cJSON *id_copy = cJSON_Duplicate(request_id, true);
    if (!id_copy) {
        cJSON_Delete(payload);
        cJSON_Delete(result);
        return false;
    }
    if (!add_owned_item_to_object(payload, "id", id_copy)) {
        cJSON_Delete(payload);
        cJSON_Delete(result);
        return false;
    }
    if (result) {
        if (!add_owned_item_to_object(payload, "result", result)) {
            cJSON_Delete(payload);
            return false;
        }
    } else {
        cJSON *error = cJSON_CreateObject();
        if (!error || !cJSON_AddNumberToObject(error, "code", error_code) ||
            !add_string(error, "message", error_message ? error_message : "MCP error")) {
            cJSON_Delete(error);
            cJSON_Delete(payload);
            return false;
        }
        if (!add_owned_item_to_object(payload, "error", error)) {
            cJSON_Delete(payload);
            return false;
        }
    }
    if (!add_owned_item_to_object(root.get(), kJsonFieldPayload, payload)) {
        return false;
    }
    response[0] = '\0';
    return cJSON_PrintPreallocated(root.get(),
                                   response,
                                   static_cast<int>(response_len),
                                   false);
}
} // namespace

bool xiaozhi_mcp_message_calls_weather_city(const char *message, size_t message_len)
{
    if (!message || message_len == 0 || !xiaozhi_mcp_json_token_present(message, message_len)) {
        return false;
    }
    XiaozhiMcpRequestDocument request;
    return request.parse(message, message_len) &&
           request.type() && strcmp(request.type(), kMcpType) == 0 &&
           request.method() && strcmp(request.method(), kMcpToolsCallMethod) == 0 &&
           request.tool_name() && strcmp(request.tool_name(), kSetWeatherCityTool) == 0;
}

XiaozhiMcpMessageResult xiaozhi_mcp_handle_message(const char *message,
                                                   size_t message_len,
                                                   const char *session_id,
                                                   char *response,
                                                   size_t response_len,
                                                   bool allow_alarm_disable)
{
    if (!message || message_len == 0) {
        return kXiaozhiMcpNotHandled;
    }
    // 普通 STT/TTS/LLM 文本占绝大多数，先做有界 token 筛选，避免每帧
    // 在 MCP 和原会话处理器中重复分配并解析两棵 cJSON 树。
    if (!xiaozhi_mcp_json_token_present(message, message_len)) {
        return kXiaozhiMcpNotHandled;
    }
    XiaozhiMcpRequestDocument request;
    if (!request.parse(message, message_len) || !request.type() ||
        strcmp(request.type(), kMcpType) != 0) {
        return kXiaozhiMcpNotHandled;
    }
    if (response && response_len > 0) {
        response[0] = '\0';
    }
    const char *version = request.version();
    const char *method = request.method();
    if (!version || strcmp(version, kMcpJsonRpcVersion) != 0 || !method) {
        return kXiaozhiMcpHandledWithoutResponse;
    }
    if (strncmp(method, kMcpNotificationPrefix, strlen(kMcpNotificationPrefix)) == 0) {
        return kXiaozhiMcpHandledWithoutResponse;
    }
    const cJSON *id = request.id();
    if (!cJSON_IsNumber(id) && !cJSON_IsString(id)) {
        return kXiaozhiMcpHandledWithoutResponse;
    }
    const cJSON *params = request.params();
    cJSON *result = nullptr;
    int error_code = kJsonRpcInvalidRequest;
    const char *error_message = "Invalid MCP request";
    if (strcmp(method, kMcpInitializeMethod) == 0) {
        result = xiaozhi_mcp_schema::create_initialize_result(APP_VERSION);
    } else if (strcmp(method, kMcpToolsListMethod) == 0) {
        result = xiaozhi_mcp_schema::create_tools_list_result(s_alarm_handler != nullptr,
                                                              s_alarm_disable_handler != nullptr,
                                                              s_countdown_handler != nullptr,
                                                              s_pomodoro_handler != nullptr,
                                                              s_weather_city_handler != nullptr);
    } else if (strcmp(method, kMcpToolsCallMethod) == 0) {
        result = create_tool_call_result(params,
                                         allow_alarm_disable,
                                         &error_code,
                                         &error_message);
    } else {
        error_code = kJsonRpcMethodNotFound;
        error_message = "Method not implemented";
    }
    if (!result && error_code == kJsonRpcInvalidRequest) {
        error_code = kJsonRpcInternalError;
        error_message = "MCP response allocation failed";
    }
    return write_response(session_id,
                          id,
                          result,
                          error_code,
                          error_message,
                          response,
                          response_len)
               ? kXiaozhiMcpHandledWithResponse
               : kXiaozhiMcpHandledWithoutResponse;
}

bool xiaozhi_mcp_volume_save_pending()
{
    return s_volume_save_pending.load();
}

bool xiaozhi_mcp_flush_pending_settings()
{
    if (!s_volume_save_pending.load()) {
        return true;
    }
    if (!save_hourly_chime_setting()) {
        return false;
    }
    s_volume_save_pending.store(false);
    return true;
}

void xiaozhi_mcp_register_alarm_handler(XiaozhiMcpAlarmHandler handler)
{
    s_alarm_handler = handler;
}

void xiaozhi_mcp_register_alarm_disable_handler(XiaozhiMcpAlarmDisableHandler handler)
{
    s_alarm_disable_handler = handler;
}

void xiaozhi_mcp_register_countdown_handler(XiaozhiMcpCountdownHandler handler)
{
    s_countdown_handler = handler;
}

void xiaozhi_mcp_register_pomodoro_handler(XiaozhiMcpPomodoroHandler handler)
{
    s_pomodoro_handler = handler;
}

void xiaozhi_mcp_register_weather_city_handler(XiaozhiMcpWeatherCityHandler handler)
{
    s_weather_city_handler = handler;
}
