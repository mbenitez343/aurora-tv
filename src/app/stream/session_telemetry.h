#pragma once

#include <stdbool.h>

struct VIDEO_STATS;

/* Per-session performance telemetry.
 *
 * Design goal: never contend with the live video stream. Samples are appended
 * to a small in-memory ring buffer during the session (one snapshot every
 * stats window, ~1-2s, off the per-frame path) and shipped to a Seq instance
 * only AFTER the session ends -- when WiFi bandwidth and CPU are free again --
 * from a detached background thread. A manual flush is available for live
 * debugging when the bandwidth hit is acceptable.
 *
 * The Seq API key is NEVER compiled in; it comes from on-device settings. */

/** Apply config (from settings) before a session. url may default; key empty = disabled. */
void telemetry_configure(bool enabled, const char *seq_url, const char *seq_api_key);

/** Fast check used on the stats thread to avoid work when telemetry is off. */
bool telemetry_enabled(void);

/** Begin a session: reset the buffer and record the stream context. */
void telemetry_session_begin(const char *host, int width, int height, int fps, int bitrate,
                             const char *codec, bool hdr);

/** Append one sample. Called from the stats-submit (depacketizer) thread ~1-2s. */
void telemetry_sample(const struct VIDEO_STATS *snap);

/** End of session: ship the buffer to Seq asynchronously (no-op if disabled/empty). */
void telemetry_session_end(void);

/** Manual flush (e.g. overlay button) during a session. Returns false if nothing sent. */
bool telemetry_flush_now(void);
