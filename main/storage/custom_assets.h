// 声明自定义资源包格式、入口表和资源读取接口。
#pragma once

#include <stddef.h>
#include <stdint.h>

static constexpr uint32_t kCustomAssetsMagic = 0x31414357; // WCA1
static constexpr uint16_t kCustomAssetsVersion = 1;
static constexpr uint16_t kCustomAssetTypeMainGif = 1;
static constexpr uint16_t kCustomAssetTypeGalleryImage = 2;
static constexpr int kCustomAssetMaxEntries = 32;

struct CustomAssetsHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint16_t entry_count;
    uint16_t reserved;
    uint32_t total_size;
    uint32_t header_crc;
    uint32_t payload_crc;
} __attribute__((packed));

struct CustomAssetEntry {
    uint16_t type;
    uint16_t index;
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;
    uint16_t bytes_per_row;
    uint32_t offset;
    uint32_t length;
    uint32_t crc32;
} __attribute__((packed));

void custom_assets_init();
bool custom_assets_available();
bool custom_assets_has_main_gif();
int custom_assets_gallery_count();
bool custom_assets_read_main_gif_frame(int frame, uint8_t *out, size_t out_len);
bool custom_assets_read_gallery_image(int index, uint8_t *out, size_t out_len);
