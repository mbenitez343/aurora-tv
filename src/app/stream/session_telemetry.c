#include "session_telemetry.h"
#include "session.h"

#include "logging.h"

#include <SDL.h>
#include <curl/curl.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>

/* Ring buffer of per-window samples. At ~2s per window this holds ~68 minutes;
 * older samples are overwritten so a long session keeps its most recent tail. */
#define TELEM_CAP 2048

typedef struct {
    int64_t wall_ms;
    float receivedFps, decodedFps;
    uint32_t totalFrames, receivedFrames, submittedFrames, networkDroppedFrames;
    uint32_t rtt, rttVariance;
    float decoderLatencyMs;   /* NDL render-buffer latency (not raw decode) */
    float submitMs;           /* avg SS4S feed time per frame */
    uint32_t bitrateBps;      /* measured received bitrate */
} telem_sample_t;

static struct {
    bool enabled;
    char url[512];
    char key[256];

    SDL_mutex *lock;
    telem_sample_t buf[TELEM_CAP];
    size_t head;   /* next write index */
    size_t count;  /* valid samples (<= TELEM_CAP) */

    /* Session context (immutable during a session). */
    char session_id[40];
    char host[128];
    char codec[24];
    int width, height, fps, bitrate;
    bool hdr;
} tel;

static bool tel_inited = false;

static void tel_lazy_init(void) {
    if (!tel_inited) {
        memset(&tel, 0, sizeof(tel));
        tel.lock = SDL_CreateMutex();
        tel_inited = true;
    }
}

void telemetry_configure(bool enabled, const char *seq_url, const char *seq_api_key) {
    tel_lazy_init();
    /* Only active when explicitly enabled AND a key is present (the URL may be
     * baked in, but the key is user-supplied on-device and never in source). */
    tel.enabled = enabled && seq_url != NULL && seq_url[0] != '\0' &&
                  seq_api_key != NULL && seq_api_key[0] != '\0';
    if (tel.enabled) {
        snprintf(tel.url, sizeof(tel.url), "%s", seq_url);
        snprintf(tel.key, sizeof(tel.key), "%s", seq_api_key);
    }
}

bool telemetry_enabled(void) {
    return tel_inited && tel.enabled;
}

void telemetry_session_begin(const char *host, int width, int height, int fps, int bitrate,
                             const char *codec, bool hdr) {
    tel_lazy_init();
    if (!tel.enabled) {
        return;
    }
    SDL_LockMutex(tel.lock);
    tel.head = 0;
    tel.count = 0;
    snprintf(tel.host, sizeof(tel.host), "%s", host ? host : "");
    snprintf(tel.codec, sizeof(tel.codec), "%s", codec ? codec : "");
    tel.width = width;
    tel.height = height;
    tel.fps = fps;
    tel.bitrate = bitrate;
    tel.hdr = hdr;
    snprintf(tel.session_id, sizeof(tel.session_id), "%08x%08x",
             (unsigned) time(NULL), (unsigned) SDL_GetTicks());
    SDL_UnlockMutex(tel.lock);
    commons_log_info("Telemetry", "Session begin %s (%dx%d@%d, %d kbps, %s%s)",
                     tel.session_id, width, height, fps, bitrate, codec ? codec : "?",
                     hdr ? " HDR" : "");
}

void telemetry_sample(const struct VIDEO_STATS *s) {
    if (!tel.enabled || s == NULL) {
        return;
    }
    telem_sample_t smp;
    memset(&smp, 0, sizeof(smp));
    smp.wall_ms = (int64_t) time(NULL) * 1000;
    smp.receivedFps = s->receivedFps;
    smp.decodedFps = s->decodedFps;
    smp.totalFrames = s->totalFrames;
    smp.receivedFrames = s->receivedFrames;
    smp.submittedFrames = s->submittedFrames;
    smp.networkDroppedFrames = s->networkDroppedFrames;
    smp.rtt = s->rtt;
    smp.rttVariance = s->rttVariance;
    smp.decoderLatencyMs = s->avgDecoderLatency;
    smp.submitMs = s->submittedFrames ? (float) s->totalSubmitTime / (float) s->submittedFrames : 0.0f;
    smp.bitrateBps = s->currentBitrateKbps;

    SDL_LockMutex(tel.lock);
    tel.buf[tel.head] = smp;
    tel.head = (tel.head + 1) % TELEM_CAP;
    if (tel.count < TELEM_CAP) {
        tel.count++;
    }
    SDL_UnlockMutex(tel.lock);
}

/* --- CLEF payload building --- */

typedef struct {
    char *data;
    size_t len, cap;
} strbuf_t;

static bool sb_reserve(strbuf_t *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) {
        return true;
    }
    size_t ncap = sb->cap ? sb->cap * 2 : 8192;
    while (ncap < sb->len + extra + 1) {
        ncap *= 2;
    }
    char *nd = realloc(sb->data, ncap);
    if (!nd) {
        return false;
    }
    sb->data = nd;
    sb->cap = ncap;
    return true;
}

static void sb_appendf(strbuf_t *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need > 0 && sb_reserve(sb, (size_t) need)) {
        vsnprintf(sb->data + sb->len, (size_t) need + 1, fmt, ap2);
        sb->len += (size_t) need;
    }
    va_end(ap2);
}

static void iso8601(int64_t wall_ms, char *out, size_t n) {
    time_t sec = (time_t) (wall_ms / 1000);
    struct tm tmv;
    gmtime_r(&sec, &tmv);
    char base[32];
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tmv);
    snprintf(out, n, "%s.%03dZ", base, (int) (wall_ms % 1000));
}

/* Build the CLEF body from the buffered samples and reset the buffer. Caller
 * must free the returned string. Returns NULL if nothing to send. */
static char *tel_build_and_clear(void) {
    strbuf_t sb = {0};
    SDL_LockMutex(tel.lock);
    if (tel.count == 0) {
        SDL_UnlockMutex(tel.lock);
        return NULL;
    }
    size_t start = (tel.head + TELEM_CAP - tel.count) % TELEM_CAP;
    for (size_t i = 0; i < tel.count; i++) {
        const telem_sample_t *s = &tel.buf[(start + i) % TELEM_CAP];
        char ts[40];
        iso8601(s->wall_ms, ts, sizeof(ts));
        float loss = s->totalFrames ? (float) s->networkDroppedFrames / (float) s->totalFrames * 100.0f : 0.0f;
        sb_appendf(&sb,
                   "{\"@t\":\"%s\",\"@mt\":\"perf {sid} rx={rxFps} de={deFps} loss={loss}%% "
                   "rtt={rtt} dec={decMs}ms sub={subMs}ms {mbps}Mbps\","
                   "\"sid\":\"%s\",\"host\":\"%s\",\"w\":%d,\"h\":%d,\"fps\":%d,"
                   "\"codec\":\"%s\",\"hdr\":%s,\"cfgKbps\":%d,"
                   "\"rxFps\":%.2f,\"deFps\":%.2f,\"loss\":%.3f,\"rtt\":%u,\"rttVar\":%u,"
                   "\"decMs\":%.2f,\"subMs\":%.2f,\"mbps\":%.2f,\"bps\":%u,"
                   "\"dropped\":%u,\"total\":%u}\n",
                   ts, tel.session_id, tel.host, tel.width, tel.height, tel.fps,
                   tel.codec, tel.hdr ? "true" : "false", tel.bitrate,
                   s->receivedFps, s->decodedFps, loss, s->rtt, s->rttVariance,
                   s->decoderLatencyMs, s->submitMs, (float) s->bitrateBps / 1000000.0f,
                   s->bitrateBps, s->networkDroppedFrames, s->totalFrames);
    }
    tel.head = 0;
    tel.count = 0;
    SDL_UnlockMutex(tel.lock);
    return sb.data;
}

typedef struct {
    char *payload;
    char *url;
    char *key;
} tel_flush_args_t;

static size_t discard_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void) ptr;
    (void) userdata;
    return size * nmemb;
}

static int tel_flush_thread(void *arg) {
    tel_flush_args_t *a = arg;
    CURL *curl = curl_easy_init();
    if (curl) {
        char endpoint[576];
        snprintf(endpoint, sizeof(endpoint), "%s/ingest/clef", a->url);
        char keyhdr[300];
        snprintf(keyhdr, sizeof(keyhdr), "X-Seq-ApiKey: %s", a->key);
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/vnd.serilog.clef");
        headers = curl_slist_append(headers, keyhdr);
        curl_easy_setopt(curl, CURLOPT_URL, endpoint);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, a->payload);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) strlen(a->payload));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        CURLcode rc = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (rc != CURLE_OK) {
            commons_log_warn("Telemetry", "Flush failed: %s", curl_easy_strerror(rc));
        } else {
            commons_log_info("Telemetry", "Flushed to Seq (HTTP %ld)", status);
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    free(a->payload);
    free(a->url);
    free(a->key);
    free(a);
    return 0;
}

static bool tel_dispatch_flush(void) {
    char *payload = tel_build_and_clear();
    if (payload == NULL) {
        return false;
    }
    tel_flush_args_t *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        free(payload);
        return false;
    }
    a->payload = payload;
    a->url = strdup(tel.url);
    a->key = strdup(tel.key);
    /* Detached: it outlives the session and cleans up after itself. */
    SDL_Thread *t = SDL_CreateThread(tel_flush_thread, "telemetry", a);
    if (t == NULL) {
        free(a->payload);
        free(a->url);
        free(a->key);
        free(a);
        return false;
    }
    SDL_DetachThread(t);
    return true;
}

void telemetry_session_end(void) {
    if (!tel.enabled) {
        return;
    }
    tel_dispatch_flush();
}

bool telemetry_flush_now(void) {
    if (!tel.enabled) {
        return false;
    }
    return tel_dispatch_flush();
}
