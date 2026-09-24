#include "runtime_health_policy.h"

#include <cassert>

int main()
{
    assert(runtime_health_wifi_total_us(false, -1, 100, 42) == 42);
    assert(runtime_health_wifi_total_us(true, 100, 250, 42) == 192);
    assert(runtime_health_wifi_total_us(true, 250, 100, 42) == 42);
    assert(runtime_health_lower_value(UINT32_MAX, 4096) == 4096);
    assert(runtime_health_lower_value(4096, 2048) == 2048);
    assert(runtime_health_lower_value(2048, 4096) == 2048);
    assert(runtime_health_lower_value(2048, 0) == 0);
    assert(!runtime_health_periodic_due(100, 0));
    assert(!runtime_health_periodic_due(99, 100));
    assert(runtime_health_periodic_due(100, 100));
    return 0;
}
