/* -*- c-file-style:"stroustrup"; indent-tabs-mode: nil -*- */
#include "pubnub_ntf_callback.h"

#include <winsock2.h>
#include <windows.h>
#include <process.h>

#include "pubnub_internal.h"
#include "pubnub_assert.h"
#include "pubnub_log.h"
#include "pbntf_trans_outcome_common.h"
#include "pubnub_timer_list.h"
#include "pbpal.h"


#include "pubnub_get_native_socket.h"

#include <stdlib.h>
#include <string.h>


struct SocketWatcherData {
    _Guarded_by_(mutw) size_t apoll_size;
    _Guarded_by_(mutw) size_t apoll_cap;
    _Guarded_by_(mutw) _Field_size_(apoll_cap) WSAPOLLFD *apoll;
    _Guarded_by_(mutw) _Field_size_(apoll_cap) pubnub_t **apb;
    CRITICAL_SECTION mutw;
    uintptr_t thread_handle;
    DWORD thread_id;
#if PUBNUB_TIMERS_API
    _Guarded_by_(mutw) pubnub_t *timer_head;
#endif
    CRITICAL_SECTION queue_lock;
    _Guarded_by_(queue_lock) size_t queue_size;
    _Guarded_by_(queue_lock) size_t queue_head;
    _Guarded_by_(queue_lock) size_t queue_tail;
    _Guarded_by_(queue_lock) pubnub_t **queue_apb;
};

static struct SocketWatcherData m_watcher;


static void save_socket(struct SocketWatcherData *watcher, pubnub_t *pb)
{
    size_t i;
    pbpal_native_socket_t sockt = pubnub_get_native_socket(pb);

    PUBNUB_ASSERT(watcher->apoll_size <= watcher->apoll_cap);

    if (INVALID_SOCKET == sockt) {
        return;
    }
    for (i = 0; i < watcher->apoll_size; ++i) {
        PUBNUB_ASSERT_OPT(watcher->apoll[i].fd != sockt);
    }
    if (watcher->apoll_size == watcher->apoll_cap) {
        size_t newcap = watcher->apoll_cap + 2;
        WSAPOLLFD *npapoll = (WSAPOLLFD*)realloc(watcher->apoll, sizeof watcher->apoll[0] * newcap);
        if (NULL == npapoll) {
            return;
        }
        else {
            pubnub_t **npapb = (pubnub_t**)realloc(watcher->apb, sizeof watcher->apb[0] * newcap);
            watcher->apoll = npapoll;
            if (NULL == npapb) {
                return;
            }
            watcher->apb = npapb;
        }
        watcher->apoll_cap = newcap;
    }

    watcher->apoll[watcher->apoll_size].fd = sockt;
    watcher->apoll[watcher->apoll_size].events = POLLOUT;
    watcher->apb[watcher->apoll_size] = pb;
    ++watcher->apoll_size;
}


static void remove_socket(struct SocketWatcherData *watcher, pubnub_t *pb)
{
    size_t i;
    pbpal_native_socket_t sockt = pubnub_get_native_socket(pb);

    PUBNUB_ASSERT(watcher->apoll_size <= watcher->apoll_cap);

    if (INVALID_SOCKET == sockt) {
        return;
    }
    for (i = 0; i < watcher->apoll_size; ++i) {
        if (watcher->apoll[i].fd == sockt) {
            size_t to_move = watcher->apoll_size - i - 1;
            PUBNUB_ASSERT(watcher->apb[i] == pb);
            if (to_move > 0) {
                memmove(watcher->apoll + i, watcher->apoll + i + 1, sizeof watcher->apoll[0] * to_move);
                memmove(watcher->apb + i, watcher->apb + i + 1, sizeof watcher->apb[0] * to_move);
            }
            --watcher->apoll_size;
            break;
        }
    }
}


static int elapsed_ms(FILETIME prev_timspec, FILETIME timspec)
{
    ULARGE_INTEGER prev;
    ULARGE_INTEGER current;
    prev.LowPart = prev_timspec.dwLowDateTime;
    prev.HighPart = prev_timspec.dwHighDateTime;
    current.LowPart = timspec.dwLowDateTime;
    current.HighPart = timspec.dwHighDateTime;
    return (int)((current.QuadPart - prev.QuadPart) / (10 * 1000));
}


void socket_watcher_thread(void *arg)
{
    FILETIME prev_time;
    GetSystemTimeAsFileTime(&prev_time);

    for (;;) {
        const DWORD ms = 100;

        EnterCriticalSection(&m_watcher.queue_lock);
        if (m_watcher.queue_head != m_watcher.queue_tail) {
            pubnub_t *pbp = m_watcher.queue_apb[m_watcher.queue_tail++];
            LeaveCriticalSection(&m_watcher.queue_lock);
            if (pbp != NULL) {
                pubnub_mutex_lock(pbp->monitor);
                pbnc_fsm(pbp);
                pubnub_mutex_unlock(pbp->monitor);
            }
            EnterCriticalSection(&m_watcher.queue_lock);
            if (m_watcher.queue_tail == m_watcher.queue_size) {
                m_watcher.queue_tail = 0;
            }
        }
        LeaveCriticalSection(&m_watcher.queue_lock);

        EnterCriticalSection(&m_watcher.mutw);
        if (0 == m_watcher.apoll_size) {
            LeaveCriticalSection(&m_watcher.mutw);
            continue;
        }
        {
            int rslt = WSAPoll(m_watcher.apoll, m_watcher.apoll_size, ms);
            if (SOCKET_ERROR == rslt) {
                /* error? what to do about it? */
                PUBNUB_LOG_WARNING("poll size = %d, error = %d\n", m_watcher.apoll_size, WSAGetLastError());
            }
            else if (rslt > 0) {
                size_t i;
                size_t apoll_size = m_watcher.apoll_size;
                for (i = 0; i < apoll_size; ++i) {
                    if (m_watcher.apoll[i].revents & (POLLIN | POLLOUT)) {
                        pubnub_t *pbp = m_watcher.apb[i];
                        pubnub_mutex_lock(pbp->monitor);
                        pbnc_fsm(pbp);
                        if (apoll_size == m_watcher.apoll_size) {
                            if (m_watcher.apoll[i].events == POLLOUT) {
                                if ((pbp->state == PBS_WAIT_DNS_RCV) ||
                                    (pbp->state >= PBS_RX_HTTP_VER)) {
                                    m_watcher.apoll[i].events = POLLIN;
                                }
                            }
                            else {
                                if ((pbp->state > PBS_WAIT_DNS_RCV) &&
                                    (pbp->state < PBS_RX_HTTP_VER)) {
                                    m_watcher.apoll[i].events = POLLOUT;
                                }
                            }
                        }
                        else {
                            PUBNUB_ASSERT_OPT(apoll_size == m_watcher.apoll_size + 1);
                            apoll_size = m_watcher.apoll_size;
                            --i;
                        }
                        pubnub_mutex_unlock(pbp->monitor);
                    }
                }
            }
        }
        if (PUBNUB_TIMERS_API) {
            FILETIME current_time;
            int elapsed;
            GetSystemTimeAsFileTime(&current_time);
            elapsed = elapsed_ms(prev_time, current_time);
            if (elapsed > 0) {
                pubnub_t *expired = pubnub_timer_list_as_time_goes_by(&m_watcher.timer_head, elapsed);
                while (expired != NULL) {
                    pubnub_t *next = expired->next;

                    pubnub_mutex_lock(expired->monitor);
                    pbnc_stop(expired, PNR_TIMEOUT);
                    pubnub_mutex_unlock(expired->monitor);

                    expired->next = NULL;
                    expired->previous = NULL;
                    expired = next;
                }
                prev_time = current_time;
            }
        }

        LeaveCriticalSection(&m_watcher.mutw);
    }
}


int pbntf_init(void)
{
    InitializeCriticalSection(&m_watcher.mutw);
    m_watcher.thread_handle = _beginthread(socket_watcher_thread, 0, NULL);
    m_watcher.thread_id = GetThreadId(m_watcher.thread_handle);

    InitializeCriticalSection(&m_watcher.queue_lock);
    m_watcher.queue_size = 1024;
    m_watcher.queue_head = m_watcher.queue_tail = 0;
    m_watcher.queue_apb = calloc(m_watcher.queue_size, sizeof m_watcher.queue_apb[0]);

    return (m_watcher.queue_apb == NULL) ? -1 : 0;
}


int pbntf_got_socket(pubnub_t *pb, pb_socket_t socket)
{
    EnterCriticalSection(&m_watcher.mutw);

    save_socket(&m_watcher, pb);
    if (PUBNUB_TIMERS_API) {
        m_watcher.timer_head = pubnub_timer_list_add(m_watcher.timer_head, pb);
    }
    pb->options.use_blocking_io = false;
    pbpal_set_blocking_io(pb);
    
    LeaveCriticalSection(&m_watcher.mutw);

    return +1;
}


static void remove_timer_safe(pubnub_t *to_remove)
{
    if (PUBNUB_TIMERS_API) {
        if ((to_remove->previous != NULL) || (to_remove->next != NULL)
            || (to_remove == m_watcher.timer_head)) {
            m_watcher.timer_head = pubnub_timer_list_remove(m_watcher.timer_head, to_remove);
        }
    }
}


static void remove_from_processing_queue(pubnub_t *pb)
{
    bool found = false;
    size_t i;

    PUBNUB_ASSERT_OPT(pb != NULL);

    EnterCriticalSection(&m_watcher.queue_lock);
    for (i = m_watcher.queue_tail; i != m_watcher.queue_head; i = (((i + 1) == m_watcher.queue_size) ? 0 : i + 1)) {
        if (m_watcher.queue_apb[i] == pb) {
            m_watcher.queue_apb[i] = NULL;
            break;
        }
    }
    LeaveCriticalSection(&m_watcher.queue_lock);
}


void pbntf_lost_socket(pubnub_t *pb, pb_socket_t socket)
{
    EnterCriticalSection(&m_watcher.mutw);

    remove_socket(&m_watcher, pb);
    remove_timer_safe(pb);
    remove_from_processing_queue(pb);

    LeaveCriticalSection(&m_watcher.mutw);
}


static void update_socket(struct SocketWatcherData *watcher, pubnub_t *pb)
{
    size_t i;
    pbpal_native_socket_t scket = pubnub_get_native_socket(pb);
    if (INVALID_SOCKET == scket) {
        return;
    }

    for (i = 0; i < watcher->apoll_size; ++i) {
        if (watcher->apb[i] == pb) {
            watcher->apoll[i].fd = scket;
            return;
        }
    }
}


void pbntf_update_socket(pubnub_t *pb, pb_socket_t socket)
{
    PUBNUB_UNUSED(socket);

    EnterCriticalSection(&m_watcher.mutw);

    update_socket(&m_watcher, pb);

    LeaveCriticalSection(&m_watcher.mutw);
}


void pbntf_trans_outcome(pubnub_t *pb)
{
    PBNTF_TRANS_OUTCOME_COMMON(pb);
    if (pb->cb != NULL) {
        pb->cb(pb, pb->trans, pb->core.last_result, pb->user_data);
    }
}


int pbntf_enqueue_for_processing(pubnub_t *pb)
{
    int result;
    size_t next_head;

    PUBNUB_ASSERT_OPT(pb != NULL);

    EnterCriticalSection(&m_watcher.queue_lock);
    next_head = m_watcher.queue_head + 1;
    if (next_head == m_watcher.queue_size) {
        next_head = 0;
    }
    if (next_head != m_watcher.queue_tail) {
        m_watcher.queue_apb[m_watcher.queue_head] = pb;
        m_watcher.queue_head = next_head;
        result = +1;
    }
    else {
        result = -1;
    }
    LeaveCriticalSection(&m_watcher.queue_lock);

    return result;
}


int pbntf_requeue_for_processing(pubnub_t *pb)
{
    bool found = false;
    size_t i;

    PUBNUB_ASSERT_OPT(pb != NULL);

    EnterCriticalSection(&m_watcher.queue_lock);
    for (i = m_watcher.queue_tail; i != m_watcher.queue_head; i = (((i + 1) == m_watcher.queue_size) ? 0 : i + 1)) {
        if (m_watcher.queue_apb[i] == pb) {
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&m_watcher.queue_lock);

    return !found ? pbntf_enqueue_for_processing(pb) : 0;
}


enum pubnub_res pubnub_last_result(pubnub_t *pb)
{
    PUBNUB_ASSERT(pb_valid_ctx_ptr(pb));
    return pb->core.last_result;
}


enum pubnub_res pubnub_register_callback(pubnub_t *pb, pubnub_callback_t cb, void *user_data)
{
    PUBNUB_ASSERT(pb_valid_ctx_ptr(pb));
    pb->cb = cb;
    pb->user_data = user_data;
    return PNR_OK;
}


void *pubnub_get_user_data(pubnub_t *pb)
{
    PUBNUB_ASSERT(pb_valid_ctx_ptr(pb));
    return pb->user_data;
}