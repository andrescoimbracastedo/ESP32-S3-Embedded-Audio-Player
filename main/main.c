#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "driver/spi_common.h"

#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "mp3dec.h"
#include "sdmmc_cmd.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/unistd.h>

// SD card
#define SD_CS           1
#define PIN_NUM_MISO    40
#define PIN_NUM_MOSI    42
#define PIN_NUM_CLK     41

// I2S / PCM5102
#define I2S_BCLK        10
#define I2S_LRC         9
#define I2S_DOUT        11

//button matrix

#define MATRIX_ROWS 2
#define MATRIX_COLS 4

static const gpio_num_t row_gpios[MATRIX_ROWS] = {
    GPIO_NUM_15,
    GPIO_NUM_16
};

static const gpio_num_t col_gpios[MATRIX_COLS] = {
    GPIO_NUM_7,
    GPIO_NUM_6,
    GPIO_NUM_5,
    GPIO_NUM_4
};

//system config

#define FILE_BUFFER_SIZE       4096

// Worst case for MPEG-1 Layer III: 1152 samples/frame x 2 channels = 2304 int16s.
#define PCM_OUT_SAMPLES        (2 * 2 * 576)

#define MAX_SONGS              120
#define MAX_FILENAME_LEN       64

#define MATRIX_SCAN_DELAY_MS   50

//global state

static char playlist[MAX_SONGS][MAX_FILENAME_LEN];

static int total_songs_found = 0;
static int current_track_index = 0;

static volatile float volume_scale = 0.40f;

typedef enum {
    CMD_NONE = 0,
    CMD_NEXT,
    CMD_PREV,
    CMD_TOGGLE_PAUSE
} mp3_cmd_t;

// set by the scanner task (core 0), read by the player task (core 1).
// Single slot: a second press is ignored until the player uses the first.
static volatile mp3_cmd_t pending_system_command = CMD_NONE;
static volatile bool is_paused = false;

static i2s_chan_handle_t tx_channel = NULL;
static sdmmc_card_t *card_handle = NULL;


static void init_hardware_sd_card(void)
{
    const char *TAG = "SD_INIT";
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8000
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();

    slot_config.gpio_cs = SD_CS;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(
        "/sdcard",
        &host,
        &slot_config,
        &mount_config,
        &card_handle
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "SD card mounted successfully.");
}


static void populate_playlist(void)
{
    const char *TAG = "PL_INIT";
    DIR *dir = opendir("/sdcard");

    if (dir == NULL) {
        ESP_LOGE(TAG, "Could not open /sdcard");
        return;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && total_songs_found < MAX_SONGS) {

        if (entry->d_type != DT_REG) {
            continue;
        }

        char *filename = entry->d_name;
        size_t len = strlen(filename);

        if (len <= 4) {
            continue;
        }

        if (strcasecmp(filename + len - 4, ".mp3") != 0) {
            continue;
        }

        strncpy(playlist[total_songs_found], filename, MAX_FILENAME_LEN - 1);

        playlist[total_songs_found][MAX_FILENAME_LEN - 1] = '\0';

        ESP_LOGI(TAG,"Indexed [%d]: %s", total_songs_found, playlist[total_songs_found]);

        total_songs_found++;
    }

    closedir(dir);

    ESP_LOGI(TAG, "Total MP3 files found: %d", total_songs_found);
}


static void init_hardware_i2s_audio(void) {
    const char *TAG = "I2S_INIT";
    ESP_LOGI(TAG, "Initializing I2S Standard TX Channel for PCM5102...");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    
    chan_cfg.auto_clear = true; 
    
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_channel, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S TX channel: %s", esp_err_to_name(err));
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = I2S_BCLK,
            .ws = I2S_LRC,
            .dout = I2S_DOUT,
            .din = -1,
            .invert_flags = {
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };


    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    err = i2s_channel_init_std_mode(tx_channel, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S standard mode: %s", esp_err_to_name(err));
        return;
    }

    err = i2s_channel_enable(tx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S TX channel: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "I2S peripheral successfully armed and running.");
}


static void init_hardware_matrix_keyboard(void)
{
    const char *TAG = "KEYB_INIT";
    for (int i = 0; i < MATRIX_ROWS; i++) {

        gpio_reset_pin(row_gpios[i]);
        gpio_set_direction(row_gpios[i], GPIO_MODE_OUTPUT);
        gpio_set_level(row_gpios[i], 1);
    }

    for (int i = 0; i < MATRIX_COLS; i++) {

        gpio_reset_pin(col_gpios[i]);
        gpio_set_direction(col_gpios[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(col_gpios[i], GPIO_PULLUP_ONLY);
    }

    ESP_LOGI(TAG, "2-row matrix initialized.");
}


static void matrix_scanner_task(void *pvParameters)
{
    const char *TAG = "MTRX_SCAN";
    (void)pvParameters;

    ESP_LOGI(TAG, "Keypad scanning engine running on Core 0.");

    int last_pressed_key = -1;

    while (1) {

        int active_key_found = -1;

        for (int r = 0; r < MATRIX_ROWS; r++) {
            gpio_set_level(row_gpios[r], 0);
            // Let the row line settle before reading the columns.
            vTaskDelay(pdMS_TO_TICKS(1));

            for (int c = 0; c < MATRIX_COLS; c++) {
                if (gpio_get_level(col_gpios[c]) == 0) {
                    active_key_found = (r * MATRIX_COLS) + c + 1;
                    break;
                }
            }

            gpio_set_level(row_gpios[r], 1);
            if (active_key_found != -1) {
                break;
            }
        }

        // Act only on a change in key state, so holding a key doesn't repeat.
        // Side effect is that volume doesn't auto repeat when holding.
        if (active_key_found != last_pressed_key) {
            if (active_key_found != -1) {
                ESP_LOGI(TAG, "Switch Activated -> S%d", active_key_found);

                switch (active_key_found) {
                    //volume up
                    case 1:
                        if (volume_scale < 1.0f) {
                            volume_scale += 0.08f;
                            if (volume_scale > 1.0f) {
                                volume_scale = 1.0f;
                            }
                        }

                        ESP_LOGI(TAG, "Volume: %d%%", (int)(volume_scale * 100.0f));
                        break;

                    //previous song
                    case 2:
                        if (pending_system_command == CMD_NONE) {
                            pending_system_command = CMD_PREV;
                            ESP_LOGI(TAG, "Requesting previous track...");
                        }
                        break;

                   //pause
                    case 3:
                        if (pending_system_command == CMD_NONE) {
                            pending_system_command = CMD_TOGGLE_PAUSE;
                            ESP_LOGI(TAG, "Pause toggle requested");
                        }
                        break;

                    //next song
                    case 4:
                        if (pending_system_command == CMD_NONE) {
                            pending_system_command = CMD_NEXT;
                            ESP_LOGI(TAG, "Requesting next track...");
                        }
                        break;

                    //volume down
                    case 5:
                        if (volume_scale > 0.0f) {
                            volume_scale -= 0.08f;
                            if (volume_scale < 0.0f) {
                                volume_scale = 0.0f;
                            }
                        }

                        ESP_LOGI(TAG, "Volume: %d%%", (int)(volume_scale * 100.0f));
                        break;

                    default:
                        break;
                }
            }
            last_pressed_key = active_key_found;
        }
        vTaskDelay(pdMS_TO_TICKS(MATRIX_SCAN_DELAY_MS));
    }
}

//TODO: rewrite this function with abstracted logic and helper functions
//TODO: fix audio frame repeat on changing songs bug
//TODO: fix crash when 5th song is played
static void mp3_player(void *pvParameters)
{
    const char* TAG = "MP3_PLAYER";
    (void)pvParameters;

    if (total_songs_found == 0) {
        ESP_LOGE(TAG, "No MP3 files found.");
        vTaskDelete(NULL);
        return;
    }

    HMP3Decoder mp3_decoder_engine =
        MP3InitDecoder();
    if (mp3_decoder_engine == NULL) {
        ESP_LOGE(TAG, "MP3InitDecoder() failed.");
        vTaskDelete(NULL);
        return;
    }


    uint8_t *file_input_buffer = heap_caps_malloc(FILE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm_output_buffer = heap_caps_malloc(PCM_OUT_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    static int16_t silence_buffer[576 * 2] = {0};

    if (file_input_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate MP3, input buffer in PSRAM.");
        MP3FreeDecoder(mp3_decoder_engine);
        vTaskDelete(NULL);
        return;
    }

    if (pcm_output_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate PCM, input buffer in PSRAM.");
        free(file_input_buffer);
        MP3FreeDecoder(mp3_decoder_engine);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "MP3 input buffer: %d bytes in PSRAM", FILE_BUFFER_SIZE);
    ESP_LOGI(TAG, "PCM output buffer: %zu bytes in PSRAM", PCM_OUT_SAMPLES * sizeof(int16_t));
	
    bool i2s_enabled = true;
    while (1) {

        char full_track_path[MAX_FILENAME_LEN + 16];

        snprintf(
            full_track_path,
            sizeof(full_track_path),
            "/sdcard/%s",
            playlist[current_track_index]
        );

        ESP_LOGI(TAG, "Track Index [%d/%d] -> %s",
                 current_track_index + 1,
                 total_songs_found,
                 playlist[current_track_index]);


        FILE *file_handle = fopen(full_track_path, "rb");

        if (file_handle == NULL) {
            ESP_LOGE(TAG, "Cannot open file: %s", full_track_path);
            current_track_index++;

            if (current_track_index >= total_songs_found) {
                current_track_index = 0;
            }

            continue;
        }

        uint8_t *processing_ptr = file_input_buffer;
        int data_bytes_in_buffer = 0;
        bool track_is_playing = true;
        mp3_cmd_t active_command = CMD_NONE;


        while (track_is_playing) {

            if (pending_system_command != CMD_NONE) {
                active_command = pending_system_command;
                pending_system_command = CMD_NONE;
                switch (active_command) {
                    case CMD_TOGGLE_PAUSE:
                        is_paused = !is_paused;
                        ESP_LOGI(TAG, "Pause: %s", is_paused ? "ON" : "OFF");
                        break;
                    case CMD_NEXT:
                    case CMD_PREV:
                    {
                        //temporary on/off to see how the frame repeating issue changes
                        if (i2s_enabled) {
                            i2s_channel_disable(tx_channel);
                            i2s_enabled = false;
                        }

                        i2s_channel_enable(tx_channel);
                        i2s_enabled = true;
                        size_t bytes_written = 0;

                        //TODO: right now only 44.1 khz work, find out what to do with with mono and other freqs
                        i2s_channel_write(
                            tx_channel,
                            silence_buffer,
                            sizeof(silence_buffer),
                            &bytes_written,
                            portMAX_DELAY
                        );

                        track_is_playing = false;
                        break;
                    }

                    default:
                        break;
                }
            }

            if (is_paused) {
                if (i2s_enabled) {
                    i2s_channel_disable(tx_channel);
                    i2s_enabled = false;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            if (!i2s_enabled) {
                i2s_channel_enable(tx_channel);
                i2s_enabled = true;
            }

            if (data_bytes_in_buffer < FILE_BUFFER_SIZE) {
                if (data_bytes_in_buffer > 0 &&
                    processing_ptr != file_input_buffer) {
                    memmove(file_input_buffer, processing_ptr, data_bytes_in_buffer);
                }
                processing_ptr = file_input_buffer;
                int space_left = FILE_BUFFER_SIZE - data_bytes_in_buffer;

                if (space_left > 0) {
                    size_t bytes_read = fread(file_input_buffer + data_bytes_in_buffer, 1, space_left, file_handle);

                    if (bytes_read == 0) {
                        track_is_playing = false;
                        continue;
                    }

                    data_bytes_in_buffer += (int)bytes_read;
                }
            }

            int sync_offset = MP3FindSyncWord(processing_ptr, data_bytes_in_buffer);

            if (sync_offset < 0) {

                if (data_bytes_in_buffer > 3) {
                    // Keep the last 3 bytes: a 4-byte frame header can straddle the refill boundary.
                    int keep_bytes = 3;
                    memmove(file_input_buffer, processing_ptr + data_bytes_in_buffer - keep_bytes,keep_bytes);
                    processing_ptr = file_input_buffer;
                    data_bytes_in_buffer = keep_bytes;
                }

                continue;
            }


            processing_ptr += sync_offset;
            data_bytes_in_buffer -= sync_offset;
            uint8_t *before_decode = processing_ptr;
            MP3FrameInfo frame_info;
            int decode_result = MP3Decode(mp3_decoder_engine, &processing_ptr, &data_bytes_in_buffer, pcm_output_buffer,0);

            if (decode_result == ERR_MP3_NONE) {
                MP3GetLastFrameInfo(mp3_decoder_engine, &frame_info);
                if (frame_info.outputSamps > 0 && frame_info.outputSamps <= PCM_OUT_SAMPLES) {
                    for (int i = 0; i < frame_info.outputSamps; i++) {
                        pcm_output_buffer[i] = (int16_t)(pcm_output_buffer[i] * volume_scale);
                    }

                    size_t bytes_written = 0;
                    esp_err_t ret =
                        i2s_channel_write(tx_channel, pcm_output_buffer, frame_info.outputSamps * sizeof(int16_t), &bytes_written, portMAX_DELAY);

                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "i2s_channel_write failed: %s", esp_err_to_name(ret));
                    }
                }
                else {
                    ESP_LOGE(TAG, "Invalid output sample count: %d", frame_info.outputSamps);
                }
            }
            else {
                 ESP_LOGE(TAG, "Decode error: %d, remaining=%d", decode_result, data_bytes_in_buffer);

                if (processing_ptr == before_decode) {
                    if (data_bytes_in_buffer > 0) {
                        processing_ptr++;
                        data_bytes_in_buffer--;
                    }
                    else {
                        data_bytes_in_buffer = 0;
                    }
                }
            }


            vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (i2s_enabled) {
            i2s_channel_disable(tx_channel);
            i2s_enabled = false;
        }
        fclose(file_handle);
        processing_ptr = file_input_buffer;
        data_bytes_in_buffer = 0;


        if (active_command == CMD_PREV) {
            if (current_track_index == 0) {
                current_track_index = total_songs_found - 1;
            }
            else {
                current_track_index--;
            }
        }
        else {
            current_track_index++;

            if (current_track_index >= total_songs_found) {
                current_track_index = 0;
            }
        }

        active_command = CMD_NONE;
        MP3FreeDecoder(mp3_decoder_engine);
        mp3_decoder_engine = MP3InitDecoder();

        if (mp3_decoder_engine == NULL) {
            ESP_LOGE(TAG, "Failed to recreate MP3 decoder.");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
        i2s_channel_enable(tx_channel);
        i2s_enabled = true;
    }


    MP3FreeDecoder(mp3_decoder_engine);
    heap_caps_free(file_input_buffer);
    heap_caps_free(pcm_output_buffer);

    vTaskDelete(NULL);
}


void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));

    const char* TAG = "MAIN_FUNC";

    ESP_LOGI(TAG, "STARTING MP3 PLAYER");

    init_hardware_sd_card();
    populate_playlist();
    init_hardware_i2s_audio();
    init_hardware_matrix_keyboard();

    ESP_LOGI(TAG, "Internal free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    ESP_LOGI(TAG, "PSRAM free heap: %lu bytes", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));


    xTaskCreatePinnedToCore(matrix_scanner_task, "matrix_task", 6144, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(mp3_player, "audio_main_task", 12288, NULL, 5, NULL,1);
}
