#include "packed_1bit_diff.h"
#include "status_gif_60.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace {
constexpr uint32_t kStatusGifBits = STATUS_GIF_WIDTH * STATUS_GIF_HEIGHT;

void set_packed_bit(uint8_t *bytes, uint32_t bit, bool value)
{
    const uint8_t mask = static_cast<uint8_t>(0x80U >> (bit & 7U));
    if (value) {
        bytes[bit / 8U] |= mask;
    } else {
        bytes[bit / 8U] &= static_cast<uint8_t>(~mask);
    }
}

void apply_diff(uint8_t *target,
                const uint8_t *current,
                const uint8_t *previous,
                uint32_t bit_count)
{
    Packed1BitDiffCursor cursor;
    assert(packed_1bit_diff_begin(&cursor,
                                  current,
                                  previous,
                                  bit_count));
    uint32_t bit = 0;
    bool value = false;
    while (packed_1bit_diff_next(&cursor, &bit, &value)) {
        assert(bit < bit_count);
        set_packed_bit(target, bit, value);
    }
}
}

int main()
{
    Packed1BitDiffCursor invalid;
    assert(!packed_1bit_diff_begin(nullptr, nullptr, nullptr, 0));
    assert(!packed_1bit_diff_begin(&invalid, nullptr, nullptr, 8));
    assert(!packed_1bit_diff_begin(&invalid,
                                   status_gif_frames[0],
                                   nullptr,
                                   0));

    const uint8_t ten_bits[] = {0b10100001, 0b11000000};
    uint8_t reconstructed_ten_bits[sizeof(ten_bits)] = {};
    apply_diff(reconstructed_ten_bits, ten_bits, nullptr, 10);
    assert(std::memcmp(reconstructed_ten_bits,
                       ten_bits,
                       sizeof(ten_bits)) == 0);
    assert((reconstructed_ten_bits[1] & 0x3fU) == 0);

    std::array<uint8_t, STATUS_GIF_BYTES_PER_FRAME> reconstructed = {};
    apply_diff(reconstructed.data(),
               status_gif_frames[0],
               nullptr,
               kStatusGifBits);
    assert(std::memcmp(reconstructed.data(),
                       status_gif_frames[0],
                       reconstructed.size()) == 0);

    for (int frame = 1; frame < STATUS_GIF_FRAME_COUNT; ++frame) {
        apply_diff(reconstructed.data(),
                   status_gif_frames[frame],
                   status_gif_frames[frame - 1],
                   kStatusGifBits);
        assert(std::memcmp(reconstructed.data(),
                           status_gif_frames[frame],
                           reconstructed.size()) == 0);
    }

    Packed1BitDiffCursor unchanged;
    assert(packed_1bit_diff_begin(&unchanged,
                                  status_gif_frames[0],
                                  status_gif_frames[0],
                                  kStatusGifBits));
    uint32_t bit = 0;
    bool value = false;
    assert(!packed_1bit_diff_next(&unchanged, &bit, &value));
    return 0;
}
