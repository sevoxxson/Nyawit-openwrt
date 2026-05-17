/*
 * esim-telegram-bot - tiny Telegram long-poll bridge for 0xygen eSIM.
 *
 * Runtime dependencies are intentionally limited to BusyBox/OpenWrt tools
 * already present in the firmware: uci, curl, jq, and lpac-esim.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define APP_NAME "esim-telegram-bot"
#define UCI_PREFIX "lpac-esim.main."
#define OFFSET_FILE "/tmp/lpac-esim/telegram.offset"
#define PENDING_FILE "/tmp/lpac-esim/telegram.pending"
#define STATUS_FILE "/tmp/lpac-esim/telegram.status"
#define BUF_MAX 65536
#define MSG_MAX 3900

struct config {
    char enabled[8];
    char token[160];
    char chat_id[64];
    int reconnect_delay;
    int debug;
};

static void log_msg(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s: %s\n", APP_NAME, msg);

    pid_t pid = fork();
    if (pid == 0) {
        execlp("logger", "logger", "-t", APP_NAME, msg, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

static void trim(char *s)
{
    size_t n;
    while (*s && isspace((unsigned char)*s)) memmove(s, s + 1, strlen(s));
    n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static int valid_token(const char *s)
{
    int has_colon = 0;
    if (!s || !*s) return 0;
    for (; *s; s++) {
        if (*s == ':') has_colon = 1;
        else if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-') return 0;
    }
    return has_colon;
}

static int valid_chat_id(const char *s)
{
    if (!s || !*s) return 0;
    if (*s == '-') s++;
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

static int safe_arg(const char *s)
{
    if (!s || !*s || strlen(s) > 512) return 0;
    for (; *s; s++) {
        if ((unsigned char)*s < 32 || (unsigned char)*s == 127) return 0;
    }
    return 1;
}

static int run_capture(char *const argv[], char *out, size_t outsz, int timeout_sec)
{
    int pipefd[2];
    pid_t pid;
    time_t deadline;
    size_t used = 0;
    int status = 0;

    if (outsz == 0) return -1;
    out[0] = '\0';
    if (pipe(pipefd) != 0) return -1;

    pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(pipefd[1]);
        if (devnull >= 0) close(devnull);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0) {
        close(pipefd[0]);
        return -1;
    }

    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK);
    deadline = time(NULL) + timeout_sec;

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        ssize_t r;
        int w;

        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        select(pipefd[0] + 1, &rfds, NULL, NULL, &tv);

        while ((r = read(pipefd[0], out + used, outsz - used - 1)) > 0) {
            used += (size_t)r;
            out[used] = '\0';
            if (used >= outsz - 1) break;
        }

        w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (time(NULL) >= deadline) {
            kill(pid, SIGTERM);
            sleep(1);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            close(pipefd[0]);
            return 124;
        }
    }

    while (used < outsz - 1) {
        ssize_t r = read(pipefd[0], out + used, outsz - used - 1);
        if (r <= 0) break;
        used += (size_t)r;
    }
    out[used] = '\0';
    close(pipefd[0]);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static int run_shell(const char *cmd, char *out, size_t outsz, int timeout_sec)
{
    char *argv[] = { "sh", "-c", (char *)cmd, NULL };
    return run_capture(argv, out, outsz, timeout_sec);
}

static void get_uci(const char *option, char *buf, size_t bufsz)
{
    char key[96];
    char *argv[] = { "uci", "-q", "get", key, NULL };
    snprintf(key, sizeof(key), "%s%s", UCI_PREFIX, option);
    if (run_capture(argv, buf, bufsz, 3) != 0) buf[0] = '\0';
    trim(buf);
}

static void load_config(struct config *cfg)
{
    char tmp[32];
    memset(cfg, 0, sizeof(*cfg));
    get_uci("telegram_enabled", cfg->enabled, sizeof(cfg->enabled));
    get_uci("telegram_bot_token", cfg->token, sizeof(cfg->token));
    get_uci("telegram_allowed_chat_id", cfg->chat_id, sizeof(cfg->chat_id));
    get_uci("telegram_poll_interval", tmp, sizeof(tmp));
    cfg->reconnect_delay = atoi(tmp);
    if (cfg->reconnect_delay < 1 || cfg->reconnect_delay > 30) cfg->reconnect_delay = 2;
    get_uci("telegram_debug", tmp, sizeof(tmp));
    cfg->debug = (strcmp(tmp, "1") == 0);
}

/*
 * NOTE: chat IDs and update_ids in the Telegram Bot API are 64-bit signed
 * integers. Linksys EA6350v3 is 32-bit ARM (ipq40xx / Cortex-A7) where the
 * C `long` type is only 32-bit, so parsing those numbers with strtol() or
 * printing them with %ld silently wraps to LONG_MAX for any chat ID above
 * 2_147_483_647 — which is true for the vast majority of modern Telegram
 * users. Symptom: bot polls fine, never replies, no error logged. Always
 * use `long long` / strtoll / %lld for these.
 */

/*
 * Write a small JSON heartbeat to /tmp/lpac-esim/telegram.status every time
 * we successfully poll Telegram, and again on each failure. LuCI reads this
 * file to show the user a real, live status (● Running with "last poll 3s
 * ago" / ● Error: rc=22) instead of just `pidof` which only says whether
 * the process exists, not whether it's actually able to reach Telegram.
 */
static void write_status(const char *state, int last_rc, long long last_update_id)
{
    FILE *f;
    mkdir("/tmp/lpac-esim", 0755);
    f = fopen(STATUS_FILE ".tmp", "w");
    if (!f) return;
    fprintf(f,
            "{\"state\":\"%s\",\"last_poll\":%ld,\"last_rc\":%d,\"last_update_id\":%lld,\"pid\":%ld}\n",
            state, (long)time(NULL), last_rc, last_update_id, (long)getpid());
    fclose(f);
    rename(STATUS_FILE ".tmp", STATUS_FILE);
}

static void save_offset(long long offset)
{
    FILE *f;
    mkdir("/tmp/lpac-esim", 0755);
    f = fopen(OFFSET_FILE, "w");
    if (!f) return;
    fprintf(f, "%lld\n", offset);
    fclose(f);
}

static void pending_path(const char *chat_id, char *path, size_t pathsz)
{
    snprintf(path, pathsz, "%s.%s", PENDING_FILE, valid_chat_id(chat_id) ? chat_id : "0");
}

static void save_pending(const char *chat_id, const char *action, const char *arg)
{
    char path[96];
    FILE *f;
    mkdir("/tmp/lpac-esim", 0755);
    pending_path(chat_id, path, sizeof(path));
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n%s\n", action ? action : "", arg ? arg : "");
    fclose(f);
}

static int load_pending(const char *chat_id, char *action, size_t actionsz, char *arg, size_t argsz)
{
    char path[96];
    FILE *f;
    pending_path(chat_id, path, sizeof(path));
    f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(action, actionsz, f)) action[0] = '\0';
    if (!fgets(arg, argsz, f)) arg[0] = '\0';
    fclose(f);
    trim(action);
    trim(arg);
    return action[0] != '\0';
}

static void clear_pending(const char *chat_id)
{
    char path[96];
    pending_path(chat_id, path, sizeof(path));
    unlink(path);
}

static long long load_offset(void)
{
    FILE *f = fopen(OFFSET_FILE, "r");
    long long offset = 0;
    if (!f) return 0;
    if (fscanf(f, "%lld", &offset) != 1) offset = 0;
    fclose(f);
    return offset;
}

static int telegram_get_updates(const struct config *cfg, long long offset, char *out, size_t outsz)
{
    char url[320];
    char *argv[] = { "curl", "-g", "-fsS", "--max-time", "35", NULL, NULL };
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/getUpdates?timeout=25&offset=%lld&allowed_updates=[\"message\"]",
             cfg->token, offset);
    argv[5] = url;
    return run_capture(argv, out, outsz, 40);
}

static void send_message(const struct config *cfg, const char *chat_id, const char *text)
{
    char url[240];
    char chat_arg[96];
    char msg[MSG_MAX + 1];
    char out[512];
    int rc;
    char *argv[] = {
        "curl", "-fsS", "--max-time", "15", "-X", "POST", url,
        "-d", chat_arg, "--data-urlencode", msg, NULL
    };

    if (!valid_chat_id(chat_id)) return;
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", cfg->token);
    snprintf(chat_arg, sizeof(chat_arg), "chat_id=%s", chat_id);
    snprintf(msg, sizeof(msg), "text=%.*s", MSG_MAX - 5, text ? text : "");
    rc = run_capture(argv, out, sizeof(out), 20);
    /* Always log a failed sendMessage. The previous `&& cfg->debug` guard
     * hid the very symptom that masked the 32-bit chat_id parsing bug for
     * months: bot would silently fail to deliver every reply (Telegram
     * returns 400 "chat not found" / curl exit 22), and with debug off
     * nothing ever showed up in syslog. */
    if (rc != 0) log_msg("sendMessage failed rc=%d chat=%s", rc, chat_id);
}

static void send_keyboard(const struct config *cfg, const char *chat_id, const char *text, const char *keyboard_json)
{
    char url[240];
    char chat_arg[96];
    char msg[MSG_MAX + 1];
    char markup[2048];
    char out[512];
    int rc;
    char *argv[] = {
        "curl", "-fsS", "--max-time", "15", "-X", "POST", url,
        "-d", chat_arg, "--data-urlencode", msg, "-d", markup, NULL
    };

    if (!valid_chat_id(chat_id)) return;
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", cfg->token);
    snprintf(chat_arg, sizeof(chat_arg), "chat_id=%s", chat_id);
    snprintf(msg, sizeof(msg), "text=%.*s", MSG_MAX - 5, text ? text : "");
    snprintf(markup, sizeof(markup), "reply_markup=%s", keyboard_json ? keyboard_json : "{}");
    rc = run_capture(argv, out, sizeof(out), 20);
    if (rc != 0) log_msg("sendMessage keyboard failed rc=%d chat=%s", rc, chat_id);
}

static void send_main_menu(const struct config *cfg, const char *chat_id)
{
    send_keyboard(cfg, chat_id,
        "0xygen eSIM Bot\n"
        "Pilih menu tombol di bawah.",
        "{\"keyboard\":["
        "[{\"text\":\"Info EID\"}],"
        "[{\"text\":\"Profile List\"},{\"text\":\"Cek Kuota\"}],"
        "[{\"text\":\"Download eSIM\"}],"
        "[{\"text\":\"Klaim Free eSIM HYFE\"}],"
        "[{\"text\":\"Settings\"},{\"text\":\"Tools\"}]"
        "],\"resize_keyboard\":true,\"one_time_keyboard\":false}");
}

static void send_back_menu(const struct config *cfg, const char *chat_id, const char *text)
{
    send_keyboard(cfg, chat_id, text,
        "{\"keyboard\":["
        "[{\"text\":\"Menu Utama\"}],"
        "[{\"text\":\"Profile List\"},{\"text\":\"Settings\"}]"
        "],\"resize_keyboard\":true,\"one_time_keyboard\":false}");
}

static char *json_string_after(char *start, const char *key, char *dst, size_t dstsz)
{
    char *p = strstr(start, key);
    size_t i = 0;
    if (!p || dstsz == 0) return NULL;
    p += strlen(key);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return NULL;
    p++;
    while (*p && *p != '"' && i + 1 < dstsz) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') dst[i++] = '\n';
            else if (*p == 'r') dst[i++] = '\r';
            else if (*p == 't') dst[i++] = '\t';
            else dst[i++] = *p;
        } else {
            dst[i++] = *p;
        }
        p++;
    }
    dst[i] = '\0';
    return (*p == '"') ? p + 1 : NULL;
}

static int json_chat_id_after(char *start, char *dst, size_t dstsz)
{
    char *chat = strstr(start, "\"chat\"");
    char *id;
    long long v;
    if (!chat) return 0;
    id = strstr(chat, "\"id\":");
    if (!id) return 0;
    id += 5;
    /* MUST be strtoll/%lld, not strtol/%ld. On 32-bit ARM, strtol() clamps
     * any chat ID above 2_147_483_647 to LONG_MAX, which then never matches
     * the configured allowed_chat_id and the bot silently drops the user's
     * messages. See note above save_offset(). */
    v = strtoll(id, NULL, 10);
    snprintf(dst, dstsz, "%lld", v);
    return valid_chat_id(dst);
}

static int cmd_match(const char *text, const char *cmd)
{
    size_t n = strlen(cmd);
    if (strncmp(text, cmd, n) != 0) return 0;
    return text[n] == '\0' || isspace((unsigned char)text[n]) || text[n] == '@';
}

static const char *cmd_args(const char *text)
{
    const char *p = text;
    while (*p && !isspace((unsigned char)*p)) p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int lpac_api(char *const extra[], char *out, size_t outsz, int timeout_sec)
{
    char *argv[16];
    int i = 0, j = 0;
    argv[i++] = "/usr/bin/lpac-esim";
    argv[i++] = "--api";
    while (extra[j] && i < 15) argv[i++] = extra[j++];
    argv[i] = NULL;
    return run_capture(argv, out, outsz, timeout_sec);
}

static int esim_quota(const char *msisdn, char *out, size_t outsz)
{
    char *argv[] = { "/usr/bin/esim", "--telegram-quota", (char *)msisdn, NULL };
    return run_capture(argv, out, outsz, 45);
}

static void compact_result(const char *json, char *dst, size_t dstsz)
{
    char msg[160] = "";
    char detail[512] = "";
    char *copy = strdup(json ? json : "");
    if (!copy) {
        snprintf(dst, dstsz, "No response");
        return;
    }
    json_string_after(copy, "\"message\":", msg, sizeof(msg));
    json_string_after(copy, "\"msg\":", detail, sizeof(detail));
    if (detail[0])
        snprintf(dst, dstsz, "%s: %s", msg[0] ? msg : "result", detail);
    else if (msg[0])
        snprintf(dst, dstsz, "%s", msg);
    else
        snprintf(dst, dstsz, "%.3500s", json ? json : "No response");
    free(copy);
}

static const char *profile_label(const char *action)
{
    if (strcmp(action, "switch") == 0) return "Switch / Enable";
    if (strcmp(action, "disable") == 0) return "Disable";
    if (strcmp(action, "delete") == 0) return "Delete";
    return "Action";
}

static void format_profiles(const char *json, char *dst, size_t dstsz)
{
    char *copy = strdup(json ? json : "");
    char *p;
    int idx = 1;
    size_t used = 0;
    if (!copy) {
        snprintf(dst, dstsz, "No memory");
        return;
    }
    used += snprintf(dst + used, dstsz - used, "eSIM profiles:\n");
    p = copy;
    while ((p = strstr(p, "\"iccid\":")) && idx <= 12 && used < dstsz - 80) {
        char iccid[40] = "-", state[24] = "-", provider[64] = "-", nick[64] = "-";
        char *row_start = p;
        json_string_after(row_start, "\"iccid\":", iccid, sizeof(iccid));
        json_string_after(row_start, "\"profileState\":", state, sizeof(state));
        json_string_after(row_start, "\"serviceProviderName\":", provider, sizeof(provider));
        json_string_after(row_start, "\"profileNickname\":", nick, sizeof(nick));
        used += snprintf(dst + used, dstsz - used, "%d. %s %s %s %s\n",
                         idx, state, iccid, provider, nick);
        idx++;
        p += 8;
    }
    if (idx == 1) compact_result(json, dst, dstsz);
    else if (idx > 12 && used < dstsz - 32) snprintf(dst + used, dstsz - used, "...truncated\n");
    free(copy);
}

static void send_profile_menu(const struct config *cfg, const char *chat_id)
{
    char out[BUF_MAX];
    char msg[MSG_MAX + 1];
    int rc;
    char *args[] = { "profiles", NULL };
    rc = lpac_api(args, out, sizeof(out), 35);
    if (rc == 0) format_profiles(out, msg, sizeof(msg));
    else snprintf(msg, sizeof(msg), "Gagal membaca profiles (rc=%d).", rc);
    strncat(msg,
        "\nAksi cepat:\n"
        "/switch <ICCID>\n"
        "/disable <ICCID>\n"
        "/delete <ICCID>\n"
        "Semua aksi penting akan minta konfirmasi.",
        sizeof(msg) - strlen(msg) - 1);
    send_keyboard(cfg, chat_id, msg,
        "{\"keyboard\":["
        "[{\"text\":\"Switch / Enable eSIM\"},{\"text\":\"Disable eSIM\"}],"
        "[{\"text\":\"Delete eSIM\"},{\"text\":\"Cek Kuota\"}],"
        "[{\"text\":\"Menu Utama\"}]"
        "],\"resize_keyboard\":true,\"one_time_keyboard\":false}");
}

static void send_settings_menu(const struct config *cfg, const char *chat_id)
{
    char out[2048];
    char msg[MSG_MAX + 1];
    const char *cmd =
        "printf 'APDU backend: '; uci -q get lpac-esim.main.apdu_backend; "
        "printf 'QMI device: '; uci -q get lpac-esim.main.qmi_device; "
        "printf 'QMI slot: '; uci -q get lpac-esim.main.qmi_sim_slot; "
        "printf 'SIM slot: '; uci -q get lpac-esim.main.sim_slot; "
        "printf 'AT device: '; uci -q get lpac-esim.main.at_device; "
        "printf 'MBIM device: '; uci -q get lpac-esim.main.mbim_device; "
        "printf 'MBIM proxy: '; uci -q get lpac-esim.main.mbim_proxy; "
        "printf 'MBIM skip slot: '; uci -q get lpac-esim.main.mbim_skip_slot_mapping; "
        "printf 'ISD-R AID: '; uci -q get lpac-esim.main.custom_isd_r_aid; "
        "printf 'Reboot method: '; uci -q get lpac-esim.main.reboot_method; "
        "printf 'Modem iface: '; uci -q get lpac-esim.main.modem_iface; "
        "printf 'APDU debug: '; uci -q get lpac-esim.main.apdu_debug; "
        "printf 'HTTP debug: '; uci -q get lpac-esim.main.http_debug; "
        "printf 'AT debug: '; uci -q get lpac-esim.main.at_debug";
    run_shell(cmd, out, sizeof(out), 5);
    snprintf(msg, sizeof(msg),
        "Settings LPAC saat ini:\n%s\n"
        "Ubah setting:\n"
        "/set_config <option> <value>\n\n"
        "Option: apdu_backend, qmi_device, qmi_sim_slot, sim_slot, at_device, "
        "mbim_device, mbim_proxy, mbim_skip_slot_mapping, custom_isd_r_aid, "
        "reboot_method, modem_iface, apdu_debug, http_debug, at_debug",
        out[0] ? out : "(belum ada data UCI)");
    send_keyboard(cfg, chat_id, msg,
        "{\"keyboard\":["
        "[{\"text\":\"Backend QMI\"},{\"text\":\"Backend MBIM\"},{\"text\":\"Backend AT\"}],"
        "[{\"text\":\"Debug ON\"},{\"text\":\"Debug OFF\"}],"
        "[{\"text\":\"Menu Utama\"}]"
        "],\"resize_keyboard\":true,\"one_time_keyboard\":false}");
}

static void send_tools_menu(const struct config *cfg, const char *chat_id)
{
    send_keyboard(cfg, chat_id,
        "Tools:\n"
        "/status - status modem\n"
        "/notifications - daftar notifikasi\n"
        "/process_notifications - proses notifikasi\n"
        "/lock - status lock backend\n"
        "/help - command manual",
        "{\"keyboard\":["
        "[{\"text\":\"Status Modem\"},{\"text\":\"Notifications\"}],"
        "[{\"text\":\"Process Notifications\"},{\"text\":\"Lock Status\"}],"
        "[{\"text\":\"Menu Utama\"}]"
        "],\"resize_keyboard\":true,\"one_time_keyboard\":false}");
}

static void help_text(char *dst, size_t dstsz)
{
    snprintf(dst, dstsz,
        "0xygen eSIM Telegram bot\n"
        "/menu - tampilkan menu tombol\n"
        "/chatid - tampilkan chat ID\n"
        "/profiles atau /profile - daftar profil eSIM\n"
        "/iccid - daftar ICCID profile\n"
        "/info atau /eid - info EID/eUICC\n"
        "/status - status modem\n"
        "/switch <ICCID|AID> - aktifkan profil\n"
        "/disable <ICCID|AID> - nonaktifkan profil\n"
        "/delete <ICCID|AID> - hapus profil disabled\n"
        "/download <LPA:1$...> - download profil\n"
        "/quota <MSISDN> - cek kuota XL\n"
        "/set_config <option> <value> - ubah setting LPAC\n"
        "/notifications - daftar notifikasi\n"
        "/process_notifications - proses notifikasi\n"
        "/lock - status operasi backend\n"
        "/hyfe - klaim eSIM trial HYFE (wizard)\n"
        "/cancel - batalkan wizard yang sedang berjalan");
}

static int valid_config_key(const char *key)
{
    static const char *keys[] = {
        "apdu_backend", "qmi_device", "qmi_sim_slot", "sim_slot", "at_device",
        "mbim_device", "mbim_proxy", "mbim_skip_slot_mapping", "custom_isd_r_aid",
        "reboot_method", "modem_iface", "apdu_debug", "http_debug", "at_debug", NULL
    };
    int i;
    for (i = 0; keys[i]; i++) if (strcmp(key, keys[i]) == 0) return 1;
    return 0;
}

static int safe_config_value(const char *s)
{
    size_t len;
    if (!s) return 0;
    len = strlen(s);
    if (len == 0 || len > 160) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c == 127 || *s == '\'' || *s == '"' || *s == '`' || *s == '$' || *s == ';' || *s == '&' || *s == '|')
            return 0;
    }
    return 1;
}

/* ─── HYFE wizard helpers ────────────────────────────────────────────────── *
 * The hyfe-telegram-claim helper from luci-app-lpac-manager exposes the full
 * HYFE trial claim flow as a JSON-RPC-ish CLI (start-manual / poll-otp /
 * finish / finish-manual / show-captcha / show-config / random-name / ...).
 *
 * Previously the bot just told users "flow HYFE butuh captcha/OTP interaktif,
 * pakai CLI esim menu 7". That made the menu entry useless. We now drive the
 * helper through a 3-step pending-state wizard:
 *
 *   hyfe_msisdn  -> user sends MSISDN; bot calls start-manual
 *   hyfe_otp     -> user sends 6-digit OTP
 *                     captcha mode != manual: bot calls finish directly
 *                     captcha mode == manual: bot prompts for token
 *   hyfe_captcha -> user pastes g-recaptcha-response token; bot calls
 *                   finish-manual and presents the result.
 *
 * On success the bot replies with the LPA string in a copyable code block
 * AND a QR-code image via sendPhoto (photo URL = public api.qrserver.com
 * QR renderer, urlencoded LPA payload).
 *
 * This module deliberately reuses the existing pending-state file machinery
 * (save_pending / load_pending / clear_pending) so the wizard survives a
 * bot restart and the existing /cancel handler also cancels HYFE wizards. */

static int hyfe_call(char *const extra[], char *out, size_t outsz, int timeout_sec)
{
    char *argv[16];
    int i = 0, j = 0;
    argv[i++] = "/usr/bin/hyfe-telegram-claim";
    while (extra[j] && i < 15) argv[i++] = extra[j++];
    argv[i] = NULL;
    return run_capture(argv, out, outsz, timeout_sec);
}

static int hyfe_helper_ok(void)
{
    return access("/usr/bin/hyfe-telegram-claim", X_OK) == 0;
}

static void hyfe_random(const char *subcmd, char *dst, size_t dstsz)
{
    char buf[256];
    char *args[] = { (char *)subcmd, NULL };
    dst[0] = '\0';
    if (hyfe_call(args, buf, sizeof(buf), 6) != 0) return;
    snprintf(dst, dstsz, "%.*s", (int)dstsz - 1, buf);
    trim(dst);
}

static void hyfe_captcha_mode(char *dst, size_t dstsz)
{
    char buf[256];
    char *args[] = { "show-captcha", NULL };
    if (dstsz == 0) return;
    dst[0] = '\0';
    if (hyfe_call(args, buf, sizeof(buf), 8) != 0) {
        snprintf(dst, dstsz, "manual");
        return;
    }
    if (!json_string_after(buf, "\"mode\":", dst, dstsz) || !dst[0])
        snprintf(dst, dstsz, "manual");
}

/* hyfe_config_get: extract `KEY="VALUE"` from `hyfe-telegram-claim show-config`.
 * show-config prints a sourced-shell-style file with values double-quoted by
 * sh_quote() and password-ish fields masked. We only ever read non-secret
 * keys (HYFE_EMAIL_n / HYFE_EID_n) so masking is fine. */
static void hyfe_config_get(const char *key, char *dst, size_t dstsz)
{
    static char buf[8192];
    char *args[] = { "show-config", NULL };
    char *p;
    size_t klen;
    if (dstsz == 0) return;
    dst[0] = '\0';
    if (!key || !*key) return;
    klen = strlen(key);
    if (hyfe_call(args, buf, sizeof(buf), 8) != 0) return;
    p = buf;
    while (p && *p) {
        char *line_start = p;
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line_start, key, klen) == 0 && line_start[klen] == '=') {
            char *v = line_start + klen + 1;
            size_t i = 0;
            int quoted = 0;
            if (*v == '"') { quoted = 1; v++; }
            while (*v && i + 1 < dstsz) {
                if (quoted && *v == '"') break;
                if (!quoted && (*v == '\n' || *v == '\r')) break;
                dst[i++] = *v++;
            }
            dst[i] = '\0';
            if (nl) *nl = '\n';
            return;
        }
        if (nl) { *nl = '\n'; p = nl + 1; }
        else break;
    }
}

static int hyfe_valid_msisdn(const char *s)
{
    int len = 0;
    if (!s) return 0;
    if (*s == '+') s++;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return 0;
        s++; len++;
    }
    return len >= 8 && len <= 16;
}

static int hyfe_valid_otp(const char *s)
{
    int len = 0;
    if (!s) return 0;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return 0;
        s++; len++;
    }
    return len >= 4 && len <= 10;
}

static int hyfe_valid_recaptcha_token(const char *s)
{
    int len = 0;
    if (!s) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!(isalnum(c) || c == '_' || c == '-')) return 0;
        len++;
    }
    return len >= 20 && len <= 4096;
}

static void url_encode(const char *src, char *dst, size_t dstsz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i = 0;
    if (dstsz == 0) return;
    dst[0] = '\0';
    if (!src) return;
    while (*src && i + 4 < dstsz) {
        unsigned char c = (unsigned char)*src;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[i++] = c;
        } else {
            dst[i++] = '%';
            dst[i++] = hex[c >> 4];
            dst[i++] = hex[c & 0xF];
        }
        src++;
    }
    dst[i] = '\0';
}

static void send_photo(const struct config *cfg, const char *chat_id,
                       const char *photo_url, const char *caption)
{
    char url[240], chat_arg[96], photo_arg[1280], cap_arg[1280], out[512];
    int rc;
    char *argv[] = { "curl", "-fsS", "--max-time", "25", "-X", "POST", url,
        "-d", chat_arg, "--data-urlencode", photo_arg, "--data-urlencode", cap_arg, NULL };
    if (!valid_chat_id(chat_id) || !photo_url || !*photo_url) return;
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto", cfg->token);
    snprintf(chat_arg, sizeof(chat_arg), "chat_id=%s", chat_id);
    snprintf(photo_arg, sizeof(photo_arg), "photo=%s", photo_url);
    /* Telegram caps captions at 1024 chars; keep it short. */
    snprintf(cap_arg, sizeof(cap_arg), "caption=%.1000s", caption ? caption : "");
    rc = run_capture(argv, out, sizeof(out), 35);
    if (rc != 0) log_msg("sendPhoto failed rc=%d chat=%s", rc, chat_id);
}

/* send_message_md: same as send_message() but sets parse_mode=MarkdownV2 so
 * we can wrap the LPA string in a copyable `code` block. Caller is
 * responsible for MarkdownV2-escaping non-code text with md_escape(). */
static void send_message_md(const struct config *cfg, const char *chat_id, const char *text)
{
    char url[240], chat_arg[96], msg[MSG_MAX + 1], mode_arg[40], out[512];
    int rc;
    char *argv[] = { "curl", "-fsS", "--max-time", "15", "-X", "POST", url,
        "-d", chat_arg, "--data-urlencode", msg, "-d", mode_arg, NULL };
    if (!valid_chat_id(chat_id)) return;
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", cfg->token);
    snprintf(chat_arg, sizeof(chat_arg), "chat_id=%s", chat_id);
    snprintf(msg, sizeof(msg), "text=%.*s", MSG_MAX - 5, text ? text : "");
    snprintf(mode_arg, sizeof(mode_arg), "parse_mode=MarkdownV2");
    rc = run_capture(argv, out, sizeof(out), 20);
    if (rc != 0) log_msg("sendMessage(md) failed rc=%d chat=%s", rc, chat_id);
}

/* MarkdownV2 reserved chars per Telegram docs: _ * [ ] ( ) ~ ` > # + - = | { } . ! */
static void md_escape(const char *src, char *dst, size_t dstsz)
{
    size_t i = 0;
    if (dstsz == 0) return;
    dst[0] = '\0';
    if (!src) return;
    while (*src && i + 3 < dstsz) {
        char c = *src;
        if (strchr("_*[]()~`>#+-=|{}.!\\", c)) {
            dst[i++] = '\\';
        }
        dst[i++] = c;
        src++;
    }
    dst[i] = '\0';
}

static void send_lpa_result(const struct config *cfg, const char *chat_id,
                            const char *lpa, const char *msisdn, const char *email)
{
    char qr_enc[1024];
    char qr_url[1200];
    char caption[1024];
    char md_lpa[2400];
    char body[MSG_MAX + 1];

    if (!lpa || !*lpa) {
        send_message(cfg, chat_id, "Klaim sukses, tapi LPA kosong di response.");
        return;
    }

    url_encode(lpa, qr_enc, sizeof(qr_enc));
    snprintf(qr_url, sizeof(qr_url),
        "https://api.qrserver.com/v1/create-qr-code/?size=420x420&margin=8&data=%s",
        qr_enc);

    snprintf(caption, sizeof(caption),
        "Klaim eSIM HYFE sukses\nMSISDN: %s\nEmail: %s\n"
        "Scan QR ini dari ponsel/router lain untuk download eSIM.\n"
        "Atau ketik /download <LPA> di chat ini agar router ini sendiri yang download.",
        msisdn && *msisdn ? msisdn : "-", email && *email ? email : "-");
    send_photo(cfg, chat_id, qr_url, caption);

    md_escape(lpa, md_lpa, sizeof(md_lpa));
    snprintf(body, sizeof(body),
        "*LPA string* \\(tap untuk copy\\)\n`%s`",
        md_lpa);
    send_message_md(cfg, chat_id, body);
}

static void hyfe_handle_msisdn(const struct config *cfg, const char *chat, const char *msisdn_raw)
{
    char email[160] = "", eid[64] = "", mode[32] = "";
    char name[64] = "", wa[32] = "";
    char out[BUF_MAX];
    char msg[MSG_MAX + 1];
    char sid[160] = "", err[512] = "";
    int rc;

    if (!hyfe_valid_msisdn(msisdn_raw)) {
        send_message(cfg, chat,
            "MSISDN tidak valid. Kirim digit saja, contoh: 087812345678.\n"
            "/cancel untuk batal.");
        return;
    }

    hyfe_config_get("HYFE_EMAIL_1", email, sizeof(email));
    hyfe_config_get("HYFE_EID_1", eid, sizeof(eid));
    if (!email[0] || !eid[0]) {
        clear_pending(chat);
        send_back_menu(cfg, chat,
            "HYFE belum dikonfigurasi.\n"
            "Set dulu HYFE_EMAIL_1 dan HYFE_EID_1 lewat CLI router:\n"
            "  hyfe-telegram-claim set-config HYFE_EMAIL_1 you@gmail.com\n"
            "  hyfe-telegram-claim set-config HYFE_EID_1 <32-digit EID>\n"
            "atau pakai CLI `esim` menu 7 dulu sekali untuk men-set semua.");
        return;
    }

    hyfe_random("random-name", name, sizeof(name));
    if (!name[0]) snprintf(name, sizeof(name), "Pengguna HYFE");
    hyfe_random("random-wa", wa, sizeof(wa));
    if (!wa[0]) snprintf(wa, sizeof(wa), "0811000000");

    {
        char *args[] = { "start-manual", name, wa, email,
                         (char *)msisdn_raw, eid, NULL };
        rc = hyfe_call(args, out, sizeof(out), 70);
    }
    json_string_after(out, "\"sid\":", sid, sizeof(sid));
    if (rc != 0 || !sid[0]) {
        json_string_after(out, "\"error\":", err, sizeof(err));
        snprintf(msg, sizeof(msg),
            "Gagal memulai klaim HYFE (rc=%d).\n%s\n\n"
            "/cancel untuk batal, atau kirim MSISDN HYFE lain.",
            rc, err[0] ? err : "Cek `logread | grep hyfe` di router untuk detail.");
        send_message(cfg, chat, msg);
        return;
    }

    save_pending(chat, "hyfe_otp", sid);
    hyfe_captcha_mode(mode, sizeof(mode));
    snprintf(msg, sizeof(msg),
        "OTP HYFE telah dikirim ke email *%s*.\n"
        "MSISDN: %s\n"
        "Captcha mode: %s\n\n"
        "Kirim 6-digit OTP yang masuk ke email, atau /cancel untuk batal.",
        email, msisdn_raw, mode[0] ? mode : "manual");
    send_message(cfg, chat, msg);
}

static void hyfe_finalize_response(const struct config *cfg, const char *chat,
                                   int rc, const char *out)
{
    char msg[MSG_MAX + 1];
    char lpa[2048] = "", msisdn[32] = "", email[160] = "", err[512] = "";
    char body[BUF_MAX];
    /* json_string_after mutates buf, so work on a copy. */
    snprintf(body, sizeof(body), "%.*s", (int)sizeof(body) - 1, out ? out : "");
    json_string_after(body, "\"lpa\":", lpa, sizeof(lpa));
    json_string_after(body, "\"msisdn\":", msisdn, sizeof(msisdn));
    json_string_after(body, "\"email\":", email, sizeof(email));
    json_string_after(body, "\"error\":", err, sizeof(err));

    clear_pending(chat);
    if (rc != 0 || strncmp(lpa, "LPA:1$", 6) != 0) {
        snprintf(msg, sizeof(msg),
            "Klaim HYFE gagal (rc=%d).\n%s\n\n"
            "Coba lagi dengan klik *Klaim Free eSIM HYFE* atau pakai CLI router\n"
            "`esim` menu 7 untuk klaim manual lengkap.",
            rc, err[0] ? err : "LPA tidak ditemukan di response.");
        send_back_menu(cfg, chat, msg);
        return;
    }
    send_lpa_result(cfg, chat, lpa, msisdn, email);
    send_main_menu(cfg, chat);
}

static void hyfe_handle_otp(const struct config *cfg, const char *chat,
                            const char *sid, const char *otp)
{
    char mode[32] = "";
    char out[BUF_MAX];
    int rc;

    if (!hyfe_valid_otp(otp)) {
        send_message(cfg, chat,
            "OTP harus 4-10 digit angka. Coba lagi atau /cancel.");
        return;
    }
    hyfe_captcha_mode(mode, sizeof(mode));
    if (strcmp(mode, "manual") == 0) {
        char arg[800];
        snprintf(arg, sizeof(arg), "%.150s:%.10s", sid, otp);
        save_pending(chat, "hyfe_captcha", arg);
        send_message(cfg, chat,
            "Mode captcha = manual.\n"
            "Buka https://hyfe.id di browser, selesaikan reCAPTCHA, lalu\n"
            "copy g-recaptcha-response token (string panjang) dan kirim disini.\n\n"
            "Tip: di DevTools browser, jalankan\n"
            "  document.querySelector('[name=g-recaptcha-response]').value\n"
            "setelah klik captcha untuk dapat token-nya.\n\n"
            "/cancel untuk batal.");
        return;
    }

    {
        char *args[] = { "finish", (char *)sid, (char *)otp, mode, NULL };
        rc = hyfe_call(args, out, sizeof(out), 200);
    }
    hyfe_finalize_response(cfg, chat, rc, out);
}

static void hyfe_handle_captcha(const struct config *cfg, const char *chat,
                                const char *sid_otp, const char *token)
{
    char out[BUF_MAX];
    char sid[160] = "", otp[16] = "";
    const char *colon;
    int rc;

    colon = strchr(sid_otp, ':');
    if (!colon) {
        clear_pending(chat);
        send_back_menu(cfg, chat,
            "State HYFE rusak (sid:otp hilang). Mulai lagi dari *Klaim Free eSIM HYFE*.");
        return;
    }
    snprintf(sid, sizeof(sid), "%.*s", (int)(colon - sid_otp), sid_otp);
    snprintf(otp, sizeof(otp), "%.10s", colon + 1);

    if (!hyfe_valid_recaptcha_token(token)) {
        send_message(cfg, chat,
            "Token reCAPTCHA tidak valid (harus 20-4096 char alfanumerik/_-).\n"
            "Paste ulang token-nya, atau /cancel.");
        return;
    }

    {
        char *args[] = { "finish-manual", sid, otp, (char *)token, NULL };
        rc = hyfe_call(args, out, sizeof(out), 200);
    }
    hyfe_finalize_response(cfg, chat, rc, out);
}

static void hyfe_start_wizard(const struct config *cfg, const char *chat)
{
    char email[160] = "", eid[64] = "", mode[32] = "";
    char msg[MSG_MAX + 1];

    if (!hyfe_helper_ok()) {
        send_back_menu(cfg, chat,
            "hyfe-telegram-claim belum terpasang di router.\n"
            "Install paket luci-app-lpac-manager (yang berisi hyfe-telegram-claim),\n"
            "lalu coba lagi.");
        return;
    }

    hyfe_config_get("HYFE_EMAIL_1", email, sizeof(email));
    hyfe_config_get("HYFE_EID_1", eid, sizeof(eid));
    hyfe_captcha_mode(mode, sizeof(mode));

    if (!email[0] || !eid[0]) {
        snprintf(msg, sizeof(msg),
            "HYFE belum siap:\n"
            "  HYFE_EMAIL_1 = %s\n"
            "  HYFE_EID_1   = %s\n"
            "  Captcha mode = %s\n\n"
            "Set keduanya dulu di router sebelum klaim dari Telegram:\n"
            "  hyfe-telegram-claim set-config HYFE_EMAIL_1 you@gmail.com\n"
            "  hyfe-telegram-claim set-config HYFE_EID_1 <32-digit EID>\n"
            "Atau jalankan CLI `esim` menu 7 -> Konfigurasi sekali lewat SSH.",
            email[0] ? email : "(kosong)",
            eid[0]   ? eid   : "(kosong)",
            mode[0]  ? mode  : "manual");
        send_back_menu(cfg, chat, msg);
        return;
    }

    save_pending(chat, "hyfe_msisdn", "");
    snprintf(msg, sizeof(msg),
        "Klaim eSIM HYFE\n"
        "  Email   : %s\n"
        "  EID     : %s\n"
        "  Captcha : %s\n\n"
        "Kirim MSISDN HYFE (contoh: 087812345678) untuk klaim,\n"
        "atau /cancel untuk batal.",
        email, eid, mode[0] ? mode : "manual");
    send_back_menu(cfg, chat, msg);
}

static int save_config_value(const char *key, const char *value, char *out, size_t outsz)
{
    char cmd[512];
    if (!valid_config_key(key) || !safe_config_value(value)) {
        snprintf(out, outsz, "Config key/value tidak valid.");
        return 1;
    }
    if (strcmp(key, "apdu_backend") == 0 &&
        strcmp(value, "qmi") != 0 && strcmp(value, "mbim") != 0 && strcmp(value, "at") != 0) {
        snprintf(out, outsz, "apdu_backend harus qmi, mbim, atau at.");
        return 1;
    }
    if ((strstr(key, "debug") || strcmp(key, "mbim_proxy") == 0 || strcmp(key, "mbim_skip_slot_mapping") == 0) &&
        strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
        snprintf(out, outsz, "%s harus 0 atau 1.", key);
        return 1;
    }
    snprintf(cmd, sizeof(cmd), "uci -q set lpac-esim.main.%s='%s' && uci -q commit lpac-esim", key, value);
    return run_shell(cmd, out, outsz, 5);
}

static void handle_command(const struct config *cfg, const char *chat, const char *text)
{
    char out[BUF_MAX];
    char msg[MSG_MAX + 1];
    char argbuf[768];
    char pending_action[32];
    char pending_arg[768];
    const char *arg;
    int rc;

    if (!cfg->chat_id[0]) {
        if (cmd_match(text, "/chatid")) {
            snprintf(msg, sizeof(msg), "Chat ID: %s\nMasukkan ID ini di LuCI/CLI Telegram Bot settings.", chat);
        } else if (cmd_match(text, "/start") || cmd_match(text, "/menu")) {
            snprintf(msg, sizeof(msg), "Chat ID: %s\nBot belum dikunci. Isi Allowed Chat ID ini di settings lalu Save & Restart Bot.", chat);
        } else {
            snprintf(msg, sizeof(msg), "Bot belum dikunci ke chat. Jalankan /chatid lalu isi Allowed Chat ID di settings.");
        }
        send_message(cfg, chat, msg);
        return;
    }

    if (strcmp(chat, cfg->chat_id) != 0) {
        /* Reply to /chatid and /start even when the chat doesn't match the
         * locked allowed_chat_id, so an admin who locked the bot to the
         * wrong ID can still see their own chat ID over Telegram and fix
         * it from LuCI. We deliberately do NOT echo cfg->chat_id back here:
         * doing so would let any Telegram user who finds the bot username
         * harvest the admin's numeric chat ID. Other commands stay silent
         * so the device-control surface remains locked to the configured
         * chat. */
        if (cmd_match(text, "/chatid")) {
            snprintf(msg, sizeof(msg),
                "Chat ID Anda: %s\nBot ini dikunci ke chat lain.\n"
                "Jika ini chat admin yang seharusnya, update Allowed Chat ID di LuCI/CLI lalu Save & Restart Bot.",
                chat);
            send_message(cfg, chat, msg);
        } else if (cmd_match(text, "/start")) {
            snprintf(msg, sizeof(msg),
                "Chat ID Anda: %s\nBot ini dikunci ke chat lain. "
                "Kirim /chatid untuk konfirmasi ID, lalu update Allowed Chat ID di settings.",
                chat);
            send_message(cfg, chat, msg);
        } else if (cfg->debug) {
            send_message(cfg, chat, "Unauthorized chat ID.");
        }
        return;
    }

    if (strcmp(text, "Menu Utama") == 0 || cmd_match(text, "/menu")) {
        clear_pending(chat);
        send_main_menu(cfg, chat);
    } else if (cmd_match(text, "/start")) {
        clear_pending(chat);
        send_main_menu(cfg, chat);
    } else if (cmd_match(text, "/help")) {
        help_text(msg, sizeof(msg));
        send_back_menu(cfg, chat, msg);
    } else if (cmd_match(text, "/chatid")) {
        snprintf(msg, sizeof(msg), "Chat ID: %s", chat);
        send_message(cfg, chat, msg);
    } else if (strcmp(text, "Info EID") == 0) {
        char *args[] = { "chip", NULL };
        rc = lpac_api(args, out, sizeof(out), 25);
        if (rc == 0) compact_result(out, msg, sizeof(msg));
        else snprintf(msg, sizeof(msg), "Gagal membaca chip info (rc=%d).", rc);
        send_back_menu(cfg, chat, msg);
    } else if (strcmp(text, "Profile List") == 0) {
        send_profile_menu(cfg, chat);
    } else if (strcmp(text, "Switch / Enable eSIM") == 0 ||
               strcmp(text, "Disable eSIM") == 0 ||
               strcmp(text, "Delete eSIM") == 0) {
        const char *act = (text[0] == 'S') ? "switch" : (text[0] == 'D' && text[1] == 'i' ? "disable" : "delete");
        save_pending(chat, act, "");
        snprintf(msg, sizeof(msg), "Kirim ICCID/AID untuk %s, atau /cancel.", profile_label(act));
        send_back_menu(cfg, chat, msg);
    } else if (strcmp(text, "Cek Kuota") == 0) {
        save_pending(chat, "quota", "");
        send_back_menu(cfg, chat, "Kirim nomor XL/MSISDN untuk cek kuota, contoh: 08123456789. Atau /cancel.");
    } else if (strcmp(text, "Download eSIM") == 0) {
        save_pending(chat, "download", "");
        send_back_menu(cfg, chat, "Kirim activation code eSIM format LPA:1$SM-DP+$MATCHING_ID. Atau /cancel.");
    } else if (strcmp(text, "Klaim Free eSIM HYFE") == 0 || cmd_match(text, "/hyfe")) {
        hyfe_start_wizard(cfg, chat);
    } else if (strcmp(text, "Settings") == 0) {
        send_settings_menu(cfg, chat);
    } else if (strcmp(text, "Tools") == 0) {
        send_tools_menu(cfg, chat);
    } else if (strcmp(text, "Backend QMI") == 0 || strcmp(text, "Backend MBIM") == 0 || strcmp(text, "Backend AT") == 0) {
        const char *backend = (strstr(text, "QMI") ? "qmi" : (strstr(text, "MBIM") ? "mbim" : "at"));
        rc = save_config_value("apdu_backend", backend, out, sizeof(out));
        if (rc == 0) snprintf(msg, sizeof(msg), "Backend LPAC diset ke %s.", backend);
        else snprintf(msg, sizeof(msg), "Gagal set backend: %.500s", out);
        send_settings_menu(cfg, chat);
        send_message(cfg, chat, msg);
    } else if (strcmp(text, "Debug ON") == 0 || strcmp(text, "Debug OFF") == 0) {
        const char *v = strstr(text, "ON") ? "1" : "0";
        save_config_value("apdu_debug", v, out, sizeof(out));
        save_config_value("http_debug", v, out, sizeof(out));
        save_config_value("at_debug", v, out, sizeof(out));
        send_settings_menu(cfg, chat);
    } else if (strcmp(text, "Status Modem") == 0) {
        char *args[] = { "modem-status", NULL };
        rc = lpac_api(args, out, sizeof(out), 25);
        if (rc == 0) compact_result(out, msg, sizeof(msg));
        else snprintf(msg, sizeof(msg), "Gagal membaca modem status (rc=%d).", rc);
        send_back_menu(cfg, chat, msg);
    } else if (strcmp(text, "Notifications") == 0) {
        char *args[] = { "notif-list", NULL };
        rc = lpac_api(args, out, sizeof(out), 25);
        if (rc == 0) compact_result(out, msg, sizeof(msg));
        else snprintf(msg, sizeof(msg), "Gagal membaca notifications (rc=%d).", rc);
        send_back_menu(cfg, chat, msg);
    } else if (strcmp(text, "Process Notifications") == 0) {
        char *args[] = { "notif-process", NULL };
        rc = lpac_api(args, out, sizeof(out), 70);
        compact_result(out, msg, sizeof(msg));
        if (rc != 0 && !msg[0]) snprintf(msg, sizeof(msg), "Gagal memproses notifications (rc=%d).", rc);
        send_back_menu(cfg, chat, msg);
    } else if (strcmp(text, "Lock Status") == 0) {
        char *args[] = { "lock-status", NULL };
        rc = lpac_api(args, out, sizeof(out), 10);
        if (rc == 0) compact_result(out, msg, sizeof(msg));
        else snprintf(msg, sizeof(msg), "Gagal membaca lock status (rc=%d).", rc);
        send_back_menu(cfg, chat, msg);
    } else if (cmd_match(text, "/cancel")) {
        clear_pending(chat);
        send_back_menu(cfg, chat, "Dibatalkan.");
    } else if (load_pending(chat, pending_action, sizeof(pending_action), pending_arg, sizeof(pending_arg)) &&
               strncmp(pending_action, "hyfe_", 5) == 0) {
        /* HYFE wizard dispatch. Must run BEFORE the generic YA/BATAL handler
         * below because hyfe_otp/hyfe_captcha pending entries carry the sid
         * (and the sid:otp pair) in pending_arg, which the generic handler
         * would otherwise mistakenly feed to lpac-esim as a profile action. */
        char hyfe_input[800];
        snprintf(hyfe_input, sizeof(hyfe_input), "%.700s", text);
        trim(hyfe_input);
        if (strcmp(pending_action, "hyfe_msisdn") == 0) {
            hyfe_handle_msisdn(cfg, chat, hyfe_input);
        } else if (strcmp(pending_action, "hyfe_otp") == 0) {
            hyfe_handle_otp(cfg, chat, pending_arg, hyfe_input);
        } else if (strcmp(pending_action, "hyfe_captcha") == 0) {
            hyfe_handle_captcha(cfg, chat, pending_arg, hyfe_input);
        } else {
            clear_pending(chat);
            send_back_menu(cfg, chat, "State HYFE tidak dikenal, mulai lagi dari menu.");
        }
    } else if ((strcmp(text, "YA") == 0 || strcmp(text, "BATAL") == 0) &&
               load_pending(chat, pending_action, sizeof(pending_action), pending_arg, sizeof(pending_arg))) {
        if (strcmp(text, "BATAL") == 0) {
            clear_pending(chat);
            send_back_menu(cfg, chat, "Dibatalkan.");
            return;
        }
        clear_pending(chat);
        if (strcmp(pending_action, "download") == 0) {
            char *args[] = { "download", "--lpa", pending_arg, NULL };
            rc = lpac_api(args, out, sizeof(out), 45);
        } else if (strcmp(pending_action, "quota") == 0) {
            rc = esim_quota(pending_arg, out, sizeof(out));
        } else {
            char *args[] = { pending_action, pending_arg, NULL };
            rc = lpac_api(args, out, sizeof(out), 45);
        }
        compact_result(out, msg, sizeof(msg));
        if (rc != 0 && !msg[0]) snprintf(msg, sizeof(msg), "Command %s gagal (rc=%d).", pending_action, rc);
        send_back_menu(cfg, chat, msg);
    } else if (load_pending(chat, pending_action, sizeof(pending_action), pending_arg, sizeof(pending_arg)) && pending_arg[0] == '\0') {
        snprintf(argbuf, sizeof(argbuf), "%.700s", text);
        trim(argbuf);
        if (!safe_arg(argbuf)) {
            send_message(cfg, chat, "Input tidak valid. /cancel untuk batal.");
            return;
        }
        if (strcmp(pending_action, "download") == 0 && strncmp(argbuf, "LPA:1$", 6) != 0) {
            send_message(cfg, chat, "Format download harus LPA:1$SM-DP+$MATCHING_ID");
            return;
        }
        save_pending(chat, pending_action, argbuf);
        snprintf(msg, sizeof(msg), "Konfirmasi %s:\n%s\n\nTekan YA untuk lanjut atau BATAL.", profile_label(pending_action), argbuf);
        send_keyboard(cfg, chat, msg,
            "{\"keyboard\":[[{\"text\":\"YA\"},{\"text\":\"BATAL\"}],[{\"text\":\"Menu Utama\"}]],\"resize_keyboard\":true,\"one_time_keyboard\":true}");
    } else if (cmd_match(text, "/profiles") || cmd_match(text, "/profile") || cmd_match(text, "/iccid")) {
        char *args[] = { "profiles", NULL };
        rc = lpac_api(args, out, sizeof(out), 35);
        if (rc == 0) format_profiles(out, msg, sizeof(msg));
        else snprintf(msg, sizeof(msg), "Gagal membaca profiles (rc=%d).", rc);
        send_message(cfg, chat, msg);
    } else if (cmd_match(text, "/info") || cmd_match(text, "/eid")) {
        char *args[] = { "chip", NULL };
        rc = lpac_api(args, out, sizeof(out), 25);
        if (rc == 0) compact_result(out, msg, sizeof(msg));
        else snprintf(msg, sizeof(msg), "Gagal membaca chip info (rc=%d).", rc);
        send_message(cfg, chat, msg);
    } else if (cmd_match(text, "/status")) {
        char *args[] = { "modem-status", NULL };
        rc = lpac_api(args, out, sizeof(out), 25);
        if (rc == 0) compact_result(out, msg, sizeof(msg));
        else snprintf(msg, sizeof(msg), "Gagal membaca modem status (rc=%d).", rc);
        send_message(cfg, chat, msg);
    } else if (cmd_match(text, "/lock")) {
        char *args[] = { "lock-status", NULL };
        rc = lpac_api(args, out, sizeof(out), 10);
        if (rc == 0) compact_result(out, msg, sizeof(msg));
        else snprintf(msg, sizeof(msg), "Gagal membaca lock status (rc=%d).", rc);
        send_message(cfg, chat, msg);
    } else if (cmd_match(text, "/notifications")) {
        char *args[] = { "notif-list", NULL };
        rc = lpac_api(args, out, sizeof(out), 25);
        if (rc == 0) compact_result(out, msg, sizeof(msg));
        else snprintf(msg, sizeof(msg), "Gagal membaca notifications (rc=%d).", rc);
        send_message(cfg, chat, msg);
    } else if (cmd_match(text, "/process_notifications")) {
        char *args[] = { "notif-process", NULL };
        rc = lpac_api(args, out, sizeof(out), 70);
        compact_result(out, msg, sizeof(msg));
        if (rc != 0 && !msg[0]) snprintf(msg, sizeof(msg), "Gagal memproses notifications (rc=%d).", rc);
        send_message(cfg, chat, msg);
    } else if (cmd_match(text, "/switch") || cmd_match(text, "/disable") || cmd_match(text, "/delete")) {
        const char *cmd = cmd_match(text, "/switch") ? "switch" : (cmd_match(text, "/disable") ? "disable" : "delete");
        arg = cmd_args(text);
        snprintf(argbuf, sizeof(argbuf), "%.700s", arg);
        trim(argbuf);
        if (!safe_arg(argbuf) || strchr(argbuf, ' ')) {
            send_message(cfg, chat, "Format: /switch <ICCID|AID>, /disable <ICCID|AID>, atau /delete <ICCID|AID>");
            return;
        }
        save_pending(chat, cmd, argbuf);
        snprintf(msg, sizeof(msg), "Konfirmasi %s:\n%s\n\nTekan YA untuk lanjut atau BATAL.", profile_label(cmd), argbuf);
        send_keyboard(cfg, chat, msg,
            "{\"keyboard\":[[{\"text\":\"YA\"},{\"text\":\"BATAL\"}],[{\"text\":\"Menu Utama\"}]],\"resize_keyboard\":true,\"one_time_keyboard\":true}");
    } else if (cmd_match(text, "/quota")) {
        arg = cmd_args(text);
        snprintf(argbuf, sizeof(argbuf), "%.700s", arg);
        trim(argbuf);
        if (!safe_arg(argbuf) || strchr(argbuf, ' ')) {
            send_message(cfg, chat, "Format: /quota <nomor XL>");
            return;
        }
        rc = esim_quota(argbuf, out, sizeof(out));
        snprintf(msg, sizeof(msg), "%.3900s", out[0] ? out : (rc == 0 ? "Tidak ada output kuota." : "Gagal cek kuota."));
        send_back_menu(cfg, chat, msg);
    } else if (cmd_match(text, "/download")) {
        arg = cmd_args(text);
        snprintf(argbuf, sizeof(argbuf), "%.700s", arg);
        trim(argbuf);
        if (!safe_arg(argbuf) || strncmp(argbuf, "LPA:1$", 6) != 0) {
            send_message(cfg, chat, "Format: /download LPA:1$SM-DP+$MATCHING_ID");
            return;
        }
        {
            char *args[] = { "download", "--lpa", argbuf, NULL };
            rc = lpac_api(args, out, sizeof(out), 45);
        }
        compact_result(out, msg, sizeof(msg));
        if (rc != 0 && !msg[0]) snprintf(msg, sizeof(msg), "Download gagal diproses (rc=%d).", rc);
        send_message(cfg, chat, msg);
    } else if (cmd_match(text, "/set_config")) {
        char *space;
        arg = cmd_args(text);
        snprintf(argbuf, sizeof(argbuf), "%.700s", arg);
        trim(argbuf);
        space = strchr(argbuf, ' ');
        if (!space) {
            send_message(cfg, chat, "Format: /set_config <option> <value>");
            return;
        }
        *space++ = '\0';
        trim(space);
        rc = save_config_value(argbuf, space, out, sizeof(out));
        if (rc == 0) snprintf(msg, sizeof(msg), "Config %s disimpan: %s", argbuf, space);
        else snprintf(msg, sizeof(msg), "Gagal simpan config %.80s: %.500s", argbuf, out);
        send_back_menu(cfg, chat, msg);
    } else {
        send_main_menu(cfg, chat);
    }
}

static void process_updates(const struct config *cfg, char *json, long long *offset)
{
    char *p = json;
    while ((p = strstr(p, "\"update_id\":"))) {
        long long update_id = strtoll(p + 12, NULL, 10);
        char *next = strstr(p + 12, "\"update_id\":");
        char saved = '\0';
        char text[1024] = "";
        char chat[64] = "";

        if (next) {
            saved = *next;
            *next = '\0';
        }
        json_chat_id_after(p, chat, sizeof(chat));
        json_string_after(p, "\"text\":", text, sizeof(text));
        if (update_id >= *offset) *offset = update_id + 1;
        if (chat[0] && text[0]) handle_command(cfg, chat, text);
        if (next) {
            *next = saved;
            p = next;
        } else {
            break;
        }
    }
}

int main(void)
{
    struct config cfg;
    long long offset = load_offset();
    char updates[BUF_MAX];

    load_config(&cfg);
    if (strcmp(cfg.enabled, "1") != 0) {
        log_msg("disabled by UCI");
        return 0;
    }
    if (!valid_token(cfg.token)) {
        log_msg("missing or invalid telegram_bot_token");
        return 1;
    }
    if (cfg.chat_id[0] && !valid_chat_id(cfg.chat_id)) {
        log_msg("invalid telegram_allowed_chat_id");
        return 1;
    }

    log_msg("started (poll=%ds, debug=%d, chat_locked=%s)",
            cfg.reconnect_delay, cfg.debug,
            cfg.chat_id[0] ? cfg.chat_id : "no");
    write_status("starting", 0, offset);
    {
        int consecutive_fail = 0;
        time_t last_fail_log = 0;
        for (;;) {
            int rc = telegram_get_updates(&cfg, offset, updates, sizeof(updates));
            if (rc == 0) {
                process_updates(&cfg, updates, &offset);
                save_offset(offset);
                consecutive_fail = 0;
                write_status("ok", 0, offset);
            } else {
                consecutive_fail++;
                /* Rate-limited error logging so a persistent failure (bad
                 * token, no DNS, no internet) leaves a visible trail in
                 * syslog without spamming it. Log every failure when debug
                 * is on; otherwise log at most once a minute and always log
                 * the first failure of a new outage. */
                time_t now = time(NULL);
                int should_log = cfg.debug || consecutive_fail == 1 ||
                                 (now - last_fail_log) >= 60;
                if (should_log) {
                    log_msg("getUpdates failed rc=%d (consecutive=%d)",
                            rc, consecutive_fail);
                    last_fail_log = now;
                }
                write_status("error", rc, offset);
                /* Back off harder on repeated failures (max ~30s) so we
                 * don't hammer api.telegram.org during outages. */
                int backoff = consecutive_fail > 5 ? 30 : 10;
                sleep((unsigned int)backoff);
            }
            sleep((unsigned int)cfg.reconnect_delay);
        }
    }
}
