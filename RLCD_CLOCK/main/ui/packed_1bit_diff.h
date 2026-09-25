// 声明 packed 1-bit 图像差异游标及遍历接口。
#pragma once

#include <stddef.h>
#include <stdint.h>

struct Packed1BitDiffCursor {
    const uint8_t *current = nullptr;
    const uint8_t *previous = nullptr;
    uint32_t bit_count = 0;
    size_t byte_count = 0;
    size_t next_byte = 0;
    uint32_t current_byte_base_bit = 0;
    uint8_t current_byte = 0;
    uint8_t changed_bits = 0;
};

bool packed_1bit_diff_begin(Packed1BitDiffCursor *cursor,
                            const uint8_t *current,
                            const uint8_t *previous,
                            uint32_t bit_count);

bool packed_1bit_diff_next(Packed1BitDiffCursor *cursor,
                           uint32_t *bit_index,
                           bool *bit_set);
