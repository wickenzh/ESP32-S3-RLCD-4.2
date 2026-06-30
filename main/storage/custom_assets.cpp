// 读取并校验上位机写入 assets 分区的自定义图片资源包。
#include "custom_assets.h"

#include "app_state.h"
#include "clock_gallery_images.h"
#include "status_gif_60.h"

#include "esp_partition.h"

static const esp_partition_t *s_assets_partition = nullptr;
static CustomAssetsHeader s_assets_header = {};
static CustomAssetEntry s_entries[kCustomAssetMaxEntries] = {};
static int s_entry_count = 0;
static const CustomAssetEntry *s_main_gif_entry = nullptr;
static const CustomAssetEntry *s_gallery_entries[kCustomAssetMaxEntries] = {};
static int s_gallery_count = 0;
static bool s_assets_ready = false;
static constexpr const char *kCustomAssetsPartitionLabel = "assets";
static constexpr size_t kCustomAssetCrcChunkSize = 256;
static constexpr uint16_t kBitsPerByte = 8;
static constexpr uint32_t kCustomAssetCrc32Polynomial = 0xEDB88320U;
static constexpr uint32_t kCustomAssetCrc32Initial = 0xFFFFFFFFU;
static constexpr uint16_t kCustomAssetMaxGalleryImages = 24;
static constexpr int kCustomAssetDiagGifFrames[] = {0, 1, 30, 59};

class CustomAssetTempBuffer {
public:
    explicit CustomAssetTempBuffer(size_t size)
        : data_((uint8_t *)malloc(size))
    {
    }

    ~CustomAssetTempBuffer()
    {
        free(data_);
    }

    CustomAssetTempBuffer(const CustomAssetTempBuffer &) = delete;
    CustomAssetTempBuffer &operator=(const CustomAssetTempBuffer &) = delete;

    uint8_t *data() const
    {
        return data_;
    }

private:
    uint8_t *data_ = nullptr;
};

static uint32_t crc32_update_raw(uint32_t crc, const uint8_t *data, size_t len)
{
    if (!data && len > 0) {
        return crc;
    }
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < kBitsPerByte; ++bit) {
            uint32_t mask = -(crc & 1U);
            crc = (crc >> 1) ^ (kCustomAssetCrc32Polynomial & mask);
        }
    }
    return crc;
}

static uint32_t crc32_bytes(const uint8_t *data, size_t len)
{
    if (!data && len > 0) {
        return 0;
    }
    return ~crc32_update_raw(kCustomAssetCrc32Initial, data, len);
}

static bool partition_range_valid(uint32_t offset, size_t length)
{
    if (!s_assets_partition) {
        return false;
    }
    if (offset > s_assets_partition->size || length > s_assets_partition->size - offset) {
        ESP_LOGW(TAG,
                 "custom assets partition range invalid offset=0x%08lx length=%u partition=%lu",
                 (unsigned long)offset,
                 (unsigned)length,
                 (unsigned long)s_assets_partition->size);
        return false;
    }
    return true;
}

static bool partition_crc(uint32_t offset, uint32_t length, uint32_t *crc_out)
{
    if (!s_assets_partition || !crc_out) {
        return false;
    }
    if (!partition_range_valid(offset, length)) {
        return false;
    }
    uint8_t buffer[kCustomAssetCrcChunkSize];
    uint32_t crc = kCustomAssetCrc32Initial;
    uint32_t remaining = length;
    uint32_t cursor = offset;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        esp_err_t err = esp_partition_read(s_assets_partition, cursor, buffer, chunk);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "read assets partition failed: %s", esp_err_to_name(err));
            return false;
        }
        crc = crc32_update_raw(crc, buffer, chunk);
        cursor += chunk;
        remaining -= chunk;
    }
    *crc_out = ~crc;
    return true;
}

static bool read_checked(uint32_t offset, void *out, size_t len)
{
    if (!s_assets_partition || !out) {
        return false;
    }
    if (!partition_range_valid(offset, len)) {
        return false;
    }
    esp_err_t err = esp_partition_read(s_assets_partition, offset, out, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read custom asset failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool validate_entry_bounds(const CustomAssetEntry &entry)
{
    if (!s_assets_partition) {
        return false;
    }
    if (entry.offset < s_assets_header.header_size) {
        ESP_LOGW(TAG, "custom asset entry before payload type=%u index=%u", entry.type, entry.index);
        return false;
    }
    if (entry.length == 0 || entry.offset > s_assets_header.total_size) {
        ESP_LOGW(TAG, "custom asset entry offset invalid type=%u index=%u", entry.type, entry.index);
        return false;
    }
    if (entry.length > s_assets_header.total_size - entry.offset) {
        ESP_LOGW(TAG, "custom asset entry length invalid type=%u index=%u", entry.type, entry.index);
        return false;
    }
    if (s_assets_header.total_size > s_assets_partition->size) {
        return false;
    }
    return true;
}

static uint16_t packed_1bit_bytes_per_row(uint16_t width)
{
    return (width + kBitsPerByte - 1) / kBitsPerByte;
}

static bool validate_entry_shape(const CustomAssetEntry &entry)
{
    bool valid = false;
    if (entry.type == kCustomAssetTypeMainGif) {
        size_t expected = (size_t)STATUS_GIF_FRAME_COUNT * STATUS_GIF_BYTES_PER_FRAME;
        bool row_ok = entry.bytes_per_row == 0 || entry.bytes_per_row == packed_1bit_bytes_per_row(STATUS_GIF_WIDTH);
        valid = entry.index == 0 &&
                entry.width == STATUS_GIF_WIDTH &&
                entry.height == STATUS_GIF_HEIGHT &&
                entry.frame_count == STATUS_GIF_FRAME_COUNT &&
                row_ok &&
                entry.length == expected;
    } else if (entry.type == kCustomAssetTypeGalleryImage) {
        size_t expected = (size_t)CLOCK_GALLERY_IMAGE_BYTES_PER_ROW * CLOCK_GALLERY_IMAGE_HEIGHT;
        valid = entry.index < kCustomAssetMaxGalleryImages &&
                entry.width == CLOCK_GALLERY_IMAGE_WIDTH &&
                entry.height == CLOCK_GALLERY_IMAGE_HEIGHT &&
                entry.frame_count == 1 &&
                entry.bytes_per_row == CLOCK_GALLERY_IMAGE_BYTES_PER_ROW &&
                entry.length == expected;
    }
    if (!valid) {
        ESP_LOGW(TAG,
                 "custom asset entry shape invalid type=%u index=%u size=%ux%u frames=%u row=%u length=%lu",
                 (unsigned)entry.type,
                 (unsigned)entry.index,
                 (unsigned)entry.width,
                 (unsigned)entry.height,
                 (unsigned)entry.frame_count,
                 (unsigned)entry.bytes_per_row,
                 (unsigned long)entry.length);
    }
    return valid;
}

static bool validate_entry_crc(const CustomAssetEntry &entry)
{
    uint32_t crc = 0;
    if (!partition_crc(entry.offset, entry.length, &crc)) {
        return false;
    }
    if (crc != entry.crc32) {
        ESP_LOGW(TAG, "custom asset entry crc mismatch type=%u index=%u", entry.type, entry.index);
        return false;
    }
    return true;
}

static bool validate_header_crc()
{
    size_t header_bytes = sizeof(CustomAssetsHeader) + (size_t)s_entry_count * sizeof(CustomAssetEntry);
    CustomAssetTempBuffer buffer(header_bytes);
    if (!buffer.data()) {
        ESP_LOGW(TAG, "custom assets header crc alloc failed");
        return false;
    }
    memcpy(buffer.data(), &s_assets_header, sizeof(CustomAssetsHeader));
    memcpy(buffer.data() + sizeof(CustomAssetsHeader), s_entries, (size_t)s_entry_count * sizeof(CustomAssetEntry));
    CustomAssetsHeader *header = (CustomAssetsHeader *)buffer.data();
    header->header_crc = 0;
    uint32_t crc = crc32_bytes(buffer.data(), header_bytes);
    if (crc != s_assets_header.header_crc) {
        ESP_LOGW(TAG, "custom assets header crc mismatch");
        return false;
    }
    return true;
}

static bool validate_payload_crc()
{
    if (s_assets_header.header_size > s_assets_header.total_size) {
        ESP_LOGW(TAG,
                 "custom assets payload range invalid header=%u total=%lu",
                 (unsigned)s_assets_header.header_size,
                 (unsigned long)s_assets_header.total_size);
        return false;
    }
    uint32_t payload_offset = s_assets_header.header_size;
    uint32_t payload_length = s_assets_header.total_size - payload_offset;
    uint32_t crc = 0;
    if (!partition_crc(payload_offset, payload_length, &crc)) {
        return false;
    }
    if (crc != s_assets_header.payload_crc) {
        ESP_LOGW(TAG, "custom assets payload crc mismatch");
        return false;
    }
    return true;
}

static void reset_custom_assets()
{
    s_assets_header = {};
    memset(s_entries, 0, sizeof(s_entries));
    memset(s_gallery_entries, 0, sizeof(s_gallery_entries));
    s_entry_count = 0;
    s_main_gif_entry = nullptr;
    s_gallery_count = 0;
    s_assets_ready = false;
}

static int count_black_bits(const uint8_t *data, size_t len)
{
    if (!data) {
        return 0;
    }
    int total = 0;
    for (size_t i = 0; i < len; ++i) {
        uint8_t value = data[i];
        while (value) {
            total += value & 1U;
            value >>= 1;
        }
    }
    return total;
}

static void log_custom_gif_frame_density(int frame)
{
    if (!s_main_gif_entry || frame < 0 || frame >= STATUS_GIF_FRAME_COUNT) {
        return;
    }
    uint8_t buffer[STATUS_GIF_BYTES_PER_FRAME];
    uint32_t offset = s_main_gif_entry->offset + (uint32_t)frame * STATUS_GIF_BYTES_PER_FRAME;
    if (!read_checked(offset, buffer, sizeof(buffer))) {
        ESP_LOGW(TAG, "custom assets diag: gif frame %d read failed", frame);
        return;
    }
    ESP_LOGI(TAG,
             "custom assets diag: gif frame %d black_bits=%d/%d",
             frame,
             count_black_bits(buffer, sizeof(buffer)),
             STATUS_GIF_WIDTH * STATUS_GIF_HEIGHT);
}

void custom_assets_init()
{
    ESP_LOGI(TAG, "custom assets diag: init enter");
    reset_custom_assets();
    s_assets_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                  ESP_PARTITION_SUBTYPE_ANY,
                                                  kCustomAssetsPartitionLabel);
    if (!s_assets_partition) {
        ESP_LOGI(TAG, "custom assets diag: partition not found");
        return;
    }
    ESP_LOGI(TAG,
             "custom assets diag: partition found address=0x%08lx size=%lu",
             (unsigned long)s_assets_partition->address,
             (unsigned long)s_assets_partition->size);
    if (!read_checked(0, &s_assets_header, sizeof(s_assets_header))) {
        ESP_LOGW(TAG, "custom assets diag: header read failed");
        return;
    }
    ESP_LOGI(TAG,
             "custom assets diag: header magic=0x%08lx version=%u header=%u entries=%u total=%lu header_crc=0x%08lx payload_crc=0x%08lx",
             (unsigned long)s_assets_header.magic,
             (unsigned)s_assets_header.version,
             (unsigned)s_assets_header.header_size,
             (unsigned)s_assets_header.entry_count,
             (unsigned long)s_assets_header.total_size,
             (unsigned long)s_assets_header.header_crc,
             (unsigned long)s_assets_header.payload_crc);
    if (s_assets_header.magic != kCustomAssetsMagic ||
        s_assets_header.version != kCustomAssetsVersion ||
        s_assets_header.entry_count == 0 ||
        s_assets_header.entry_count > kCustomAssetMaxEntries) {
        ESP_LOGI(TAG, "custom assets diag: no valid package");
        return;
    }
    s_entry_count = s_assets_header.entry_count;
    size_t min_header_size = sizeof(CustomAssetsHeader) + (size_t)s_entry_count * sizeof(CustomAssetEntry);
    if (s_assets_header.header_size != min_header_size ||
        s_assets_header.total_size <= s_assets_header.header_size ||
        s_assets_header.total_size > s_assets_partition->size) {
        ESP_LOGW(TAG, "custom assets header size invalid");
        reset_custom_assets();
        return;
    }
    if (!read_checked(sizeof(CustomAssetsHeader), s_entries, (size_t)s_entry_count * sizeof(CustomAssetEntry))) {
        ESP_LOGW(TAG, "custom assets diag: entries read failed");
        reset_custom_assets();
        return;
    }
    if (!validate_header_crc() || !validate_payload_crc()) {
        reset_custom_assets();
        return;
    }

    for (int i = 0; i < s_entry_count; ++i) {
        CustomAssetEntry &entry = s_entries[i];
        ESP_LOGI(TAG,
                 "custom assets diag: entry[%d] type=%u index=%u size=%ux%u frames=%u row=%u offset=0x%08lx length=%lu crc=0x%08lx",
                 i,
                 (unsigned)entry.type,
                 (unsigned)entry.index,
                 (unsigned)entry.width,
                 (unsigned)entry.height,
                 (unsigned)entry.frame_count,
                 (unsigned)entry.bytes_per_row,
                 (unsigned long)entry.offset,
                 (unsigned long)entry.length,
                 (unsigned long)entry.crc32);
        if (!validate_entry_bounds(entry) ||
            !validate_entry_shape(entry) ||
            !validate_entry_crc(entry)) {
            ESP_LOGW(TAG, "custom assets diag: entry[%d] rejected", i);
            reset_custom_assets();
            return;
        }
        if (entry.type == kCustomAssetTypeMainGif) {
            if (s_main_gif_entry) {
                ESP_LOGW(TAG, "custom assets diag: duplicate main gif entry");
                reset_custom_assets();
                return;
            }
            s_main_gif_entry = &entry;
        } else if (entry.type == kCustomAssetTypeGalleryImage && s_gallery_count < kCustomAssetMaxEntries) {
            for (int j = 0; j < s_gallery_count; ++j) {
                if (s_gallery_entries[j] && s_gallery_entries[j]->index == entry.index) {
                    ESP_LOGW(TAG, "custom assets diag: duplicate gallery entry index=%u", entry.index);
                    reset_custom_assets();
                    return;
                }
            }
            s_gallery_entries[s_gallery_count++] = &entry;
        }
    }
    for (int i = 0; i < s_gallery_count - 1; ++i) {
        for (int j = i + 1; j < s_gallery_count; ++j) {
            if (s_gallery_entries[j]->index < s_gallery_entries[i]->index) {
                const CustomAssetEntry *tmp = s_gallery_entries[i];
                s_gallery_entries[i] = s_gallery_entries[j];
                s_gallery_entries[j] = tmp;
            }
        }
    }
    s_assets_ready = s_main_gif_entry || s_gallery_count > 0;
    ESP_LOGI(TAG, "custom assets diag: ready main_gif=%d gallery=%d", s_main_gif_entry ? 1 : 0, s_gallery_count);
    if (s_main_gif_entry) {
        for (int frame : kCustomAssetDiagGifFrames) {
            log_custom_gif_frame_density(frame);
        }
    }
}

bool custom_assets_available()
{
    return s_assets_ready;
}

bool custom_assets_has_main_gif()
{
    return s_main_gif_entry != nullptr;
}

int custom_assets_gallery_count()
{
    return s_gallery_count;
}

bool custom_assets_read_main_gif_frame(int frame, uint8_t *out, size_t out_len)
{
    if (!s_main_gif_entry || !out || out_len < STATUS_GIF_BYTES_PER_FRAME) {
        return false;
    }
    if (frame < 0) {
        frame = 0;
    } else if (frame >= STATUS_GIF_FRAME_COUNT) {
        frame = STATUS_GIF_FRAME_COUNT - 1;
    }
    uint32_t offset = s_main_gif_entry->offset + (uint32_t)frame * STATUS_GIF_BYTES_PER_FRAME;
    return read_checked(offset, out, STATUS_GIF_BYTES_PER_FRAME);
}

bool custom_assets_read_gallery_image(int index, uint8_t *out, size_t out_len)
{
    if (s_gallery_count <= 0 || !out) {
        return false;
    }
    size_t expected = (size_t)CLOCK_GALLERY_IMAGE_BYTES_PER_ROW * CLOCK_GALLERY_IMAGE_HEIGHT;
    if (out_len < expected) {
        return false;
    }
    if (index < 0) {
        index = 0;
    }
    const CustomAssetEntry *entry = s_gallery_entries[index % s_gallery_count];
    if (!entry) {
        return false;
    }
    return read_checked(entry->offset, out, expected);
}
