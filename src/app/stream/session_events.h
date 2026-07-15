#pragma once

#include <stdbool.h>
#include <SDL_events.h>

typedef struct session_t session_t;

bool session_handle_input_event(session_t *session, const SDL_Event *event);

/** Flush any controller input coalesced during the last event batch. */
void session_handle_input_flush(session_t *session);
