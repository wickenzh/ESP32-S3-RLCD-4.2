// 对固定五次ADC读数取中值，不分配堆内存。
#pragma once

inline int battery_adc_median(int (&samples)[5])
{
    for (int i = 1; i < 5; ++i) {
        const int value = samples[i];
        int j = i;
        while (j > 0 && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            --j;
        }
        samples[j] = value;
    }
    return samples[2];
}
