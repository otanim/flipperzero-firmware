#include "subghz_i.h"

#include <math.h>
#include <furi.h>
#include <furi_hal.h>
#include <input/input.h>
#include <gui/elements.h>
#include <notification/notification_messages.h>
#include <flipper_format/flipper_format.h>
#include "../notification/notification.h"
#include "views/receiver.h"

#include <flipper_format/flipper_format_i.h>
#include <lib/toolbox/stream/stream.h>
#include <lib/subghz/protocols/raw.h>
#include <lib/toolbox/path.h>

#define TAG "SubGhz"

bool subghz_set_preset(SubGhz* subghz, const char* preset) {
    if(!strcmp(preset, "FuriHalSubGhzPresetOok270Async")) {
        subghz->txrx->preset = FuriHalSubGhzPresetOok270Async;
    } else if(!strcmp(preset, "FuriHalSubGhzPresetOok650Async")) {
        subghz->txrx->preset = FuriHalSubGhzPresetOok650Async;
    } else if(!strcmp(preset, "FuriHalSubGhzPreset2FSKDev238Async")) {
        subghz->txrx->preset = FuriHalSubGhzPreset2FSKDev238Async;
    } else if(!strcmp(preset, "FuriHalSubGhzPreset2FSKDev476Async")) {
        subghz->txrx->preset = FuriHalSubGhzPreset2FSKDev476Async;
    } else {
        FURI_LOG_E(TAG, "Unknown preset");
        return false;
    }
    return true;
}

void subghz_get_frequency_modulation(SubGhz* subghz, string_t frequency, string_t modulation) {
    furi_assert(subghz);
    if(frequency != NULL) {
        string_printf(
            frequency,
            "%03ld.%02ld",
            subghz->txrx->frequency / 1000000 % 1000,
            subghz->txrx->frequency / 10000 % 100);
    }

    if(modulation != NULL) {
        if(subghz->txrx->preset == FuriHalSubGhzPresetOok650Async ||
           subghz->txrx->preset == FuriHalSubGhzPresetOok270Async) {
            string_set(modulation, "AM");
        } else if(
            subghz->txrx->preset == FuriHalSubGhzPreset2FSKDev238Async ||
            subghz->txrx->preset == FuriHalSubGhzPreset2FSKDev476Async) {
            string_set(modulation, "FM");
        } else {
            furi_crash(NULL);
        }
    }
}

void subghz_begin(SubGhz* subghz, FuriHalSubGhzPreset preset) {
    furi_assert(subghz);
    furi_hal_subghz_reset();
    furi_hal_subghz_idle();
    furi_hal_subghz_load_preset(preset);
    furi_hal_gpio_init(&gpio_cc1101_g0, GpioModeInput, GpioPullNo, GpioSpeedLow);
    subghz->txrx->txrx_state = SubGhzTxRxStateIDLE;
}

uint32_t subghz_rx(SubGhz* subghz, uint32_t frequency) {
    furi_assert(subghz);
    if(!furi_hal_subghz_is_frequency_valid(frequency)) {
        furi_crash(NULL);
    }
    furi_assert(
        subghz->txrx->txrx_state != SubGhzTxRxStateRx &&
        subghz->txrx->txrx_state != SubGhzTxRxStateSleep);

    furi_hal_subghz_idle();
    uint32_t value = furi_hal_subghz_set_frequency_and_path(frequency);
    furi_hal_gpio_init(&gpio_cc1101_g0, GpioModeInput, GpioPullNo, GpioSpeedLow);
    furi_hal_subghz_flush_rx();
    furi_hal_subghz_rx();

    furi_hal_subghz_start_async_rx(subghz_worker_rx_callback, subghz->txrx->worker);
    subghz_worker_start(subghz->txrx->worker);
    subghz->txrx->txrx_state = SubGhzTxRxStateRx;
    return value;
}

static bool subghz_tx(SubGhz* subghz, uint32_t frequency) {
    furi_assert(subghz);
    if(!furi_hal_subghz_is_frequency_valid(frequency)) {
        furi_crash(NULL);
    }
    furi_assert(subghz->txrx->txrx_state != SubGhzTxRxStateSleep);
    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency_and_path(frequency);
    furi_hal_gpio_init(&gpio_cc1101_g0, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_write(&gpio_cc1101_g0, true);
    bool ret = furi_hal_subghz_tx();
    subghz->txrx->txrx_state = SubGhzTxRxStateTx;
    return ret;
}

void subghz_idle(SubGhz* subghz) {
    furi_assert(subghz);
    furi_assert(subghz->txrx->txrx_state != SubGhzTxRxStateSleep);
    furi_hal_subghz_idle();
    subghz->txrx->txrx_state = SubGhzTxRxStateIDLE;
}

void subghz_rx_end(SubGhz* subghz) {
    furi_assert(subghz);
    furi_assert(subghz->txrx->txrx_state == SubGhzTxRxStateRx);
    if(subghz_worker_is_running(subghz->txrx->worker)) {
        subghz_worker_stop(subghz->txrx->worker);
        furi_hal_subghz_stop_async_rx();
    }
    furi_hal_subghz_idle();
    subghz->txrx->txrx_state = SubGhzTxRxStateIDLE;
}

void subghz_sleep(SubGhz* subghz) {
    furi_assert(subghz);
    furi_hal_subghz_sleep();
    subghz->txrx->txrx_state = SubGhzTxRxStateSleep;
}

bool subghz_tx_start(SubGhz* subghz, FlipperFormat* flipper_format) {
    furi_assert(subghz);

    bool ret = false;
    string_t temp_str;
    string_init(temp_str);
    uint32_t repeat = 200;
    do {
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }
        if(!flipper_format_read_string(flipper_format, "Protocol", temp_str)) {
            FURI_LOG_E(TAG, "Missing Protocol");
            break;
        }
        //ToDo FIX
        if(!flipper_format_insert_or_update_uint32(flipper_format, "Repeat", &repeat, 1)) {
            FURI_LOG_E(TAG, "Unable Repeat");
            break;
        }

        subghz->txrx->transmitter =
            subghz_transmitter_alloc_init(subghz->txrx->environment, string_get_cstr(temp_str));

        if(subghz->txrx->transmitter) {
            if(subghz_transmitter_deserialize(subghz->txrx->transmitter, flipper_format)) {
                if(subghz->txrx->preset) {
                    subghz_begin(subghz, subghz->txrx->preset);
                } else {
                    subghz_begin(subghz, FuriHalSubGhzPresetOok270Async);
                }
                if(subghz->txrx->frequency) {
                    ret = subghz_tx(subghz, subghz->txrx->frequency);
                } else {
                    ret = subghz_tx(subghz, 433920000);
                }
                if(ret) {
                    //Start TX
                    furi_hal_subghz_start_async_tx(
                        subghz_transmitter_yield, subghz->txrx->transmitter);
                }
            }
        }
        if(!ret) {
            subghz_transmitter_free(subghz->txrx->transmitter);
            subghz_idle(subghz);
        }

    } while(false);
    string_clear(temp_str);
    return ret;
}

void subghz_tx_stop(SubGhz* subghz) {
    furi_assert(subghz);
    furi_assert(subghz->txrx->txrx_state == SubGhzTxRxStateTx);
    //Stop TX
    furi_hal_subghz_stop_async_tx();
    subghz_transmitter_stop(subghz->txrx->transmitter);
    subghz_transmitter_free(subghz->txrx->transmitter);

    //if protocol dynamic then we save the last upload
    if((subghz->txrx->decoder_result->protocol->type == SubGhzProtocolTypeDynamic) &&
       (strcmp(subghz->file_name, ""))) {
        subghz_save_protocol_to_file(subghz, subghz->txrx->fff_data, subghz->file_name);
    }
    subghz_idle(subghz);
    notification_message(subghz->notifications, &sequence_reset_red);
}

bool subghz_key_load(SubGhz* subghz, const char* file_path) {
    furi_assert(subghz);
    furi_assert(file_path);

    Storage* storage = furi_record_open("storage");
    FlipperFormat* fff_data_file = flipper_format_file_alloc(storage);
    Stream* fff_data_stream = flipper_format_get_raw_stream(subghz->txrx->fff_data);

    bool loaded = false;
    string_t temp_str;
    string_init(temp_str);
    uint32_t version;

    do {
        stream_clean(fff_data_stream);
        if(!flipper_format_file_open_existing(fff_data_file, file_path)) {
            FURI_LOG_E(TAG, "Error open file %s", file_path);
            break;
        }

        if(!flipper_format_read_header(fff_data_file, temp_str, &version)) {
            FURI_LOG_E(TAG, "Missing or incorrect header");
            break;
        }

        if(((!strcmp(string_get_cstr(temp_str), SUBGHZ_KEY_FILE_TYPE)) ||
            (!strcmp(string_get_cstr(temp_str), SUBGHZ_RAW_FILE_TYPE))) &&
           version == SUBGHZ_KEY_FILE_VERSION) {
        } else {
            FURI_LOG_E(TAG, "Type or version mismatch");
            break;
        }

        if(!flipper_format_read_uint32(
               fff_data_file, "Frequency", (uint32_t*)&subghz->txrx->frequency, 1)) {
            FURI_LOG_E(TAG, "Missing Frequency");
            break;
        }

        if(!flipper_format_read_string(fff_data_file, "Preset", temp_str)) {
            FURI_LOG_E(TAG, "Missing Preset");
            break;
        }
        if(!subghz_set_preset(subghz, string_get_cstr(temp_str))) {
            break;
        }

        if(!flipper_format_read_string(fff_data_file, "Protocol", temp_str)) {
            FURI_LOG_E(TAG, "Missing Protocol");
            break;
        }
        if(!strcmp(string_get_cstr(temp_str), "RAW")) {
            //if RAW
            string_t file_name;
            string_init(file_name);
            path_extract_filename_no_ext(file_path, file_name);
            subghz_protocol_raw_gen_fff_data(subghz->txrx->fff_data, string_get_cstr(file_name));
            string_clear(file_name);

        } else {
            stream_copy_full(
                flipper_format_get_raw_stream(fff_data_file),
                flipper_format_get_raw_stream(subghz->txrx->fff_data));
        }

        subghz->txrx->decoder_result = subghz_receiver_search_decoder_base_by_name(
            subghz->txrx->receiver, string_get_cstr(temp_str));
        if(subghz->txrx->decoder_result) {
            subghz_protocol_decoder_base_deserialize(
                subghz->txrx->decoder_result, subghz->txrx->fff_data);
        }

        loaded = true;
    } while(0);

    if(!loaded) {
        dialog_message_show_storage_error(subghz->dialogs, "Cannot parse\nfile");
    }

    string_clear(temp_str);
    flipper_format_free(fff_data_file);
    furi_record_close("storage");

    return loaded;
}

bool subghz_get_next_name_file(SubGhz* subghz) {
    furi_assert(subghz);

    Storage* storage = furi_record_open("storage");
    string_t temp_str;
    string_init(temp_str);
    bool res = false;

    if(strcmp(subghz->file_name, "")) {
        //get the name of the next free file
        storage_get_next_filename(
            storage, SUBGHZ_RAW_FOLDER, subghz->file_name, SUBGHZ_APP_EXTENSION, temp_str);

        strcpy(subghz->file_name, string_get_cstr(temp_str));
        res = true;
    }

    string_clear(temp_str);
    furi_record_close("storage");

    return res;
}

bool subghz_save_protocol_to_file(
    SubGhz* subghz,
    FlipperFormat* flipper_format,
    const char* dev_name) {
    furi_assert(subghz);
    furi_assert(flipper_format);
    furi_assert(dev_name);

    Storage* storage = furi_record_open("storage");
    Stream* flipper_format_stream = flipper_format_get_raw_stream(flipper_format);

    string_t dev_file_name;
    string_init(dev_file_name);
    bool saved = false;

    do {
        //removing additional fields
        flipper_format_delete_key(flipper_format, "Repeat");
        flipper_format_delete_key(flipper_format, "Manufacture");

        // Create subghz folder directory if necessary
        if(!storage_simply_mkdir(storage, SUBGHZ_APP_FOLDER)) {
            dialog_message_show_storage_error(subghz->dialogs, "Cannot create\nfolder");
            break;
        }

        // First remove subghz device file if it was saved
        string_printf(dev_file_name, "%s/%s%s", SUBGHZ_APP_FOLDER, dev_name, SUBGHZ_APP_EXTENSION);

        if(!storage_simply_remove(storage, string_get_cstr(dev_file_name))) {
            break;
        }
        //ToDo check Write
        stream_seek(flipper_format_stream, 0, StreamOffsetFromStart);
        stream_save_to_file(
            flipper_format_stream, storage, string_get_cstr(dev_file_name), FSOM_CREATE_ALWAYS);

        saved = true;
    } while(0);

    string_clear(dev_file_name);
    furi_record_close("storage");
    return saved;
}

bool subghz_load_protocol_from_file(SubGhz* subghz) {
    furi_assert(subghz);

    string_t file_name;
    string_init(file_name);

    // Input events and views are managed by file_select
    bool res = dialog_file_select_show(
        subghz->dialogs,
        SUBGHZ_APP_FOLDER,
        SUBGHZ_APP_EXTENSION,
        subghz->file_name,
        sizeof(subghz->file_name),
        NULL);

    if(res) {
        string_printf(
            file_name, "%s/%s%s", SUBGHZ_APP_FOLDER, subghz->file_name, SUBGHZ_APP_EXTENSION);

        res = subghz_key_load(subghz, string_get_cstr(file_name));
    }

    string_clear(file_name);

    return res;
}

bool subghz_rename_file(SubGhz* subghz) {
    furi_assert(subghz);
    bool ret = true;
    string_t old_path;
    string_t new_path;

    Storage* storage = furi_record_open("storage");

    string_init_printf(
        old_path, "%s/%s%s", SUBGHZ_APP_FOLDER, subghz->file_name_tmp, SUBGHZ_APP_EXTENSION);

    string_init_printf(
        new_path, "%s/%s%s", SUBGHZ_APP_FOLDER, subghz->file_name, SUBGHZ_APP_EXTENSION);

    FS_Error fs_result =
        storage_common_rename(storage, string_get_cstr(old_path), string_get_cstr(new_path));

    if(fs_result != FSE_OK) {
        dialog_message_show_storage_error(subghz->dialogs, "Cannot rename\n file/directory");
        ret = false;
    }

    string_clear(old_path);
    string_clear(new_path);
    furi_record_close("storage");

    return ret;
}

bool subghz_delete_file(SubGhz* subghz) {
    furi_assert(subghz);

    Storage* storage = furi_record_open("storage");
    string_t file_path;
    string_init_printf(
        file_path, "%s/%s%s", SUBGHZ_APP_FOLDER, subghz->file_name_tmp, SUBGHZ_APP_EXTENSION);
    bool result = storage_simply_remove(storage, string_get_cstr(file_path));
    furi_record_close("storage");

    subghz_file_name_clear(subghz);

    return result;
}

void subghz_file_name_clear(SubGhz* subghz) {
    furi_assert(subghz);
    memset(subghz->file_name, 0, sizeof(subghz->file_name));
    memset(subghz->file_name_tmp, 0, sizeof(subghz->file_name_tmp));
}

uint32_t subghz_random_serial(void) {
    static bool rand_generator_inited = false;

    if(!rand_generator_inited) {
        srand(DWT->CYCCNT);
        rand_generator_inited = true;
    }
    return (uint32_t)rand();
}

void subghz_hopper_update(SubGhz* subghz) {
    furi_assert(subghz);

    switch(subghz->txrx->hopper_state) {
    case SubGhzHopperStateOFF:
        return;
        break;
    case SubGhzHopperStatePause:
        return;
        break;
    case SubGhzHopperStateRSSITimeOut:
        if(subghz->txrx->hopper_timeout != 0) {
            subghz->txrx->hopper_timeout--;
            return;
        }
        break;
    default:
        break;
    }
    float rssi = -127.0f;
    if(subghz->txrx->hopper_state != SubGhzHopperStateRSSITimeOut) {
        // See RSSI Calculation timings in CC1101 17.3 RSSI
        rssi = furi_hal_subghz_get_rssi();

        // Stay if RSSI is high enough
        if(rssi > -90.0f) {
            subghz->txrx->hopper_timeout = 10;
            subghz->txrx->hopper_state = SubGhzHopperStateRSSITimeOut;
            return;
        }
    } else {
        subghz->txrx->hopper_state = SubGhzHopperStateRunnig;
    }

    // Select next frequency
    if(subghz->txrx->hopper_idx_frequency < subghz_hopper_frequencies_count - 1) {
        subghz->txrx->hopper_idx_frequency++;
    } else {
        subghz->txrx->hopper_idx_frequency = 0;
    }

    if(subghz->txrx->txrx_state == SubGhzTxRxStateRx) {
        subghz_rx_end(subghz);
    };
    if(subghz->txrx->txrx_state == SubGhzTxRxStateIDLE) {
        subghz_receiver_reset(subghz->txrx->receiver);
        subghz->txrx->frequency = subghz_hopper_frequencies[subghz->txrx->hopper_idx_frequency];
        subghz_rx(subghz, subghz->txrx->frequency);
    }
}
