// The SVS's own settings, and the bridge's description of the switch.

#include "svs_settings.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "ArduinoJson.h"

#include "svs_flasher.h"
#include "svs_usb.h"

namespace svs_settings {

using namespace svs_config;

static const char *TAG = "svs_settings";
static const char *NVS_NAMESPACE = "svs_layout";
static const char *KEY_LAYOUT = "json";

static const uint32_t ANSWER_TIMEOUT_MS = 800;
static const int ATTEMPTS = 2;
static const uint32_t WRITE_GAP_MS = 60;         // the utility waits 50 ms after each W
static const uint32_t INPUT_CHANGE_TIMEOUT_MS = 3000;
static const size_t MAX_LAYOUT = 3900;  // NVS strings take up to 4000 bytes
static const size_t MAX_NAME = 32;
static const size_t MAX_DEVICE = 16;
static const size_t MAX_OUTPUTS = 6;

static SemaphoreHandle_t s_mutex;

// Guarded by s_mutex
static Task s_task = Task::Idle;
static std::string s_phase;
static int s_progress = 0;
static std::string s_result;
static bool s_result_ok = false;
static Task s_result_of = Task::Idle;

// The last read, guarded by s_mutex. Tied to one connection of the SVS: a
// replug (maybe of another SVS, or after a firmware update) invalidates it.
struct Read {
    bool valid = false;
    uint32_t seq = 0;
    uint32_t connection = 0;
    int64_t at_ms = 0;
    std::string firmware;
    int inputs = 0;
    Eeprom eeprom;
    Hardware hardware;
    std::vector<InputSettings> settings;
};
static Read s_read;
static uint32_t s_seq = 0;

// What the background write is to write (set before its task starts)
static std::vector<InputSettings> s_want;

// The layout, guarded by s_mutex. Each input's name and the id of the
// console or device picked for it (from the web UI's list; "" if typed)
struct Label {
    std::string name, device;
};
static std::string s_layout = "{}";
static std::vector<Label> s_labels;

static int64_t now_ms() { return esp_timer_get_time() / 1000; }

static void set_phase(const std::string &phase, int progress)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_phase = phase;
    s_progress = progress;
    xSemaphoreGive(s_mutex);
}

static void finish(const std::string &result, bool ok)
{
    svs_usb::session_end();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_result = result;
    s_result_ok = ok;
    s_result_of = s_task;
    s_task = Task::Idle;
    s_phase.clear();
    s_progress = 0;
    xSemaphoreGive(s_mutex);
    svs_usb::log_note(result);
    ESP_LOGI(TAG, "%s", result.c_str());
}

// Asks the SVS (R/Y/G) and parses its byte answer, trying twice
static bool ask_byte(const std::string &cmd, int &value)
{
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        std::string answer;
        if (svs_usb::query(cmd, answer, ANSWER_TIMEOUT_MS) == ESP_OK) {
            value = parse_byte_reply(answer);
            if (value >= 0) {
                return true;
            }
        }
    }
    return false;
}

static std::string no_answer(const std::string &cmd)
{
    return "The SVS did not answer " + cmd +
           ". Is the RetroTINK's HD-15 still plugged in? It must be unplugged to read or save settings.";
}

// Why a read cannot start now, "" if it can
static std::string read_blocker()
{
    if (!svs_usb::is_connected()) {
        return "The SVS is not connected";
    }
    svs_usb::Info info = svs_usb::info();
    if (info.firmware.empty()) {
        return "The SVS has not reported its firmware yet. Restart it from the Control panel.";
    }
    int fw = firmware_number(info.firmware);
    if (fw < MIN_FIRMWARE) {
        return "Reading the SVS's settings needs its firmware 1.20 or newer (it runs " +
               info.firmware.substr(7) + ")";
    }
    if (info.total_inputs < 1 || info.total_inputs > MAX_INPUTS) {
        return "The SVS has not reported how many inputs it has yet";
    }
    if (svs_flasher::busy()) {
        return "An SVS firmware update is in progress";
    }
    return "";
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

static void read_task(void *arg)
{
    svs_usb::Info info = svs_usb::info();
    const int inputs = info.total_inputs;
    Read r;
    r.firmware = info.firmware;
    r.inputs = inputs;
    r.connection = info.connections;
    svs_usb::session_begin();

    // The bitmaps and the transcoders fitted
    std::vector<int> addrs = bitmap_addresses(inputs);
    for (size_t i = 0; i < addrs.size(); i++) {
        set_phase("Reading the settings", (int)(40 * i / addrs.size()));
        int v;
        if (!ask_byte(read_command(addrs[i]), v)) {
            finish(no_answer(read_command(addrs[i])), false);
            vTaskDelete(NULL);
        }
        r.eeprom.set(addrs[i], (uint8_t)v);
    }
    r.hardware = decode_hardware(r.eeprom, inputs);
    r.settings.assign(inputs, InputSettings());

    // Which inputs each fitted transcoder is on for (0 = on)
    for (int n = 1; n <= inputs; n++) {
        set_phase("Reading the transcoders", 40 + 15 * (n - 1) / inputs);
        InputSettings &s = r.settings[n - 1];
        struct { bool fitted; const char *cmd; bool *on; } tx[] = {
            {r.hardware.tx_rgb_to_ypbpr, "Y", &s.rgb_to_ypbpr},
            {r.hardware.tx_ypbpr_to_rgb, "G", &s.ypbpr_to_rgb},
        };
        for (auto &t : tx) {
            if (!t.fitted) {
                continue;
            }
            std::string cmd = t.cmd + std::to_string(n);
            int v;
            if (!ask_byte(cmd, v)) {
                finish(no_answer(cmd), false);
                vTaskDelete(NULL);
            }
            *t.on = v == 0;
        }
    }

    // The IR codes, each input's up to its first empty slot
    const int slots = ir_slots(inputs);
    for (int n = 1; n <= inputs; n++) {
        set_phase("Reading the IR codes", 55 + 45 * (n - 1) / inputs);
        for (int k = 0; k < slots; k++) {
            int a, c;
            int addr_a = ir_address_addr(n, k, inputs), addr_c = ir_command_addr(n, k, inputs);
            if (!ask_byte(read_command(addr_a), a)) {
                finish(no_answer(read_command(addr_a)), false);
                vTaskDelete(NULL);
            }
            r.eeprom.set(addr_a, (uint8_t)a);
            if (!ask_byte(read_command(addr_c), c)) {
                finish(no_answer(read_command(addr_c)), false);
                vTaskDelete(NULL);
            }
            r.eeprom.set(addr_c, (uint8_t)c);
            if (a == IR_EMPTY) {
                break;
            }
        }
    }
    for (int n = 1; n <= inputs; n++) {
        decode_input(r.eeprom, n, inputs, r.settings[n - 1]);
    }

    r.valid = true;
    r.at_ms = now_ms();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    r.seq = ++s_seq;
    s_read = r;
    xSemaphoreGive(s_mutex);
    finish("Read the settings of the SVS's " + std::to_string(inputs) + " inputs", true);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

static bool wait_for_input(int n)
{
    for (uint32_t t = 0; t < INPUT_CHANGE_TIMEOUT_MS; t += 50) {
        if (svs_usb::info().current_input == n) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

static void write_task(void *arg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    Read r = s_read;
    std::vector<InputSettings> want = s_want;
    xSemaphoreGive(s_mutex);
    const int inputs = r.inputs;
    svs_usb::session_begin();

    std::vector<std::pair<int, uint8_t>> writes;
    std::string error;
    if (!plan_writes(r.eeprom, want, inputs, writes, &error)) {
        finish(error, false);
        vTaskDelete(NULL);
    }

    // Transcoders: set for the input on screen, so each is switched to in turn
    struct TxChange { int input; bool rgb_to_ypbpr; bool on; };
    std::vector<TxChange> tx;
    for (int n = 1; n <= inputs; n++) {
        const InputSettings &w = want[n - 1], &old = r.settings[n - 1];
        if (w.rgb_to_ypbpr != old.rgb_to_ypbpr) tx.push_back({n, true, w.rgb_to_ypbpr});
        if (w.ypbpr_to_rgb != old.ypbpr_to_rgb) tx.push_back({n, false, w.ypbpr_to_rgb});
    }
    const size_t steps = writes.size() * 2 + tx.size() + 1;
    size_t step = 0;

    // The EEPROM bytes, then each one read back
    for (auto &w : writes) {
        set_phase("Saving the settings", (int)(100 * step++ / steps));
        esp_err_t err = svs_usb::send_quiet(write_command(w.first, w.second));
        if (err != ESP_OK) {
            finish(std::string("Could not send to the SVS: ") + esp_err_to_name(err), false);
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(WRITE_GAP_MS));
    }
    for (auto &w : writes) {
        set_phase("Checking what the SVS saved", (int)(100 * step++ / steps));
        int v;
        std::string cmd = read_command(w.first);
        if (!ask_byte(cmd, v)) {
            finish(no_answer(cmd), false);
            vTaskDelete(NULL);
        }
        if (v != w.second) {
            char buf[120];
            snprintf(buf, sizeof(buf), "The SVS kept %d at %d instead of %u. Nothing else was changed after it.",
                     v, w.first, (unsigned)w.second);
            finish(buf, false);
            vTaskDelete(NULL);
        }
        r.eeprom.set(w.first, w.second);
    }

    // The transcoders, then back to the input that was on screen
    if (!tx.empty()) {
        int prev = svs_usb::info().current_input;
        for (auto &t : tx) {
            set_phase("Setting the transcoder of input " + std::to_string(t.input), (int)(100 * step++ / steps));
            std::string change = "SVS_Change_Input_" + std::to_string(t.input);
            std::string cmd = t.rgb_to_ypbpr ? (t.on ? "SVS_RGB_Comp_ON" : "SVS_RGB_Comp_OFF")
                                             : (t.on ? "SVS_Comp_RGB_ON" : "SVS_Comp_RGB_OFF");
            if (svs_usb::send_quiet(change) != ESP_OK || !wait_for_input(t.input) ||
                svs_usb::send_quiet(cmd) != ESP_OK) {
                finish("The SVS did not switch to input " + std::to_string(t.input) +
                           " to set its transcoder. The other settings were saved.", false);
                vTaskDelete(NULL);
            }
            vTaskDelay(pdMS_TO_TICKS(WRITE_GAP_MS));
            int v;
            std::string check = (t.rgb_to_ypbpr ? "Y" : "G") + std::to_string(t.input);
            if (!ask_byte(check, v) || (v == 0) != t.on) {
                finish("The transcoder of input " + std::to_string(t.input) +
                           " did not change. The other settings were saved.", false);
                vTaskDelete(NULL);
            }
            (t.rgb_to_ypbpr ? r.settings[t.input - 1].rgb_to_ypbpr : r.settings[t.input - 1].ypbpr_to_rgb) = t.on;
        }
        svs_usb::send_quiet("SVS_Change_Input_" + std::to_string(prev > 0 ? prev : 0));
    }

    for (int n = 1; n <= inputs; n++) {
        decode_input(r.eeprom, n, inputs, r.settings[n - 1]);
    }
    r.at_ms = now_ms();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    r.seq = ++s_seq;
    s_read = r;
    xSemaphoreGive(s_mutex);
    char buf[96];
    snprintf(buf, sizeof(buf), "Saved to the SVS: %u bytes and %u transcoder settings",
             (unsigned)writes.size(), (unsigned)tx.size());
    finish(writes.empty() && tx.empty() ? "Nothing to save: the SVS already has these settings" : buf, true);
    vTaskDelete(NULL);
}

static esp_err_t start_task(TaskFunction_t fn, const char *name)
{
    if (xTaskCreate(fn, name, 6144, NULL, 5, NULL) != pdPASS) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_task = Task::Idle;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// Claims the task slot; false if busy
static bool claim(Task task)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool free = s_task == Task::Idle;
    if (free) {
        s_task = task;
        s_phase = "Starting";
        s_progress = 0;
        s_result.clear();
        s_result_of = Task::Idle;
    }
    xSemaphoreGive(s_mutex);
    return free;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static void load_layout();

void init()
{
    s_mutex = xSemaphoreCreateMutex();
    load_layout();
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
    uint32_t connection = svs_usb::info().connections;
    bool connected = svs_usb::is_connected();
    std::string blocker = read_blocker();
    Status st;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    st.task = s_task;
    st.phase = s_phase;
    st.progress = s_progress;
    st.result = s_result;
    st.result_ok = s_result_ok;
    st.result_of = s_result_of;
    st.blocker = s_task != Task::Idle ? "" : blocker;
    Snapshot &sn = st.snapshot;
    sn.valid = s_read.valid && connected && s_read.connection == connection;
    sn.seq = s_read.seq;
    if (sn.valid) {
        sn.age_ms = now_ms() - s_read.at_ms;
        sn.firmware = s_read.firmware;
        sn.inputs = s_read.inputs;
        sn.ir_slots = ir_slots(s_read.inputs);
        sn.hardware = s_read.hardware;
        sn.settings = s_read.settings;
    } else {
        sn.age_ms = 0;
        sn.inputs = 0;
        sn.ir_slots = 0;
    }
    xSemaphoreGive(s_mutex);
    return st;
}

esp_err_t start_read(std::string *error)
{
    if (!svs_usb::send_allowed()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    std::string blocker = read_blocker();
    if (!blocker.empty() || !claim(Task::Reading)) {
        *error = blocker.empty() ? "The SVS's settings are being read or saved" : blocker;
        return ESP_ERR_INVALID_STATE;
    }
    return start_task(read_task, "svs_read");
}

esp_err_t start_write(const std::vector<InputSettings> &want_in, std::string *error)
{
    if (!svs_usb::send_allowed()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    std::string blocker = read_blocker();
    if (!blocker.empty()) {
        *error = blocker;
        return ESP_ERR_INVALID_STATE;
    }
    Status st = status();
    if (!st.snapshot.valid) {
        *error = "Read the settings from the SVS first";
        return ESP_ERR_INVALID_STATE;
    }
    if ((int)want_in.size() != st.snapshot.inputs || svs_usb::info().total_inputs != st.snapshot.inputs) {
        *error = "The SVS now reports a different number of inputs. Read its settings again.";
        return ESP_ERR_INVALID_STATE;
    }
    // Keep what a module or a missing transcoder cannot have as it was read
    std::vector<InputSettings> want = want_in;
    const Hardware &hw = st.snapshot.hardware;
    for (size_t i = 0; i < want.size(); i++) {
        const InputSettings &old = st.snapshot.settings[i];
        if (!hw.scart_v3[i] && !hw.vga_v3[i]) want[i].rgsb = old.rgsb;
        if (!hw.scart_v3[i]) want[i].sync_bypass = old.sync_bypass;
        if (!hw.tx_rgb_to_ypbpr) want[i].rgb_to_ypbpr = old.rgb_to_ypbpr;
        if (!hw.tx_ypbpr_to_rgb) want[i].ypbpr_to_rgb = old.ypbpr_to_rgb;
        if ((int)want[i].ir.size() > st.snapshot.ir_slots) {
            *error = "Input " + std::to_string(i + 1) + " has more IR codes than fit (" +
                     std::to_string(st.snapshot.ir_slots) + ")";
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!claim(Task::Writing)) {
        *error = "The SVS's settings are being read or saved";
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_want = want;
    xSemaphoreGive(s_mutex);
    return start_task(write_task, "svs_write");
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

static bool valid_kind(const std::string &kind, bool output)
{
    static const char *IN[] = {"", "scart", "component", "vga", "svideo", "dterm"};  // "": not set up yet
    // Outputs, and the transcoders where they sit among them
    static const char *OUT[] = {"scart", "component", "vga", "svideo", "bnc", "tx_rgb_to_ypbpr", "tx_ypbpr_to_rgb"};
    if (output) {
        for (const char *k : OUT) {
            if (kind == k) return true;
        }
    } else {
        for (const char *k : IN) {
            if (kind == k) return true;
        }
    }
    return false;
}

// Normalizes a layout: known keys only. false with *error if it is not valid.
// A device id from the web UI's list: lower-case letters, digits, '-' and '_'
static bool valid_device(const std::string &id)
{
    if (id.size() > MAX_DEVICE) {
        return false;
    }
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

static bool normalize(const std::string &json, std::string &out, std::vector<Label> &labels,
                      std::string *error)
{
    JsonDocument in;
    if (json.size() > MAX_LAYOUT || deserializeJson(in, json) != DeserializationError::Ok ||
        !in.is<JsonObject>()) {
        *error = "Invalid layout";
        return false;
    }
    JsonDocument doc;
    labels.clear();
    for (const char *list : {"inputs", "outputs"}) {
        bool output = strcmp(list, "outputs") == 0;
        JsonArrayConst src = in[list].as<JsonArrayConst>();
        JsonArray dst = doc[list].to<JsonArray>();
        if (src.size() > (output ? MAX_OUTPUTS + 2 : (size_t)MAX_INPUTS)) {
            *error = output ? "At most 6 outputs" : "At most 32 inputs";
            return false;
        }
        size_t outputs = 0;
        bool tx_seen[2] = {false, false};
        for (JsonObjectConst e : src) {
            std::string kind = e["kind"] | "";
            std::string name = e["name"] | "";
            std::string device = e["device"] | "";
            if (!valid_device(device)) {
                *error = "Unknown device: " + device;
                return false;
            }
            if (!valid_kind(kind, output)) {
                *error = "Unknown module: " + kind;
                return false;
            }
            if (name.size() > MAX_NAME) {
                *error = "Names are up to 32 characters";
                return false;
            }
            if (output && kind.rfind("tx_", 0) == 0) {
                bool &seen = tx_seen[kind == "tx_rgb_to_ypbpr" ? 0 : 1];
                if (seen) {
                    *error = "A transcoder can only be placed once";
                    return false;
                }
                seen = true;
                name.clear();
                device.clear();
            } else if (output && ++outputs > MAX_OUTPUTS) {
                *error = "At most 6 outputs";
                return false;
            }
            JsonObject o = dst.add<JsonObject>();
            o["kind"] = kind;
            o["name"] = name;
            if (!device.empty()) {
                o["device"] = device;
            }
            if (!output) {
                labels.push_back({name, device});
            }
        }
    }
    serializeJson(doc, out);
    return true;
}

static void load_layout()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    if (nvs_get_str(h, KEY_LAYOUT, nullptr, &len) == ESP_OK && len > 0 && len <= MAX_LAYOUT + 1) {
        std::string json(len, '\0');
        if (nvs_get_str(h, KEY_LAYOUT, &json[0], &len) == ESP_OK) {
            json.resize(len - 1);
            std::string out, error;
            std::vector<Label> labels;
            if (normalize(json, out, labels, &error)) {
                s_layout = out;
                s_labels = labels;
            }
        }
    }
    nvs_close(h);
}

std::string layout_json()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    std::string out = s_layout;
    xSemaphoreGive(s_mutex);
    return out;
}

esp_err_t set_layout_json(const std::string &json, std::string *error)
{
    std::string out;
    std::vector<Label> labels;
    if (!normalize(json, out, labels, error)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_LAYOUT, out.c_str());
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        *error = std::string("Could not save: ") + esp_err_to_name(err);
        return err;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_layout = out;
    s_labels = labels;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

std::string input_name(int input)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    std::string name = input >= 1 && input <= (int)s_labels.size() ? s_labels[input - 1].name : "";
    xSemaphoreGive(s_mutex);
    return name;
}

std::string input_device(int input)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    std::string device = input >= 1 && input <= (int)s_labels.size() ? s_labels[input - 1].device : "";
    xSemaphoreGive(s_mutex);
    return device;
}

}  // namespace svs_settings
