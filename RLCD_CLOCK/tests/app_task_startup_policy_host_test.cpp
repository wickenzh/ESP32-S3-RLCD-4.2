#include "app_task_startup_policy.h"

#include <cassert>

int main()
{
    static_assert(kAllRegularAppTaskBits == 0x7fU);
    static_assert(regular_app_task_bit(RegularAppTaskId::kNetworkSync) == 0x01U);
    static_assert(regular_app_task_bit(RegularAppTaskId::kPomodoro) == 0x40U);

    AppTaskStartupResult complete = {kAllRegularAppTaskBits, 0};
    assert(regular_app_tasks_healthy_for_ota(complete,
                                             kAllRegularAppTaskBits));
    assert(regular_app_task_missing_ready_mask(complete,
                                               kAllRegularAppTaskBits) == 0);

    const uint32_t missing_ui =
        kAllRegularAppTaskBits &
        ~regular_app_task_bit(RegularAppTaskId::kUi);
    assert(!regular_app_tasks_healthy_for_ota(complete, missing_ui));
    assert(regular_app_task_missing_ready_mask(complete, missing_ui) ==
           regular_app_task_bit(RegularAppTaskId::kUi));

    AppTaskStartupResult create_failed = {
        missing_ui,
        regular_app_task_bit(RegularAppTaskId::kUi),
    };
    assert(!regular_app_tasks_healthy_for_ota(create_failed,
                                              kAllRegularAppTaskBits));
    assert(regular_app_task_missing_ready_mask(create_failed,
                                               kAllRegularAppTaskBits) ==
           regular_app_task_bit(RegularAppTaskId::kUi));
    return 0;
}
