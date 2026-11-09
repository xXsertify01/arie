#include <furi.h>
#include <furi_hal_region.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>
#include <power/power_service/power.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/environment.h>
#include <lib/subghz/receiver.h>
#include <lib/subghz/protocols/raw.h>
#include <lib/subghz/protocols/public_api.h>
#include <lib/subghz/transmitter.h>
#include <lib/subghz/subghz_setting.h>
#include <lib/subghz/subghz_protocol_registry.h>
#include <lib/subghz/subghz_worker.h>
#include <lib/flipper_format/flipper_format_i.h>
#include <lib/flipper_format/flipper_format.h>
#include <storage/storage.h>
#include <toolbox/stream/stream.h>
#include <furi_hal_random.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define SCOUT_SETTING_PATH EXT_PATH("subghz/assets/setting_user")
#define MAX_SUBGHZ_FILES 128U
#define MAX_SUBGHZ_FILENAME 48U
#define MAX_DETECTIONS 16U
#define MAX_DETECTION_DETAIL 192U
#define FILE_CONTENT_CAPACITY 2048U
#define FILE_DETAIL_MAX_LINES 112U
#define MAX_EDITABLE_BASENAME (MAX_SUBGHZ_FILENAME - 5U)
#define RAW_RSSI_HISTORY_SIZE 100U
#define DETAIL_LINE_CHARS 23U
#define RAW_SIGNAL_PRETRIGGER_SAMPLES 32U
#define RAW_SIGNAL_TAIL_SAMPLES 512U
#define RAW_SIGNAL_TAIL_HOLD_US 25000U
#define RAW_DEFAULT_RSSI_MARGIN_DB 8U
#define RSSI_BAR_MIN_DBM -110.0f
#define STOCK_RSSI_FLOOR_DBM -90.0f
#define EVENT_QUEUE_SIZE 8U
#define RAW_CAPTURE_NAME "Scout_RAW"
#define RAW_CAPTURE_PATH EXT_PATH("subghz/Scout_RAW.sub")
#define LAB_TEST_MAX_DURATION_MS 5000U

static const char* const raw_threshold_text[] = {
    "-----", "-85.0", "-80.0", "-75.0", "-70.0", "-65.0",
    "-60.0", "-55.0", "-50.0", "-45.0", "-40.0",
};

static const float raw_threshold_values[] = {
    -90.0f, -85.0f, -80.0f, -75.0f, -70.0f, -65.0f,
    -60.0f, -55.0f, -50.0f, -45.0f, -40.0f,
};

#define RAW_THRESHOLD_COUNT (sizeof(raw_threshold_values) / sizeof(raw_threshold_values[0]))

typedef enum {
    ScoutViewStartMenu,
    ScoutViewListening,
    ScoutViewSettings,
    ScoutViewAnalyzer,
    ScoutViewLabTest,
    ScoutViewRawRecording,
    ScoutViewSubFiles,
    ScoutViewFileDetails,
    ScoutViewDeleteConfirmation,
    ScoutViewNameFile,
    ScoutViewDetectionDetails,
    ScoutViewRegionInformation,
    ScoutViewManualUnavailable,
    ScoutViewRadioModule,
} ScoutView;

typedef enum {
    ScoutSettingFrequency,
    ScoutSettingHopping,
    ScoutSettingModulation,
    ScoutSettingBinRaw,
    ScoutSettingRawThreshold,
    ScoutSettingCount,
} ScoutSettingIndex;

typedef struct {
    char protocol[32];
    char detail[MAX_DETECTION_DETAIL];
    uint32_t fingerprint;
    uint32_t frequency;
    uint32_t last_seen_tick;
    uint16_t sightings;
    uint8_t preset_index;
    bool serialization_attempted;
    uint8_t* serialized_data;
    size_t serialized_size;
} ScoutDetection;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* event_queue;
    ViewPort* view_port;
    const SubGhzDevice* device;
    SubGhzEnvironment* protocol_environment;
    SubGhzReceiver* protocol_receiver;
    SubGhzProtocolDecoderBinRAW* binraw_decoder;
    SubGhzWorker* protocol_worker;
    SubGhzSetting* radio_setting;
    Storage* storage;
    uint32_t frequency_hz;
    uint32_t frequency_index;
    uint8_t preset_index;
    bool receiving;
    bool radio_ready;
    bool lab_test_active;
    bool file_tx_active;
    bool file_tx_repeat_held;
    bool lab_test_resume_receiver;
    SubGhzTransmitter* lab_test_transmitter;
    uint32_t lab_test_started_tick;
    uint32_t lab_test_code;
    bool external_module_available;
    bool external_module_selected;
    bool radio_device_begun;
    bool should_exit;
    bool storage_ready;
    bool hopping_enabled;
    bool binraw_enabled;
    volatile bool raw_recording;
    volatile bool raw_capture_complete;
    volatile bool raw_gate_active;
    SubGhzProtocolDecoderRAW* raw_decoder;
    volatile float rssi;
    float noise_floor_dbm;
    volatile float raw_trigger_dbm;
    uint8_t raw_rssi_margin_db;
    uint8_t raw_rssi_history[RAW_RSSI_HISTORY_SIZE];
    uint8_t raw_rssi_history_head;
    uint8_t raw_rssi_history_count;
    uint32_t raw_below_threshold_us;
    uint8_t raw_pretrigger_head;
    uint8_t raw_pretrigger_count;
    int32_t raw_pretrigger[RAW_SIGNAL_PRETRIGGER_SAMPLES];
    int32_t* raw_tail_samples;
    uint16_t raw_tail_count;
    bool raw_capture_saved;
    uint32_t analyzer_last_step_tick;
    uint32_t hopper_last_step_tick;
    uint32_t analyzer_saved_frequency_index;
    uint32_t analyzer_saved_frequency_hz;
    uint32_t analyzer_peak_frequency;
    uint8_t analyzer_saved_preset_index;
    float analyzer_peak_rssi;
    ScoutView view;
    ScoutView raw_return_view;
    ScoutView settings_return_view;
    uint8_t settings_selection;
    uint8_t start_menu_selection;
    uint8_t selected_preset;
    ScoutView detections_return_view;
    ScoutDetection detections[MAX_DETECTIONS];
    uint8_t detection_count;
    uint8_t selected_detection;
    uint8_t detection_scroll_offset;
    uint8_t detection_detail_scroll_offset;
    uint8_t detection_save_feedback;
    uint8_t raw_threshold_index;
    uint8_t hopper_index;
    char (*subghz_files)[MAX_SUBGHZ_FILENAME];
    uint8_t subghz_file_count;
    uint8_t selected_subghz_file;
    uint8_t file_detail_scroll_offset;
    uint8_t name_edit_cursor;
    uint8_t name_return_view;
    uint8_t name_detection_index;
    uint8_t file_detail_row_count;
    bool name_edit_is_rename;
    uint16_t region_scroll_offset;
    bool resume_after_file_browser;
    char file_name[MAX_SUBGHZ_FILENAME];
    char name_edit_buffer[MAX_EDITABLE_BASENAME + 1U];
    char name_edit_source[MAX_SUBGHZ_FILENAME];
    char selected_file[256];
    char* file_contents;
    size_t file_contents_length;
    char (*file_detail_lines)[DETAIL_LINE_CHARS + 1U];
    bool file_contents_truncated;
    char file_protocol[32];
    char file_preset[40];
    uint32_t file_frequency;
    uint32_t file_bit_count;
    uint64_t file_key;
    bool file_has_bit_count;
    bool file_has_key;
    bool file_is_raw;
    char status[32];
} ScoutApp;

static void set_status(ScoutApp* app, const char* status) {
    if(furi_mutex_acquire(app->mutex, FuriWaitForever) == FuriStatusOk) {
        snprintf(app->status, sizeof(app->status), "%s", status);
        furi_mutex_release(app->mutex);
    } else {
        FURI_LOG_E("SubGhzScout", "Could not update status");
    }
}

static size_t scout_preset_count(const ScoutApp* app) {
    return app->radio_setting ? subghz_setting_get_preset_count(app->radio_setting) : 0U;
}

static const char* scout_preset_name(const ScoutApp* app, size_t index) {
    if(index >= scout_preset_count(app)) return "Unavailable";
    return subghz_setting_get_preset_name(app->radio_setting, index);
}

static uint8_t* scout_preset_data(const ScoutApp* app, size_t index) {
    if(index >= scout_preset_count(app)) return NULL;
    return subghz_setting_get_preset_data(app->radio_setting, index);
}

static size_t scout_frequency_count(const ScoutApp* app) {
    return app->radio_setting ? subghz_setting_get_frequency_count(app->radio_setting) : 0U;
}

static uint32_t scout_frequency_at(const ScoutApp* app, size_t index) {
    if(index >= scout_frequency_count(app)) return 0U;
    return subghz_setting_get_frequency(app->radio_setting, index);
}

static bool ensure_directory(Storage* storage, const char* path);
static uint8_t build_file_detail_rows(ScoutApp* app);

static bool handle_selected_file(ScoutApp* app, const char* selected_file);
static bool ensure_receiver(ScoutApp* app);
static bool start_receiver(ScoutApp* app);
static void stop_receiver(ScoutApp* app);
static void stop_lab_test_transmission(ScoutApp* app);

static bool record_detection(
    ScoutApp* app,
    const char* protocol_name,
    const char* detail,
    uint32_t fingerprint,
    uint8_t* serialized_data,
    size_t serialized_size) {
    if(furi_mutex_acquire(app->mutex, FuriWaitForever) != FuriStatusOk) {
        FURI_LOG_E("SubGhzScout", "Could not update detection history");
        free(serialized_data);
        return false;
    }

    int32_t found = -1;
    for(uint8_t i = 0; i < app->detection_count; i++) {
        ScoutDetection* detection = &app->detections[i];
        if(detection->fingerprint == fingerprint &&
           detection->frequency == app->frequency_hz &&
           detection->preset_index == app->preset_index &&
           strcmp(detection->protocol, protocol_name) == 0) {
            found = i;
            break;
        }
    }
    if(found < 0) {
        uint8_t slot;
        if(app->detection_count < MAX_DETECTIONS) {
            slot = app->detection_count++;
        } else {
            slot = 0;
            for(uint8_t i = 1; i < app->detection_count; i++) {
                if((int32_t)(app->detections[i].last_seen_tick -
                             app->detections[slot].last_seen_tick) < 0) {
                    slot = i;
                }
            }
        }
        found = slot;
        ScoutDetection* detection = &app->detections[slot];
        free(detection->serialized_data);
        memset(detection, 0, sizeof(*detection));
        snprintf(detection->protocol, sizeof(detection->protocol), "%s", protocol_name);
        snprintf(detection->detail, sizeof(detection->detail), "%s", detail);
        detection->fingerprint = fingerprint;
        detection->frequency = app->frequency_hz;
        detection->preset_index = app->preset_index;
        detection->serialization_attempted = true;
    }

    ScoutDetection* detection = &app->detections[found];
    detection->serialization_attempted = true;
    if(serialized_data && serialized_size > 0U) {
        free(detection->serialized_data);
        detection->serialized_data = serialized_data;
        detection->serialized_size = serialized_size;
        serialized_data = NULL;
    }
    uint32_t now = furi_get_tick();
    if(detection->sightings == 0U ||
       (now - detection->last_seen_tick) >= furi_ms_to_ticks(500)) {
        if(detection->sightings < UINT16_MAX) detection->sightings++;
    }
    detection->last_seen_tick = now;
    snprintf(app->status, sizeof(app->status), "Protocol detected");
    furi_mutex_release(app->mutex);
    free(serialized_data);
    return true;
}

static bool detection_needs_serialization(
    ScoutApp* app,
    const char* protocol_name,
    uint32_t fingerprint) {
    if(furi_mutex_acquire(app->mutex, FuriWaitForever) != FuriStatusOk) {
        FURI_LOG_E("SubGhzScout", "Could not check decoded signal state");
        return false;
    }

    bool needed = true;
    for(uint8_t i = 0U; i < app->detection_count; i++) {
        ScoutDetection* detection = &app->detections[i];
        if(detection->fingerprint == fingerprint &&
           detection->frequency == app->frequency_hz &&
           detection->preset_index == app->preset_index &&
           strcmp(detection->protocol, protocol_name) == 0) {
            needed = !detection->serialization_attempted;
            if(needed) detection->serialization_attempted = true;
            break;
        }
    }
    furi_mutex_release(app->mutex);
    return needed;
}

static uint8_t* serialize_detection(
    ScoutApp* app,
    SubGhzProtocolDecoderBase* decoder,
    size_t* serialized_size) {
    *serialized_size = 0U;
    FlipperFormat* format = flipper_format_string_alloc();
    FuriString* preset_name = furi_string_alloc();
    if(!format || !preset_name) {
        if(format) flipper_format_free(format);
        if(preset_name) furi_string_free(preset_name);
        FURI_LOG_E("SubGhzScout", "Could not allocate decoded signal serializer");
        return NULL;
    }

    furi_string_set(preset_name, scout_preset_name(app, app->preset_index));
    SubGhzRadioPreset preset = {
        .name = preset_name,
        .frequency = app->frequency_hz,
        .data = NULL,
        .data_size = 0U,
    };
    uint32_t frequency = app->frequency_hz;
    bool serialized =
        flipper_format_write_header_cstr(
            format, SUBGHZ_KEY_FILE_TYPE, SUBGHZ_KEY_FILE_VERSION) &&
        flipper_format_write_uint32(format, "Frequency", &frequency, 1U) &&
        flipper_format_write_string(format, "Preset", preset_name) &&
        subghz_protocol_decoder_base_serialize(decoder, format, &preset) ==
            SubGhzProtocolStatusOk;

    uint8_t* data = NULL;
    Stream* stream = flipper_format_get_raw_stream(format);
    size_t size = serialized && stream ? stream_size(stream) : 0U;
    if(size > 0U) {
        data = malloc(size);
        if(!data) {
            FURI_LOG_E("SubGhzScout", "Could not allocate saved signal data");
        } else if(!stream_rewind(stream) || stream_read(stream, data, size) != size) {
            FURI_LOG_E("SubGhzScout", "Could not read serialized signal data");
            free(data);
            data = NULL;
        } else {
            *serialized_size = size;
        }
    } else {
        FURI_LOG_W("SubGhzScout", "Decoded signal could not be serialized");
    }

    furi_string_free(preset_name);
    flipper_format_free(format);
    return data;
}

static bool is_valid_subghz_basename(const char* name) {
    size_t length = strlen(name);
    if(length == 0U || length > MAX_EDITABLE_BASENAME) return false;
    for(size_t i = 0U; i < length; i++) {
        char ch = name[i];
        if(!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
             (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

static bool save_detection(ScoutApp* app, uint8_t index, const char* base_name) {
    if(!app->storage_ready || index >= app->detection_count) {
        set_status(app, "SD unavailable");
        return false;
    }
    if(!is_valid_subghz_basename(base_name)) {
        set_status(app, "Invalid file name");
        return false;
    }
    if(!ensure_directory(app->storage, EXT_PATH("subghz"))) {
        set_status(app, "Sub-GHz folder error");
        return false;
    }

    uint8_t* data = NULL;
    size_t data_size = 0U;
    if(furi_mutex_acquire(app->mutex, FuriWaitForever) != FuriStatusOk) {
        FURI_LOG_E("SubGhzScout", "Could not access selected signal");
        set_status(app, "Save failed");
        return false;
    }
    const ScoutDetection* detection = &app->detections[index];
    if(detection->serialized_data && detection->serialized_size > 0U) {
        data_size = detection->serialized_size;
        data = malloc(data_size);
        if(data) {
            memcpy(data, detection->serialized_data, data_size);
        }
    }
    furi_mutex_release(app->mutex);
    if(!data) {
        FURI_LOG_W("SubGhzScout", "Selected detection has no saveable decoder data");
        set_status(app, "No saveable decode");
        return false;
    }

    FlipperFormat* format = flipper_format_file_alloc(app->storage);
    if(!format) {
        FURI_LOG_E("SubGhzScout", "Could not allocate signal file writer");
        free(data);
        set_status(app, "Save failed");
        return false;
    }

    bool opened = false;
    bool written = false;
    bool name_conflict = false;
    char filename[MAX_SUBGHZ_FILENAME];
    char path[sizeof("/ext/subghz/") + MAX_SUBGHZ_FILENAME];
    snprintf(filename, sizeof(filename), "%s.sub", base_name);
    int path_length = snprintf(path, sizeof(path), "%s%s", EXT_PATH("subghz/"), filename);
    if(path_length < 0 || (size_t)path_length >= sizeof(path)) {
        FURI_LOG_E("SubGhzScout", "Saved signal path too long");
    } else if(storage_file_exists(app->storage, path)) {
        name_conflict = true;
    } else if(flipper_format_file_open_new(format, path)) {
        opened = true;
    }
    if(opened) {
        Stream* stream = flipper_format_get_raw_stream(format);
        written = stream && stream_write(stream, data, data_size) == data_size;
        if(!flipper_format_file_close(format)) {
            FURI_LOG_E("SubGhzScout", "Could not close saved signal file");
            written = false;
        }
        if(!written && !storage_simply_remove(app->storage, path)) {
            FURI_LOG_E("SubGhzScout", "Could not remove incomplete saved signal file");
        }
    } else {
        FURI_LOG_E("SubGhzScout", "Could not create saved signal file");
    }
    flipper_format_free(format);
    free(data);

    if(written) {
        set_status(app, "Signal saved");
    } else if(name_conflict) {
        set_status(app, "Name already used");
    } else {
        set_status(app, "Save failed");
    }
    return written;
}

static bool ensure_directory(Storage* storage, const char* path) {
    if(storage_dir_exists(storage, path)) return true;
    FS_Error error = storage_common_mkdir(storage, path);
    if(error != FSE_OK && error != FSE_EXIST) {
        FURI_LOG_E("SubGhzScout", "Could not create directory: %d", error);
        return false;
    }
    return true;
}

static bool has_sub_extension(const char* name);
static void load_subghz_file_list(ScoutApp* app);

static void begin_name_edit(
    ScoutApp* app,
    bool rename_existing_file,
    uint8_t detection_index) {
    app->name_edit_is_rename = rename_existing_file;
    app->name_detection_index = detection_index;
    app->name_edit_cursor = 0U;
    if(!rename_existing_file) app->detection_save_feedback = 0U;
    if(rename_existing_file) {
        snprintf(app->name_edit_source, sizeof(app->name_edit_source), "%s", app->file_name);
        size_t length = strlen(app->file_name);
        if(length > 4U) length -= 4U;
        if(length > MAX_EDITABLE_BASENAME) length = MAX_EDITABLE_BASENAME;
        memcpy(app->name_edit_buffer, app->file_name, length);
        app->name_edit_buffer[length] = '\0';
        app->name_return_view = ScoutViewFileDetails;
    } else {
        snprintf(app->name_edit_buffer, sizeof(app->name_edit_buffer), "Signal");
        app->name_edit_source[0] = '\0';
        app->name_return_view = ScoutViewDetectionDetails;
    }
    app->view = ScoutViewNameFile;
}

static bool rename_subghz_file(ScoutApp* app, const char* base_name) {
    if(!app->storage_ready || !is_valid_subghz_basename(base_name) ||
       !has_sub_extension(app->name_edit_source)) {
        set_status(app, "Invalid file name");
        return false;
    }
    char target_name[MAX_SUBGHZ_FILENAME];
    char source_path[sizeof("/ext/subghz/") + MAX_SUBGHZ_FILENAME];
    char target_path[sizeof("/ext/subghz/") + MAX_SUBGHZ_FILENAME];
    snprintf(target_name, sizeof(target_name), "%s.sub", base_name);
    int source_length = snprintf(
        source_path,
        sizeof(source_path),
        "%s%s",
        EXT_PATH("subghz/"),
        app->name_edit_source);
    int target_length = snprintf(
        target_path,
        sizeof(target_path),
        "%s%s",
        EXT_PATH("subghz/"),
        target_name);
    if(source_length < 0 || (size_t)source_length >= sizeof(source_path) ||
       target_length < 0 || (size_t)target_length >= sizeof(target_path)) {
        FURI_LOG_E("SubGhzScout", "Rename path too long");
        set_status(app, "File path too long");
        return false;
    }
    if(strcmp(app->name_edit_source, target_name) != 0 &&
       storage_file_exists(app->storage, target_path)) {
        set_status(app, "Name already used");
        return false;
    }
    if(storage_common_rename(app->storage, source_path, target_path) != FSE_OK) {
        FURI_LOG_E("SubGhzScout", "Could not rename Sub-GHz file");
        set_status(app, "Rename failed");
        return false;
    }

    snprintf(app->file_name, sizeof(app->file_name), "%s", target_name);
    load_subghz_file_list(app);
    for(uint8_t i = 0U; i < app->subghz_file_count; i++) {
        if(strcmp(app->subghz_files[i], target_name) == 0) {
            app->selected_subghz_file = i;
            break;
        }
    }
    set_status(app, "File renamed");
    return true;
}

static void cycle_edit_character(ScoutApp* app, bool increase) {
    static const char characters[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
    size_t character_count = sizeof(characters) - 1U;
    size_t length = strlen(app->name_edit_buffer);
    if(app->name_edit_cursor >= MAX_EDITABLE_BASENAME) return;
    if(app->name_edit_cursor == length) {
        app->name_edit_buffer[length] = characters[0];
        app->name_edit_buffer[length + 1U] = '\0';
    }

    char* current = &app->name_edit_buffer[app->name_edit_cursor];
    size_t index = 0U;
    while(index < character_count && characters[index] != *current) index++;
    if(index == character_count) index = 0U;
    index = increase ? (index + 1U) % character_count :
                       (index == 0U ? character_count - 1U : index - 1U);
    *current = characters[index];
}

static bool remove_edit_character_before_cursor(ScoutApp* app) {
    size_t length = strlen(app->name_edit_buffer);
    if(app->name_edit_cursor == 0U || length == 0U) return false;
    uint8_t remove_at = app->name_edit_cursor > length ?
                            (uint8_t)(length - 1U) :
                            (uint8_t)(app->name_edit_cursor - 1U);
    memmove(
        &app->name_edit_buffer[remove_at],
        &app->name_edit_buffer[remove_at + 1U],
        length - remove_at);
    app->name_edit_cursor = remove_at;
    return true;
}

static bool has_sub_extension(const char* name) {
    size_t length = strlen(name);
    return length > 4U && strcmp(name + length - 4U, ".sub") == 0;
}

static void load_subghz_file_list(ScoutApp* app) {
    free(app->subghz_files);
    app->subghz_files = NULL;
    app->subghz_file_count = 0;
    app->selected_subghz_file = 0;
    if(!app->storage_ready) {
        set_status(app, "SD unavailable");
        return;
    }

    File* directory = storage_file_alloc(app->storage);
    if(!directory) {
        FURI_LOG_E("SubGhzScout", "Could not allocate Sub-GHz directory handle");
        set_status(app, "Folder open failed");
        return;
    }
    bool opened = storage_dir_open(directory, EXT_PATH("subghz"));
    if(!opened) {
        storage_file_free(directory);
        set_status(app, "Sub-GHz folder missing");
        return;
    }

    app->subghz_files = malloc(MAX_SUBGHZ_FILES * sizeof(*app->subghz_files));
    if(!app->subghz_files) {
        FURI_LOG_E("SubGhzScout", "Could not allocate Sub-GHz file list");
        storage_dir_close(directory);
        storage_file_free(directory);
        set_status(app, "File list memory error");
        return;
    }

    bool overflow = false;
    FileInfo info;
    char name[MAX_SUBGHZ_FILENAME];
    while(storage_dir_read(directory, &info, name, sizeof(name))) {
        if(!file_info_is_dir(&info) && has_sub_extension(name)) {
            if(app->subghz_file_count < MAX_SUBGHZ_FILES) {
                snprintf(
                    app->subghz_files[app->subghz_file_count],
                    sizeof(app->subghz_files[app->subghz_file_count]),
                    "%s",
                    name);
                app->subghz_file_count++;
            } else {
                overflow = true;
            }
        }
    }
    if(!storage_dir_close(directory)) {
        FURI_LOG_E("SubGhzScout", "Could not close Sub-GHz directory");
    }
    storage_file_free(directory);
    set_status(
        app,
        overflow ? "Showing first 128 .sub files" :
                   (app->subghz_file_count ? "OK opens selected file" : "No .sub files found"));
}

static bool read_subghz_file_details(ScoutApp* app, const char* filename) {
    if(!app->storage_ready || !filename || !has_sub_extension(filename)) {
        set_status(app, "Invalid Sub-GHz file");
        return false;
    }
    free(app->file_contents);
    app->file_contents = NULL;
    app->file_contents_length = 0U;
    free(app->file_detail_lines);
    app->file_detail_lines = NULL;
    app->file_detail_row_count = 0U;

    char path[sizeof("/ext/subghz/") + MAX_SUBGHZ_FILENAME];
    int path_length = snprintf(path, sizeof(path), "%s%s", EXT_PATH("subghz/"), filename);
    if(path_length < 0 || (size_t)path_length >= sizeof(path)) {
        FURI_LOG_E("SubGhzScout", "Sub-GHz file path too long");
        set_status(app, "File path too long");
        return false;
    }

    FlipperFormat* format = flipper_format_file_alloc(app->storage);
    FuriString* filetype = furi_string_alloc();
    FuriString* protocol = furi_string_alloc();
    FuriString* preset = furi_string_alloc();
    if(!format || !filetype || !protocol || !preset) {
        if(format) flipper_format_free(format);
        if(filetype) furi_string_free(filetype);
        if(protocol) furi_string_free(protocol);
        if(preset) furi_string_free(preset);
        FURI_LOG_E("SubGhzScout", "Could not allocate Sub-GHz file parser");
        set_status(app, "File parser memory error");
        return false;
    }

    bool opened = flipper_format_file_open_existing(format, path);
    uint32_t version = 0;
    uint32_t frequency = 0;
    uint32_t bit_count = 0;
    uint64_t key = 0U;
    bool header_ok = opened && flipper_format_read_header(format, filetype, &version);
    bool protocol_ok = header_ok && flipper_format_rewind(format) &&
                       flipper_format_read_string(format, "Protocol", protocol);
    bool frequency_ok = header_ok && flipper_format_rewind(format) &&
                        flipper_format_read_uint32(format, "Frequency", &frequency, 1);
    bool preset_ok = header_ok && flipper_format_rewind(format) &&
                     flipper_format_read_string(format, "Preset", preset);
    bool bit_count_ok = header_ok && flipper_format_rewind(format) &&
                        flipper_format_read_uint32(format, "Bit", &bit_count, 1);
    bool key_ok = header_ok && flipper_format_rewind(format) &&
                  flipper_format_read_hex_uint64(format, "Key", &key, 1U);
    bool is_raw = protocol_ok &&
                  strcmp(furi_string_get_cstr(filetype), SUBGHZ_RAW_FILE_TYPE) == 0 &&
                  strcmp(furi_string_get_cstr(protocol), "RAW") == 0;
    bool valid = header_ok && protocol_ok && frequency_ok &&
                 (is_raw || strcmp(furi_string_get_cstr(filetype), SUBGHZ_KEY_FILE_TYPE) == 0);

    if(valid) {
        app->file_contents = malloc(FILE_CONTENT_CAPACITY);
        app->file_detail_lines =
            malloc(FILE_DETAIL_MAX_LINES * sizeof(*app->file_detail_lines));
        if(!app->file_contents || !app->file_detail_lines) {
            FURI_LOG_E("SubGhzScout", "Could not allocate Sub-GHz file detail buffer");
            free(app->file_contents);
            app->file_contents = NULL;
            free(app->file_detail_lines);
            app->file_detail_lines = NULL;
            valid = false;
        }
    }

    if(valid) {
        app->file_contents_truncated = false;
        Stream* stream = flipper_format_get_raw_stream(format);
        size_t stream_length = stream ? stream_size(stream) : 0U;
        size_t bytes_to_read = stream_length;
        if(bytes_to_read >= FILE_CONTENT_CAPACITY) {
            bytes_to_read = FILE_CONTENT_CAPACITY - 1U;
            app->file_contents_truncated = true;
        }
        if(!stream || !stream_rewind(stream) ||
           stream_read(stream, (uint8_t*)app->file_contents, bytes_to_read) != bytes_to_read) {
            FURI_LOG_E("SubGhzScout", "Could not read Sub-GHz file contents");
            valid = false;
        } else {
            app->file_contents[bytes_to_read] = '\0';
            app->file_contents_length = bytes_to_read;
            size_t write_index = 0U;
            for(size_t read_index = 0U; read_index < app->file_contents_length; read_index++) {
                if(app->file_contents[read_index] != '\r') {
                    app->file_contents[write_index++] = app->file_contents[read_index];
                }
            }
            app->file_contents[write_index] = '\0';
            app->file_contents_length = write_index;
        }
    }

    if(valid) {
        snprintf(app->file_name, sizeof(app->file_name), "%s", filename);
        snprintf(
            app->file_protocol,
            sizeof(app->file_protocol),
            "%s",
            furi_string_get_cstr(protocol));
        snprintf(
            app->file_preset,
            sizeof(app->file_preset),
            "%s",
            preset_ok ? furi_string_get_cstr(preset) : "Not listed");
        app->file_frequency = frequency;
        app->file_bit_count = bit_count;
        app->file_key = key;
        app->file_has_bit_count = bit_count_ok;
        app->file_has_key = key_ok;
        app->file_is_raw = is_raw;
        app->file_detail_row_count = build_file_detail_rows(app);
        set_status(app, is_raw ? "RAW capture details" : "Decoded key details");
    }

    if(opened && !flipper_format_file_close(format)) {
        FURI_LOG_E("SubGhzScout", "Could not close Sub-GHz file");
        valid = false;
    }
    flipper_format_free(format);
    furi_string_free(filetype);
    furi_string_free(protocol);
    furi_string_free(preset);
    if(!valid) {
        free(app->file_contents);
        app->file_contents = NULL;
        app->file_contents_length = 0U;
        free(app->file_detail_lines);
        app->file_detail_lines = NULL;
        app->file_detail_row_count = 0U;
        set_status(app, header_ok ? "Unsupported .sub file" : "Invalid .sub file");
    }
    return valid;
}

static bool handle_selected_file(ScoutApp* app, const char* selected_file) {
    if(!selected_file || !has_sub_extension(selected_file)) {
        set_status(app, "Invalid Sub-GHz file");
        return false;
    }

    bool listed = false;
    for(uint8_t i = 0U; i < app->subghz_file_count; i++) {
        if(strcmp(app->subghz_files[i], selected_file) == 0) {
            app->selected_subghz_file = i;
            listed = true;
            break;
        }
    }
    if(!listed) {
        set_status(app, "File not in Saved list");
        return false;
    }

    snprintf(app->selected_file, sizeof(app->selected_file), "%s", selected_file);
    if(!read_subghz_file_details(app, selected_file)) return false;
    app->file_detail_scroll_offset = 0U;
    app->view = ScoutViewFileDetails;
    return true;
}

static bool delete_selected_file(ScoutApp* app) {
    if(!app->storage_ready || !has_sub_extension(app->file_name)) {
        set_status(app, "Invalid Sub-GHz file");
        return false;
    }

    char path[sizeof("/ext/subghz/") + MAX_SUBGHZ_FILENAME];
    int path_length = snprintf(path, sizeof(path), "%s%s", EXT_PATH("subghz/"), app->file_name);
    if(path_length < 0 || (size_t)path_length >= sizeof(path)) {
        FURI_LOG_E("SubGhzScout", "Sub-GHz file path too long");
        set_status(app, "File path too long");
        return false;
    }
    if(!storage_file_exists(app->storage, path) ||
       !storage_simply_remove(app->storage, path)) {
        FURI_LOG_E("SubGhzScout", "Could not delete Sub-GHz file");
        set_status(app, "Delete failed");
        return false;
    }

    free(app->file_contents);
    app->file_contents = NULL;
    app->file_contents_length = 0U;
    free(app->file_detail_lines);
    app->file_detail_lines = NULL;
    app->file_detail_row_count = 0U;
    load_subghz_file_list(app);
    app->view = ScoutViewSubFiles;
    set_status(app, "File deleted");
    return true;
}

static bool start_lab_test_transmission(ScoutApp* app) {
    app->lab_test_resume_receiver = app->receiving;
    if(!ensure_receiver(app)) return false;

    int preset_index = subghz_setting_get_inx_preset_by_name(app->radio_setting, "AM650");
    if(preset_index < 0 || (size_t)preset_index >= scout_preset_count(app)) {
        set_status(app, "AM650 preset unavailable");
        if(!app->lab_test_resume_receiver) stop_receiver(app);
        return false;
    }

    if(!furi_hal_region_is_frequency_allowed(app->frequency_hz)) {
        set_status(app, "Outside region TX band");
        if(!app->lab_test_resume_receiver) stop_receiver(app);
        return false;
    }

    uint8_t* preset_data = scout_preset_data(app, (size_t)preset_index);
    if(!preset_data || !subghz_devices_is_frequency_valid(app->device, app->frequency_hz)) {
        set_status(app, "TX frequency unavailable");
        if(!app->lab_test_resume_receiver) stop_receiver(app);
        return false;
    }

    FlipperFormat* format = flipper_format_string_alloc();
    if(!format) {
        FURI_LOG_E("SubGhzScout", "Could not allocate lab-test signal");
        set_status(app, "Test signal memory error");
        if(!app->lab_test_resume_receiver) stop_receiver(app);
        return false;
    }

    app->lab_test_code = furi_hal_random_get() & 0x00FFFFFFU;
    if(app->lab_test_code == 0U || app->lab_test_code == 0x00FFFFFFU) {
        app->lab_test_code ^= 0x005A39C7U;
    }
    uint8_t key[sizeof(uint64_t)] = {0};
    key[5] = (uint8_t)(app->lab_test_code >> 16);
    key[6] = (uint8_t)(app->lab_test_code >> 8);
    key[7] = (uint8_t)app->lab_test_code;
    uint32_t bit_count = 24U;
    uint32_t te = 390U;
    uint32_t repeat = 1U;
    bool format_ready =
        flipper_format_write_header_cstr(
            format, SUBGHZ_KEY_FILE_TYPE, SUBGHZ_KEY_FILE_VERSION) &&
        flipper_format_write_uint32(format, "Frequency", &app->frequency_hz, 1U) &&
        flipper_format_write_string_cstr(format, "Preset", "AM650") &&
        flipper_format_write_string_cstr(format, "Protocol", "Princeton") &&
        flipper_format_write_uint32(format, "Bit", &bit_count, 1U) &&
        flipper_format_write_hex(format, "Key", key, sizeof(key)) &&
        flipper_format_write_uint32(format, "TE", &te, 1U) &&
        flipper_format_write_uint32(format, "Repeat", &repeat, 1U);
    if(!format_ready) {
        FURI_LOG_E("SubGhzScout", "Could not prepare lab-test signal");
        flipper_format_free(format);
        set_status(app, "Test signal setup failed");
        if(!app->lab_test_resume_receiver) stop_receiver(app);
        return false;
    }

    app->lab_test_transmitter =
        subghz_transmitter_alloc_init(app->protocol_environment, "Princeton");
    if(!app->lab_test_transmitter ||
       subghz_transmitter_deserialize(app->lab_test_transmitter, format) !=
           SubGhzProtocolStatusOk) {
        FURI_LOG_E("SubGhzScout", "Could not encode lab-test signal");
        if(app->lab_test_transmitter) {
            subghz_transmitter_free(app->lab_test_transmitter);
            app->lab_test_transmitter = NULL;
        }
        flipper_format_free(format);
        set_status(app, "Test signal encode failed");
        if(!app->lab_test_resume_receiver) stop_receiver(app);
        return false;
    }
    flipper_format_free(format);

    stop_receiver(app);
    subghz_devices_reset(app->device);
    subghz_devices_idle(app->device);
    subghz_devices_load_preset(app->device, FuriHalSubGhzPresetCustom, preset_data);
    subghz_devices_set_frequency(app->device, app->frequency_hz);
    if(!subghz_devices_set_tx(app->device) ||
       !subghz_devices_start_async_tx(
           app->device, subghz_transmitter_yield, app->lab_test_transmitter)) {
        FURI_LOG_E("SubGhzScout", "Could not start lab-test transmission");
        subghz_devices_idle(app->device);
        subghz_transmitter_free(app->lab_test_transmitter);
        app->lab_test_transmitter = NULL;
        if(app->lab_test_resume_receiver) start_receiver(app);
        set_status(app, "Test transmission failed");
        return false;
    }

    app->lab_test_active = true;
    app->lab_test_started_tick = furi_get_tick();
    set_status(app, "Test signal transmitting");
    return true;
}

static void stop_lab_test_transmission(ScoutApp* app) {
    if(!app->lab_test_active) return;

    subghz_devices_stop_async_tx(app->device);
    subghz_transmitter_stop(app->lab_test_transmitter);
    subghz_devices_idle(app->device);
    subghz_transmitter_free(app->lab_test_transmitter);
    app->lab_test_transmitter = NULL;
    app->lab_test_active = false;
    if(app->radio_ready && app->lab_test_resume_receiver) start_receiver(app);
    app->lab_test_resume_receiver = false;
    set_status(app, "Test transmission stopped");
}

static bool start_saved_file_transmission(ScoutApp* app, const char* filename) {
    if(!filename || !has_sub_extension(filename)) {
        set_status(app, "Invalid Sub-GHz file");
        return false;
    }

    // Load the file details if they are not already for this selection.
    if(strcmp(app->selected_file, filename) != 0) {
        if(!handle_selected_file(app, filename)) return false;
    }

    if(!app->file_protocol[0] || !app->file_preset[0] ||
       app->file_frequency == 0U) {
        set_status(app, "Missing signal details");
        return false;
    }

    int preset_index =
        subghz_setting_get_inx_preset_by_name(app->radio_setting, app->file_preset);
    if(preset_index < 0 || (size_t)preset_index >= scout_preset_count(app)) {
        set_status(app, "File preset unavailable");
        return false;
    }

    if(!furi_hal_region_is_frequency_allowed(app->file_frequency) ||
       !subghz_devices_is_frequency_valid(app->device, app->file_frequency)) {
        set_status(app, "File frequency unavailable");
        return false;
    }

    char path[sizeof("/ext/subghz/") + MAX_SUBGHZ_FILENAME];
    int path_length = snprintf(
        path, sizeof(path), "%s%s", EXT_PATH("subghz/"), filename);
    if(path_length < 0 || (size_t)path_length >= sizeof(path)) {
        set_status(app, "File path too long");
        return false;
    }

    FlipperFormat* format = flipper_format_file_alloc(app->storage);
    if(!format || !flipper_format_file_open_existing(format, path)) {
        if(format) flipper_format_free(format);
        set_status(app, "Could not open file");
        return false;
    }

    SubGhzTransmitter* transmitter =
        subghz_transmitter_alloc_init(app->protocol_environment, app->file_protocol);
    if(!transmitter ||
       subghz_transmitter_deserialize(transmitter, format) !=
           SubGhzProtocolStatusOk) {
        if(transmitter) subghz_transmitter_free(transmitter);
        flipper_format_free(format);
        set_status(app, "Could not load signal");
        return false;
    }
    flipper_format_free(format);

    bool resume_receiver = app->receiving;
    stop_receiver(app);
    subghz_devices_reset(app->device);
    subghz_devices_idle(app->device);
    subghz_devices_load_preset(
        app->device, FuriHalSubGhzPresetCustom,
        scout_preset_data(app, (size_t)preset_index));
    subghz_devices_set_frequency(app->device, app->file_frequency);

    if(!subghz_devices_set_tx(app->device) ||
       !subghz_devices_start_async_tx(
           app->device, subghz_transmitter_yield, transmitter)) {
        subghz_devices_idle(app->device);
        subghz_transmitter_free(transmitter);
        if(resume_receiver) start_receiver(app);
        set_status(app, "Transmission failed");
        return false;
    }

    app->lab_test_transmitter = transmitter;
    app->lab_test_active = true;
    app->lab_test_resume_receiver = resume_receiver;
    set_status(app, "Selected file transmitting");
    return true;
}

static bool transmission_handling_logic(ScoutApp* app, const char* selected_file) {
    if(!selected_file || !has_sub_extension(selected_file)) {
        set_status(app, "Invalid Sub-GHz file");
      return false;
    }

  for(uint8_t i = 0U; i < app->subghz_file_count; i++) {
      if(strcmp(app->subghz_files[i], selected_file) == 0) return true;
  }

  set_status(app, "File not in Saved list");
  return false;
}

void handle_transmission(ScoutApp* app, const char* selected_file) {
    if(!transmission_handling_logic(app, selected_file)) return;
    start_saved_file_transmission(app, selected_file);
}

void on_file_picked(ScoutApp* app, const char* selected_file, bool picked) {
    if (picked) {
        handle_transmission(app, selected_file);
        FURI_LOG_I("Scout", "Selected: %s", selected_file);
    }
}


static void protocol_decoded_callback(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder,
    void* context) {
    UNUSED(receiver);
    ScoutApp* app = context;
    if(!decoder->protocol || !decoder->protocol->name) return;
    if(app->raw_recording) return;

    char detail[MAX_DETECTION_DETAIL] = {0};
    uint32_t fingerprint = 2166136261U;
    FuriString* decoder_text = furi_string_alloc();
    bool has_text = decoder_text &&
                    subghz_protocol_decoder_base_get_string(decoder, decoder_text);
    if(has_text) {
        const char* text = furi_string_get_cstr(decoder_text);
        size_t out = 0;
        bool previous_space = false;
        for(size_t i = 0; text[i] != '\0'; i++) {
            uint8_t value = (uint8_t)text[i];
            fingerprint = (fingerprint ^ value) * 16777619U;
            if(value < 32U || value == 127U) value = ' ';
            if(value == ' ' && previous_space) continue;
            if(out + 1U < sizeof(detail)) detail[out++] = (char)value;
            previous_space = value == ' ';
        }
        detail[out] = '\0';
    }
    if(!has_text || detail[0] == '\0') {
        uint8_t hash = subghz_protocol_decoder_base_get_hash_data(decoder);
        fingerprint = (fingerprint ^ hash) * 16777619U;
        snprintf(detail, sizeof(detail), "Decoder hash %02X", hash);
    }

    size_t serialized_size = 0U;
    uint8_t* serialized_data = detection_needs_serialization(
                                   app, decoder->protocol->name, fingerprint) ?
                                   serialize_detection(app, decoder, &serialized_size) :
                                   NULL;
    record_detection(
        app,
        decoder->protocol->name,
        detail,
        fingerprint,
        serialized_data,
        serialized_size);
    if(decoder_text) furi_string_free(decoder_text);
    subghz_receiver_reset(app->protocol_receiver);
}

static void protocol_worker_pair_callback(void* context, bool level, uint32_t duration) {
    ScoutApp* app = context;
    if(app->raw_capture_complete) return;
    if(app->raw_recording) {
        int32_t sample = duration > INT32_MAX ? INT32_MAX : (int32_t)duration;
        if(!level) sample = -sample;

        float trigger_dbm = app->raw_trigger_dbm;
        bool above_threshold = app->rssi >=
                               (app->raw_gate_active ? trigger_dbm - 2.0f : trigger_dbm);
        if(!app->raw_gate_active && !above_threshold) {
            app->raw_pretrigger[app->raw_pretrigger_head] = sample;
            app->raw_pretrigger_head =
                (app->raw_pretrigger_head + 1U) % RAW_SIGNAL_PRETRIGGER_SAMPLES;
            if(app->raw_pretrigger_count < RAW_SIGNAL_PRETRIGGER_SAMPLES) {
                app->raw_pretrigger_count++;
            }
            return;
        }

        if(above_threshold) {
            if(!app->raw_gate_active) {
                app->raw_gate_active = true;
                uint8_t first =
                    (app->raw_pretrigger_head + RAW_SIGNAL_PRETRIGGER_SAMPLES -
                     app->raw_pretrigger_count) %
                    RAW_SIGNAL_PRETRIGGER_SAMPLES;
                for(uint8_t i = 0; i < app->raw_pretrigger_count; i++) {
                    int32_t buffered = app->raw_pretrigger[
                        (first + i) % RAW_SIGNAL_PRETRIGGER_SAMPLES];
                    uint32_t buffered_duration =
                        buffered < 0 ? (uint32_t)(-(int64_t)buffered) : (uint32_t)buffered;
                    subghz_protocol_decoder_raw_feed(
                        app->raw_decoder, buffered >= 0, buffered_duration);
                }
                app->raw_pretrigger_count = 0;
            } else {
                for(uint16_t i = 0; i < app->raw_tail_count; i++) {
                    int32_t buffered = app->raw_tail_samples[i];
                    uint32_t buffered_duration =
                        buffered < 0 ? (uint32_t)(-(int64_t)buffered) : (uint32_t)buffered;
                    subghz_protocol_decoder_raw_feed(
                        app->raw_decoder, buffered >= 0, buffered_duration);
                }
                app->raw_tail_count = 0;
            }
            app->raw_below_threshold_us = 0;
            subghz_protocol_decoder_raw_feed(app->raw_decoder, level, duration);
        } else if(app->raw_gate_active) {
            app->raw_below_threshold_us += duration;
            if(app->raw_below_threshold_us >= RAW_SIGNAL_TAIL_HOLD_US ||
               app->raw_tail_count >= RAW_SIGNAL_TAIL_SAMPLES) {
                app->raw_recording = false;
                app->raw_capture_complete = true;
                return;
            }
            app->raw_tail_samples[app->raw_tail_count++] = sample;
        } else {
            app->raw_pretrigger[app->raw_pretrigger_head] = sample;
            app->raw_pretrigger_head =
                (app->raw_pretrigger_head + 1U) % RAW_SIGNAL_PRETRIGGER_SAMPLES;
            if(app->raw_pretrigger_count < RAW_SIGNAL_PRETRIGGER_SAMPLES) {
                app->raw_pretrigger_count++;
            }
        }
        return;
    }
    subghz_receiver_decode(app->protocol_receiver, level, duration);
}

static void protocol_worker_overrun_callback(void* context) {
    ScoutApp* app = context;
    subghz_receiver_reset(app->protocol_receiver);
}

static int32_t recent_detection_index(const ScoutApp* app, uint8_t rank) {
    for(uint8_t i = 0; i < app->detection_count; i++) {
        uint8_t newer_count = 0;
        for(uint8_t j = 0; j < app->detection_count; j++) {
            if(i == j) continue;
            int32_t elapsed =
                (int32_t)(app->detections[j].last_seen_tick - app->detections[i].last_seen_tick);
            if(elapsed > 0 || (elapsed == 0 && j > i)) newer_count++;
        }
        if(newer_count == rank) return i;
    }
    return -1;
}

static int32_t recent_detection_rank(const ScoutApp* app, uint8_t index) {
    for(uint8_t rank = 0; rank < app->detection_count; rank++) {
        if(recent_detection_index(app, rank) == index) return rank;
    }
    return -1;
}

static void update_detection_scroll(ScoutApp* app, uint8_t selected_rank) {
    if(selected_rank < app->detection_scroll_offset) {
        app->detection_scroll_offset = selected_rank;
    } else if(selected_rank >= app->detection_scroll_offset + 4U) {
        app->detection_scroll_offset = selected_rank - 3U;
    }
}

static void draw_stock_list_row(
    Canvas* canvas,
    uint8_t row,
    bool selected,
    const char* text) {
    int32_t y = row * 12;
    if(selected) {
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_box(canvas, 0, y, 127, 12);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_set_color(canvas, ColorBlack);
    }
    canvas_draw_str(canvas, 4, y + 9, text);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_stock_rssi(Canvas* canvas, float rssi) {
    if(rssi < STOCK_RSSI_FLOOR_DBM) return;
    uint8_t level = (uint8_t)(rssi - STOCK_RSSI_FLOOR_DBM);
    if(level > 78U) level = 78U;
    for(uint8_t i = 1; i < level; i++) {
        if(i % 5U) {
            canvas_draw_dot(canvas, 46 + i, 52);
            canvas_draw_dot(canvas, 47 + i, 53);
            canvas_draw_dot(canvas, 46 + i, 54);
        }
    }
}

static uint8_t rssi_graph_level(float rssi) {
    if(rssi <= RSSI_BAR_MIN_DBM) return 0U;
    uint8_t level = (uint8_t)((rssi - RSSI_BAR_MIN_DBM) / 2.7f);
    return level > 33U ? 33U : level;
}

static void draw_raw_rssi_graph(Canvas* canvas, const ScoutApp* app) {
    canvas_draw_line(canvas, 0, 14, 115, 14);
    canvas_draw_line(canvas, 0, 48, 115, 48);
    canvas_draw_line(canvas, 115, 14, 115, 48);
    for(uint8_t x = 15U; x < 115U; x += 15U) {
        canvas_draw_line(canvas, x, 14, x, 17);
    }

    uint8_t count = app->raw_rssi_history_count;
    uint8_t first = (app->raw_rssi_history_head + RAW_RSSI_HISTORY_SIZE - count) %
                    RAW_RSSI_HISTORY_SIZE;
    for(uint8_t i = 0; i < count; i++) {
        uint8_t sample =
            app->raw_rssi_history[(first + i) % RAW_RSSI_HISTORY_SIZE];
        int32_t x = (int32_t)i;
        if(x >= 115) continue;
        canvas_draw_line(canvas, x, 47, x, 47 - sample);
    }

    uint8_t threshold = rssi_graph_level(app->raw_trigger_dbm);
    int32_t threshold_y = 47 - threshold;
    for(uint8_t x = 0U; x < 114U; x += 6U) {
        canvas_draw_line(canvas, x, threshold_y, x + 3U, threshold_y);
    }
    canvas_draw_line(canvas, 117, threshold_y - 2, 117, threshold_y + 2);
    canvas_draw_dot(canvas, 116, threshold_y);
}

static uint8_t wrapped_line_count(const char* text) {
    size_t length = strlen(text);
    return (uint8_t)((length + DETAIL_LINE_CHARS - 1U) / DETAIL_LINE_CHARS);
}

static void append_wrapped_rows(
    char rows[][DETAIL_LINE_CHARS + 1U],
    uint8_t* row_count,
    uint8_t max_rows,
    const char* text) {
    size_t length = strlen(text);
    for(size_t offset = 0; offset < length && *row_count < max_rows;
        offset += DETAIL_LINE_CHARS) {
        size_t remaining = length - offset;
        size_t chunk = remaining < DETAIL_LINE_CHARS ? remaining : DETAIL_LINE_CHARS;
        memcpy(rows[*row_count], text + offset, chunk);
        rows[*row_count][chunk] = '\0';
        (*row_count)++;
    }
    if(length == 0U && *row_count < max_rows) {
        rows[*row_count][0] = '\0';
        (*row_count)++;
    }
}

static uint8_t build_file_detail_rows(ScoutApp* app) {
    char (*rows)[DETAIL_LINE_CHARS + 1U] = app->file_detail_lines;
    char field[80];
    uint8_t row_count = 0U;
    snprintf(field, sizeof(field), "File: %s", app->file_name);
    append_wrapped_rows(rows, &row_count, (uint8_t)FILE_DETAIL_MAX_LINES, field);
    snprintf(
        field,
        sizeof(field),
        "Frequency: %lu.%03lu MHz",
        (unsigned long)(app->file_frequency / 1000000U),
        (unsigned long)((app->file_frequency % 1000000U) / 1000U));
    append_wrapped_rows(rows, &row_count, (uint8_t)FILE_DETAIL_MAX_LINES, field);
    snprintf(field, sizeof(field), "Protocol: %s", app->file_protocol);
    append_wrapped_rows(rows, &row_count, (uint8_t)FILE_DETAIL_MAX_LINES, field);
    snprintf(field, sizeof(field), "Preset: %s", app->file_preset);
    append_wrapped_rows(rows, &row_count, (uint8_t)FILE_DETAIL_MAX_LINES, field);
    if(app->file_has_bit_count) {
        snprintf(field, sizeof(field), "Key: %lu bits", (unsigned long)app->file_bit_count);
    } else {
        snprintf(field, sizeof(field), "%s", app->file_is_raw ? "RAW capture" : "Decoded key");
    }
    append_wrapped_rows(rows, &row_count, (uint8_t)FILE_DETAIL_MAX_LINES, field);
    if(app->file_has_key) {
        snprintf(field, sizeof(field), "Key: %016llX", (unsigned long long)app->file_key);
        append_wrapped_rows(rows, &row_count, (uint8_t)FILE_DETAIL_MAX_LINES, field);
    }

    size_t position = 0U;
    while(app->file_contents && app->file_contents[position] != '\0' &&
          row_count < FILE_DETAIL_MAX_LINES) {
        size_t line_end = position;
        while(app->file_contents[line_end] != '\0' &&
              app->file_contents[line_end] != '\n') {
            line_end++;
        }
        while(position < line_end && row_count < FILE_DETAIL_MAX_LINES) {
            size_t chunk = line_end - position;
            if(chunk > DETAIL_LINE_CHARS) chunk = DETAIL_LINE_CHARS;
            memcpy(rows[row_count], &app->file_contents[position], chunk);
            rows[row_count][chunk] = '\0';
            row_count++;
            position += chunk;
        }
        if(app->file_contents[position] == '\n') position++;
    }
    if(app->file_contents_truncated && row_count < FILE_DETAIL_MAX_LINES) {
        snprintf(rows[row_count++], DETAIL_LINE_CHARS + 1U, "[data truncated]");
    }
    return row_count;
}

static void draw_callback(Canvas* canvas, void* context) {
    ScoutApp* app = context;
    char line[40];

    canvas_clear(canvas);
    if(furi_mutex_acquire(app->mutex, FuriWaitForever) != FuriStatusOk) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 35, "Display state unavailable");
        return;
    }

    canvas_set_font(canvas, FontPrimary);
    if(app->view == ScoutViewStartMenu) {
        canvas_draw_str(canvas, 2, 10, "Sub-GHz");
        canvas_set_font(canvas, FontSecondary);
        static const char* const start_items[] = {
            "Read",
            "Read RAW",
            "Saved",
            "Add Manually",
            "Frequency Analyzer",
            "Region Information",
            "Radio Settings",
            "Test Transmit",
        };
        uint8_t first = app->start_menu_selection > 3U ?
                            app->start_menu_selection - 3U :
                            0U;
        for(uint8_t row = 0; row < 4U && first + row < 8U; row++) {
            uint8_t index = first + row;
            draw_stock_list_row(
                canvas, row + 1U, index == app->start_menu_selection, start_items[index]);
        }
    } else if(app->view == ScoutViewRadioModule) {
        canvas_draw_str(canvas, 2, 10, "Radio Settings");
        canvas_set_font(canvas, FontSecondary);
        char module_text[32];
        snprintf(
            module_text,
            sizeof(module_text),
            "Module: %s",
            app->external_module_selected ? "External" : "Internal");
        draw_stock_list_row(canvas, 2, true, module_text);
        if(app->external_module_available) {
            canvas_draw_str(canvas, 2, 43, "Left/Right: change module");
        } else {
            canvas_draw_str(canvas, 2, 43, "External module unavailable");
        }
    } else if(app->view == ScoutViewRegionInformation) {
        canvas_draw_str(canvas, 2, 10, "Region Information");
        canvas_set_font(canvas, FontSecondary);
        const FuriHalRegion* region = furi_hal_region_get();
        if(!region) {
            canvas_draw_str(canvas, 2, 26, "Region: N/A");
            canvas_draw_str(canvas, 2, 40, "No region data");
        } else {
            snprintf(line, sizeof(line), "Region: %.3s", region->country_code);
            canvas_draw_str(canvas, 2, 23, line);
            if(region->bands_count == 0U) {
                canvas_draw_str(canvas, 2, 38, "No frequency bands");
            } else {
                for(uint8_t row = 0; row < 3U; row++) {
                    uint16_t index = app->region_scroll_offset + row;
                    if(index >= region->bands_count) break;
                    const FuriHalRegionBand* band = &region->bands[index];
                    snprintf(
                        line,
                        sizeof(line),
                        "%lu-%lu kHz",
                        (unsigned long)(band->start / 1000U),
                        (unsigned long)(band->end / 1000U));
                    canvas_draw_str(canvas, 2, 37 + row * 9, line);
                }
                if(region->bands_count > 3U) {
                    snprintf(
                        line,
                        sizeof(line),
                        "%u/%u",
                        app->region_scroll_offset + 1U,
                        region->bands_count - 2U);
                    canvas_draw_str(canvas, 104, 62, line);
                }
            }
        }
        elements_button_left(canvas, "Back");
    } else if(app->view == ScoutViewManualUnavailable) {
        canvas_draw_str(canvas, 2, 10, "Add Manually");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 27, "Protocol creation is not");
        canvas_draw_str(canvas, 2, 38, "implemented yet.");
        canvas_draw_str(canvas, 2, 52, "No signal will be created.");
        elements_button_left(canvas, "Back");
    } else if(app->view == ScoutViewSettings) {
        canvas_draw_str(canvas, 2, 10, "Radio Settings");
        canvas_set_font(canvas, FontSecondary);
        const char* const hopping = app->hopping_enabled ? "ON" : "OFF";
        const char* const binraw = app->binraw_enabled ? "ON" : "OFF";
        char values[ScoutSettingCount][24];
        if(app->hopping_enabled) {
            snprintf(
                values[ScoutSettingFrequency],
                sizeof(values[ScoutSettingFrequency]),
                "Freq: -----");
        } else {
            snprintf(
                values[ScoutSettingFrequency],
                sizeof(values[ScoutSettingFrequency]),
                "Freq: %lu.%02lu",
                (unsigned long)(app->frequency_hz / 1000000U),
                (unsigned long)((app->frequency_hz % 1000000U) / 10000U));
        }
        snprintf(values[ScoutSettingHopping], sizeof(values[ScoutSettingHopping]), "Hop: %s", hopping);
        snprintf(values[ScoutSettingModulation],
                 sizeof(values[ScoutSettingModulation]),
                 "Mod: %.16s",
                 scout_preset_name(app, app->preset_index));
        snprintf(values[ScoutSettingBinRaw], sizeof(values[ScoutSettingBinRaw]), "BinRAW: %s", binraw);
        snprintf(values[ScoutSettingRawThreshold],
                 sizeof(values[ScoutSettingRawThreshold]),
                 "RAW RSSI: %s",
                 raw_threshold_text[app->raw_threshold_index]);
        uint8_t first = app->settings_selection > 3U ? app->settings_selection - 3U : 0U;
        for(uint8_t row = 0; row < 4U && first + row < ScoutSettingCount; row++) {
            uint8_t index = first + row;
            draw_stock_list_row(canvas, row + 1U, index == app->settings_selection, values[index]);
        }
        elements_button_left(canvas, "Decrease");
        elements_button_right(canvas, "Increase");
    } else if(app->view == ScoutViewAnalyzer) {
        canvas_draw_str(canvas, 2, 10, "Frequency Analyzer");
        canvas_set_font(canvas, FontSecondary);
        snprintf(
            line,
            sizeof(line),
            "Scanning %lu.%03lu MHz",
            (unsigned long)(app->frequency_hz / 1000000U),
            (unsigned long)((app->frequency_hz % 1000000U) / 1000U));
        canvas_draw_str(canvas, 2, 24, line);
        snprintf(line, sizeof(line), "RSSI %d dBm", (int)app->rssi);
        canvas_draw_str(canvas, 2, 36, line);
        snprintf(
            line,
            sizeof(line),
            "Peak %lu.%02lu %d dBm",
            (unsigned long)(app->analyzer_peak_frequency / 1000000U),
            (unsigned long)((app->analyzer_peak_frequency % 1000000U) / 10000U),
            (int)app->analyzer_peak_rssi);
        canvas_draw_str(canvas, 2, 48, line);
        elements_button_left(canvas, "Back");
    } else if(app->view == ScoutViewLabTest) {
        canvas_draw_str(canvas, 2, 10, "Test Transmit");
        canvas_set_font(canvas, FontSecondary);
        snprintf(
            line,
            sizeof(line),
            "%lu.%03lu MHz AM650",
            (unsigned long)(app->frequency_hz / 1000000U),
            (unsigned long)((app->frequency_hz % 1000000U) / 1000U));
        canvas_draw_str(canvas, 2, 25, line);
        if(app->lab_test_active) {
            snprintf(line, sizeof(line), "Code %06lX", (unsigned long)app->lab_test_code);
            canvas_draw_str(canvas, 2, 38, line);
            canvas_draw_str(canvas, 2, 49, "One short test burst");
            elements_button_center(canvas, "Stop");
        } else {
            canvas_draw_str(canvas, 2, 38, "Random Princeton code");
            canvas_draw_str(canvas, 2, 49, "Test receiver only");
            elements_button_center(canvas, "Send");
        }
        elements_button_left(canvas, "Back");
    } else if(app->view == ScoutViewDetectionDetails) {
        const ScoutDetection* detection = &app->detections[app->selected_detection];
        canvas_draw_str(
            canvas,
            2,
            10,
            app->detection_save_feedback == 1U ?
                "Signal saved" :
                (app->detection_save_feedback == 2U ? "Save failed" : "Signal details"));
        canvas_set_font(canvas, FontSecondary);
        uint8_t total_lines = 4U + wrapped_line_count(detection->detail);
        uint8_t max_offset = total_lines > 4U ? total_lines - 4U : 0U;
        if(app->detection_detail_scroll_offset > max_offset) {
            app->detection_detail_scroll_offset = max_offset;
        }
        for(uint8_t row = 0U; row < 4U; row++) {
            uint8_t index = app->detection_detail_scroll_offset + row;
            if(index >= total_lines) break;
            if(index == 0U) {
                snprintf(line, sizeof(line), "Protocol: %.23s", detection->protocol);
            } else if(index == 1U) {
                snprintf(
                    line,
                    sizeof(line),
                    "%lu.%03lu MHz",
                    (unsigned long)(detection->frequency / 1000000U),
                    (unsigned long)((detection->frequency % 1000000U) / 1000U));
            } else if(index == 2U) {
                snprintf(
                    line,
                    sizeof(line),
                    "Preset: %.23s",
                    scout_preset_name(app, detection->preset_index));
            } else if(index == 3U) {
                snprintf(line, sizeof(line), "Seen %u times", detection->sightings);
            } else {
                size_t offset = (size_t)(index - 4U) * DETAIL_LINE_CHARS;
                snprintf(
                    line,
                    sizeof(line),
                    "%.*s",
                    (int)DETAIL_LINE_CHARS,
                    detection->detail + offset);
            }
            canvas_draw_str(canvas, 2, 20 + row * 10, line);
        }
        if(total_lines > 4U) {
            elements_scrollbar_pos(
                canvas, 124, 20, 40, app->detection_detail_scroll_offset, total_lines);
        }
        elements_button_left(canvas, "Back");
        elements_button_right(
            canvas, app->detection_save_feedback == 1U ? "Saved" : "Save");
    } else if(app->view == ScoutViewRawRecording) {
        canvas_set_font(canvas, FontSecondary);
        snprintf(
            line,
            sizeof(line),
            "%lu.%03lu",
            (unsigned long)(app->frequency_hz / 1000000U),
            (unsigned long)((app->frequency_hz % 1000000U) / 1000U));
        canvas_draw_str(canvas, 0, 7, line);
        canvas_draw_str(canvas, 38, 7, scout_preset_name(app, app->preset_index));
        canvas_draw_str(
            canvas,
            78,
            7,
            app->external_module_selected ? "R: Ext" : "R: Int");
        if(app->raw_recording && app->raw_decoder) {
            size_t samples = subghz_protocol_raw_get_sample_write(app->raw_decoder);
            snprintf(line, sizeof(line), "%lu spl.", (unsigned long)samples);
        } else {
            snprintf(line, sizeof(line), "%s", app->raw_capture_saved ? "SAVED" : "READY");
        }
        canvas_draw_str_aligned(canvas, 126, 7, AlignRight, AlignTop, line);
        if(app->raw_threshold_index == 0U) {
            snprintf(
                line,
                sizeof(line),
                "T %d +%u",
                (int)app->raw_trigger_dbm,
                app->raw_rssi_margin_db);
        } else {
            snprintf(line, sizeof(line), "T %d dBm", (int)app->raw_trigger_dbm);
        }
        canvas_draw_str(canvas, 2, 13, line);
        draw_raw_rssi_graph(canvas, app);
        if(app->raw_recording) {
            elements_button_center(canvas, "Stop");
        } else {
            elements_button_left(canvas, "Config");
            elements_button_center(canvas, "REC");
        }
    } else if(app->view == ScoutViewSubFiles) {
        canvas_draw_str(canvas, 2, 10, "Sub-GHz files");
        canvas_set_font(canvas, FontSecondary);
        if(app->subghz_file_count == 0U) {
            canvas_draw_str(canvas, 2, 32, "No .sub files found");
        } else {
            uint8_t first = app->selected_subghz_file > 1U ?
                                app->selected_subghz_file - 1U :
                                0U;
            uint8_t max_first = app->subghz_file_count > 4U ?
                                    app->subghz_file_count - 4U :
                                    0U;
            if(first > max_first) first = max_first;
            for(uint8_t row = 0; row < 4U && first + row < app->subghz_file_count; row++) {
                uint8_t index = first + row;
                snprintf(
                    line,
                    sizeof(line),
                    "%.29s",
                    app->subghz_files[index]);
                draw_stock_list_row(
                    canvas, row + 1U, index == app->selected_subghz_file, line);
            }
            if(app->subghz_file_count > 4U) {
                elements_scrollbar_pos(
                    canvas,
                    124,
                    12,
                    48,
                    app->selected_subghz_file,
                    app->subghz_file_count);
            }
        }
        elements_button_left(canvas, "Back");
        elements_button_right(canvas, "Open");
    } else if(app->view == ScoutViewFileDetails) {
        canvas_draw_str(canvas, 2, 10, "Saved file");
        canvas_set_font(canvas, FontSecondary);
        uint8_t row_count = app->file_detail_row_count;
        uint8_t max_offset = row_count > 4U ? row_count - 4U : 0U;
        if(app->file_detail_scroll_offset > max_offset) {
            app->file_detail_scroll_offset = max_offset;
        }
        for(uint8_t row = 0U; row < 4U; row++) {
            uint8_t index = app->file_detail_scroll_offset + row;
            if(index >= row_count) break;
            canvas_draw_str(canvas, 2, 20 + row * 10, app->file_detail_lines[index]);
        }
        if(row_count > 4U) {
            elements_scrollbar_pos(
                canvas, 124, 20, 40, app->file_detail_scroll_offset, row_count);
        }
        elements_button_left(canvas, "Back");
        elements_button_right(canvas, "Rename");
        elements_button_center(canvas, "Delete");
    } else if(app->view == ScoutViewDeleteConfirmation) {
        canvas_draw_str(canvas, 2, 10, "Delete this file?");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 29, app->file_name);
        canvas_draw_str(canvas, 2, 43, "This cannot be undone.");
        elements_button_left(canvas, "Cancel");
        elements_button_right(canvas, "Delete");
    } else if(app->view == ScoutViewNameFile) {
        canvas_draw_str(canvas, 2, 10, app->name_edit_is_rename ? "Rename .sub file" :
                                                                    "Name .sub file");
        canvas_set_font(canvas, FontSecondary);
        size_t length = strlen(app->name_edit_buffer);
        uint8_t first = app->name_edit_cursor >= 20U ?
                            (uint8_t)(app->name_edit_cursor - 19U) :
                            0U;
        for(uint8_t column = 0U; column < 20U; column++) {
            uint8_t index = first + column;
            if(index > length || index >= MAX_EDITABLE_BASENAME) break;
            int32_t x = 3 + column * 6;
            char character[2] = {index < length ? app->name_edit_buffer[index] : ' ', '\0'};
            if(index == app->name_edit_cursor) {
                canvas_set_color(canvas, ColorBlack);
                canvas_draw_box(canvas, x - 1, 21, 6, 11);
                canvas_set_color(canvas, ColorWhite);
            } else {
                canvas_set_color(canvas, ColorBlack);
            }
            canvas_draw_str(canvas, x, 30, character);
        }
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_str(canvas, 2, 40, "Up/Down: change character");
        canvas_draw_str(canvas, 2, 50, "Left/Right: move cursor");
        canvas_draw_str(canvas, 2, 60, "OK: save   Back: cancel");
        if(strcmp(app->status, "Ready") != 0 && app->status[0] != '\0') {
            canvas_draw_str(canvas, 2, 15, app->status);
        }
    } else {
        canvas_set_font(canvas, FontSecondary);
        if(app->detection_count == 0U) {
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, 63, 38, "Scanning...");
            canvas_set_font(canvas, FontSecondary);
        } else {
            int32_t selected_rank = recent_detection_rank(app, app->selected_detection);
            if(selected_rank >= 0) {
                update_detection_scroll(app, (uint8_t)selected_rank);
            }
            for(uint8_t row = 0; row < 4U; row++) {
                int32_t index =
                    recent_detection_index(app, app->detection_scroll_offset + row);
                if(index < 0) break;
                const ScoutDetection* detection = &app->detections[index];
                snprintf(line, sizeof(line), "%.13s %.9s", detection->protocol, detection->detail);
                draw_stock_list_row(
                    canvas, row, (uint8_t)index == app->selected_detection, line);
            }
        }
        draw_stock_rssi(canvas, app->rssi);
        elements_button_left(canvas, "Config");
        snprintf(
            line,
            sizeof(line),
            "%lu.%03lu",
            (unsigned long)(app->frequency_hz / 1000000U),
            (unsigned long)((app->frequency_hz % 1000000U) / 1000U));
        canvas_draw_str(canvas, 43, 64, line);
        canvas_draw_str(canvas, 80, 64, scout_preset_name(app, app->preset_index));
    }
    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* event, void* context) {
    ScoutApp* app = context;
    if(event->type == InputTypeShort || event->type == InputTypeLong ||
       event->type == InputTypeRepeat) {
        if(furi_message_queue_put(app->event_queue, event, 0) != FuriStatusOk) {
            FURI_LOG_E("SubGhzScout", "Input event queue full");
        }
    }
}

static bool start_receiver(ScoutApp* app) {
    uint8_t* preset_data = scout_preset_data(app, app->preset_index);
    if(!preset_data || !subghz_devices_is_frequency_valid(app->device, app->frequency_hz)) {
        set_status(app, "Freq unavailable");
        return false;
    }

    subghz_devices_reset(app->device);
    subghz_devices_idle(app->device);
    subghz_devices_load_preset(app->device, FuriHalSubGhzPresetCustom, preset_data);
    subghz_devices_set_frequency(app->device, app->frequency_hz);
    subghz_devices_flush_rx(app->device);
    subghz_receiver_set_filter(
        app->protocol_receiver,
        app->raw_recording ? SubGhzProtocolFlag_RAW :
                             (SubGhzProtocolFlag_Decodable |
                              (app->binraw_enabled ? SubGhzProtocolFlag_BinRAW : 0U)));
    subghz_receiver_reset(app->protocol_receiver);
    subghz_devices_start_async_rx(app->device, subghz_worker_rx_callback, app->protocol_worker);
    subghz_worker_start(app->protocol_worker);
    if(furi_mutex_acquire(app->mutex, FuriWaitForever) == FuriStatusOk) {
        app->receiving = true;
        snprintf(app->status, sizeof(app->status), "Listening");
        furi_mutex_release(app->mutex);
    } else {
        FURI_LOG_E("SubGhzScout", "Could not update receiver state");
        if(subghz_worker_is_running(app->protocol_worker)) {
            subghz_worker_stop(app->protocol_worker);
        }
        subghz_devices_stop_async_rx(app->device);
        subghz_devices_sleep(app->device);
        return false;
    }
    return true;
}

static void stop_receiver(ScoutApp* app) {
    bool was_receiving = app->receiving;
    if(furi_mutex_acquire(app->mutex, FuriWaitForever) == FuriStatusOk) {
        app->receiving = false;
        furi_mutex_release(app->mutex);
    } else {
        FURI_LOG_E("SubGhzScout", "Could not update receiver state");
    }
    if(was_receiving) {
        if(subghz_worker_is_running(app->protocol_worker)) {
            subghz_worker_stop(app->protocol_worker);
        }
        subghz_devices_stop_async_rx(app->device);
        subghz_devices_sleep(app->device);
    }
}

static bool start_raw_recording(ScoutApp* app) {
    if(!app->radio_ready) {
        set_status(app, "Radio unavailable");
        return false;
    }
    if(!app->storage_ready) {
        set_status(app, "SD unavailable");
        return false;
    }
    const char* raw_preset = scout_preset_name(app, app->preset_index);
    if(strcmp(raw_preset, "AM270") != 0 && strcmp(raw_preset, "AM650") != 0 &&
       strcmp(raw_preset, "FM238") != 0 && strcmp(raw_preset, "FM476") != 0) {
        set_status(app, "RAW preset unsupported");
        return false;
    }
    if(!ensure_directory(app->storage, EXT_PATH("subghz"))) {
        set_status(app, "Sub-GHz folder error");
        return false;
    }
    app->raw_tail_samples =
        malloc(RAW_SIGNAL_TAIL_SAMPLES * sizeof(*app->raw_tail_samples));
    if(!app->raw_tail_samples) {
        FURI_LOG_E("SubGhzScout", "Could not allocate RSSI gate buffer");
        set_status(app, "RAW memory error");
        return false;
    }

    if(app->receiving) stop_receiver(app);
    subghz_receiver_set_filter(app->protocol_receiver, SubGhzProtocolFlag_RAW);
    SubGhzProtocolDecoderBase* decoder =
        subghz_receiver_search_decoder_base_by_name(app->protocol_receiver, "RAW");
    if(!decoder) {
        FURI_LOG_E("SubGhzScout", "Firmware RAW decoder not available");
        free(app->raw_tail_samples);
        app->raw_tail_samples = NULL;
        set_status(app, "RAW decoder unavailable");
        return false;
    }

    FuriString* preset_name = furi_string_alloc();
    if(!preset_name) {
        FURI_LOG_E("SubGhzScout", "Could not allocate RAW preset name");
        free(app->raw_tail_samples);
        app->raw_tail_samples = NULL;
        set_status(app, "RAW setup failed");
        return false;
    }
    furi_string_set(preset_name, raw_preset);
    SubGhzRadioPreset preset = {
        .name = preset_name,
        .frequency = app->frequency_hz,
        .data = NULL,
        .data_size = 0,
    };
    bool initialized = subghz_protocol_raw_save_to_file_init(
        (SubGhzProtocolDecoderRAW*)decoder, RAW_CAPTURE_NAME, &preset);
    furi_string_free(preset_name);
    if(!initialized) {
        FURI_LOG_E("SubGhzScout", "Could not initialize RAW file recording");
        free(app->raw_tail_samples);
        app->raw_tail_samples = NULL;
        set_status(app, "RAW file setup failed");
        return false;
    }

    app->raw_decoder = (SubGhzProtocolDecoderRAW*)decoder;
    app->raw_pretrigger_head = 0;
    app->raw_pretrigger_count = 0;
    app->raw_tail_count = 0;
    app->raw_below_threshold_us = 0;
    app->raw_gate_active = false;
    app->raw_capture_complete = false;
    app->raw_trigger_dbm = app->raw_threshold_index == 0U ?
                               app->noise_floor_dbm + app->raw_rssi_margin_db :
                               raw_threshold_values[app->raw_threshold_index];
    app->raw_capture_saved = false;
    app->raw_recording = true;
    if(!start_receiver(app)) {
        app->raw_recording = false;
        subghz_protocol_raw_save_to_file_stop(app->raw_decoder);
        app->raw_decoder = NULL;
        free(app->raw_tail_samples);
        app->raw_tail_samples = NULL;
        set_status(app, "RAW receiver failed");
        return false;
    }
    set_status(app, "Waiting above noise");
    return true;
}

static void finish_raw_recording(ScoutApp* app) {
    if(app->receiving) stop_receiver(app);
    size_t samples =
        app->raw_decoder ? subghz_protocol_raw_get_sample_write(app->raw_decoder) : 0U;
    if(app->raw_decoder) {
        subghz_protocol_raw_save_to_file_stop(app->raw_decoder);
        app->raw_decoder = NULL;
    }
    app->raw_recording = false;
    app->raw_gate_active = false;
    bool saved = samples > 0U;
    if(!saved && app->storage_ready &&
       storage_file_exists(app->storage, RAW_CAPTURE_PATH) &&
       !storage_simply_remove(app->storage, RAW_CAPTURE_PATH)) {
        FURI_LOG_E("SubGhzScout", "Could not remove empty RAW capture");
    }
    app->raw_capture_complete = false;
    app->raw_pretrigger_count = 0;
    app->raw_tail_count = 0;
    free(app->raw_tail_samples);
    app->raw_tail_samples = NULL;
    if(app->radio_ready) start_receiver(app);
    if(saved) {
        app->raw_capture_saved = true;
        set_status(app, "RAW capture saved");
    } else {
        app->raw_capture_saved = false;
        set_status(app, "No RAW pulses captured");
    }
}

static void tune_frequency(ScoutApp* app, bool increase, bool jump_to_limit) {
    size_t count = scout_frequency_count(app);
    if(count == 0U) {
        set_status(app, "No configured frequencies");
        return;
    }

    uint32_t next_index = app->frequency_index;
    if(jump_to_limit) {
        next_index = increase ? (uint32_t)(count - 1U) : 0U;
    } else if(increase) {
        if(app->view == ScoutViewAnalyzer || app->view == ScoutViewSettings) {
            next_index = (next_index + 1U) % count;
        } else if(next_index + 1U < count) {
            next_index++;
        }
    } else if(next_index > 0U) {
        next_index--;
    } else if(app->view == ScoutViewSettings) {
        next_index = (uint32_t)(count - 1U);
    }
    uint32_t next_frequency = scout_frequency_at(app, next_index);
    if(next_frequency == app->frequency_hz) return;

    bool was_receiving = app->receiving;
    if(was_receiving) stop_receiver(app);
    app->frequency_index = next_index;
    app->frequency_hz = next_frequency;
    set_status(app, was_receiving ? "Tuned" : "Frequency selected");
    if(was_receiving) start_receiver(app);
}

static void tune_hopper_frequency(ScoutApp* app) {
    size_t count = subghz_setting_get_hopper_frequency_count(app->radio_setting);
    if(count == 0U) {
        app->hopping_enabled = false;
        set_status(app, "No hopper frequencies");
        return;
    }

    uint32_t next_index = (app->hopper_index + 1U) % count;
    uint32_t next_frequency =
        subghz_setting_get_hopper_frequency(app->radio_setting, next_index);
    if(!scout_preset_data(app, app->preset_index) ||
       !subghz_devices_is_frequency_valid(app->device, next_frequency)) {
        set_status(app, "Invalid hopper frequency");
        return;
    }

    stop_receiver(app);
    app->hopper_index = (uint8_t)next_index;
    app->frequency_hz = next_frequency;
    for(size_t i = 0; i < scout_frequency_count(app); i++) {
        if(scout_frequency_at(app, i) == next_frequency) {
            app->frequency_index = (uint32_t)i;
            break;
        }
    }
    start_receiver(app);
}

static void apply_preset(ScoutApp* app, uint8_t preset_index) {
    if(preset_index >= scout_preset_count(app)) return;
    bool was_receiving = app->receiving;
    if(was_receiving) stop_receiver(app);
    app->preset_index = preset_index;
    if(was_receiving) {
        start_receiver(app);
    } else {
        set_status(app, "Preset selected");
    }
}

static void open_settings(ScoutApp* app, ScoutView return_view) {
    app->settings_return_view = return_view;
    app->settings_selection = 0U;
    if(return_view == ScoutViewListening || return_view == ScoutViewRawRecording) {
        stop_receiver(app);
    }
    app->view = ScoutViewSettings;
}

static void close_settings(ScoutApp* app) {
    app->view = app->settings_return_view;
    if((app->settings_return_view == ScoutViewListening ||
        app->settings_return_view == ScoutViewRawRecording) &&
       app->radio_ready) {
        start_receiver(app);
    }
}

static void change_setting(ScoutApp* app, bool increase) {
    if(app->settings_selection == ScoutSettingFrequency) {
        if(!app->hopping_enabled) tune_frequency(app, increase, false);
    } else if(app->settings_selection == ScoutSettingHopping) {
        if(!app->hopping_enabled) {
            size_t count = subghz_setting_get_hopper_frequency_count(app->radio_setting);
            if(count == 0U) {
                set_status(app, "No hopper frequencies");
                return;
            }
            uint32_t first_frequency =
                subghz_setting_get_hopper_frequency(app->radio_setting, 0U);
            if(!subghz_devices_is_frequency_valid(app->device, first_frequency)) {
                set_status(app, "Invalid hopper frequency");
                return;
            }
            app->hopping_enabled = true;
            app->frequency_hz = first_frequency;
            app->hopper_index = (uint8_t)(count - 1U);
            for(size_t i = 0; i < scout_frequency_count(app); i++) {
                if(scout_frequency_at(app, i) == first_frequency) {
                    app->frequency_index = (uint32_t)i;
                    break;
                }
            }
        } else {
            app->hopping_enabled = false;
            app->frequency_hz = subghz_setting_get_default_frequency(app->radio_setting);
            app->frequency_index = subghz_setting_get_frequency_default_index(app->radio_setting);
        }
        app->hopper_last_step_tick = furi_get_tick();
    } else if(app->settings_selection == ScoutSettingModulation) {
        size_t count = scout_preset_count(app);
        if(count == 0U) return;
        app->preset_index = increase ?
                                (uint8_t)((app->preset_index + 1U) % count) :
                                (uint8_t)(app->preset_index == 0U ? count - 1U :
                                                                    app->preset_index - 1U);
    } else if(app->settings_selection == ScoutSettingBinRaw) {
        app->binraw_enabled = !app->binraw_enabled;
    } else if(app->settings_selection == ScoutSettingRawThreshold) {
        app->raw_threshold_index = increase ?
                                       (uint8_t)((app->raw_threshold_index + 1U) %
                                                 RAW_THRESHOLD_COUNT) :
                                       (uint8_t)(app->raw_threshold_index == 0U ?
                                                     RAW_THRESHOLD_COUNT - 1U :
                                                     app->raw_threshold_index - 1U);
    }
}

static bool initialize_protocol_decoder(ScoutApp* app) {
    app->protocol_environment = subghz_environment_alloc();
    if(!app->protocol_environment) {
        FURI_LOG_E("SubGhzScout", "Could not allocate protocol environment");
        set_status(app, "Decoder memory error");
        return false;
    }
    if(!subghz_environment_load_keystore(
           app->protocol_environment, SUBGHZ_KEYSTORE_DIR_NAME)) {
        FURI_LOG_W("SubGhzScout", "Factory Sub-GHz keystore unavailable");
    }
    if(!subghz_environment_load_keystore(
           app->protocol_environment, SUBGHZ_KEYSTORE_DIR_USER_NAME)) {
        FURI_LOG_W("SubGhzScout", "User Sub-GHz keystore unavailable");
    }
    subghz_environment_set_came_atomo_rainbow_table_file_name(
        app->protocol_environment, SUBGHZ_CAME_ATOMO_DIR_NAME);
    subghz_environment_set_alutech_at_4n_rainbow_table_file_name(
        app->protocol_environment, SUBGHZ_ALUTECH_AT_4N_DIR_NAME);
    subghz_environment_set_nice_flor_s_rainbow_table_file_name(
        app->protocol_environment, SUBGHZ_NICE_FLOR_S_DIR_NAME);
    subghz_environment_set_protocol_registry(
        app->protocol_environment, &subghz_protocol_registry);
    app->protocol_receiver = subghz_receiver_alloc_init(app->protocol_environment);
    if(!app->protocol_receiver) {
        FURI_LOG_E("SubGhzScout", "Could not allocate protocol receiver");
        subghz_environment_free(app->protocol_environment);
        app->protocol_environment = NULL;
        set_status(app, "Decoder memory error");
        return false;
    }
    subghz_receiver_set_filter(
        app->protocol_receiver,
        SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_RAW);
    subghz_receiver_set_rx_callback(app->protocol_receiver, protocol_decoded_callback, app);
    app->binraw_decoder = (SubGhzProtocolDecoderBinRAW*)
        subghz_receiver_search_decoder_base_by_name(app->protocol_receiver, "BinRAW");
    if(!app->binraw_decoder) {
        FURI_LOG_E("SubGhzScout", "Firmware BinRAW decoder not available");
    }
    app->protocol_worker = subghz_worker_alloc();
    if(!app->protocol_worker) {
        FURI_LOG_E("SubGhzScout", "Could not allocate protocol worker");
        subghz_receiver_free(app->protocol_receiver);
        subghz_environment_free(app->protocol_environment);
        app->protocol_receiver = NULL;
        app->protocol_environment = NULL;
        set_status(app, "Decoder memory error");
        return false;
    }
    subghz_worker_set_pair_callback(app->protocol_worker, protocol_worker_pair_callback);
    subghz_worker_set_overrun_callback(app->protocol_worker, protocol_worker_overrun_callback);
    subghz_worker_set_filter(app->protocol_worker, 30U);
    subghz_worker_set_context(app->protocol_worker, app);
    return true;
}

static bool initialize_radio(ScoutApp* app) {
    if(!app->protocol_receiver && !initialize_protocol_decoder(app)) return false;
    subghz_devices_init();
    const char* device_name =
        app->external_module_selected ? SUBGHZ_DEVICE_CC1101_EXT_NAME : "cc1101_int";
    if(app->external_module_selected) {
        Power* power = furi_record_open(RECORD_POWER);
        power_enable_otg(power, true);
        furi_record_close(RECORD_POWER);
    }
    app->device = subghz_devices_get_by_name(device_name);
    if(!app->device ||
       (app->external_module_selected && !subghz_devices_is_connect(app->device))) {
        set_status(app, "Radio unavailable");
        subghz_devices_deinit();
        app->device = NULL;
        if(app->external_module_selected) {
            Power* power = furi_record_open(RECORD_POWER);
            power_enable_otg(power, false);
            furi_record_close(RECORD_POWER);
        }
        return false;
    }
    if(app->external_module_selected) {
        app->radio_device_begun = subghz_devices_begin(app->device);
        if(!app->radio_device_begun) {
            set_status(app, "External radio failed");
            subghz_devices_deinit();
            app->device = NULL;
            Power* power = furi_record_open(RECORD_POWER);
            power_enable_otg(power, false);
            furi_record_close(RECORD_POWER);
            return false;
        }
    }
    app->radio_ready = true;
    if(!start_receiver(app)) {
        if(app->radio_device_begun) {
            subghz_devices_end(app->device);
            app->radio_device_begun = false;
        }
        subghz_devices_deinit();
        app->radio_ready = false;
        app->device = NULL;
        if(app->external_module_selected) {
            Power* power = furi_record_open(RECORD_POWER);
            power_enable_otg(power, false);
            furi_record_close(RECORD_POWER);
        }
        return false;
    }
    return true;
}

static bool external_radio_is_available(void) {
    subghz_devices_init();
    bool otg_was_enabled = furi_hal_power_is_otg_enabled();
    if(!otg_was_enabled) {
        Power* power = furi_record_open(RECORD_POWER);
        power_enable_otg(power, true);
        furi_record_close(RECORD_POWER);
    }
    const SubGhzDevice* external =
        subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_EXT_NAME);
    bool available = external && subghz_devices_is_connect(external);
    subghz_devices_deinit();
    if(!otg_was_enabled) {
        Power* power = furi_record_open(RECORD_POWER);
        power_enable_otg(power, false);
        furi_record_close(RECORD_POWER);
    }
    return available;
}

static void select_radio_module(ScoutApp* app, bool external) {
    if(app->external_module_selected == external) return;
    if(app->radio_ready) {
        stop_receiver(app);
        if(app->radio_device_begun) {
            subghz_devices_end(app->device);
            app->radio_device_begun = false;
        }
        subghz_devices_deinit();
        app->radio_ready = false;
        app->device = NULL;
        if(app->external_module_selected) {
            Power* power = furi_record_open(RECORD_POWER);
            power_enable_otg(power, false);
            furi_record_close(RECORD_POWER);
        }
    }
    app->external_module_selected = external;
    set_status(app, external ? "External module selected" : "Internal module selected");
}

static bool ensure_receiver(ScoutApp* app) {
    if(!app->radio_ready) return initialize_radio(app);
    if(!app->receiving) return start_receiver(app);
    return true;
}

int32_t subghz_scout_app(void* p) {
    UNUSED(p);
    ScoutApp app = {0};
    app.mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app.event_queue = furi_message_queue_alloc(EVENT_QUEUE_SIZE, sizeof(InputEvent));
    app.rssi = RSSI_BAR_MIN_DBM;
    app.noise_floor_dbm = -100.0f;
    app.raw_rssi_margin_db = RAW_DEFAULT_RSSI_MARGIN_DB;
    app.raw_trigger_dbm = app.noise_floor_dbm + app.raw_rssi_margin_db;
    app.view = ScoutViewStartMenu;
    snprintf(app.status, sizeof(app.status), "Ready");
    app.external_module_available = external_radio_is_available();
    app.external_module_selected = app.external_module_available;
    app.storage = furi_record_open(RECORD_STORAGE);
    app.storage_ready = app.storage != NULL;
    if(!app.storage_ready) set_status(&app, "SD unavailable");
    app.radio_setting = subghz_setting_alloc();
    if(!app.radio_setting) {
        FURI_LOG_E("SubGhzScout", "Could not allocate radio settings");
        set_status(&app, "Settings unavailable");
    } else {
        subghz_setting_load(app.radio_setting, SCOUT_SETTING_PATH);
        size_t frequency_count = scout_frequency_count(&app);
        size_t preset_count = scout_preset_count(&app);
        if(frequency_count == 0U || preset_count == 0U) {
            set_status(&app, "No radio settings");
        } else {
            app.frequency_hz = subghz_setting_get_default_frequency(app.radio_setting);
            bool default_found = false;
            for(size_t i = 0; i < frequency_count; i++) {
                if(scout_frequency_at(&app, i) == app.frequency_hz) {
                    app.frequency_index = (uint32_t)i;
                    default_found = true;
                    break;
                }
            }
            if(!default_found) {
                app.frequency_index = 0U;
                app.frequency_hz = scout_frequency_at(&app, 0U);
            }
            int default_preset =
                subghz_setting_get_inx_preset_by_name(app.radio_setting, "AM650");
            app.preset_index =
                default_preset >= 0 && (size_t)default_preset < preset_count ?
                    (uint8_t)default_preset :
                    0U;
        }
    }

    Gui* gui = furi_record_open(RECORD_GUI);
    app.view_port = view_port_alloc();
    view_port_draw_callback_set(app.view_port, draw_callback, &app);
    view_port_input_callback_set(app.view_port, input_callback, &app);
    gui_add_view_port(gui, app.view_port, GuiLayerFullscreen);

    if(app.radio_setting && scout_frequency_count(&app) > 0U &&
       scout_preset_count(&app) > 0U) {
        initialize_protocol_decoder(&app);
    }
    view_port_update(app.view_port);

    while(!app.should_exit) {
        InputEvent event;
        FuriStatus status = furi_message_queue_get(app.event_queue, &event, 20);
        if(status == FuriStatusOk) {
            bool navigation_event =
                event.type == InputTypeShort || event.type == InputTypeRepeat;
            bool short_back = event.key == InputKeyBack && event.type == InputTypeShort;
            if(short_back) {
                if(app.view == ScoutViewStartMenu) {
                    app.should_exit = true;
                } else if(app.view == ScoutViewListening) {
                    stop_receiver(&app);
                    app.view = ScoutViewStartMenu;
                } else if(app.view == ScoutViewSettings) {
                    close_settings(&app);
                } else if(app.view == ScoutViewRawRecording) {
                    if(app.raw_recording) finish_raw_recording(&app);
                    if(app.raw_return_view == ScoutViewStartMenu) stop_receiver(&app);
                    app.view = app.raw_return_view;
                } else if(app.view == ScoutViewFileDetails) {
                    app.view = ScoutViewSubFiles;
                } else if(app.view == ScoutViewDeleteConfirmation) {
                    app.view = ScoutViewFileDetails;
                } else if(app.view == ScoutViewNameFile) {
                    app.view = app.name_return_view;
                } else if(app.view == ScoutViewSubFiles) {
                    app.view = ScoutViewStartMenu;
                } else if(app.view == ScoutViewAnalyzer) {
                    stop_receiver(&app);
                    app.frequency_index = app.analyzer_saved_frequency_index;
                    app.frequency_hz = app.analyzer_saved_frequency_hz;
                    app.preset_index = app.analyzer_saved_preset_index;
                    start_receiver(&app);
                    app.view = ScoutViewStartMenu;
                } else if(app.view == ScoutViewLabTest) {
                    stop_lab_test_transmission(&app);
                    app.view = ScoutViewStartMenu;
                } else if(app.view == ScoutViewDetectionDetails) {
                    app.view = ScoutViewListening;
                } else if(app.view == ScoutViewRegionInformation ||
                          app.view == ScoutViewManualUnavailable ||
                          app.view == ScoutViewRadioModule) {
                    app.view = ScoutViewStartMenu;
                }
            } else if(app.view == ScoutViewStartMenu && navigation_event &&
                      (event.key == InputKeyUp || event.key == InputKeyDown)) {
                if(event.key == InputKeyUp) {
                    if(app.start_menu_selection > 0U) app.start_menu_selection--;
                } else if(app.start_menu_selection < 7U) {
                    app.start_menu_selection++;
                }
            } else if(app.view == ScoutViewStartMenu && event.type == InputTypeShort &&
                      event.key == InputKeyOk) {
                if(app.start_menu_selection == 0U) {
                    if(ensure_receiver(&app)) {
                        int32_t newest = recent_detection_index(&app, 0);
                        if(newest >= 0) app.selected_detection = (uint8_t)newest;
                        app.view = ScoutViewListening;
                    }
                } else if(app.start_menu_selection == 1U) {
                    app.raw_return_view = ScoutViewStartMenu;
                    if(ensure_receiver(&app)) {
                        app.view = ScoutViewRawRecording;
                    }
                } else if(app.start_menu_selection == 2U) {
                    load_subghz_file_list(&app);
                    app.view = ScoutViewSubFiles;
                } else if(app.start_menu_selection == 3U) {
                    app.view = ScoutViewManualUnavailable;
                } else if(app.start_menu_selection == 4U) {
                    if(ensure_receiver(&app)) {
                        app.analyzer_saved_frequency_index = app.frequency_index;
                        app.analyzer_saved_frequency_hz = app.frequency_hz;
                        app.analyzer_saved_preset_index = app.preset_index;
                        int analyzer_preset =
                            subghz_setting_get_inx_preset_by_name(app.radio_setting, "AM650");
                        if(analyzer_preset >= 0 &&
                           (size_t)analyzer_preset < scout_preset_count(&app)) {
                            apply_preset(&app, (uint8_t)analyzer_preset);
                        }
                        if(app.receiving) {
                            app.analyzer_peak_frequency = app.frequency_hz;
                            app.analyzer_peak_rssi = RSSI_BAR_MIN_DBM;
                            app.analyzer_last_step_tick = furi_get_tick();
                            app.view = ScoutViewAnalyzer;
                        }
                    }
                } else if(app.start_menu_selection == 5U) {
                    app.region_scroll_offset = 0U;
                    app.view = ScoutViewRegionInformation;
                } else if(app.start_menu_selection == 6U) {
                    app.view = ScoutViewRadioModule;
                } else {
                    app.view = ScoutViewLabTest;
                }
            } else if(app.view == ScoutViewRadioModule && event.type == InputTypeShort &&
                      app.external_module_available &&
                      (event.key == InputKeyLeft || event.key == InputKeyRight)) {
                select_radio_module(&app, !app.external_module_selected);
            } else if(app.view == ScoutViewLabTest &&
                      event.key == InputKeyOk && event.type == InputTypeShort) {
                if(app.lab_test_active) {
                    stop_lab_test_transmission(&app);
                } else {
                    start_lab_test_transmission(&app);
                }
            } else if(app.view == ScoutViewSettings && navigation_event &&
                      (event.key == InputKeyUp || event.key == InputKeyDown)) {
                if(event.key == InputKeyUp) {
                    if(app.settings_selection > 0U) app.settings_selection--;
                } else if(app.settings_selection + 1U < ScoutSettingCount) {
                    app.settings_selection++;
                }
            } else if(app.view == ScoutViewSettings && event.type == InputTypeShort &&
                      (event.key == InputKeyLeft || event.key == InputKeyRight)) {
                change_setting(&app, event.key == InputKeyRight);
            } else if(app.view == ScoutViewListening && event.key == InputKeyLeft &&
                      event.type == InputTypeShort) {
                open_settings(&app, ScoutViewListening);
            } else if(app.view == ScoutViewRegionInformation && navigation_event &&
                      (event.key == InputKeyUp || event.key == InputKeyDown)) {
                const FuriHalRegion* region = furi_hal_region_get();
                if(region && region->bands_count > 3U) {
                    uint16_t max_offset = region->bands_count - 3U;
                    if(event.key == InputKeyUp && app.region_scroll_offset > 0U) {
                        app.region_scroll_offset--;
                    } else if(event.key == InputKeyDown &&
                              app.region_scroll_offset < max_offset) {
                        app.region_scroll_offset++;
                    }
                }
            } else if(app.view == ScoutViewSubFiles && navigation_event &&
                      (event.key == InputKeyUp || event.key == InputKeyDown)) {
                if(app.subghz_file_count > 0U) {
                    if(event.key == InputKeyUp) {
                        if(app.selected_subghz_file > 0U) app.selected_subghz_file--;
                    } else if(app.selected_subghz_file + 1U < app.subghz_file_count) {
                        app.selected_subghz_file++;
                    }
                }
            } else if(app.view == ScoutViewSubFiles &&
                      (event.key == InputKeyOk || event.key == InputKeyRight) &&
                      event.type == InputTypeShort && app.subghz_file_count > 0U) {
                handle_selected_file(
                    &app, app.subghz_files[app.selected_subghz_file]);
            } else if(app.view == ScoutViewRawRecording &&
                      event.key == InputKeyLeft && event.type == InputTypeShort &&
                      !app.raw_recording) {
                open_settings(&app, ScoutViewRawRecording);
            } else if(app.view == ScoutViewRawRecording &&
                      event.key == InputKeyOk &&
                      event.type == InputTypeShort) {
                if(app.raw_recording) {
                    finish_raw_recording(&app);
                } else {
                    start_raw_recording(&app);
                }
            } else if(app.view == ScoutViewRawRecording && !app.raw_recording &&
                      navigation_event &&
                      (event.key == InputKeyUp || event.key == InputKeyDown)) {
                if(app.raw_threshold_index == 0U) {
                    if(event.key == InputKeyUp && app.raw_rssi_margin_db < 30U) {
                        app.raw_rssi_margin_db++;
                    } else if(event.key == InputKeyDown && app.raw_rssi_margin_db > 0U) {
                        app.raw_rssi_margin_db--;
                    }
                } else if(event.key == InputKeyUp &&
                          app.raw_threshold_index + 1U < RAW_THRESHOLD_COUNT) {
                    app.raw_threshold_index++;
                } else if(event.key == InputKeyDown && app.raw_threshold_index > 0U) {
                    app.raw_threshold_index--;
                }
            } else if(app.view == ScoutViewFileDetails && navigation_event &&
                      (event.key == InputKeyUp || event.key == InputKeyDown)) {
                uint8_t row_count = app.file_detail_row_count;
                if(event.key == InputKeyUp && app.file_detail_scroll_offset > 0U) {
                    app.file_detail_scroll_offset--;
                } else if(event.key == InputKeyDown &&
                          app.file_detail_scroll_offset + 4U < row_count) {
                    app.file_detail_scroll_offset++;
                }
            } else if(app.view == ScoutViewFileDetails &&
                      event.key == InputKeyRight && event.type == InputTypeShort) {
                begin_name_edit(&app, true, 0U);
                set_status(&app, "Rename file");
            } else if(app.view == ScoutViewFileDetails &&
                      event.key == InputKeyOk && event.type == InputTypeShort) {
                app.file_tx_repeat_held = false;
                if(!app.file_tx_active) {
                    start_saved_file_transmission(&app, app.selected_file);
                }
            } else if(app.view == ScoutViewFileDetails &&
                      event.key == InputKeyOk && event.type == InputTypeLong) {
                app.file_tx_repeat_held = true;
                if(!app.file_tx_active) {
                    start_saved_file_transmission(&app, app.selected_file);
                }
            } else if(app.view == ScoutViewFileDetails &&
          event.key == InputKeyOk && event.type == InputTypeRelease) {
                app.file_tx_repeat_held = false;

            } else if(app.view == ScoutViewDeleteConfirmation &&
                      event.key == InputKeyLeft && event.type == InputTypeShort) {
                app.view = ScoutViewFileDetails;
            } else if(app.view == ScoutViewDeleteConfirmation &&
                      event.key == InputKeyRight && event.type == InputTypeShort) {
                delete_selected_file(&app);
            } else if(app.view == ScoutViewNameFile &&
                      event.key == InputKeyOk && event.type == InputTypeShort) {
                if(!is_valid_subghz_basename(app.name_edit_buffer)) {
                    set_status(&app, "Invalid file name");
                } else if(app.name_edit_is_rename) {
                    if(rename_subghz_file(&app, app.name_edit_buffer)) {
                        app.view = app.name_return_view;
                    }
                } else if(save_detection(
                              &app, app.name_detection_index, app.name_edit_buffer)) {
                    app.detection_save_feedback = 1U;
                    load_subghz_file_list(&app);
                    app.view = app.name_return_view;
                }
            } else if(app.view == ScoutViewNameFile && navigation_event &&
                      (event.key == InputKeyUp || event.key == InputKeyDown)) {
                cycle_edit_character(&app, event.key == InputKeyUp);
                set_status(&app, "Edit file name");
            } else if(app.view == ScoutViewNameFile && event.type == InputTypeShort &&
                      event.key == InputKeyLeft) {
                if(app.name_edit_cursor > 0U) app.name_edit_cursor--;
            } else if(app.view == ScoutViewNameFile && event.type == InputTypeShort &&
                      event.key == InputKeyRight) {
                size_t length = strlen(app.name_edit_buffer);
                if(app.name_edit_cursor < MAX_EDITABLE_BASENAME - 1U) {
                    if(app.name_edit_cursor >= length) {
                        app.name_edit_buffer[length] = 'A';
                        app.name_edit_buffer[length + 1U] = '\0';
                    }
                    app.name_edit_cursor++;
                }
            } else if(app.view == ScoutViewNameFile && event.type == InputTypeLong &&
                      event.key == InputKeyLeft) {
                if(remove_edit_character_before_cursor(&app)) {
                    set_status(&app, "Edit file name");
                }
            } else if(app.view == ScoutViewDetectionDetails && navigation_event &&
                      (event.key == InputKeyUp || event.key == InputKeyDown)) {
                const ScoutDetection* detection = &app.detections[app.selected_detection];
                uint8_t total_lines = 4U + wrapped_line_count(detection->detail);
                uint8_t max_offset = total_lines > 4U ? total_lines - 4U : 0U;
                if(event.key == InputKeyUp && app.detection_detail_scroll_offset > 0U) {
                    app.detection_detail_scroll_offset--;
                } else if(event.key == InputKeyDown &&
                          app.detection_detail_scroll_offset < max_offset) {
                    app.detection_detail_scroll_offset++;
                }
            } else if(app.view == ScoutViewListening && navigation_event &&
                      (event.key == InputKeyUp || event.key == InputKeyDown) &&
                      app.detection_count > 0U) {
                uint8_t selected_rank = 0;
                for(uint8_t rank = 0; rank < app.detection_count; rank++) {
                    if(recent_detection_index(&app, rank) == app.selected_detection) {
                        selected_rank = rank;
                        break;
                    }
                }
                if(event.key == InputKeyUp && selected_rank > 0U) {
                    selected_rank--;
                } else if(event.key == InputKeyDown &&
                          selected_rank + 1U < app.detection_count) {
                    selected_rank++;
                }
                int32_t selected = recent_detection_index(&app, selected_rank);
                if(selected >= 0) {
                    app.selected_detection = (uint8_t)selected;
                    update_detection_scroll(&app, selected_rank);
                }
            } else if(app.view == ScoutViewDetectionDetails &&
                      event.key == InputKeyRight && event.type == InputTypeShort) {
                begin_name_edit(&app, false, app.selected_detection);
                set_status(&app, "Name saved signal");
            } else if(app.view == ScoutViewListening && event.key == InputKeyOk &&
                      event.type == InputTypeShort) {
                if(app.detection_count > 0U) {
                    app.detection_detail_scroll_offset = 0U;
                    app.detection_save_feedback = 0U;
                    app.view = ScoutViewDetectionDetails;
                }
            }
        } else if(status != FuriStatusErrorTimeout) {
            FURI_LOG_E("SubGhzScout", "Input queue read failed: %d", status);
            break;
        }
        if(app.radio_ready && app.receiving) {
            float rssi = subghz_devices_get_rssi(app.device);
            if(furi_mutex_acquire(app.mutex, FuriWaitForever) == FuriStatusOk) {
                app.rssi = rssi;
                uint8_t sample = rssi_graph_level(rssi);
                app.raw_rssi_history[app.raw_rssi_history_head] = sample;
                app.raw_rssi_history_head =
                    (app.raw_rssi_history_head + 1U) % RAW_RSSI_HISTORY_SIZE;
                if(app.raw_rssi_history_count < RAW_RSSI_HISTORY_SIZE) {
                    app.raw_rssi_history_count++;
                }
                furi_mutex_release(app.mutex);
            } else {
                FURI_LOG_E("SubGhzScout", "Could not update RSSI");
            }
            if(!app.raw_recording) {
                if(rssi < app.noise_floor_dbm) {
                    app.noise_floor_dbm += (rssi - app.noise_floor_dbm) * 0.25f;
                } else if(rssi < app.noise_floor_dbm + 4.0f) {
                    app.noise_floor_dbm += (rssi - app.noise_floor_dbm) * 0.02f;
                }
                app.raw_trigger_dbm = app.raw_threshold_index == 0U ?
                                          app.noise_floor_dbm + app.raw_rssi_margin_db :
                                          raw_threshold_values[app.raw_threshold_index];
                if(app.binraw_enabled && app.binraw_decoder) {
                    subghz_protocol_decoder_bin_raw_data_input_rssi(app.binraw_decoder, rssi);
                }
            }
            if(app.view == ScoutViewAnalyzer) {
                if(rssi > app.analyzer_peak_rssi) {
                    app.analyzer_peak_rssi = rssi;
                    app.analyzer_peak_frequency = app.frequency_hz;
                }
                uint32_t now = furi_get_tick();
                if((now - app.analyzer_last_step_tick) >= furi_ms_to_ticks(250)) {
                    app.analyzer_last_step_tick = now;
                    tune_frequency(&app, true, false);
                }
            } else if(app.view == ScoutViewListening && app.hopping_enabled) {
                uint32_t now = furi_get_tick();
                if((now - app.hopper_last_step_tick) >= furi_ms_to_ticks(100)) {
                    app.hopper_last_step_tick = now;
                    tune_hopper_frequency(&app);
                }
            }
        }
        if(app.lab_test_active &&
           (subghz_devices_is_async_complete_tx(app.device) ||
            (furi_get_tick() - app.lab_test_started_tick) >=
                furi_ms_to_ticks(LAB_TEST_MAX_DURATION_MS))) {
            stop_lab_test_transmission(&app);
        }
        if(app.raw_capture_complete) finish_raw_recording(&app);
        view_port_update(app.view_port);
    }

    stop_receiver(&app);
    if(app.raw_recording) finish_raw_recording(&app);
    free(app.raw_tail_samples);
    if(app.radio_ready) {
        if(app.radio_device_begun) subghz_devices_end(app.device);
        subghz_devices_deinit();
        if(app.external_module_selected) {
            Power* power = furi_record_open(RECORD_POWER);
            power_enable_otg(power, false);
            furi_record_close(RECORD_POWER);
        }
    }
    if(app.protocol_worker) subghz_worker_free(app.protocol_worker);
    if(app.protocol_receiver) subghz_receiver_free(app.protocol_receiver);
    if(app.protocol_environment) subghz_environment_free(app.protocol_environment);
    if(app.radio_setting) subghz_setting_free(app.radio_setting);
    for(uint8_t i = 0U; i < app.detection_count; i++) {
        free(app.detections[i].serialized_data);
    }

    gui_remove_view_port(gui, app.view_port);
    view_port_free(app.view_port);
    if(app.storage) furi_record_close(RECORD_STORAGE);
    free(app.subghz_files);
    free(app.file_contents);
    free(app.file_detail_lines);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app.event_queue);
    furi_mutex_free(app.mutex);
    return 0;
}
