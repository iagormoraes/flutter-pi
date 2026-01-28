// SPDX-License-Identifier: MIT
/*
 * Event Loop
 *
 * - multithreaded event loop based on GLib GMainLoop
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_EVENT_LOOP_H
#define _FLUTTERPI_SRC_EVENT_LOOP_H

#include <stdint.h>

#include "util/refcounting.h"

struct evloop;

struct evloop *evloop_new(void);

void evloop_destroy(struct evloop *loop);

DECLARE_REF_OPS(evloop)

int evloop_get_fd(struct evloop *loop);

int evloop_run(struct evloop *loop);

int evloop_schedule_exit(struct evloop *loop);

// Thread-safe task posting - can be called from any thread
int evloop_post_task(struct evloop *loop, void_callback_t callback, void *userdata);

int evloop_post_delayed_task(struct evloop *loop, void_callback_t callback, void *userdata, uint64_t target_time_usec);

struct evsrc;

void evsrc_destroy(struct evsrc *src);

enum event_handler_return { kNoAction_EventHandlerReturn, kRemoveSrc_EventHandlerReturn };

typedef enum event_handler_return (*evloop_io_handler_t)(int fd, uint32_t revents, void *userdata);

// Thread-safe I/O source addition - can be called from any thread
struct evsrc *evloop_add_io(struct evloop *loop, int fd, uint32_t events, evloop_io_handler_t callback, void *userdata);

struct evthread;

struct evthread *evthread_start(void);

struct evloop *evthread_get_evloop(struct evthread *thread);

void evthread_stop(struct evthread *thread);

#endif  // _FLUTTERPI_SRC_EVENT_LOOP_H
