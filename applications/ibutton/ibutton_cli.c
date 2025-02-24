#include <furi.h>
#include <furi_hal.h>
#include <stdarg.h>
#include <cli/cli.h>
#include <lib/toolbox/args.h>
#include <one_wire/ibutton/ibutton_worker.h>
#include <one_wire/one_wire_host.h>

void ibutton_cli(Cli* cli, string_t args, void* context);
void onewire_cli(Cli* cli, string_t args, void* context);

// app cli function
void ibutton_on_system_start() {
#ifdef SRV_CLI
    Cli* cli = furi_record_open("cli");
    cli_add_command(cli, "ikey", CliCommandFlagDefault, ibutton_cli, cli);
    cli_add_command(cli, "onewire", CliCommandFlagDefault, onewire_cli, cli);
    furi_record_close("cli");
#endif
}

void ibutton_cli_print_usage() {
    printf("Usage:\r\n");
    printf("ikey read\r\n");
    printf("ikey emulate <key_type> <key_data>\r\n");
    printf("ikey write Dallas <key_data>\r\n");
    printf("\t<key_type> choose from:\r\n");
    printf("\tDallas (8 bytes key_data)\r\n");
    printf("\tCyfral (2 bytes key_data)\r\n");
    printf("\tMetakom (4 bytes key_data), must contain correct parity\r\n");
    printf("\t<key_data> are hex-formatted\r\n");
};

bool ibutton_cli_get_key_type(string_t data, iButtonKeyType* type) {
    bool result = false;

    if(string_cmp_str(data, "Dallas") == 0 || string_cmp_str(data, "dallas") == 0) {
        result = true;
        *type = iButtonKeyDS1990;
    } else if(string_cmp_str(data, "Cyfral") == 0 || string_cmp_str(data, "cyfral") == 0) {
        result = true;
        *type = iButtonKeyCyfral;
    } else if(string_cmp_str(data, "Metakom") == 0 || string_cmp_str(data, "metakom") == 0) {
        result = true;
        *type = iButtonKeyMetakom;
    }

    return result;
}

void ibutton_cli_print_key_data(iButtonKey* key) {
    const uint8_t* key_data = ibutton_key_get_data_p(key);
    iButtonKeyType type = ibutton_key_get_type(key);

    printf("%s ", ibutton_key_get_string_by_type(type));
    for(size_t i = 0; i < ibutton_key_get_size_by_type(type); i++) {
        printf("%02X", key_data[i]);
    }

    printf("\r\n");
}

#define EVENT_FLAG_IBUTTON_COMPLETE (1 << 0)

static void ibutton_cli_worker_read_cb(void* context) {
    furi_assert(context);
    osEventFlagsId_t event = context;
    osEventFlagsSet(event, EVENT_FLAG_IBUTTON_COMPLETE);
}

void ibutton_cli_read(Cli* cli) {
    iButtonKey* key = ibutton_key_alloc();
    iButtonWorker* worker = ibutton_worker_alloc();
    osEventFlagsId_t event = osEventFlagsNew(NULL);

    ibutton_worker_start_thread(worker);
    ibutton_worker_read_set_callback(worker, ibutton_cli_worker_read_cb, event);

    printf("Reading iButton...\r\nPress Ctrl+C to abort\r\n");
    ibutton_worker_read_start(worker, key);
    while(true) {
        uint32_t flags = osEventFlagsWait(event, EVENT_FLAG_IBUTTON_COMPLETE, osFlagsWaitAny, 100);

        if(flags & EVENT_FLAG_IBUTTON_COMPLETE) {
            ibutton_cli_print_key_data(key);

            if(ibutton_key_get_type(key) == iButtonKeyDS1990) {
                if(!ibutton_key_dallas_crc_is_valid(key)) {
                    printf("Warning: invalid CRC\r\n");
                }

                if(!ibutton_key_dallas_is_1990_key(key)) {
                    printf("Warning: not a key\r\n");
                }
            }
            break;
        }

        if(cli_cmd_interrupt_received(cli)) break;
    }
    ibutton_worker_stop(worker);

    ibutton_worker_stop_thread(worker);
    ibutton_worker_free(worker);
    ibutton_key_free(key);

    osEventFlagsDelete(event);
};

typedef struct {
    osEventFlagsId_t event;
    iButtonWorkerWriteResult result;
} iButtonWriteContext;

static void ibutton_cli_worker_write_cb(void* context, iButtonWorkerWriteResult result) {
    furi_assert(context);
    iButtonWriteContext* write_context = (iButtonWriteContext*)context;
    write_context->result = result;
    osEventFlagsSet(write_context->event, EVENT_FLAG_IBUTTON_COMPLETE);
}

void ibutton_cli_write(Cli* cli, string_t args) {
    iButtonKey* key = ibutton_key_alloc();
    iButtonWorker* worker = ibutton_worker_alloc();
    iButtonKeyType type;
    iButtonWriteContext write_context;
    uint8_t key_data[IBUTTON_KEY_DATA_SIZE];
    string_t data;

    write_context.event = osEventFlagsNew(NULL);

    string_init(data);
    ibutton_worker_start_thread(worker);
    ibutton_worker_write_set_callback(worker, ibutton_cli_worker_write_cb, &write_context);

    do {
        if(!args_read_string_and_trim(args, data)) {
            ibutton_cli_print_usage();
            break;
        }

        if(!ibutton_cli_get_key_type(data, &type)) {
            ibutton_cli_print_usage();
            break;
        }

        if(type != iButtonKeyDS1990) {
            ibutton_cli_print_usage();
            break;
        }

        if(!args_read_hex_bytes(args, key_data, ibutton_key_get_size_by_type(type))) {
            ibutton_cli_print_usage();
            break;
        }

        ibutton_key_set_type(key, type);
        ibutton_key_set_data(key, key_data, ibutton_key_get_size_by_type(type));

        printf("Writing key ");
        ibutton_cli_print_key_data(key);
        printf("Press Ctrl+C to abort\r\n");

        ibutton_worker_write_start(worker, key);
        while(true) {
            uint32_t flags = osEventFlagsWait(
                write_context.event, EVENT_FLAG_IBUTTON_COMPLETE, osFlagsWaitAny, 100);

            if(flags & EVENT_FLAG_IBUTTON_COMPLETE) {
                if(write_context.result == iButtonWorkerWriteSameKey ||
                   write_context.result == iButtonWorkerWriteOK) {
                    printf("Write success\r\n");
                    break;
                } else if(write_context.result == iButtonWorkerWriteCannotWrite) {
                    printf("Write fail\r\n");
                    break;
                }
            }

            if(cli_cmd_interrupt_received(cli)) break;
        }
        ibutton_worker_stop(worker);
    } while(false);

    string_clear(data);
    ibutton_worker_stop_thread(worker);
    ibutton_worker_free(worker);
    ibutton_key_free(key);

    osEventFlagsDelete(write_context.event);
};

void ibutton_cli_emulate(Cli* cli, string_t args) {
    iButtonKey* key = ibutton_key_alloc();
    iButtonWorker* worker = ibutton_worker_alloc();
    iButtonKeyType type;
    uint8_t key_data[IBUTTON_KEY_DATA_SIZE];
    string_t data;

    string_init(data);
    ibutton_worker_start_thread(worker);

    do {
        if(!args_read_string_and_trim(args, data)) {
            ibutton_cli_print_usage();
            break;
        }

        if(!ibutton_cli_get_key_type(data, &type)) {
            ibutton_cli_print_usage();
            break;
        }

        if(!args_read_hex_bytes(args, key_data, ibutton_key_get_size_by_type(type))) {
            ibutton_cli_print_usage();
            break;
        }

        ibutton_key_set_type(key, type);
        ibutton_key_set_data(key, key_data, ibutton_key_get_size_by_type(type));

        printf("Emulating key ");
        ibutton_cli_print_key_data(key);
        printf("Press Ctrl+C to abort\r\n");

        ibutton_worker_emulate_start(worker, key);
        while(!cli_cmd_interrupt_received(cli)) {
            furi_hal_delay_ms(100);
        };
        ibutton_worker_stop(worker);
    } while(false);

    string_clear(data);
    ibutton_worker_stop_thread(worker);
    ibutton_worker_free(worker);
    ibutton_key_free(key);
};

void ibutton_cli(Cli* cli, string_t args, void* context) {
    string_t cmd;
    string_init(cmd);

    if(!args_read_string_and_trim(args, cmd)) {
        string_clear(cmd);
        ibutton_cli_print_usage();
        return;
    }

    if(string_cmp_str(cmd, "read") == 0) {
        ibutton_cli_read(cli);
    } else if(string_cmp_str(cmd, "write") == 0) {
        ibutton_cli_write(cli, args);
    } else if(string_cmp_str(cmd, "emulate") == 0) {
        ibutton_cli_emulate(cli, args);
    } else {
        ibutton_cli_print_usage();
    }

    string_clear(cmd);
}

void onewire_cli_print_usage() {
    printf("Usage:\r\n");
    printf("onewire search\r\n");
};

void onewire_cli_search(Cli* cli) {
    OneWireHost* onewire = onewire_host_alloc();
    uint8_t address[8];
    bool done = false;

    printf("Search started\r\n");

    onewire_host_start(onewire);
    furi_hal_power_enable_otg();

    while(!done) {
        if(onewire_host_search(onewire, address, NORMAL_SEARCH) != 1) {
            printf("Search finished\r\n");
            onewire_host_reset_search(onewire);
            done = true;
        } else {
            printf("Found: ");
            for(uint8_t i = 0; i < 8; i++) {
                printf("%02X", address[i]);
            }
            printf("\r\n");
        }
        furi_hal_delay_ms(100);
    }

    furi_hal_power_disable_otg();
    onewire_host_free(onewire);
}

void onewire_cli(Cli* cli, string_t args, void* context) {
    string_t cmd;
    string_init(cmd);

    if(!args_read_string_and_trim(args, cmd)) {
        string_clear(cmd);
        onewire_cli_print_usage();
        return;
    }

    if(string_cmp_str(cmd, "search") == 0) {
        onewire_cli_search(cli);
    }

    string_clear(cmd);
}
