// 验证工作页名称、启用掩码、离线筛选和自定义顺序规则。
#include "ui_work_page_catalog.h"

#include "app_state.h"

#include <assert.h>
#include <atomic>
#include <string.h>
#include <thread>

namespace {

constexpr uint8_t page_bit(int page)
{
    return static_cast<uint8_t>(1U << page);
}

void expect_default_order()
{
    const uint8_t expected[kWorkPageCount] = {
        kWorkPageWeatherClock,
        kWorkPageGallery,
        kWorkPageWeatherBoard,
        kWorkPageFlipClock,
        kWorkPageCalendar,
        kWorkPageHistory,
        kWorkPageXiaozhiAI,
    };
    uint8_t actual[kWorkPageCount] = {};
    assert(work_page_order_copy(actual, sizeof(actual)));
    assert(memcmp(actual, expected, sizeof(expected)) == 0);
}

} // namespace

int main()
{
    reset_work_page_order();
    expect_default_order();
    work_page_enabled_mask_store(static_cast<uint8_t>((1U << kWorkPageCount) - 1U));

    assert(strcmp(work_page_name(kWorkPageWeatherClock), "天气时钟") == 0);
    assert(strcmp(work_page_name(kWorkPageHistory), "温湿历史") == 0);
    assert(strcmp(work_page_name(kWorkPageXiaozhiAI), "小智AI") == 0);
    assert(strcmp(work_page_name(-1), "未知页面") == 0);
    assert(display_settings_item_work_page(0) == kWorkPageWeatherClock);
    assert(display_settings_item_work_page(kWorkPageCount - 1) == kWorkPageXiaozhiAI);
    assert(display_settings_item_work_page(-1) == -1);
    assert(display_settings_item_work_page(kWorkPageCount) == -1);

    assert(work_page_requires_network(kWorkPageWeatherClock));
    assert(work_page_requires_network(kWorkPageGallery));
    assert(work_page_requires_network(kWorkPageWeatherBoard));
    assert(work_page_requires_network(kWorkPageXiaozhiAI));
    assert(!work_page_requires_network(kWorkPageFlipClock));
    assert(!work_page_requires_network(kWorkPageCalendar));
    assert(!work_page_requires_network(kWorkPageHistory));
    assert(!work_page_requires_network(-1));

    assert(!work_page_uses_low_refresh_idle(kWorkPageWeatherClock));
    assert(work_page_uses_low_refresh_idle(kWorkPageGallery));
    assert(work_page_uses_low_refresh_idle(kWorkPageWeatherBoard));
    assert(!work_page_uses_low_refresh_idle(kWorkPageFlipClock));
    assert(work_page_uses_low_refresh_idle(kWorkPageCalendar));
    assert(work_page_uses_low_refresh_idle(kWorkPageHistory));
    assert(!work_page_uses_low_refresh_idle(kWorkPageXiaozhiAI));
    assert(!work_page_uses_low_refresh_idle(-1));

    const uint8_t all_pages = static_cast<uint8_t>((1U << kWorkPageCount) - 1U);
    assert(normalize_work_page_enabled_mask(all_pages) == all_pages);
    assert(normalize_work_page_enabled_mask(0) == all_pages);
    assert(normalize_work_page_enabled_mask(0x80) == all_pages);
    assert(normalize_work_page_enabled_mask(page_bit(kWorkPageXiaozhiAI)) ==
           (page_bit(kWorkPageWeatherClock) | page_bit(kWorkPageXiaozhiAI)));
    assert(normalize_work_page_enabled_mask(page_bit(kWorkPageCalendar) |
                                            page_bit(kWorkPageXiaozhiAI)) ==
           (page_bit(kWorkPageCalendar) | page_bit(kWorkPageXiaozhiAI)));

    const uint8_t local_pages = page_bit(kWorkPageFlipClock) |
                                page_bit(kWorkPageCalendar);
    assert(work_page_mask_for_offline_mode(work_page_enabled_mask_load()) ==
           (page_bit(kWorkPageFlipClock) |
            page_bit(kWorkPageCalendar) |
            page_bit(kWorkPageHistory)));
    assert(work_page_mask_for_offline_mode(local_pages |
                                           page_bit(kWorkPageWeatherClock)) ==
           local_pages);
    assert(work_page_mask_for_offline_mode(page_bit(kWorkPageWeatherClock)) ==
           page_bit(kWorkPageFlipClock));

    const uint8_t xiaozhi_first[kWorkPageCount] = {
        kWorkPageXiaozhiAI,
        kWorkPageHistory,
        kWorkPageCalendar,
        kWorkPageFlipClock,
        kWorkPageWeatherBoard,
        kWorkPageGallery,
        kWorkPageWeatherClock,
    };
    work_page_order_replace(xiaozhi_first, sizeof(xiaozhi_first));
    normalize_work_page_order();
    uint8_t normalized[kWorkPageCount] = {};
    assert(work_page_order_copy(normalized, sizeof(normalized)));
    assert(normalized[0] == kWorkPageHistory);
    assert(normalized[1] == kWorkPageXiaozhiAI);
    assert(first_enabled_work_page() == kWorkPageHistory);
    assert(work_page_order_has_valid_home());

    work_page_enabled_mask_store(page_bit(kWorkPageXiaozhiAI));
    normalize_work_page_order();
    assert(!work_page_mask_has_valid_home(work_page_enabled_mask_load()));
    assert(!work_page_order_has_valid_home());

    work_page_enabled_mask_store(page_bit(kWorkPageWeatherClock) |
                                 page_bit(kWorkPageCalendar) |
                                 page_bit(kWorkPageHistory));
    reset_work_page_order();
    assert(first_enabled_work_page() == kWorkPageWeatherClock);
    assert(next_enabled_work_page(kWorkPageWeatherClock) == kWorkPageCalendar);
    assert(next_enabled_work_page(kWorkPageCalendar) == kWorkPageHistory);
    assert(next_enabled_work_page(kWorkPageHistory) == kWorkPageWeatherClock);

    active_work_page_store(kWorkPageGallery);
    ensure_active_work_page_enabled();
    assert(active_work_page_load() == kWorkPageWeatherClock);

    const uint8_t invalid_order[kWorkPageCount] = {
        kWorkPageWeatherClock,
        kWorkPageWeatherClock,
    };
    work_page_order_replace(invalid_order, sizeof(invalid_order));
    expect_default_order();

    const uint8_t default_order[kWorkPageCount] = {
        kWorkPageWeatherClock,
        kWorkPageGallery,
        kWorkPageWeatherBoard,
        kWorkPageFlipClock,
        kWorkPageCalendar,
        kWorkPageHistory,
        kWorkPageXiaozhiAI,
    };
    const uint8_t alternate_order[kWorkPageCount] = {
        kWorkPageHistory,
        kWorkPageCalendar,
        kWorkPageFlipClock,
        kWorkPageWeatherBoard,
        kWorkPageGallery,
        kWorkPageWeatherClock,
        kWorkPageXiaozhiAI,
    };
    work_page_enabled_mask_store(all_pages);
    std::atomic<bool> writer_done{false};
    std::thread writer([&]() {
        for (int i = 0; i < 10000; ++i) {
            const uint8_t *order = (i & 1) ? default_order : alternate_order;
            work_page_order_replace(order, kWorkPageCount);
        }
        writer_done.store(true, std::memory_order_release);
    });
    do {
        uint8_t snapshot[kWorkPageCount] = {};
        assert(work_page_order_copy(snapshot, sizeof(snapshot)));
        assert(memcmp(snapshot, default_order, sizeof(snapshot)) == 0 ||
               memcmp(snapshot, alternate_order, sizeof(snapshot)) == 0);
    } while (!writer_done.load(std::memory_order_acquire));
    writer.join();
    return 0;
}
