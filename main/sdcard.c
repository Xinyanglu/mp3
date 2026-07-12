/*
 * SD-card storage scaffold.
 *
 * This module owns SD-card mounting, song discovery, and the selected track.
 */
#include "sdcard.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

typedef struct {
    const char* mount_path;
    spi_host_device_t host;
    gpio_num_t gpio_miso;
    gpio_num_t gpio_mosi;
    gpio_num_t gpio_sclk;
    gpio_num_t gpio_cs;
    int max_files;
    bool format_if_mount_failed;
    int max_freq_khz;
    int max_transfer_sz;
} sdcard_config_t;

static const char* TAG = "sdcard";

static char ROOT_PATH[SDCARD_MAX_PATH_LEN] = "/sdcard";
static sdcard_song_t songs[SDCARD_MAX_SONGS];
static size_t song_count;
static sdmmc_card_t* sd_card;
static bool sdcard_mounted;
static bool sdcard_initialized;
static sdcard_config_t sdcard_default_config = {
    .mount_path             = ROOT_PATH,
    .host                   = SPI3_HOST,
    .gpio_miso              = GPIO_NUM_19,
    .gpio_mosi              = GPIO_NUM_23,
    .gpio_sclk              = GPIO_NUM_18,
    .gpio_cs                = GPIO_NUM_5,
    .max_files              = 5,
    .format_if_mount_failed = false,
    .max_freq_khz           = 10000,
    .max_transfer_sz        = 4000,
};

static esp_err_t sdcard_mount(void);
static bool sdcard_has_mp3_extension(const char* name);
static esp_err_t sdcard_build_path(char* path, size_t path_size, const char* name);
static bool sdcard_is_regular_file(const char* path, struct stat* st);
static esp_err_t sdcard_add_song(const char* name);

esp_err_t sdcard_init(void) {
    esp_err_t mount_ret;

    if (sdcard_initialized) {
        return ESP_OK;
    }

    mount_ret = sdcard_mount();
    if (mount_ret != ESP_OK) {
        return mount_ret;
    }

    sdcard_initialized = true;
    return ESP_OK;
}

static esp_err_t sdcard_mount(void) {
    esp_err_t ret;
    const char* mount_path = sdcard_default_config.mount_path;
    sdmmc_host_t host      = SDSPI_HOST_DEFAULT();

    if (sdcard_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    host.slot         = sdcard_default_config.host;
    host.max_freq_khz = sdcard_default_config.max_freq_khz;

    spi_bus_config_t bus_config = {
        .mosi_io_num     = sdcard_default_config.gpio_mosi,
        .miso_io_num     = sdcard_default_config.gpio_miso,
        .sclk_io_num     = sdcard_default_config.gpio_sclk,
        .quadwp_io_num   = GPIO_NUM_NC,
        .quadhd_io_num   = GPIO_NUM_NC,
        .max_transfer_sz = sdcard_default_config.max_transfer_sz,
    };

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id               = host.slot;
    slot_config.gpio_cs               = sdcard_default_config.gpio_cs;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = sdcard_default_config.format_if_mount_failed,
        .max_files              = sdcard_default_config.max_files,
    };

    ESP_LOGI(TAG, "Initializing SD card on SPI host %d", host.slot);

    ret = spi_bus_initialize(host.slot, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SD SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Mounting SD card at %s", mount_path);
    ret = esp_vfs_fat_sdspi_mount(mount_path, &host, &slot_config, &mount_config, &sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        spi_bus_free(host.slot);
        return ret;
    }

    sdcard_mounted = true;
    sdmmc_card_print_info(stdout, sd_card);
    return ESP_OK;
}

esp_err_t sdcard_scan_songs(void) {
    DIR* dir;
    struct dirent* entry;
    song_count        = 0;

    dir = opendir(ROOT_PATH);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open SD card root path: %s", ROOT_PATH);
        return ESP_FAIL;
    }

    while ((entry = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, "SD card entry: %s", entry->d_name);

        esp_err_t ret = sdcard_add_song(entry->d_name);
        if (ret == ESP_ERR_NO_MEM) {
            ESP_LOGW(TAG, "Song list full; ignoring remaining files");
            break;
        }
    }

    closedir(dir);
    ESP_LOGI(TAG, "Found %u MP3 song(s)", (unsigned int)song_count);
    return ESP_OK;
}

size_t sdcard_get_song_count(void) {
    return song_count;
}

const sdcard_song_t* sdcard_get_song(size_t index) {
    if (index >= song_count) {
        return NULL;
    }

    return &songs[index];
}

esp_err_t sdcard_get_song_path(size_t index, char* path, size_t path_size) {
    if (index >= song_count || path == NULL || path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return sdcard_build_path(path, path_size, songs[index].name);
}

static bool sdcard_has_mp3_extension(const char* name) {
    const char* ext;

    if (name == NULL) {
        return false;
    }

    ext = strrchr(name, '.');
    if (ext == NULL) {
        return false;
    }

    return strcasecmp(ext, ".mp3") == 0;
}

static esp_err_t sdcard_build_path(char* path, size_t path_size, const char* name) {
    if (strlcpy(path, ROOT_PATH, path_size) >= path_size || strlcat(path, "/", path_size) >= path_size ||
        strlcat(path, name, path_size) >= path_size) {
        ESP_LOGW(TAG, "Skipping file with too-long path: %s", name);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static bool sdcard_is_regular_file(const char* path, struct stat* st) {
    if (stat(path, st) != 0 || st->st_size == 0) {
        return false;
    }

    return S_ISREG(st->st_mode);
}

static esp_err_t sdcard_add_song(const char* name) {
    sdcard_song_t* song;
    struct stat st;
    char path[SDCARD_MAX_PATH_LEN];

    if (song_count >= SDCARD_MAX_SONGS) {
        return ESP_ERR_NO_MEM;
    }

    if (sdcard_build_path(path, sizeof(path), name) != ESP_OK) {
        return ESP_OK;
    }

    if (!sdcard_has_mp3_extension(path) || !sdcard_is_regular_file(path, &st)) {
        return ESP_OK;
    }

    song = &songs[song_count];
    memset(song, 0, sizeof(*song));

    strlcpy(song->name, name, sizeof(song->name));
    song->size_bytes = (uint32_t)st.st_size;

    ESP_LOGI(TAG, "Found song: %s", song->name);
    song_count++;
    return ESP_OK;
}
