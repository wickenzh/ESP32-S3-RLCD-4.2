// 验证恢复出厂配置键全集、擦除顺序、条件提交和错误短路。
#include "network_factory_reset.h"

#include <assert.h>
#include <map>
#include <string>
#include <vector>

namespace {
const char *const kExpectedKeys[] = {
    "ssid",
    "pass",
    "ssid_b",
    "pass_b",
    "wifi_pri_v1",
    "api_key",
    "weather_city_v1",
    "asset_city_skip",
    "api_host",
    "weather_src_v1",
    "offline_v1",
    "hourly_chime_v2",
    "hour_all_v1",
    "chime_vol_v1",
    "chime_snd_v1",
    "page_mask_v1",
    "page_mask_v2",
    "page_mask_v3",
    "page_mask_v4",
    "page_mask_v5", "page_mask_v6",
    "page_order_v1",
    "page_order_v2",
    "page_order_v3",
    "page_order_v4",
    "page_order_v5", "page_order_v6",
    "xz_auto_ret_v1",
    "gallery_rot_v1",
};

std::map<std::string, std::string> g_values;
std::vector<std::string> g_erased_keys;
std::string g_fail_key;
int g_commit_calls = 0;

void reset_store(bool populate)
{
    g_values.clear();
    g_erased_keys.clear();
    g_fail_key.clear();
    g_commit_calls = 0;
    if (populate) {
        for (const char *key : kExpectedKeys) {
            g_values[key] = "saved";
        }
    }
}

void assert_erase_prefix(size_t count)
{
    assert(g_erased_keys.size() == count);
    for (size_t i = 0; i < count; ++i) {
        assert(g_erased_keys[i] == kExpectedKeys[i]);
    }
}
} // namespace

const char *esp_err_to_name(esp_err_t)
{
    return "host";
}

esp_err_t nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *)
{
    return ESP_FAIL;
}

void nvs_close(nvs_handle_t) {}

esp_err_t nvs_commit(nvs_handle_t)
{
    ++g_commit_calls;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t, const char *key)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    g_erased_keys.emplace_back(key);
    if (g_fail_key == key) {
        return ESP_FAIL;
    }
    return g_values.erase(key) > 0 ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *)
{
    return ESP_FAIL;
}

esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *)
{
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t)
{
    return ESP_FAIL;
}

esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *)
{
    return ESP_ERR_NVS_NOT_FOUND;
}

int main()
{
    constexpr nvs_handle_t kNvs = 1;
    constexpr size_t kExpectedKeyCount = sizeof(kExpectedKeys) / sizeof(kExpectedKeys[0]);
    static_assert(kExpectedKeyCount == 29,
                  "factory reset host test must enumerate every registered key");

    reset_store(true);
    assert(network_factory_reset::erase_saved_config_keys(kNvs) == ESP_OK);
    assert_erase_prefix(kExpectedKeyCount);
    assert(g_values.empty());
    assert(g_commit_calls == 1);

    reset_store(false);
    assert(network_factory_reset::erase_saved_config_keys(kNvs) == ESP_OK);
    assert_erase_prefix(kExpectedKeyCount);
    assert(g_commit_calls == 0);

    reset_store(true);
    g_fail_key = kExpectedKeys[3];
    assert(network_factory_reset::erase_saved_config_keys(kNvs) == ESP_FAIL);
    assert_erase_prefix(4);
    assert(g_commit_calls == 0);
    assert(g_values.count(kExpectedKeys[3]) == 1);
    assert(g_values.count(kExpectedKeys[4]) == 1);
    return 0;
}
