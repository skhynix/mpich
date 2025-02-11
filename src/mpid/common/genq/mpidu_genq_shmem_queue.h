/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#ifndef MPIDU_GENQ_SHMEM_QUEUE_H_INCLUDED
#define MPIDU_GENQ_SHMEM_QUEUE_H_INCLUDED

#include "mpidimpl.h"
#include "mpidu_genqi_shmem_types.h"

#include <stdint.h>
#include <stdio.h>

#define MPIDU_GENQ_SHMEM_QUEUE_TYPE__MPSC MPIDU_GENQ_SHMEM_QUEUE_TYPE__NEM_MPSC
#define MPIDU_GENQ_SHMEM_QUEUE_TYPE__MPMC MPIDU_GENQ_SHMEM_QUEUE_TYPE__NEM_MPMC
#define MPIDU_GENQ_SHMEM_QUEUE_TYPE__SPSC MPIDU_GENQ_SHMEM_QUEUE_TYPE__NEM_SPSC

typedef enum {
    MPIDU_GENQ_SHMEM_QUEUE_TYPE__SERIAL,
    MPIDU_GENQ_SHMEM_QUEUE_TYPE__INV_MPSC,
    MPIDU_GENQ_SHMEM_QUEUE_TYPE__NEM_MPSC,
    MPIDU_GENQ_SHMEM_QUEUE_TYPE__NEM_MPMC,
    MPIDU_GENQ_SHMEM_QUEUE_TYPE__NEM_SPSC,
} MPIDU_genq_shmem_queue_type_e;

/* SERIAL */

static inline int MPIDU_genqi_serial_init(MPIDU_genq_shmem_queue_u * queue)
{
    queue->q.head.s = 0;
    queue->q.tail.s = 0;
    return 0;
}

static inline int MPIDU_genqi_serial_dequeue(MPIDU_genqi_shmem_pool_s * pool_obj,
                                             MPIDU_genq_shmem_queue_u * queue, void **cell)
{
    MPIDU_genqi_shmem_cell_header_s *cell_h = NULL;
    cell_h = HANDLE_TO_HEADER(pool_obj, queue->q.head.s);
    if (queue->q.head.s) {
        queue->q.head.s = cell_h->u.serial_queue.next;
        if (!cell_h->u.serial_queue.next) {
            queue->q.tail.s = 0;
        }
        *cell = HEADER_TO_CELL(cell_h);
    } else {
        *cell = NULL;
    }
    return 0;
}

static inline int MPIDU_genqi_serial_enqueue(MPIDU_genqi_shmem_pool_s * pool_obj,
                                             MPIDU_genq_shmem_queue_u * queue, void *cell)
{
    MPIDU_genqi_shmem_cell_header_s *cell_h = CELL_TO_HEADER(cell);
    cell_h->u.serial_queue.next = 0;

    uintptr_t handle = cell_h->handle;

    if (queue->q.tail.s) {
        HANDLE_TO_HEADER(pool_obj, queue->q.tail.s)->u.serial_queue.next = handle;
    }
    queue->q.tail.s = handle;
    if (!queue->q.head.s) {
        queue->q.head.s = queue->q.tail.s;
    }
    return 0;
}

/* NEMESIS MPSC QUEUE */

static inline int MPIDU_genqi_nem_mpsc_init(MPIDU_genq_shmem_queue_u * queue)
{
    MPL_atomic_store_ptr(&queue->q.head.m, NULL);
    MPL_atomic_store_ptr(&queue->q.tail.m, NULL);
    return 0;
}

static inline int MPIDU_genqi_nem_mpsc_dequeue(MPIDU_genqi_shmem_pool_s * pool_obj,
                                               MPIDU_genq_shmem_queue_u * queue, void **cell)
{
    // if (cxl_acquire_lock(&queue->q.lock, MPIR_Process.rank)) {
    //    fprintf(stderr, "Rank %d MPIDU_genqi_nem_mpsc_enqueue failed to acquire the lock of queue\n", MPIR_Process.rank);
    //    return -1;
    // } 
    clflush_region_with_mfence(queue, sizeof(MPIDU_genq_shmem_queue_u));
    void *handle = cxl_nt_load_ptr(&queue->q.head.m.v);
    if (!handle) {
        /* queue is empty */
        *cell = NULL;
    } else {
        MPIDU_genqi_shmem_cell_header_s *cell_h = NULL;
        cell_h = HANDLE_TO_HEADER(pool_obj, handle);
        *cell = HEADER_TO_CELL(cell_h);
        // if (cxl_acquire_lock(&cell_h->lock, MPIR_Process.rank)) {
        //    fprintf(stderr, "Rank %d MPIDU_genqi_nem_mpsc_enqueue failed to acquire the lock of cell\n", MPIR_Process.rank);
        //    return -1;
        // }
        clflush_region_with_mfence(cell_h, pool_obj->cell_alloc_size);
        void *next_handle = cxl_nt_load_ptr(&cell_h->u.nem_queue.next_m.v);
        if (next_handle != NULL) {
            /* just dequeue the head */
            cxl_nt_store_ptr(&queue->q.head.m.v, next_handle);
           // printf("MPIDU_genqi_nem_mpsc_dequeue queue->q.head.m:  %#" PRIxPTR "\n", queue->q.head.m);
        } else {
            /* single element, tail == head,
             * have to make sure no enqueuing is in progress */
            cxl_nt_store_ptr(&queue->q.head.m.v, NULL);
            // printf("MPIDU_genqi_nem_mpsc_dequeue queue->q.head.m:  %#" PRIxPTR "\n", queue->q.head.m);
            if (cxl_acquire_lock(&queue->q.lock, MPIR_Process.rank)) {
                fprintf(stderr, "Rank %d MPIDU_genqi_nem_mpsc_enqueue failed to acquire the lock of queue\n", MPIR_Process.rank);
                return -1;
            }             
            clflush_region_with_mfence(queue, sizeof(MPIDU_genq_shmem_queue_u));
            // void* cas_ret = MPL_atomic_cas_ptr(&queue->q.tail.m, handle, NULL);
            void* cas_ret = cxl_nt_cas_ptr(&queue->q.tail.m.v, handle, NULL);
            clflush_region_with_mfence(queue, sizeof(MPIDU_genq_shmem_queue_u));
            cxl_release_lock(&queue->q.lock, MPIR_Process.rank);
            if (cas_ret == handle) {
                /* no enqueuing in progress, we are done */
            } else {
                /* busy wait for the enqueuing to finish */
                do {
                    clflush_region_with_mfence(cell_h, pool_obj->cell_alloc_size);
                    next_handle = cxl_nt_load_ptr(&cell_h->u.nem_queue.next_m.v);
                } while (next_handle == NULL);
                /* then set the header */
                cxl_nt_store_ptr(&queue->q.head.m.v, next_handle);
                // printf("MPIDU_genqi_nem_mpsc_dequeue queue->q.head.m:  %#" PRIxPTR "\n", queue->q.head.m);
            }
        }
        clflush_region_with_mfence(cell_h, pool_obj->cell_alloc_size);
        // if (cxl_release_lock(&cell_h->lock, MPIR_Process.rank)) {
        //    fprintf(stderr, "Rank %d MPIDU_genqi_nem_mpsc_enqueue failed to release the lock of cell\n", MPIR_Process.rank);
        //    return -1;
        // }
    }
    clflush_region_with_mfence(queue, sizeof(MPIDU_genq_shmem_queue_u));
    // if (cxl_release_lock(&queue->q.lock, MPIR_Process.rank)) {
    //    fprintf(stderr, "Rank %d MPIDU_genqi_nem_mpsc_enqueue failed to release the lock of queue\n", MPIR_Process.rank);
    //    return -1;
    // } 
    return 0;
}

static inline int MPIDU_genqi_nem_mpsc_enqueue(MPIDU_genqi_shmem_pool_s * pool_obj,
                                               MPIDU_genq_shmem_queue_u * queue, void *cell)
{
    MPIDU_genqi_shmem_cell_header_s *cell_h = CELL_TO_HEADER(cell);
    // if (cxl_acquire_lock(&queue->q.lock, MPIR_Process.rank)) {
    //   fprintf(stderr, "Rank %d MPIDU_genqi_nem_mpsc_enqueue failed to acquire the lock of queue\n", MPIR_Process.rank);
    //   return -1;
    // }
    // if (cxl_acquire_lock(&cell_h->lock, MPIR_Process.rank)) {
    //    fprintf(stderr, "Rank %d MPIDU_genqi_nem_mpsc_enqueue failed to acquire the lock of cell\n", MPIR_Process.rank);
    //    return -1;
    // }
    clflush_region_with_mfence(cell_h, pool_obj->cell_alloc_size);
    clflush_region_with_mfence(queue, sizeof(MPIDU_genq_shmem_queue_u));

    cxl_nt_store_ptr(&cell_h->u.nem_queue.next_m.v, NULL);

    void *handle = (void *) cell_h->handle;

    void *tail_handle = NULL;
    if (cxl_acquire_lock(&queue->q.lock, MPIR_Process.rank)) {
      fprintf(stderr, "Rank %d MPIDU_genqi_nem_mpsc_enqueue failed to acquire the lock of queue\n", MPIR_Process.rank);
      return -1;
    }
    while (1) {
        // we may not fetch the correct tail_handle because of the writing conflicts
        // tail_handle = cxl_nt_swap_ptr(&queue->q.tail.m.v, handle);
        clflush_region_with_mfence(queue, sizeof(MPIDU_genq_shmem_queue_u));
        tail_handle = MPL_atomic_swap_ptr(&queue->q.tail.m, handle);
        clflush_region_with_mfence(queue, sizeof(MPIDU_genq_shmem_queue_u));
        if (cxl_nt_load_ptr(&queue->q.tail.m.v) == handle) {
            break;
        }
    }
    // tail_handle = MPL_atomic_swap_ptr(&queue->q.tail.m, handle);
    // clflush_region_with_mfence(queue, sizeof(MPIDU_genq_shmem_queue_u));
    cxl_release_lock(&queue->q.lock, MPIR_Process.rank);
    if (tail_handle == handle) {
        fprintf(stderr,  "tail_handle == handle err!!!!\n");
        MPIR_Assert(tail_handle != handle);
    }
    if (tail_handle == NULL) {
        /* queue was empty */
        cxl_nt_store_ptr(&queue->q.head.m.v, handle);
    } else {
        MPIDU_genqi_shmem_cell_header_s *tail_cell_h = NULL;
        tail_cell_h = HANDLE_TO_HEADER(pool_obj, tail_handle);
        cxl_nt_store_ptr(&tail_cell_h->u.nem_queue.next_m.v, handle);
        clflush_region_with_mfence(tail_cell_h, pool_obj->cell_alloc_size);
    }
    clflush_region_with_mfence(cell_h, pool_obj->cell_alloc_size);
    clflush_region_with_mfence(queue, sizeof(MPIDU_genq_shmem_queue_u));
    // if (cxl_release_lock(&cell_h->lock, MPIR_Process.rank)) {
    //   fprintf(stderr, "Rank %d MPIDU_genqi_nem_mpsc_enqueue failed to release the lock of cell\n", MPIR_Process.rank);
    //   return -1;
    // }
    // if (cxl_release_lock(&queue->q.lock, MPIR_Process.rank)) {
    //    fprintf(stderr, "Rank %d MPIDU_genqi_nem_mpsc_enqueue failed to release the lock of queue\n", MPIR_Process.rank);
    //    return -1;
    // } 
    return 0;
}

/* NEMESIS MPMC QUEUE */

static inline int MPIDU_genqi_nem_mpmc_init(MPIDU_genq_shmem_queue_u * queue)
{
    MPL_atomic_store_ptr(&queue->q.head.m, NULL);
    MPL_atomic_store_ptr(&queue->q.tail.m, NULL);
    return 0;
}

static inline int MPIDU_genqi_nem_mpmc_dequeue(MPIDU_genqi_shmem_pool_s * pool_obj,
                                               MPIDU_genq_shmem_queue_u * queue, void **cell)
{
    int counter = 0;

    /* Add an inner loop here to avoid going all the way around the progress engine in the case of
     * contention on this lock. If this is heavily contended, eventually this will give up and kick
     * back out to the full progress engine. */
    while (counter++ <= 20) {
        void *handle = MPL_atomic_load_ptr(&queue->q.head.m);
        if (!handle) {
            /* queue is empty */
            *cell = NULL;
            break;
        } else {
            MPIDU_genqi_shmem_cell_header_s *cell_h = NULL;
            cell_h = HANDLE_TO_HEADER(pool_obj, handle);
            *cell = HEADER_TO_CELL(cell_h);

            void *next_handle = MPL_atomic_load_ptr(&cell_h->u.nem_queue.next_m);
            if (next_handle != NULL) {
                /* just dequeue the head */
                if (MPL_atomic_cas_ptr(&queue->q.head.m, handle, next_handle) != handle) {
                    /* Multiple head dequeues at the same time. Give up holding the head of the queue
                     * and start over. */
                    *cell = NULL;
                    continue;
                }
            } else {
                /* single element, tail == head,
                 * have to make sure no enqueuing is in progress */
                if (MPL_atomic_cas_ptr(&queue->q.head.m, handle, NULL) != handle) {
                    /* Conflicts over head/tail pointers. Give up holding the head of the queue and
                     * start over. */
                    *cell = NULL;
                    continue;
                }
                if (MPL_atomic_cas_ptr(&queue->q.tail.m, handle, NULL) == handle) {
                    /* no enqueuing in progress, we are done */
                } else {
                    /* busy wait for the enqueuing to finish */
                    do {
                        next_handle = MPL_atomic_load_ptr(&cell_h->u.nem_queue.next_m);
                    } while (next_handle == NULL);
                    /* then set the header */
                    MPL_atomic_store_ptr(&queue->q.head.m, next_handle);
                }
            }
        }
    }
    return 0;
}

static inline int MPIDU_genqi_nem_mpmc_enqueue(MPIDU_genqi_shmem_pool_s * pool_obj,
                                               MPIDU_genq_shmem_queue_u * queue, void *cell)
{
    MPIDU_genqi_shmem_cell_header_s *cell_h = CELL_TO_HEADER(cell);
    MPL_atomic_store_ptr(&cell_h->u.nem_queue.next_m, NULL);

    void *handle = (void *) cell_h->handle;

    void *tail_handle = NULL;
    tail_handle = MPL_atomic_swap_ptr(&queue->q.tail.m, handle);
    if (tail_handle == NULL) {
        /* queue was empty */
        MPL_atomic_store_ptr(&queue->q.head.m, handle);
    } else {
        MPIDU_genqi_shmem_cell_header_s *tail_cell_h = NULL;
        tail_cell_h = HANDLE_TO_HEADER(pool_obj, tail_handle);
        MPL_atomic_store_ptr(&tail_cell_h->u.nem_queue.next_m, handle);
    }
    return 0;
}

/* NEMESIS SPSC QUEUE */
static inline int MPIDU_genqi_nem_spsc_init(MPIDU_genq_shmem_queue_u * queue)
{
    cxl_nt_store_uint64(&queue->q.head.s, 0);
    cxl_nt_store_uint64(&queue->q.tail.s, 0);
    return 0;
}

static inline int MPIDU_genqi_nem_spsc_dequeue(MPIDU_genq_shmem_pool_t pool, MPIDU_genq_shmem_queue_t queue, int sender_id, int recv_id, void **cell)
{
    MPIDU_genqi_shmem_pool_s *pool_obj = (MPIDU_genqi_shmem_pool_s *) pool;
    MPIDU_genq_shmem_queue_u *queue_obj = (MPIDU_genq_shmem_queue_u *) queue;

    int cur_head = cxl_nt_load_uint64(&queue_obj->q.head.s);
    int cur_tail = cxl_nt_load_uint64(&queue_obj->q.tail.s);
    if (cur_head == cur_tail) {
        // Ring queue is empty.
        *cell = NULL;
        return 0;
    }
    int cell_id = cur_tail;
    MPIDU_genqi_shmem_cell_header_s *cell_h = NULL;
    cell_h = SENDER_RECV_CELL_TO_HEADER(pool_obj, sender_id, recv_id, cell_id);
    *cell = HEADER_TO_CELL(cell_h);
    // fprintf(stderr, "MPIDU_genqi_nem_spsc_dequeue my_rank %d, peer_id %d, cell_id %d, cell pointer %p, pool_obj pointer %p, cell_idx %d, num_proc %d, num_cell_per_proc %d, &queue_obj->q.tail.s %p\n", recv_id, sender_id, cell_id, *cell, pool_obj, (recv_id * (pool_obj)->num_proc * (pool_obj)->cells_per_proc + sender_id * (pool_obj)->cells_per_proc + cell_id), (pool_obj)->num_proc , (pool_obj)->cells_per_proc, &queue_obj->q.tail.s);


    // int next_tail = (cur_tail + 1) % (pool_obj->cells_per_proc);
    // cxl_nt_store_uint64(&queue_obj->q.tail.s, next_tail);

    return 0;
}

static inline int MPIDU_genqi_nem_spsc_dequeue_commit(MPIDU_genq_shmem_pool_t pool, MPIDU_genq_shmem_queue_t queue, int sender_id, int recv_id, void **cell)
{
    _mm_mfence(); 
    MPIDU_genqi_shmem_pool_s *pool_obj = (MPIDU_genqi_shmem_pool_s *) pool;
    MPIDU_genq_shmem_queue_u *queue_obj = (MPIDU_genq_shmem_queue_u *) queue;

    int cur_tail = cxl_nt_load_uint64(&queue_obj->q.tail.s);
    int next_tail = (cur_tail + 1) % (pool_obj->cells_per_proc);
    cxl_nt_store_uint64(&queue_obj->q.tail.s, next_tail);

    // fprintf(stderr, "MPIDU_genqi_nem_spsc_dequeue_commit my_rank %d, peer_id %d, cell_id %d\n", recv_id, sender_id, cur_tail);

    return 0;
}

static inline int MPIDU_genqi_nem_spsc_enqueue(MPIDU_genq_shmem_pool_t pool, MPIDU_genq_shmem_queue_t queue, int sender_id, int recv_id, void **cell)
{
    MPIDU_genqi_shmem_pool_s *pool_obj = (MPIDU_genqi_shmem_pool_s *) pool;
    MPIDU_genq_shmem_queue_u *queue_obj = (MPIDU_genq_shmem_queue_u *) queue;

    int cur_head = cxl_nt_load_uint64(&queue_obj->q.head.s);
    int cur_tail = cxl_nt_load_uint64(&queue_obj->q.tail.s);
    int next_head = (cur_head + 1) % (pool_obj->cells_per_proc);
    if (next_head == cur_tail) {
        // Ring queue is full.
        *cell = NULL;
        return 0;
    }
    int cell_id = cur_head;
    MPIDU_genqi_shmem_cell_header_s *cell_h = NULL;
    cell_h = SENDER_RECV_CELL_TO_HEADER(pool_obj, sender_id, recv_id, cell_id);
    *cell = HEADER_TO_CELL(cell_h);
    // fprintf(stderr, "MPIDU_genqi_nem_spsc_enqueue my_rank %d, peer_id %d, cell_id %d, cell pointer %p, cell_idx %d\n", sender_id, recv_id, cell_id, *cell, (recv_id * (pool_obj)->num_proc * (pool_obj)->cells_per_proc + sender_id * (pool_obj)->cells_per_proc + cell_id));

    // cxl_nt_store_uint64(&queue_obj->q.head.s, next_head);

    return 0;
}
static inline int MPIDU_genqi_nem_spsc_enqueue_commit(MPIDU_genq_shmem_pool_t pool, MPIDU_genq_shmem_queue_t queue, int sender_id, int recv_id, void **cell)
{
    _mm_mfence(); 
    MPIDU_genqi_shmem_pool_s *pool_obj = (MPIDU_genqi_shmem_pool_s *) pool;
    MPIDU_genq_shmem_queue_u *queue_obj = (MPIDU_genq_shmem_queue_u *) queue;

    int cur_head = cxl_nt_load_uint64(&queue_obj->q.head.s);
    int next_head = (cur_head + 1) % (pool_obj->cells_per_proc);
    cxl_nt_store_uint64(&queue_obj->q.head.s, next_head);

    // fprintf(stderr, "MPIDU_genqi_nem_spsc_enqueue_commit my_rank %d, peer_id %d, cell_id %d\n", sender_id, recv_id, cur_head);
    return 0;
}


/* INVERSE QUEUE */

static inline int MPIDU_genqi_inv_mpsc_init(MPIDU_genq_shmem_queue_u * queue)
{
    queue->q.head.s = 0;
    /* sp and mp all use atomic tail */
    MPL_atomic_store_ptr(&queue->q.tail.m, NULL);

    return 0;
}

static inline MPIDU_genqi_shmem_cell_header_s
    * MPIDU_genqi_shmem_get_head_cell_header(MPIDU_genqi_shmem_pool_s * pool_obj,
                                             MPIDU_genq_shmem_queue_u * queue)
{
    void *tail = NULL;
    MPIDU_genqi_shmem_cell_header_s *head_cell_h = NULL;

    /* prepares the cells for dequeuing from the head in the following steps.
     * 1. atomic detaching all cells frm the queue tail
     * 2. find the head of the queue and rebuild the "next" pointers for cells
     */
    tail = MPL_atomic_swap_ptr(&queue->q.tail.m, NULL);
    if (!tail) {
        return NULL;
    }
    head_cell_h = HANDLE_TO_HEADER(pool_obj, tail);

    if (head_cell_h != NULL) {
        uintptr_t curr_handle = head_cell_h->handle;
        while (head_cell_h->u.inverse_queue.prev) {
            MPIDU_genqi_shmem_cell_header_s *prev_cell_h;
            prev_cell_h = HANDLE_TO_HEADER(pool_obj, head_cell_h->u.inverse_queue.prev);
            prev_cell_h->u.inverse_queue.next = curr_handle;
            curr_handle = head_cell_h->u.inverse_queue.prev;
            head_cell_h = prev_cell_h;
        }
        return head_cell_h;
    } else {
        return NULL;
    }
}

static inline int MPIDU_genqi_inv_mpsc_dequeue(MPIDU_genqi_shmem_pool_s * pool_obj,
                                               MPIDU_genq_shmem_queue_u * queue, void **cell)
{
    int rc = MPI_SUCCESS;
    MPIDU_genqi_shmem_cell_header_s *cell_h = NULL;

    if (!queue->q.head.s) {
        cell_h = MPIDU_genqi_shmem_get_head_cell_header(pool_obj, queue);
        if (cell_h) {
            *cell = HEADER_TO_CELL(cell_h);
            queue->q.head.s = cell_h->u.inverse_queue.next;
        } else {
            *cell = NULL;
        }
    } else {
        cell_h = HANDLE_TO_HEADER(pool_obj, queue->q.head.s);
        *cell = HEADER_TO_CELL(cell_h);
        queue->q.head.s = cell_h->u.inverse_queue.next;
    }

    return rc;
}

static inline int MPIDU_genqi_inv_mpsc_enqueue(MPIDU_genqi_shmem_pool_s * pool_obj,
                                               MPIDU_genq_shmem_queue_u * queue, void *cell)
{
    int rc = MPI_SUCCESS;
    MPIDU_genqi_shmem_cell_header_s *cell_h = CELL_TO_HEADER(cell);
    cell_h->u.inverse_queue.next = 0;
    cell_h->u.inverse_queue.prev = 0;

    void *prev_handle = NULL;
    void *handle = (void *) cell_h->handle;

    do {
        prev_handle = MPL_atomic_load_ptr(&queue->q.tail.m);
        cell_h->u.inverse_queue.prev = (uintptr_t) prev_handle;
    } while (MPL_atomic_cas_ptr(&queue->q.tail.m, prev_handle, handle) != prev_handle);

    return rc;
}

/* EXTERNAL INTERFACE */

static inline int MPIDU_genq_shmem_queue_dequeue(MPIDU_genq_shmem_pool_t pool,
                                                 MPIDU_genq_shmem_queue_t queue, void **cell)
{
    int rc = MPI_SUCCESS;
    MPIR_FUNC_ENTER;

    MPIDU_genqi_shmem_pool_s *pool_obj = (MPIDU_genqi_shmem_pool_s *) pool;
    MPIDU_genq_shmem_queue_u *queue_obj = (MPIDU_genq_shmem_queue_u *) queue;
    int flags = queue_obj->q.flags;
    if (flags == MPIDU_GENQ_SHMEM_QUEUE_TYPE__SERIAL) {
        rc = MPIDU_genqi_serial_dequeue(pool_obj, queue_obj, cell);
    } else if (flags == MPIDU_GENQ_SHMEM_QUEUE_TYPE__INV_MPSC) {
        rc = MPIDU_genqi_inv_mpsc_dequeue(pool_obj, queue_obj, cell);
    } else if (flags == MPIDU_GENQ_SHMEM_QUEUE_TYPE__NEM_MPSC) {
        rc = MPIDU_genqi_nem_mpsc_dequeue(pool_obj, queue_obj, cell);
    } else if (flags == MPIDU_GENQ_SHMEM_QUEUE_TYPE__NEM_MPMC) {
        rc = MPIDU_genqi_nem_mpmc_dequeue(pool_obj, queue_obj, cell);
    } else {
        MPIR_Assert_error("Invalid GenQ flag");
    }

    MPIR_FUNC_EXIT;
    return rc;
}

static inline int MPIDU_genq_shmem_queue_enqueue(MPIDU_genq_shmem_pool_t pool,
                                                 MPIDU_genq_shmem_queue_t queue, void *cell)
{
    int rc = MPI_SUCCESS;
    MPIR_FUNC_ENTER;

    MPIDU_genqi_shmem_pool_s *pool_obj = (MPIDU_genqi_shmem_pool_s *) pool;
    MPIDU_genq_shmem_queue_u *queue_obj = (MPIDU_genq_shmem_queue_u *) queue;
    int flags = queue_obj->q.flags;
    if (flags == MPIDU_GENQ_SHMEM_QUEUE_TYPE__SERIAL) {
        rc = MPIDU_genqi_serial_enqueue(pool_obj, queue_obj, cell);
    } else if (flags == MPIDU_GENQ_SHMEM_QUEUE_TYPE__INV_MPSC) {
        rc = MPIDU_genqi_inv_mpsc_enqueue(pool_obj, queue_obj, cell);
    } else if (flags == MPIDU_GENQ_SHMEM_QUEUE_TYPE__NEM_MPSC) {
        rc = MPIDU_genqi_nem_mpsc_enqueue(pool_obj, queue_obj, cell);
    } else if (flags == MPIDU_GENQ_SHMEM_QUEUE_TYPE__NEM_MPMC) {
        rc = MPIDU_genqi_nem_mpmc_enqueue(pool_obj, queue_obj, cell);
    } else {
        MPIR_Assert_error("Invalid GenQ flag");
    }

    MPIR_FUNC_EXIT;
    return rc;
}

#endif /* ifndef MPIDU_GENQ_SHMEM_QUEUE_H_INCLUDED */