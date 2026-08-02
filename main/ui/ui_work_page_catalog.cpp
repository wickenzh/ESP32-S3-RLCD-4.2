// 管理工作页名称、启用状态、设置映射和用户自定义顺序。
#include "ui_work_page_catalog.h"

#include "app_constexpr.h"
#include "app_state.h"
#include "freertos/FreeRTOS.h"

#include <atomic>

namespace {
constexpr int kFirstWorkPage = kWorkPageWeatherClock;
constexpr int kFallbackWorkPage = kWorkPageWeatherClock;
constexpr int kInvalidWorkPage = -1;
constexpr int kInvalidWorkPageOrderIndex = -1;
constexpr uint8_t kDefaultWorkPageOrder[kWorkPageCount] = {
    kWorkPageWeatherClock,
    kWorkPageGallery,
    kWorkPageWeatherBoard,
    kWorkPageRadio,
    kWorkPageCalendar,
    kWorkPageHistory,
    kWorkPageXiaozhiAI,
};
portMUX_TYPE s_work_page_order_mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t s_work_page_order[kWorkPageCount] = {
    kWorkPageWeatherClock,
    kWorkPageGallery,
    kWorkPageWeatherBoard,
    kWorkPageRadio,
    kWorkPageCalendar,
    kWorkPageHistory,
    kWorkPageXiaozhiAI,
};
constexpr int kDisplaySettingPages[kDisplaySettingsPageItemCount] = {
    kWorkPageWeatherClock,
    kWorkPageGallery,
    kWorkPageWeatherBoard,
    kWorkPageRadio,
    kWorkPageCalendar,
    kWorkPageHistory,
    kWorkPageXiaozhiAI,
};
constexpr const char *kWorkPageNames[kWorkPageCount] = {
    "天气时钟",
    "图片时钟",
    "天气看板",
    "网络电台",
    "日历",
    "温湿历史",
    "小智AI",
};
constexpr const char *kUnknownWorkPageName = "未知页面";

bool is_work_page_index(int page)
{
    return page >= kFirstWorkPage && page < kWorkPageCount;
}

bool is_work_page_order_index(int index)
{
    return index >= 0 && index < kWorkPageCount;
}

bool work_page_order_index_found(int index)
{
    return index != kInvalidWorkPageOrderIndex;
}

constexpr uint8_t work_page_mask(int page)
{
    return static_cast<uint8_t>(1U << page);
}

constexpr uint8_t kAllWorkPageMask = static_cast<uint8_t>((1U << kWorkPageCount) - 1U);
std::atomic<uint8_t> s_work_page_enabled_mask{kAllWorkPageMask};
constexpr uint8_t kNetworkWorkPageMask = work_page_mask(kWorkPageWeatherClock) |
                                         work_page_mask(kWorkPageGallery) |
                                         work_page_mask(kWorkPageWeatherBoard) |
                                         work_page_mask(kWorkPageXiaozhiAI);
constexpr uint8_t kLowRefreshIdleWorkPageMask = work_page_mask(kWorkPageGallery) |
                                                work_page_mask(kWorkPageWeatherBoard) |
                                                work_page_mask(kWorkPageCalendar) |
                                                work_page_mask(kWorkPageHistory);
constexpr uint8_t kLocalWorkPageMask = static_cast<uint8_t>(~kNetworkWorkPageMask) & kAllWorkPageMask;

bool page_mask_has_non_xiaozhi_page(uint8_t page_mask)
{
    for (int page = kFirstWorkPage; page < kWorkPageCount; ++page) {
        if (page != kWorkPageXiaozhiAI && (page_mask & work_page_mask(page)) != 0) {
            return true;
        }
    }
    return false;
}

template <typename T, size_t N>
constexpr bool page_list_covers_each_work_page_once(const T (&pages)[N])
{
    if (N != kWorkPageCount) {
        return false;
    }
    for (int page = kFirstWorkPage; page < kWorkPageCount; ++page) {
        int hits = 0;
        for (size_t i = 0; i < N; ++i) {
            if (pages[i] == page) {
                ++hits;
            }
        }
        if (hits != 1) {
            return false;
        }
    }
    return true;
}

bool work_page_order_valid(const uint8_t *order, size_t order_size)
{
    if (!order || order_size != kWorkPageCount) {
        return false;
    }
    bool seen[kWorkPageCount] = {};
    for (int i = 0; i < kWorkPageCount; ++i) {
        uint8_t page = order[i];
        if (page >= kWorkPageCount || seen[page]) {
            return false;
        }
        seen[page] = true;
    }
    for (bool present : seen) {
        if (!present) {
            return false;
        }
    }
    return true;
}

bool page_enabled_in_mask(int page, uint8_t page_mask)
{
    return is_work_page_index(page) && (page_mask & work_page_mask(page)) != 0;
}

int work_page_order_index(const uint8_t *order, int page)
{
    for (int i = 0; i < kWorkPageCount; ++i) {
        if (order[i] == page) {
            return i;
        }
    }
    return kInvalidWorkPageOrderIndex;
}

int first_enabled_work_page_order_index_or_invalid(const uint8_t *order, uint8_t page_mask)
{
    for (int i = 0; i < kWorkPageCount; ++i) {
        if (page_enabled_in_mask(order[i], page_mask)) {
            return i;
        }
    }
    return kInvalidWorkPageOrderIndex;
}

int next_enabled_work_page_order_index_or_invalid(const uint8_t *order,
                                                  uint8_t page_mask,
                                                  int current_order_index)
{
    for (int step = 1; step <= kWorkPageCount; ++step) {
        int index = (current_order_index + step + kWorkPageCount) % kWorkPageCount;
        if (page_enabled_in_mask(order[index], page_mask)) {
            return index;
        }
    }
    return kInvalidWorkPageOrderIndex;
}

int first_enabled_work_page_order_index_or_zero(const uint8_t *order, uint8_t page_mask)
{
    int index = first_enabled_work_page_order_index_or_invalid(order, page_mask);
    return work_page_order_index_found(index) ? index : 0;
}

int valid_enabled_work_page_order_index(const uint8_t *order,
                                        uint8_t page_mask,
                                        int current_order_index)
{
    if (!is_work_page_order_index(current_order_index) ||
        !page_enabled_in_mask(order[current_order_index], page_mask)) {
        return first_enabled_work_page_order_index_or_zero(order, page_mask);
    }
    return current_order_index;
}

void normalize_work_page_order_values(uint8_t *order, uint8_t page_mask)
{
    if (!work_page_order_valid(order, kWorkPageCount)) {
        memcpy(order, kDefaultWorkPageOrder, sizeof(kDefaultWorkPageOrder));
    }
    int first_enabled = first_enabled_work_page_order_index_or_invalid(order, page_mask);
    if (!work_page_order_index_found(first_enabled) ||
        order[first_enabled] != kWorkPageXiaozhiAI) {
        return;
    }
    for (int i = first_enabled + 1; i < kWorkPageCount; ++i) {
        if (page_enabled_in_mask(order[i], page_mask) && order[i] != kWorkPageXiaozhiAI) {
            uint8_t replacement = order[i];
            order[i] = order[first_enabled];
            order[first_enabled] = replacement;
            return;
        }
    }
}

static_assert(kFirstWorkPage == 0, "work page ids must start at zero");
static_assert(kFallbackWorkPage == kWorkPageWeatherClock, "work page order fallback must remain weather clock");
static_assert(kWorkPageCount > 0, "there must be at least one work page");
static_assert(kWorkPageCount <= static_cast<int>(sizeof(uint8_t) * 8),
              "work page enabled mask is stored as uint8_t");
static_assert(kLocalWorkPageMask != 0, "offline mode requires at least one local work page");
static_assert((kLocalWorkPageMask & kNetworkWorkPageMask) == 0,
              "local and network work page masks must not overlap");
static_assert(kLowRefreshIdleWorkPageMask != 0,
              "low-refresh idle page mask must not be empty");
static_assert((kLowRefreshIdleWorkPageMask & ~kAllWorkPageMask) == 0,
              "low-refresh idle page mask must only contain work pages");
static_assert(array_count(kDefaultWorkPageOrder) == kWorkPageCount,
              "default work page order must cover every work page");
static_assert(array_count(kDisplaySettingPages) == kDisplaySettingsPageItemCount,
              "display setting page mapping must match the settings item count");
static_assert(array_count(kWorkPageNames) == kWorkPageCount,
              "work page names must cover every work page");
static_assert(cstr_array_nonempty(kWorkPageNames), "work page names must be non-empty");
static_assert(cstr_nonempty(kUnknownWorkPageName), "unknown work page name must be non-empty");
static_assert(page_list_covers_each_work_page_once(kDefaultWorkPageOrder),
              "default work page order must include every work page exactly once");
static_assert(page_list_covers_each_work_page_once(kDisplaySettingPages),
              "display settings must map every work page exactly once");
}

bool is_work_page_enabled(int page)
{
    if (!is_work_page_index(page)) {
        return false;
    }
    return (work_page_enabled_mask_load() & work_page_mask(page)) != 0;
}

uint8_t work_page_enabled_mask_load()
{
    return s_work_page_enabled_mask.load(std::memory_order_acquire);
}

void work_page_enabled_mask_store(uint8_t page_mask)
{
    s_work_page_enabled_mask.store(page_mask, std::memory_order_release);
}

bool work_page_requires_network(int page)
{
    return is_work_page_index(page) && (kNetworkWorkPageMask & work_page_mask(page)) != 0;
}

bool work_page_uses_low_refresh_idle(int page)
{
    return is_work_page_index(page) &&
           (kLowRefreshIdleWorkPageMask & work_page_mask(page)) != 0;
}

uint8_t normalize_work_page_enabled_mask(uint8_t page_mask)
{
    page_mask &= kAllWorkPageMask;
    if (page_mask == 0) {
        return kAllWorkPageMask;
    }
    if (!page_mask_has_non_xiaozhi_page(page_mask)) {
        page_mask |= work_page_mask(kWorkPageWeatherClock);
    }
    return page_mask;
}

uint8_t work_page_mask_for_offline_mode(uint8_t page_mask)
{
    uint8_t local_mask = static_cast<uint8_t>(page_mask & kLocalWorkPageMask);
    if (local_mask != 0) {
        return local_mask;
    }
    uint8_t order[kWorkPageCount] = {};
    if (work_page_order_copy(order, sizeof(order)) &&
        work_page_order_valid(order, sizeof(order))) {
        for (uint8_t page : order) {
            if (!work_page_requires_network(page)) {
                return work_page_mask(page);
            }
        }
    }
    return work_page_mask(kWorkPageRadio);
}

const char *work_page_name(int page)
{
    if (!is_work_page_index(page)) {
        return kUnknownWorkPageName;
    }
    return kWorkPageNames[page];
}

int display_settings_item_work_page(int item)
{
    if (item < 0 || item >= kDisplaySettingsPageItemCount) {
        return kInvalidWorkPage;
    }
    return kDisplaySettingPages[item];
}

int first_enabled_work_page()
{
    normalize_work_page_order();
    uint8_t order[kWorkPageCount] = {};
    if (!work_page_order_copy(order, sizeof(order))) {
        return kFallbackWorkPage;
    }
    int index = first_enabled_work_page_order_index_or_invalid(
        order, work_page_enabled_mask_load());
    return work_page_order_index_found(index) ? order[index] : kFallbackWorkPage;
}

void reset_work_page_order()
{
    static_assert(sizeof(kDefaultWorkPageOrder) == sizeof(s_work_page_order),
                  "default work page order storage must match runtime order storage");
    portENTER_CRITICAL(&s_work_page_order_mux);
    memcpy(s_work_page_order, kDefaultWorkPageOrder, sizeof(s_work_page_order));
    portEXIT_CRITICAL(&s_work_page_order_mux);
}

void normalize_work_page_order()
{
    const uint8_t page_mask = work_page_enabled_mask_load();
    portENTER_CRITICAL(&s_work_page_order_mux);
    normalize_work_page_order_values(s_work_page_order, page_mask);
    portEXIT_CRITICAL(&s_work_page_order_mux);
}

bool work_page_mask_has_valid_home(uint8_t page_mask)
{
    return page_mask_has_non_xiaozhi_page(page_mask);
}

bool work_page_order_has_valid_home()
{
    uint8_t order[kWorkPageCount] = {};
    if (!work_page_order_copy(order, sizeof(order))) {
        return false;
    }
    int first_enabled = first_enabled_work_page_order_index_or_invalid(
        order, work_page_enabled_mask_load());
    return work_page_order_index_found(first_enabled) &&
           order[first_enabled] != kWorkPageXiaozhiAI;
}

bool work_page_order_copy(uint8_t *order, size_t order_size)
{
    if (!order || order_size != sizeof(s_work_page_order)) {
        return false;
    }
    portENTER_CRITICAL(&s_work_page_order_mux);
    memcpy(order, s_work_page_order, sizeof(s_work_page_order));
    portEXIT_CRITICAL(&s_work_page_order_mux);
    return true;
}

void work_page_order_replace(const uint8_t *order, size_t order_size)
{
    uint8_t replacement[kWorkPageCount] = {};
    if (work_page_order_valid(order, order_size)) {
        memcpy(replacement, order, sizeof(replacement));
    } else {
        memcpy(replacement, kDefaultWorkPageOrder, sizeof(replacement));
    }
    normalize_work_page_order_values(replacement, work_page_enabled_mask_load());
    portENTER_CRITICAL(&s_work_page_order_mux);
    memcpy(s_work_page_order, replacement, sizeof(s_work_page_order));
    portEXIT_CRITICAL(&s_work_page_order_mux);
}

bool swap_work_page_order_entries_preserving_home(int first_index, int second_index)
{
    if (!is_work_page_order_index(first_index) ||
        !is_work_page_order_index(second_index)) {
        return false;
    }
    const uint8_t page_mask = work_page_enabled_mask_load();
    bool accepted = false;
    portENTER_CRITICAL(&s_work_page_order_mux);
    uint8_t candidate[kWorkPageCount] = {};
    memcpy(candidate, s_work_page_order, sizeof(candidate));
    uint8_t page = candidate[first_index];
    candidate[first_index] = candidate[second_index];
    candidate[second_index] = page;
    int first_enabled = first_enabled_work_page_order_index_or_invalid(candidate, page_mask);
    if (work_page_order_index_found(first_enabled) &&
        candidate[first_enabled] != kWorkPageXiaozhiAI) {
        memcpy(s_work_page_order, candidate, sizeof(s_work_page_order));
        accepted = true;
    }
    portEXIT_CRITICAL(&s_work_page_order_mux);
    return accepted;
}

int next_enabled_work_page(int current_page)
{
    normalize_work_page_order();
    uint8_t order[kWorkPageCount] = {};
    if (!work_page_order_copy(order, sizeof(order))) {
        return kFallbackWorkPage;
    }
    const uint8_t page_mask = work_page_enabled_mask_load();
    if (!is_work_page_index(current_page)) {
        int first_index = first_enabled_work_page_order_index_or_invalid(order, page_mask);
        current_page = work_page_order_index_found(first_index)
                           ? order[first_index]
                           : kFallbackWorkPage;
    }
    int current_index = work_page_order_index(order, current_page);
    int next_index = next_enabled_work_page_order_index_or_invalid(
        order, page_mask, current_index);
    return work_page_order_index_found(next_index) ? order[next_index] : kFallbackWorkPage;
}

int first_enabled_work_page_order_index()
{
    normalize_work_page_order();
    uint8_t order[kWorkPageCount] = {};
    if (!work_page_order_copy(order, sizeof(order))) {
        return 0;
    }
    return first_enabled_work_page_order_index_or_zero(order, work_page_enabled_mask_load());
}

int next_enabled_work_page_order_index(int current_order_index)
{
    normalize_work_page_order();
    uint8_t order[kWorkPageCount] = {};
    if (!work_page_order_copy(order, sizeof(order))) {
        return 0;
    }
    const uint8_t page_mask = work_page_enabled_mask_load();
    current_order_index = valid_enabled_work_page_order_index(
        order, page_mask, current_order_index);
    int next_index = next_enabled_work_page_order_index_or_invalid(
        order, page_mask, current_order_index);
    return work_page_order_index_found(next_index)
               ? next_index
               : first_enabled_work_page_order_index_or_zero(order, page_mask);
}

int valid_enabled_work_page_order_index(int current_order_index)
{
    normalize_work_page_order();
    uint8_t order[kWorkPageCount] = {};
    if (!work_page_order_copy(order, sizeof(order))) {
        return 0;
    }
    return valid_enabled_work_page_order_index(
        order, work_page_enabled_mask_load(), current_order_index);
}

void ensure_active_work_page_enabled()
{
    if (!is_work_page_enabled(active_work_page_load())) {
        active_work_page_store(first_enabled_work_page());
    }
}
