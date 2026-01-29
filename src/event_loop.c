#include "event_loop.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/epoll.h>
#include <time.h>

#include <glib.h>
#include <glib-unix.h>

#include "util/asserts.h"
#include "util/collection.h"
#include "util/logging.h"
#include "util/refcounting.h"

struct evloop {
    refcount_t n_refs;
    GMainContext *context;
    GMainLoop *loop;
    pthread_t owning_thread;
};

struct evloop *evloop_new(void) {
    struct evloop *loop;

    loop = malloc(sizeof *loop);
    if (loop == NULL) {
        return NULL;
    }

    loop->context = g_main_context_new();
    if (loop->context == NULL) {
        LOG_ERROR("Couldn't create GLib main context.\n");
        goto fail_free_loop;
    }

    loop->loop = g_main_loop_new(loop->context, FALSE);
    if (loop->loop == NULL) {
        LOG_ERROR("Couldn't create GLib main loop.\n");
        goto fail_unref_context;
    }

    loop->n_refs = REFCOUNT_INIT_1;
    loop->owning_thread = pthread_self();
    return loop;

fail_unref_context:
    g_main_context_unref(loop->context);

fail_free_loop:
    free(loop);
    return NULL;
}

void evloop_destroy(struct evloop *loop) {
    ASSERT_NOT_NULL(loop);
    g_main_loop_unref(loop->loop);
    g_main_context_unref(loop->context);
    free(loop);
}

DEFINE_REF_OPS(evloop, n_refs)

int evloop_get_fd(struct evloop *loop) {
    ASSERT_NOT_NULL(loop);
    // GLib doesn't expose a single pollable fd like sd-event does.
    // This function is kept for API compatibility but returns -1.
    // Users should use evloop_run() instead of polling manually.
    (void) loop;
    return -1;
}

int evloop_run(struct evloop *loop) {
    ASSERT_NOT_NULL(loop);

    g_main_context_push_thread_default(loop->context);
    g_main_loop_run(loop->loop);
    g_main_context_pop_thread_default(loop->context);

    return 0;
}

int evloop_schedule_exit(struct evloop *loop) {
    ASSERT_NOT_NULL(loop);
    // g_main_loop_quit is thread-safe
    g_main_loop_quit(loop->loop);
    return 0;
}

struct task {
    void_callback_t callback;
    void *userdata;
};

static gboolean on_execute_task(gpointer userdata) {
    struct task *task = userdata;

    ASSERT_NOT_NULL(task);
    task->callback(task->userdata);
    free(task);

    return G_SOURCE_REMOVE;
}

int evloop_post_task(struct evloop *loop, void_callback_t callback, void *userdata) {
    struct task *task;
    GSource *source;

    ASSERT_NOT_NULL(loop);
    ASSERT_NOT_NULL(callback);

    task = malloc(sizeof *task);
    if (task == NULL) {
        return ENOMEM;
    }

    task->callback = callback;
    task->userdata = userdata;

    source = g_idle_source_new();
    g_source_set_callback(source, on_execute_task, task, NULL);
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_attach(source, loop->context);
    g_source_unref(source);

    return 0;
}

static uint64_t get_monotonic_time_usec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000 + (uint64_t) ts.tv_nsec / 1000;
}

int evloop_post_delayed_task(struct evloop *loop, void_callback_t callback, void *userdata, uint64_t target_time_usec) {
    struct task *task;
    GSource *source;
    uint64_t now_usec, delay_usec;
    guint delay_ms;

    ASSERT_NOT_NULL(loop);
    ASSERT_NOT_NULL(callback);

    task = malloc(sizeof *task);
    if (task == NULL) {
        return ENOMEM;
    }

    task->callback = callback;
    task->userdata = userdata;

    now_usec = get_monotonic_time_usec();
    delay_usec = (target_time_usec > now_usec) ? (target_time_usec - now_usec) : 0;
    delay_ms = (guint) (delay_usec / 1000);

    // If delay is 0, use idle source for immediate execution
    if (delay_ms == 0) {
        source = g_idle_source_new();
    } else {
        source = g_timeout_source_new(delay_ms);
    }

    g_source_set_callback(source, on_execute_task, task, NULL);
    g_source_attach(source, loop->context);
    g_source_unref(source);

    return 0;
}

struct evsrc {
    struct evloop *loop;
    GSource *gsource;
    evloop_io_handler_t io_callback;
    void *userdata;
    int fd;
};

void evsrc_destroy(struct evsrc *src) {
    ASSERT_NOT_NULL(src);

    if (src->gsource != NULL) {
        g_source_destroy(src->gsource);
        g_source_unref(src->gsource);
    }

    evloop_unref(src->loop);
    free(src);
}

static gboolean on_io_ready(gint fd, GIOCondition condition, gpointer userdata) {
    struct evsrc *evsrc = userdata;
    enum event_handler_return ret;
    uint32_t revents = 0;

    ASSERT_NOT_NULL(evsrc);

    // Convert GIOCondition to EPOLL-style events
    if (condition & G_IO_IN)   revents |= EPOLLIN;
    if (condition & G_IO_OUT)  revents |= EPOLLOUT;
    if (condition & G_IO_ERR)  revents |= EPOLLERR;
    if (condition & G_IO_HUP)  revents |= EPOLLHUP;
    if (condition & G_IO_PRI)  revents |= EPOLLPRI;

    ret = evsrc->io_callback(fd, revents, evsrc->userdata);

    if (ret == kRemoveSrc_EventHandlerReturn) {
        // Don't call evsrc_destroy here - just remove the source
        // The caller is responsible for cleanup
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

struct evsrc *evloop_add_io(struct evloop *loop, int fd, uint32_t events, evloop_io_handler_t callback, void *userdata) {
    struct evsrc *evsrc;
    GSource *source;
    GIOCondition condition = 0;

    ASSERT_NOT_NULL(loop);
    ASSERT_NOT_NULL(callback);

    evsrc = malloc(sizeof *evsrc);
    if (evsrc == NULL) {
        return NULL;
    }

    // Convert EPOLL events to GIOCondition
    if (events & EPOLLIN)   condition |= G_IO_IN;
    if (events & EPOLLOUT)  condition |= G_IO_OUT;
    if (events & EPOLLERR)  condition |= G_IO_ERR;
    if (events & EPOLLHUP)  condition |= G_IO_HUP;
    if (events & EPOLLPRI)  condition |= G_IO_PRI;

    source = g_unix_fd_source_new(fd, condition);
    if (source == NULL) {
        LOG_ERROR("Could not create GLib Unix FD source.\n");
        free(evsrc);
        return NULL;
    }

    evsrc->loop = evloop_ref(loop);
    evsrc->gsource = source;
    evsrc->io_callback = callback;
    evsrc->userdata = userdata;
    evsrc->fd = fd;

    g_source_set_callback(source, (GSourceFunc) on_io_ready, evsrc, NULL);
    g_source_attach(source, loop->context);

    return evsrc;
}

struct evthread {
    struct evloop *loop;
    pthread_t thread;
};

struct evthread_startup_args {
    struct evthread *evthread;
    sem_t initialization_done;
    bool initialization_success;
};

static void *evthread_entry(void *userdata) {
    struct evthread_startup_args *args;
    struct evthread *evthread;
    struct evloop *evloop;

    ASSERT_NOT_NULL(userdata);
    args = userdata;

    evthread = malloc(sizeof *evthread);
    if (evthread == NULL) {
        goto fail_post_semaphore;
    }

    evloop = evloop_new();
    if (evloop == NULL) {
        goto fail_free_evthread;
    }

    evthread->loop = evloop;
    evthread->thread = pthread_self();

    args->evthread = evthread;
    args->initialization_success = true;
    sem_post(&args->initialization_done);

    // Run the event loop
    evloop_run(evloop);

    return NULL;

fail_free_evthread:
    free(evthread);

fail_post_semaphore:
    args->initialization_success = false;
    sem_post(&args->initialization_done);
    return NULL;
}

struct evthread *evthread_start(void) {
    struct evthread_startup_args args;
    struct evthread *evthread;
    pthread_t tid;
    int ok;

    args.evthread = NULL;
    args.initialization_success = false;

    ok = sem_init(&args.initialization_done, 0, 0);
    if (ok != 0) {
        LOG_ERROR("Could not initialize semaphore. sem_init: %s\n", strerror(errno));
        return NULL;
    }

    ok = pthread_create(&tid, NULL, evthread_entry, &args);
    if (ok != 0) {
        LOG_ERROR("Could not create new event thread. pthread_create: %s\n", strerror(ok));
        sem_destroy(&args.initialization_done);
        return NULL;
    }

    ok = sem_wait(&args.initialization_done);
    if (ok != 0) {
        LOG_ERROR("Couldn't wait for event thread initialization. sem_wait: %s\n", strerror(errno));
        pthread_cancel(tid);
        sem_destroy(&args.initialization_done);
        return NULL;
    }

    sem_destroy(&args.initialization_done);

    if (!args.initialization_success) {
        pthread_join(tid, NULL);
        return NULL;
    }

    evthread = args.evthread;
    return evthread;
}

struct evloop *evthread_get_evloop(struct evthread *thread) {
    ASSERT_NOT_NULL(thread);
    return thread->loop;
}

void evthread_stop(struct evthread *thread) {
    ASSERT_NOT_NULL(thread);
    evloop_schedule_exit(thread->loop);
    pthread_join(thread->thread, NULL);
    evloop_unref(thread->loop);
    free(thread);
}
