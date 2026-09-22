#include "svs_flasher.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "psa/crypto.h"

#include "svs_usb.h"

namespace svs_flasher {

static const char *TAG = "svs_flasher";

// ATmega328P
static const uint16_t MCU_ID = 119;  // avrdude/urboot MCU id
static const size_t FLASH_SIZE = 32768;
static const size_t PAGE_SIZE = 128;

static const uint32_t FLASH_BAUD = 115200;

// Protocol bytes (STK500v1 values, reused by urprotocol)
static const uint8_t STK_INSYNC = 0x14;
static const uint8_t STK_OK = 0x10;
static const uint8_t EOP = 0x20;
static const uint8_t CMD_GET_SYNC = 0x30;
static const uint8_t CMD_LEAVE_PROGMODE = 0x51;
static const uint8_t UR_PROG_PAGE_FL = 0x02;
static const uint8_t UR_READ_PAGE_FL = 0x03;

// Feature bits encoded in the sync reply
static const uint16_t UB_N_MCU = 2040;
static const uint16_t UB_READ_FLASH = 4;
static const uint16_t UB_FLASH_LL_NOR = 8;

// Timeouts (ms). avrdude waits 25 ms for a sync reply on a PC; through the
// ESP32's USB host and the CH340 replies arrive later, and the bootloader
// only listens for about half a second after reset, so wait longer per
// attempt but retry back to back instead of backing off.
static const uint32_t SYNC_TIMEOUT = 80;
static const uint32_t DRAIN_QUIET = 20;
static const uint32_t CMD_TIMEOUT = 500;
static const int MAX_SYNC_ATTEMPTS = 20;

// Reset timings to try, in order: DTR pulse length, then wait before syncing
struct ResetTiming {
    int pulse_ms;
    int wait_ms;
};
static const ResetTiming RESET_TIMINGS[] = {{50, 30}, {100, 150}};
static const int BANNER_WAIT_MS = 8000;  // for the SVS to boot and print its version

// What the bootloader reveals about itself: the two sync reply bytes and the
// 6-byte table at the top of flash. Two identifications of the same SVS give
// identical values.
struct BootInfo {
    uint8_t insync = 0;
    uint8_t ok = 0;
    uint8_t table[6] = {};
    bool table_read = false;
};

static SemaphoreHandle_t s_mutex = nullptr;
static std::vector<uint8_t> s_image;  // staged flash image, 0xFF-filled gaps
static Image s_image_info = {false, "", 0, ""};
static const Device NO_DEVICE = {false, false, false, false, "", "", 0, 0};
static Device s_device = NO_DEVICE;
static BootInfo s_checked_boot;         // from the last successful check
static uint32_t s_checked_connection = 0;
static Task s_task = Task::Idle;
static std::string s_phase;
static int s_progress = 0;
static std::string s_result;
static bool s_result_ok = false;
static Task s_result_of = Task::Idle;

// Sync bytes of the current bootloader session, used to frame replies
static uint8_t s_insync = 0;
static uint8_t s_ok = 0;

// ---------------------------------------------------------------------------
// Status helpers
// ---------------------------------------------------------------------------

static void set_phase(const char *phase, int progress)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_phase = phase;
    s_progress = progress;
    xSemaphoreGive(s_mutex);
}

static void finish(const std::string &result, bool ok)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_result_of = s_task;
    s_task = Task::Idle;
    s_phase.clear();
    s_result = result;
    s_result_ok = ok;
    xSemaphoreGive(s_mutex);
    if (ok) {
        ESP_LOGI(TAG, "%s", result.c_str());
    } else {
        ESP_LOGE(TAG, "%s", result.c_str());
    }
}

// Waits for the SVS to print its banner after leaving the bootloader
static std::string wait_for_banner(uint32_t boots_before)
{
    for (int i = 0; i < BANNER_WAIT_MS / 100; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        svs_usb::Info info = svs_usb::info();
        if (info.boots_seen != boots_before) {
            return info.firmware;
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// Intel HEX
// ---------------------------------------------------------------------------

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Decodes an Intel HEX file into a flash image, checking every record
static bool decode_hex(const char *hex, size_t len, std::vector<uint8_t> &image, std::string *error)
{
    std::vector<uint8_t> flash(FLASH_SIZE, 0xFF);
    size_t top = 0;  // one past the highest written address
    uint32_t base = 0;
    bool eof = false;
    int line_no = 0;
    size_t pos = 0;
    char msg[96];

    while (pos < len && !eof) {
        size_t end = pos;
        while (end < len && hex[end] != '\n') {
            end++;
        }
        const char *line = hex + pos;
        size_t n = end - pos;
        pos = end + 1;
        line_no++;
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) {
            n--;
        }
        if (n == 0) {
            continue;
        }

        if (line[0] != ':' || n < 11 || (n - 1) % 2 != 0) {
            snprintf(msg, sizeof(msg), "Line %d is not a valid Intel HEX record", line_no);
            *error = msg;
            return false;
        }
        uint8_t rec[260];
        size_t rec_len = (n - 1) / 2;
        if (rec_len > sizeof(rec)) {
            snprintf(msg, sizeof(msg), "Line %d is too long", line_no);
            *error = msg;
            return false;
        }
        uint8_t sum = 0;
        for (size_t i = 0; i < rec_len; i++) {
            int hi = hex_nibble(line[1 + 2 * i]);
            int lo = hex_nibble(line[2 + 2 * i]);
            if (hi < 0 || lo < 0) {
                snprintf(msg, sizeof(msg), "Line %d has invalid characters", line_no);
                *error = msg;
                return false;
            }
            rec[i] = (uint8_t)(hi << 4 | lo);
            sum += rec[i];
        }
        uint8_t count = rec[0];
        if (rec_len != (size_t)count + 5) {
            snprintf(msg, sizeof(msg), "Line %d has a wrong length", line_no);
            *error = msg;
            return false;
        }
        if (sum != 0) {
            snprintf(msg, sizeof(msg), "Line %d has a wrong checksum (corrupted file?)", line_no);
            *error = msg;
            return false;
        }
        uint16_t addr = (uint16_t)(rec[1] << 8 | rec[2]);
        uint8_t type = rec[3];
        const uint8_t *data = rec + 4;

        bool address_record = type == 0x02 || type == 0x04;
        if (address_record && count != 2) {
            snprintf(msg, sizeof(msg), "Line %d has a malformed address record", line_no);
            *error = msg;
            return false;
        }

        switch (type) {
        case 0x00: {  // data
            uint32_t start = base + addr;
            if (start + count > FLASH_SIZE) {
                snprintf(msg, sizeof(msg), "Line %d writes past the ATmega328P flash (0x%05X)",
                         line_no, (unsigned)(start + count));
                *error = msg;
                return false;
            }
            memcpy(&flash[start], data, count);
            if (start + count > top) {
                top = start + count;
            }
            break;
        }
        case 0x01:  // end of file
            eof = true;
            break;
        case 0x02:  // extended segment address
            base = (uint32_t)(data[0] << 8 | data[1]) << 4;
            break;
        case 0x04:  // extended linear address
            base = (uint32_t)(data[0] << 8 | data[1]) << 16;
            break;
        case 0x03:  // start segment address
        case 0x05:  // start linear address: irrelevant for flashing
            break;
        default:
            snprintf(msg, sizeof(msg), "Line %d has an unsupported record (type 0x%02X)", line_no, type);
            *error = msg;
            return false;
        }
    }

    if (!eof) {
        *error = "The file has no end record (truncated?)";
        return false;
    }
    if (top == 0) {
        *error = "The file contains no data";
        return false;
    }
    flash.resize(top);
    image.swap(flash);
    return true;
}

static std::string sha256_hex(const std::vector<uint8_t> &data)
{
    uint8_t hash[32];
    size_t hash_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, data.data(), data.size(), hash, sizeof(hash), &hash_len) !=
        PSA_SUCCESS) {
        return "";
    }
    std::string out;
    for (size_t i = 0; i < hash_len; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", hash[i]);
        out += hex;
    }
    return out;
}

// ---------------------------------------------------------------------------
// urclock protocol
// ---------------------------------------------------------------------------

// Pulses DTR/RTS like avrdude -c urclock: the CH340's DTR resets the AVR
static esp_err_t reset_into_bootloader(const ResetTiming &timing)
{
    esp_err_t err = svs_usb::raw_set_lines(false, false);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));
    err = svs_usb::raw_set_lines(true, true);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(timing.pulse_ms));
    err = svs_usb::raw_set_lines(false, false);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(timing.wait_ms));
    return ESP_OK;
}

// Follows avrdude's urclock_getsync(): repeat the sync request until two
// consecutive identical, distinct reply bytes arrive. What came back is
// summarized in *trace for the log.
static bool get_sync(std::string *trace)
{
    bool seen = false;
    bool synced = false;
    int silent = 0;
    for (int attempt = 0; attempt < MAX_SYNC_ATTEMPTS && !synced; attempt++) {
        uint8_t req[2] = {attempt == 0 ? CMD_GET_SYNC : EOP, EOP};
        if (svs_usb::raw_write(req, sizeof(req)) != ESP_OK) {
            *trace += "write failed";
            return false;
        }
        uint8_t resp[2];
        size_t got = svs_usb::raw_read(resp, 2, SYNC_TIMEOUT);
        if (got == 2) {
            char hex[12];
            snprintf(hex, sizeof(hex), "%02X %02X, ", resp[0], resp[1]);
            *trace += hex;
            if (!seen || resp[0] != s_insync || resp[1] != s_ok || resp[0] == resp[1]) {
                s_insync = resp[0];
                s_ok = resp[1];
                svs_usb::raw_drain(DRAIN_QUIET);  // guard against line noise
                seen = true;
            } else {
                synced = true;
            }
        } else {
            silent++;
            if (got == 1) {
                char hex[16];
                snprintf(hex, sizeof(hex), "%02X (1 byte), ", resp[0]);
                *trace += hex;
            }
        }
    }
    if (silent > 0) {
        *trace += std::to_string(silent) + " without reply";
    }
    if (!synced) {
        return false;
    }
    // We could be one byte out of step: send an extra EOP, and a second one
    // if the first got no reply
    uint8_t eop = EOP;
    uint8_t dummy;
    svs_usb::raw_write(&eop, 1);
    if (svs_usb::raw_read(&dummy, 1, SYNC_TIMEOUT) == 0) {
        svs_usb::raw_write(&eop, 1);
    }
    svs_usb::raw_drain(DRAIN_QUIET);
    return true;
}

// Sends a command and checks the INSYNC ... OK framing around resp_len bytes
static esp_err_t command(const uint8_t *cmd, size_t cmd_len, uint8_t *resp, size_t resp_len)
{
    esp_err_t err = svs_usb::raw_write(cmd, cmd_len);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t b;
    if (svs_usb::raw_read(&b, 1, CMD_TIMEOUT) != 1 || b != s_insync) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (resp_len > 0 && svs_usb::raw_read(resp, resp_len, CMD_TIMEOUT) != resp_len) {
        return ESP_ERR_TIMEOUT;
    }
    if (svs_usb::raw_read(&b, 1, CMD_TIMEOUT) != 1 || b != s_ok) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t read_flash(uint16_t addr, uint8_t *buf, size_t len)
{
    uint8_t cmd[5] = {UR_READ_PAGE_FL, (uint8_t)(addr & 0xFF), (uint8_t)(addr >> 8),
                      (uint8_t)len /* 256 is sent as 0 */, EOP};
    return command(cmd, sizeof(cmd), buf, len);
}

static esp_err_t write_page(uint16_t addr, const uint8_t *page)
{
    uint8_t cmd[4 + PAGE_SIZE + 1];
    cmd[0] = UR_PROG_PAGE_FL;
    cmd[1] = addr & 0xFF;
    cmd[2] = addr >> 8;
    cmd[3] = PAGE_SIZE;
    memcpy(cmd + 4, page, PAGE_SIZE);
    cmd[4 + PAGE_SIZE] = EOP;
    return command(cmd, sizeof(cmd), nullptr, 0);
}

// Resets the SVS into its bootloader and syncs, trying each reset timing.
// Logs what happened so a failure can be diagnosed from the web UI.
static bool enter_bootloader()
{
    for (size_t i = 0; i < sizeof(RESET_TIMINGS) / sizeof(RESET_TIMINGS[0]); i++) {
        const ResetTiming &t = RESET_TIMINGS[i];
        std::string trace;
        bool ok = reset_into_bootloader(t) == ESP_OK && get_sync(&trace);
        char head[80];
        snprintf(head, sizeof(head), "Bootloader sync (reset %d ms, wait %d ms): %s. Replies: ",
                 t.pulse_ms, t.wait_ms, ok ? "OK" : "no answer");
        svs_usb::log_note(head + (trace.empty() ? std::string("none") : trace));
        if (ok) {
            return true;
        }
        svs_usb::raw_drain(DRAIN_QUIET);
    }
    return false;
}

static void leave_bootloader()
{
    uint8_t leave[2] = {CMD_LEAVE_PROGMODE, EOP};
    command(leave, sizeof(leave), nullptr, 0);
}

// Decodes the sync reply into MCU id and feature bits (see avrdude urclock.c)
static void decode_sync(uint8_t insync, uint8_t ok, uint16_t *mcuid, uint16_t *features)
{
    uint8_t in = insync;
    if (in == 255 && ok == 254) {
        in = STK_INSYNC;
        ok = STK_OK;
    } else if (ok > in) {
        ok--;
    }
    uint16_t info = (uint16_t)(in * 255 + ok);
    *mcuid = info % UB_N_MCU;
    *features = info / UB_N_MCU;
}

// After get_sync(): reads what the bootloader reveals about itself. The table
// is only read when the reply says it is a flash-reading urboot on an
// ATmega328P; evaluate() explains any other case.
static void read_boot_info(BootInfo &bi)
{
    bi.insync = s_insync;
    bi.ok = s_ok;
    bi.table_read = false;
    if (s_insync == STK_INSYNC && s_ok == STK_OK) {
        return;  // classic STK500 bootloader
    }
    uint16_t mcuid, features;
    decode_sync(s_insync, s_ok, &mcuid, &features);
    if (mcuid == MCU_ID && (features & UB_READ_FLASH)) {
        bi.table_read = read_flash(FLASH_SIZE - 6, bi.table, sizeof(bi.table)) == ESP_OK;
    }
}

// ---------------------------------------------------------------------------
// AVR reset-vector patching (ported from avrdude's urclock.c)
//
// The ATmega328P has 32 KB flash (> 8 KB), so its interrupt vectors are 4-byte
// jmp instructions (vecsz 4). urboot's reset-vector protection still writes a
// 2-byte rjmp to the bootloader followed by a 2-byte "ur" marker, because a
// backward rjmp reaches a bootloader at the top of this power-of-two flash.
// These mirror avrdude exactly so the patch is identical to the official tool's.
// ---------------------------------------------------------------------------

static const int VEC_SZ = 4;  // ATmega328P uses 4-byte jmp vectors

static uint32_t buf2u32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

static void u32tobuf(uint8_t *b, uint32_t v)
{
    b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}

static bool is_rjmp(uint16_t op) { return (op & 0xF000) == 0xC000; }
static bool is_jmp(uint16_t op) { return (op & 0xFE0E) == 0x940C; }

static int rjmp_dist_wrap(int dist, int flashsize)
{
    int size = flashsize > 8182 ? 8192 : flashsize;
    if ((size & (size - 1)) == 0) {  // power of two
        dist &= size - 1;
        if (dist >= size / 2) {
            dist -= size;
        }
    }
    return dist;
}

// Byte address an rjmp at address 0 (the reset) jumps to
static int rjmp_reset_target(uint16_t op, int flashsize)
{
    int16_t dist = op & 0x0FFF;
    dist = (int16_t)(dist << 4) >> 3;  // sign-extend 12 bits and multiply by 2
    int addr = rjmp_dist_wrap(dist + 2, flashsize);
    while (addr < 0) addr += flashsize;
    while (addr > flashsize) addr -= flashsize;
    return addr;
}

// Byte address a 4-byte jmp opcode targets
static int jmp_target(const uint8_t *b)
{
    uint32_t op = buf2u32(b);
    int addr = op >> 16;
    addr |= (op & 1) << 16;
    addr |= (op & 0x1F0) << (17 - 4);
    return addr << 1;
}

// 4-byte jmp opcode to a byte address
static uint32_t jmp_opcode(int32_t addr)
{
    return (((addr >> 1) & 0xFFFF) << 16) | 0x940C | (((addr >> 18) & 31) << 4) | ((addr >> 17) & 1);
}

// Reset rjmp that reaches the bootloader (urboot's own formula)
static uint16_t rjmp_to_bootloader(int blstart, int flashsize)
{
    return 0xC000 | ((uint16_t)((blstart - flashsize - 2) / 2) & 0x0FFF);
}

// Patches a flash image so the reset vector points at the bootloader and the
// application's real entry is preserved in the bootloader's chosen vector,
// exactly as avrdude does for this urboot: it writes ONLY the 2-byte rjmp over
// the first word of the reset and leaves the second word (image[2..3]) as it
// came from the .hex — the low word of the original reset jmp, i.e. the app
// address. Confirmed against the SVS: a firmware whose reset is `jmp 0x1A6`
// (0C 94 D3 00) ends up on the chip as 7F CF D3 00. Returns false (and *error)
// without touching the image if the reset opcode is not a jmp/rjmp or the app
// entry is out of range. reset_rjmp is the 2 bytes written over the reset word.
static bool patch_vectors(std::vector<uint8_t> &image, int blstart, int vector_num,
                          uint16_t &reset_rjmp, int &app_start, std::string *error)
{
    char m[96];
    int app_vec_loc = vector_num * VEC_SZ;
    if (image.size() < (size_t)app_vec_loc + VEC_SZ) {
        *error = "Image too small to patch";
        return false;
    }
    uint16_t reset_op16 = (uint16_t)(image[0] | image[1] << 8);
    if (is_jmp(reset_op16)) {
        app_start = jmp_target(&image[0]);
    } else if (is_rjmp(reset_op16)) {
        app_start = rjmp_reset_target(reset_op16, FLASH_SIZE);
    } else {
        snprintf(m, sizeof(m), "Reset word 0x%04X is not a jmp or rjmp; not patching", reset_op16);
        *error = m;
        return false;
    }

    reset_rjmp = rjmp_to_bootloader(blstart, FLASH_SIZE);
    if (app_start == blstart) {
        return true;  // already patched (a fresh .hex should not be)
    }
    if (app_start < app_vec_loc || app_start >= (int)image.size()) {
        snprintf(m, sizeof(m), "App start 0x%04X out of range [0x%04X, 0x%04X); not patching",
                 app_start, app_vec_loc, (unsigned)image.size());
        *error = m;
        return false;
    }

    image[0] = reset_rjmp & 0xFF;                          // reset word -> rjmp to bootloader
    image[1] = reset_rjmp >> 8;                            // image[2..3] left as-is (avrdude does too)
    u32tobuf(&image[app_vec_loc], jmp_opcode(app_start));  // moved-away vector -> app start

    // The patched reset must decode back to the bootloader, or do not proceed
    if (rjmp_reset_target(reset_rjmp, FLASH_SIZE) != blstart) {
        *error = "Internal check failed: patched reset does not reach the bootloader";
        return false;
    }
    return true;
}

static bool same_bootloader(const BootInfo &a, const BootInfo &b)
{
    return a.insync == b.insync && a.ok == b.ok && a.table_read == b.table_read &&
           memcmp(a.table, b.table, sizeof(a.table)) == 0;
}

// Decides whether it is safe to flash this bootloader, and describes it
static Device evaluate(const BootInfo &bi)
{
    Device d = NO_DEVICE;
    d.checked = true;
    char buf[160];

    if (bi.insync == STK_INSYNC && bi.ok == STK_OK) {
        d.summary = "Classic STK500 bootloader (not urboot)";
        d.problem = "The SVS has a classic STK500 bootloader instead of urboot; this bridge only "
                    "flashes urboot. Use the official tool.";
        return d;
    }
    uint16_t mcuid, features;
    decode_sync(bi.insync, bi.ok, &mcuid, &features);
    if (mcuid != MCU_ID) {
        snprintf(buf, sizeof(buf), "urboot on an unexpected microcontroller (MCU id %u)", mcuid);
        d.summary = buf;
        d.problem = "The SVS is not an ATmega328P as expected; not flashing it.";
        return d;
    }
    if (!(features & UB_READ_FLASH)) {
        d.summary = "ATmega328P · urboot without flash read";
        d.problem = "The bootloader cannot read flash, so it can be neither checked nor verified.";
        return d;
    }
    if (features & UB_FLASH_LL_NOR) {
        d.summary = "ATmega328P · urboot without page erase";
        d.problem = "The bootloader does not erase pages when writing (unsupported).";
        return d;
    }
    if (!bi.table_read) {
        d.summary = "ATmega328P · urboot";
        d.problem = "Could not read the bootloader information.";
        return d;
    }

    // Top 6 bytes: pages, vector number, rjmp to pgm_write_page (2), capabilities, version
    uint8_t pages = bi.table[0] & 0x7F;
    uint8_t vector = bi.table[1] & 0x7F;
    uint16_t rjmp = (uint16_t)(bi.table[2] | bi.table[3] << 8);
    uint8_t cap = bi.table[4];
    uint8_t version = bi.table[5];
    size_t bl_size = (size_t)pages * PAGE_SIZE;

    bool is_urboot = version >= 075 && version <= 0147 &&
                     (rjmp == 0x9508 /* ret */ || (rjmp & 0xF000) == 0xC000 /* rjmp */);
    if (!is_urboot || bl_size < 64 || bl_size > 2048) {
        d.summary = "ATmega328P · unrecognized bootloader information";
        d.problem = "The bootloader does not look like urboot v7.5 or later; not flashing it.";
        return d;
    }
    // Up to v7.7 the capability byte also flags vector bootloaders
    bool vector_bl = vector != 0 || (version <= 077 && (cap & 0x0C) != 0);
    snprintf(buf, sizeof(buf), "ATmega328P · urboot v%u.%u · %u-byte bootloader · %s",
             version >> 3, version & 7, (unsigned)bl_size, vector_bl ? "vector boot" : "hardware boot");
    d.summary = buf;
    d.compatible = true;
    d.app_space = FLASH_SIZE - bl_size;
    if (vector_bl) {
        d.vector = true;
        d.vector_num = vector;
        d.problem = "This is a vector bootloader. Before flashing, run the preview: it checks that "
                    "the reset-vector patch the bridge would write matches what is already on the chip.";
    }
    return d;
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------

static void check_task(void *arg)
{
    uint32_t boots_before = svs_usb::info().boots_seen;
    uint32_t connection = svs_usb::info().connections;
    if (svs_usb::raw_begin(FLASH_BAUD) != ESP_OK) {
        finish("The SVS is not available", false);
        vTaskDelete(NULL);
        return;
    }
    set_phase("Entering the bootloader", 0);
    BootInfo bi;
    bool answered = enter_bootloader();
    if (answered) {
        read_boot_info(bi);
        leave_bootloader();
    }
    svs_usb::raw_end();

    if (!answered) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_device = NO_DEVICE;
        xSemaphoreGive(s_mutex);
        finish("The SVS bootloader did not answer. Check the USB connection and try again.", false);
        vTaskDelete(NULL);
        return;
    }

    Device d = evaluate(bi);
    ESP_LOGI(TAG, "Bootloader: %s (sync 0x%02X 0x%02X)", d.summary.c_str(), bi.insync, bi.ok);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_device = d;
    s_checked_boot = bi;
    s_checked_connection = connection;
    xSemaphoreGive(s_mutex);

    set_phase("Waiting for the SVS to start", 0);
    std::string firmware = wait_for_banner(boots_before);
    // The outcome itself shows in the device summary; only report the unusual
    std::string result = d.compatible ? "" : d.problem;
    if (firmware.empty()) {
        result += result.empty() ? "" : " ";
        result += "The SVS did not report its firmware version after restarting.";
    }
    finish(result, d.compatible);
    vTaskDelete(NULL);
}

// Validates the vector patch without writing: reads the reset vector the
// official tool already put on the chip and checks the bridge would write the
// exact same value. This is what makes flashing a vector bootloader safe
// without a sacrificial board.
static void preview_task(void *arg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int blstart = (int)s_device.app_space;
    int vector_num = s_device.vector_num;
    BootInfo checked = s_checked_boot;
    xSemaphoreGive(s_mutex);

    if (svs_usb::raw_begin(FLASH_BAUD) != ESP_OK) {
        finish("The SVS is not available", false);
        vTaskDelete(NULL);
        return;
    }
    set_phase("Reading the reset vector", 0);

    bool ok = false;
    std::string result;
    BootInfo bi;
    if (!enter_bootloader()) {
        result = "The SVS bootloader did not answer";
    } else {
        read_boot_info(bi);
        uint8_t reset[4] = {}, appv[4] = {};
        if (!same_bootloader(bi, checked)) {
            result = "The SVS changed since it was checked; check it again";
        } else if (read_flash(0, reset, VEC_SZ) != ESP_OK) {
            result = "Could not read the reset vector from the SVS";
        } else {
            read_flash(vector_num * VEC_SZ, appv, VEC_SZ);
            // avrdude writes only the 2-byte rjmp over the reset word, so only
            // those two bytes are ours to compare; image[2..3] stays firmware
            // specific (the old app address) and differs between versions
            uint16_t chip_rjmp = (uint16_t)(reset[0] | reset[1] << 8);
            uint16_t computed = rjmp_to_bootloader(blstart, FLASH_SIZE);
            int chip_app = is_jmp((uint16_t)(appv[0] | appv[1] << 8)) ? jmp_target(appv) : -1;
            char msg[240];
            snprintf(msg, sizeof(msg),
                     "Preview: chip reset %02X%02X%02X%02X, bridge rjmp 0x%04X (bytes %02X %02X) "
                     "-> bootloader 0x%04X; current app vector %d -> 0x%05X",
                     reset[0], reset[1], reset[2], reset[3], chip_rjmp, computed & 0xFF, computed >> 8,
                     (unsigned)blstart, vector_num, chip_app);
            svs_usb::log_note(msg);
            ok = chip_rjmp == computed;
            result = ok ? "Preview OK: the bridge's reset rjmp matches what the official tool wrote. "
                          "Flashing this SVS is safe."
                        : "Preview mismatch: the bridge's reset rjmp differs from the chip's. "
                          "NOT flashing; please report this.";
        }
        leave_bootloader();
    }
    svs_usb::raw_end();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_device.vector_verified = ok;
    xSemaphoreGive(s_mutex);
    finish(result, ok);
    vTaskDelete(NULL);
}

static void flash_task(void *arg)
{
    std::vector<uint8_t> image;
    BootInfo checked;
    size_t app_space;
    bool vector;
    int vector_num;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    image = s_image;
    checked = s_checked_boot;
    app_space = s_device.app_space;
    vector = s_device.vector;
    vector_num = s_device.vector_num;
    xSemaphoreGive(s_mutex);

    // Pad to whole pages with erased flash
    size_t pages = (image.size() + PAGE_SIZE - 1) / PAGE_SIZE;
    image.resize(pages * PAGE_SIZE, 0xFF);

    std::string error;
    bool written_any = false;
    uint32_t boots_before = svs_usb::info().boots_seen;
    if (svs_usb::raw_begin(FLASH_BAUD) != ESP_OK) {
        finish("The SVS is not available", false);
        vTaskDelete(NULL);
        return;
    }

    // Must still be the bootloader that was checked
    set_phase("Entering the bootloader", 0);
    BootInfo bi;
    if (!enter_bootloader()) {
        error = "The SVS bootloader did not answer. Nothing was written.";
    } else {
        read_boot_info(bi);
        if (!same_bootloader(bi, checked)) {
            error = "The SVS does not match the one that was checked. Nothing was written; check it again.";
        } else if (image.size() > app_space) {
            error = "The firmware does not fit below the bootloader. Nothing was written.";
        }
    }

    // Vector bootloader: patch the reset vector in the image before writing
    // anything. The reset page is written first (below), so even an interrupted
    // flash leaves the reset vector pointing at the bootloader (recoverable).
    if (error.empty() && vector) {
        uint16_t reset_rjmp;
        int app_start;
        std::string perr;
        if (!patch_vectors(image, (int)app_space, vector_num, reset_rjmp, app_start, &perr)) {
            error = perr + ". Nothing was written.";
        } else {
            char note[150];
            snprintf(note, sizeof(note), "Vector patch: reset rjmp 0x%04X (bootloader 0x%04X), "
                     "app entry 0x%05X saved in vector %d", reset_rjmp, (unsigned)app_space, app_start,
                     vector_num);
            svs_usb::log_note(note);
        }
    }

    // Write
    for (size_t i = 0; error.empty() && i < pages; i++) {
        set_phase("Writing", (int)(i * 50 / pages));
        uint16_t addr = (uint16_t)(i * PAGE_SIZE);
        written_any = true;
        if (write_page(addr, &image[addr]) != ESP_OK) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Writing failed at 0x%04X", addr);
            error = msg;
        }
    }

    // Verify
    uint8_t page[PAGE_SIZE];
    for (size_t i = 0; error.empty() && i < pages; i++) {
        set_phase("Verifying", 50 + (int)(i * 50 / pages));
        uint16_t addr = (uint16_t)(i * PAGE_SIZE);
        if (read_flash(addr, page, PAGE_SIZE) != ESP_OK) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Reading back failed at 0x%04X", addr);
            error = msg;
        } else if (memcmp(page, &image[addr], PAGE_SIZE) != 0) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Verification failed: flash differs at page 0x%04X", addr);
            error = msg;
        }
    }

    if (error.empty()) {
        leave_bootloader();  // starts the new firmware
    }
    svs_usb::raw_end();

    if (!error.empty()) {
        if (written_any) {
            error += ". The SVS firmware is incomplete but its bootloader is intact: flash again.";
        }
        finish(error, false);
        vTaskDelete(NULL);
        return;
    }

    // Like the official utility, confirm with the version the SVS reports
    set_phase("Waiting for the SVS to start", 100);
    std::string running = wait_for_banner(boots_before);
    std::string msg = "SVS firmware written and verified";
    msg += running.empty() ? ", but the SVS did not report its version after restarting"
                           : ". The SVS now runs " + running;
    finish(msg, true);
    vTaskDelete(NULL);
}

// --- Bootloader diagnostics ---------------------------------------------------

static std::string hex_bytes(const uint8_t *b, size_t n, size_t max)
{
    std::string out;
    for (size_t i = 0; i < n && i < max; i++) {
        char h[4];
        snprintf(h, sizeof(h), i ? " %02X" : "%02X", b[i]);
        out += h;
    }
    if (n > max) {
        out += " …";
    }
    return out.empty() ? "nothing" : out;
}

static void pulse_reset(int pulse_ms)
{
    svs_usb::raw_set_lines(true, true);
    vTaskDelay(pdMS_TO_TICKS(pulse_ms));
    svs_usb::raw_set_lines(false, false);
}

// Records what arrives for duration_ms, optionally sending a sync request
// every interval. Reports when bytes first arrived, relative to start.
struct Capture {
    uint8_t bytes[96];
    size_t count = 0;
    int first_ms = -1;
};

static void capture(Capture &cap, int duration_ms, bool send_sync, int64_t start_us)
{
    int64_t end = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    int64_t next_send = 0;
    while (esp_timer_get_time() < end) {
        if (send_sync && esp_timer_get_time() >= next_send) {
            uint8_t req[2] = {CMD_GET_SYNC, EOP};
            svs_usb::raw_write(req, sizeof(req));
            next_send = esp_timer_get_time() + 20000;
        }
        uint8_t buf[32];
        size_t n = svs_usb::raw_read(buf, sizeof(buf), 5);
        if (n > 0 && cap.first_ms < 0) {
            cap.first_ms = (int)((esp_timer_get_time() - start_us) / 1000);
        }
        for (size_t i = 0; i < n && cap.count < sizeof(cap.bytes); i++) {
            cap.bytes[cap.count++] = buf[i];
        }
    }
}

static void log_capture(const char *label, const Capture &cap)
{
    char head[128];
    if (cap.first_ms < 0) {
        snprintf(head, sizeof(head), "Probe %s: nothing received", label);
        svs_usb::log_note(head);
        return;
    }
    snprintf(head, sizeof(head), "Probe %s: first byte %d ms after reset, %u bytes: ", label,
             cap.first_ms, (unsigned)cap.count);
    svs_usb::log_note(head + hex_bytes(cap.bytes, cap.count, 32));
}

static void probe_task(void *arg)
{
    if (svs_usb::raw_begin(FLASH_BAUD) != ESP_OK) {
        finish("The SVS is not available", false);
        vTaskDelete(NULL);
        return;
    }
    svs_usb::log_note("Bootloader diagnostics started; nothing is written to the SVS");

    // 1. Listen only: when does the application start talking after a reset?
    set_phase("Listening after a reset", 0);
    {
        svs_usb::raw_drain(DRAIN_QUIET);
        int64_t t0 = esp_timer_get_time();
        pulse_reset(50);
        Capture cap;
        capture(cap, 1500, false, t0);
        log_capture("listen only @115200", cap);
    }
    vTaskDelay(pdMS_TO_TICKS(1500));

    // 2. avrdude's exact sequence, including the DTR pulse Windows makes when
    //    it opens the port: assert, 20 ms, release, 20 ms, assert, release, 120 ms
    set_phase("avrdude sequence", 0);
    {
        svs_usb::raw_drain(DRAIN_QUIET);
        svs_usb::raw_set_lines(true, true);
        vTaskDelay(pdMS_TO_TICKS(20));
        svs_usb::raw_set_lines(false, false);
        vTaskDelay(pdMS_TO_TICKS(20));
        svs_usb::raw_set_lines(true, true);
        vTaskDelay(pdMS_TO_TICKS(1));
        svs_usb::raw_set_lines(false, false);
        vTaskDelay(pdMS_TO_TICKS(120));
        std::string trace;
        bool ok = get_sync(&trace);
        svs_usb::log_note(std::string("Probe avrdude sequence @115200: ") + (ok ? "SYNC" : "no sync") +
                          ". Replies: " + (trace.empty() ? "none" : trace));
        if (ok) {
            leave_bootloader();
        }
    }
    vTaskDelay(pdMS_TO_TICKS(1500));

    // 3. Burst: sync requests every 20 ms from the moment of the reset, at
    //    several baud rates in case the bootloader does not use 115200
    const uint32_t bauds[] = {115200, 57600, 38400, 19200};
    for (size_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i++) {
        char label[40];
        snprintf(label, sizeof(label), "sync burst @%u", (unsigned)bauds[i]);
        set_phase(label, (int)(i * 25));
        svs_usb::raw_set_baudrate(bauds[i]);
        svs_usb::raw_drain(DRAIN_QUIET);
        int64_t t0 = esp_timer_get_time();
        svs_usb::raw_set_lines(true, true);  // reset, keep sending meanwhile
        Capture cap;
        capture(cap, 30, true, t0);
        svs_usb::raw_set_lines(false, false);
        capture(cap, 970, true, t0);
        log_capture(label, cap);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    svs_usb::raw_end();
    svs_usb::log_note("Bootloader diagnostics finished");
    finish("Diagnostics finished: see the serial log", true);
    vTaskDelete(NULL);
}

static esp_err_t start_task(Task task, TaskFunction_t fn, const char *name)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool can = s_task == Task::Idle && svs_usb::is_connected();
    if (can) {
        s_task = task;
        s_phase = "Starting";
        s_progress = 0;
        s_result.clear();
        s_result_of = Task::Idle;
    }
    xSemaphoreGive(s_mutex);
    if (!can) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(fn, name, 6144, NULL, 5, NULL) != pdPASS) {
        finish("Could not start the task", false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void init()
{
    s_mutex = xSemaphoreCreateMutex();
}

esp_err_t start_check()
{
    ESP_LOGI(TAG, "Checking the SVS bootloader");
    return start_task(Task::Checking, check_task, "svs_check");
}

void clear()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_task == Task::Idle) {
        s_image.clear();
        s_image_info = {false, "", 0, ""};
    }
    xSemaphoreGive(s_mutex);
}

esp_err_t stage_hex(const char *hex, size_t len, const std::string &source, std::string *error)
{
    if (busy()) {
        *error = "The SVS is busy";
        return ESP_ERR_INVALID_STATE;
    }
    std::vector<uint8_t> image;
    if (!decode_hex(hex, len, image, error)) {
        clear();
        return ESP_ERR_INVALID_ARG;
    }
    std::string sha = sha256_hex(image);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_task != Task::Idle) {
        xSemaphoreGive(s_mutex);
        *error = "The SVS is busy";
        return ESP_ERR_INVALID_STATE;
    }
    s_image.swap(image);
    s_image_info = {true, source, s_image.size(), sha};
    if (s_result_of == Task::Flashing) {
        s_result.clear();  // the last flash result was about the previous image
        s_result_of = Task::Idle;
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Staged %s: %u bytes, SHA-256 %s", source.c_str(), (unsigned)s_image_info.size,
             sha.c_str());
    return ESP_OK;
}

esp_err_t start_flash()
{
    Status st = status();
    if (!st.can_flash) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "Flashing SVS firmware %s", st.image.source.c_str());
    return start_task(Task::Flashing, flash_task, "svs_flash");
}

esp_err_t start_preview()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_task == Task::Idle && s_device.checked && s_device.vector;
    xSemaphoreGive(s_mutex);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Previewing the vector patch");
    return start_task(Task::Previewing, preview_task, "svs_preview");
}

esp_err_t start_probe()
{
    ESP_LOGI(TAG, "Running bootloader diagnostics");
    return start_task(Task::Probing, probe_task, "svs_probe");
}

bool busy()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool b = s_task != Task::Idle;
    xSemaphoreGive(s_mutex);
    return b;
}

Status status()
{
    svs_usb::Info usb = svs_usb::info();
    bool connected = svs_usb::is_connected();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    Status st;
    st.task = s_task;
    st.phase = s_phase;
    st.progress = s_progress;
    st.device = s_device;
    st.image = s_image_info;
    st.result = s_result;
    st.result_ok = s_result_ok;
    st.result_of = s_result_of;
    uint32_t checked_connection = s_checked_connection;
    xSemaphoreGive(s_mutex);

    // A check only holds for the SVS that was plugged in at the time
    if (st.device.checked && (!connected || usb.connections != checked_connection)) {
        st.device = NO_DEVICE;
    }

    if (st.task != Task::Idle) {
        st.blocker = "The SVS is busy";
    } else if (!connected) {
        st.blocker = "The SVS is not connected";
    } else if (!st.device.checked) {
        st.blocker = "Check the SVS first";
    } else if (!st.device.compatible) {
        st.blocker = st.device.problem;
    } else if (!st.image.staged) {
        st.blocker = "Choose a firmware";
    } else if (st.image.size > st.device.app_space) {
        char buf[120];
        snprintf(buf, sizeof(buf), "The firmware (%u bytes) does not fit in the %u bytes below the "
                 "bootloader", (unsigned)st.image.size, (unsigned)st.device.app_space);
        st.blocker = buf;
    } else if (st.device.vector && !st.device.vector_verified) {
        st.blocker = "Preview the vector patch first";
    }
    st.can_flash = st.blocker.empty();
    return st;
}

}  // namespace svs_flasher
