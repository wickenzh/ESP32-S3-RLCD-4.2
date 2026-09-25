// 按字节跳过未变化区域并枚举 packed 1-bit 图像差异。
#include "packed_1bit_diff.h"

namespace {
constexpr uint32_t kBitsPerByte = 8;
constexpr uint8_t kPacked1BitMsbMask = 0x80;
static_assert(kBitsPerByte == 8, "packed bitmap diff requires 8-bit bytes");
}

bool packed_1bit_diff_begin(Packed1BitDiffCursor *cursor,
                            const uint8_t *current,
                            const uint8_t *previous,
                            uint32_t bit_count)
{
    if (!cursor) {
        return false;
    }
    *cursor = {};
    if (!current || bit_count == 0) {
        return false;
    }
    cursor->current = current;
    cursor->previous = previous;
    cursor->bit_count = bit_count;
    cursor->byte_count = static_cast<size_t>(bit_count / kBitsPerByte) +
                         ((bit_count % kBitsPerByte) != 0 ? 1U : 0U);
    return true;
}

bool packed_1bit_diff_next(Packed1BitDiffCursor *cursor,
                           uint32_t *bit_index,
                           bool *bit_set)
{
    if (!cursor || !bit_index || !bit_set || !cursor->current) {
        return false;
    }
    for (;;) {
        if (cursor->changed_bits != 0) {
            uint8_t mask = kPacked1BitMsbMask;
            uint32_t offset = 0;
            while ((cursor->changed_bits & mask) == 0) {
                mask >>= 1U;
                ++offset;
            }
            cursor->changed_bits &= static_cast<uint8_t>(~mask);
            const uint32_t current_bit =
                cursor->current_byte_base_bit + offset;
            if (current_bit >= cursor->bit_count) {
                continue;
            }
            *bit_index = current_bit;
            *bit_set = (cursor->current_byte & mask) != 0;
            return true;
        }
        if (cursor->next_byte >= cursor->byte_count) {
            return false;
        }
        const size_t byte_index = cursor->next_byte++;
        cursor->current_byte = cursor->current[byte_index];
        cursor->changed_bits = cursor->previous
                                   ? static_cast<uint8_t>(
                                         cursor->current_byte ^
                                         cursor->previous[byte_index])
                                   : UINT8_MAX;
        cursor->current_byte_base_bit =
            static_cast<uint32_t>(byte_index * kBitsPerByte);
    }
}
