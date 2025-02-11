/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#ifndef POSIX_EAGER_IQUEUE_RECV_H_INCLUDED
#define POSIX_EAGER_IQUEUE_RECV_H_INCLUDED

#include "iqueue_impl.h"
#include "mpidu_genq.h"

MPL_STATIC_INLINE_PREFIX int
MPIDI_POSIX_eager_recv_begin(int vci, MPIDI_POSIX_eager_recv_transaction_t * transaction)
{
    MPIDI_POSIX_eager_iqueue_transport_t *transport;
    MPIDI_POSIX_eager_iqueue_cell_t *cell = NULL;
    int ret = MPIDI_POSIX_NOK;

    MPIR_FUNC_ENTER;

    /* TODO: measure the latency overhead due to multiple vci */
    int max_vcis = MPIDI_POSIX_eager_iqueue_global.max_vcis;
    int recv_rank = MPIR_Process.rank;
    bool has_cell = false;
    for (int vci_src = 0; vci_src < max_vcis; vci_src++) {
        transport = MPIDI_POSIX_eager_iqueue_get_transport(vci_src, vci);
        int num_proc = ((MPIDU_genqi_shmem_pool_s *)transport->cell_pool)->num_proc;
        for (int sender_rank = 0; sender_rank < num_proc ; sender_rank++) {
            int queue_id = recv_rank * num_proc + sender_rank;
            MPIDU_genq_shmem_queue_t queue = &transport->terminals[queue_id];
            MPIDU_genqi_nem_spsc_dequeue(transport->cell_pool, queue, sender_rank, recv_rank, (void **) &cell);
            // MPIDU_genq_shmem_queue_dequeue(transport->cell_pool, transport->my_terminal,
            //                                (void **) &cell);
            if (cell) {
                // MPIDU_genqi_shmem_cell_header_s *cell_h = CELL_TO_HEADER(cell);
                // clflush_region_with_mfence(cell_h, ((MPIDU_genqi_shmem_pool_s *)transport->cell_pool)->cell_alloc_size);
                clflush_region_with_mfence(cell, sizeof(MPIDI_POSIX_eager_iqueue_cell_t));
                transaction->src_local_rank = cell->from;
                transaction->src_vci = vci_src;
                transaction->dst_vci = vci;
                transaction->payload = MPIDI_POSIX_EAGER_IQUEUE_CELL_PAYLOAD(cell);
                transaction->payload_sz = cell->payload_size;
                clflush_region_with_mfence(transaction->payload, transaction->payload_sz);

                if (likely(cell->type == MPIDI_POSIX_EAGER_IQUEUE_CELL_TYPE_HDR)) {
                    transaction->msg_hdr = &cell->am_header;
                } else {
                    MPIR_Assert(cell->type == MPIDI_POSIX_EAGER_IQUEUE_CELL_TYPE_DATA);
                    transaction->msg_hdr = NULL;
                }

                transaction->transport.iqueue.pointer_to_cell = cell;
                MPIDU_genqi_nem_spsc_dequeue_commit(transport->cell_pool, queue, sender_rank, recv_rank, (void **) &cell);

                ret = MPIDI_POSIX_OK;
                has_cell = true;
                break;
            }
        }
        if (has_cell) {
            break;
        }
    }

    MPIR_FUNC_EXIT;
    return ret;
}

MPL_STATIC_INLINE_PREFIX void
MPIDI_POSIX_eager_recv_memcpy(MPIDI_POSIX_eager_recv_transaction_t * transaction,
                              void *dst, const void *src, size_t size)
{
    MPIR_Typerep_copy(dst, src, size, MPIR_TYPEREP_FLAG_NONE);
}

MPL_STATIC_INLINE_PREFIX void
MPIDI_POSIX_eager_recv_commit(MPIDI_POSIX_eager_recv_transaction_t * transaction)
{
    MPIDI_POSIX_eager_iqueue_cell_t *cell;
    MPIDI_POSIX_eager_iqueue_transport_t *transport;

    MPIR_FUNC_ENTER;

    transport = MPIDI_POSIX_eager_iqueue_get_transport(transaction->src_vci, transaction->dst_vci);
    cell = (MPIDI_POSIX_eager_iqueue_cell_t *) transaction->transport.iqueue.pointer_to_cell;
    MPIDU_genq_shmem_pool_cell_free(transport->cell_pool, cell);

    MPIR_FUNC_EXIT;
}

MPL_STATIC_INLINE_PREFIX void MPIDI_POSIX_eager_recv_posted_hook(int grank)
{
}

MPL_STATIC_INLINE_PREFIX void MPIDI_POSIX_eager_recv_completed_hook(int grank)
{
}

#endif /* POSIX_EAGER_IQUEUE_RECV_H_INCLUDED */
