/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2017 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2012      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2012      Sandia National Laboratories. All rights reserved.
 * Copyright (c) 2014-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      Siberian State University of Telecommunications
 *                         and Information Sciences. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "ompi_config.h"

#include "mpi.h"
#include "opal/util/bit_ops.h"
#include "ompi/constants.h"
#include "ompi/datatype/ompi_datatype.h"
#include "ompi/communicator/communicator.h"
#include "ompi/mca/coll/coll.h"
#include "ompi/mca/coll/basic/coll_basic.h"
#include "ompi/mca/pml/pml.h"
#include "ompi/op/op.h"
#include "coll_tags.h"
#include "coll_base_functions.h"
#include "coll_base_topo.h"
#include "coll_base_util.h"


/*
 *	ompi_reduce_scatter_block_basic
 *
 *	Function:	- reduce then scatter
 *	Accepts:	- same as MPI_Reduce_scatter_block()
 *	Returns:	- MPI_SUCCESS or error code
 *
 * Algorithm:
 *     reduce and scatter (needs to be cleaned
 *     up at some point)
 */
int
ompi_coll_base_reduce_scatter_block_basic(const void *sbuf, void *rbuf, int rcount,
                                          struct ompi_datatype_t *dtype,
                                          struct ompi_op_t *op,
                                          struct ompi_communicator_t *comm,
                                          mca_coll_base_module_t *module)
{
    int rank, size, count, err = OMPI_SUCCESS;
    ptrdiff_t gap, span;
    char *recv_buf = NULL, *recv_buf_free = NULL;

    /* Initialize */
    rank = ompi_comm_rank(comm);
    size = ompi_comm_size(comm);

    /* short cut the trivial case */
    count = rcount * size;
    if (0 == count) {
        return OMPI_SUCCESS;
    }

    /* get datatype information */
    span = opal_datatype_span(&dtype->super, count, &gap);

    /* Handle MPI_IN_PLACE */
    if (MPI_IN_PLACE == sbuf) {
        sbuf = rbuf;
    }

    if (0 == rank) {
        /* temporary receive buffer.  See coll_basic_reduce.c for
           details on sizing */
        recv_buf_free = (char*) malloc(span);
        if (NULL == recv_buf_free) {
            err = OMPI_ERR_OUT_OF_RESOURCE;
            goto cleanup;
        }
        recv_buf = recv_buf_free - gap;
    }

    /* reduction */
    err =
        comm->c_coll->coll_reduce(sbuf, recv_buf, count, dtype, op, 0,
                                 comm, comm->c_coll->coll_reduce_module);

    /* scatter */
    if (MPI_SUCCESS == err) {
        err = comm->c_coll->coll_scatter(recv_buf, rcount, dtype,
                                        rbuf, rcount, dtype, 0,
                                        comm, comm->c_coll->coll_scatter_module);
    }

 cleanup:
    if (NULL != recv_buf_free) free(recv_buf_free);

    return err;
}
/*
 * ompi_rounddown: Rounds a number down to nearest multiple.
 *     rounddown(10,4) = 8, rounddown(6,3) = 6, rounddown(14,3) = 12
 */
static int ompi_rounddown(int num, int factor)
{
    num /= factor;
    return num * factor;    /* floor(num / factor) * factor */
}

/*
 * ompi_coll_base_reduce_scatter_block_intra_recursivedoubling
 *
 * Function:  Recursive doubling algorithm for reduce_scatter_block.
 * Accepts:   Same as MPI_Reduce_scatter_block
 * Returns:   MPI_SUCCESS or error code
 *
 * Description:  Implements recursive doubling algorithm for MPI_Reduce_scatter_block.
 *               The algorithm preserves order of operations so it can
 *               be used both by commutative and non-commutative operations.
 *
 * Time complexity: \alpha\log(p) + \beta*m(\log(p)-(p-1)/p) + \gamma*m(\log(p)-(p-1)/p),
 *                  where m = rcount * comm_size, p = comm_size
 * Memory requirements (per process): 2 * rcount * comm_size * typesize
 */
int
ompi_coll_base_reduce_scatter_block_intra_recursivedoubling(
    const void *sbuf, void *rbuf, int rcount, struct ompi_datatype_t *dtype,
    struct ompi_op_t *op, struct ompi_communicator_t *comm,
    mca_coll_base_module_t *module)
{
    struct ompi_datatype_t *dtypesend = NULL, *dtyperecv = NULL;
    char *tmprecv_raw = NULL, *tmpbuf_raw = NULL, *tmprecv, *tmpbuf;
    ptrdiff_t span, gap, totalcount, extent;
    int blocklens[2], displs[2];
    int err = MPI_SUCCESS;
    int comm_size = ompi_comm_size(comm);
    int rank = ompi_comm_rank(comm);

    OPAL_OUTPUT((ompi_coll_base_framework.framework_output,
                 "coll:base:reduce_scatter_block_intra_recursivedoubling: rank %d/%d",
                 rank, comm_size));
    if (rcount == 0)
        return MPI_SUCCESS;
    if (comm_size < 2)
        return MPI_SUCCESS;

    totalcount = comm_size * rcount;
    ompi_datatype_type_extent(dtype, &extent);
    span = opal_datatype_span(&dtype->super, totalcount, &gap);
    tmpbuf_raw = malloc(span);
    tmprecv_raw = malloc(span);
    if (NULL == tmpbuf_raw || NULL == tmprecv_raw) {
        err = OMPI_ERR_OUT_OF_RESOURCE;
        goto cleanup_and_return;
    }
    tmpbuf = tmpbuf_raw - gap;
    tmprecv = tmprecv_raw - gap;

    if (sbuf != MPI_IN_PLACE) {
        err = ompi_datatype_copy_content_same_ddt(dtype, totalcount, tmpbuf, (char *)sbuf);
        if (MPI_SUCCESS != err) { goto cleanup_and_return; }
    } else {
        err = ompi_datatype_copy_content_same_ddt(dtype, totalcount, tmpbuf, rbuf);
        if (MPI_SUCCESS != err) { goto cleanup_and_return; }
    }
    int is_commutative = ompi_op_is_commute(op);

    /* Recursive distance doubling */
    int rdoubling_step = 0;
    for (int mask = 1; mask < comm_size; mask <<= 1) {
        int remote = rank ^ mask;
        int cur_tree_root = ompi_rounddown(rank, mask);
        int remote_tree_root = ompi_rounddown(remote, mask);

        /*
         * Let be m is a block size in bytes (rcount), p is a comm_size,
         * p*m is a total message size in sbuf.
         * Step 1: processes send and recv (p*m-m) amount of data
         * Step 2: processes send and recv (p*m-2*m) amount of data
         * Step 3: processes send and recv (p*m-4*m) amount of data
         * ...
         * Step ceil(\log_2(p)): send and recv (p*m-m*2^floor{\log_2(p-1)})
         *
         * Send block from tmpbuf: [0..cur_tree_root - 1], [cur_tree_root + mask, p - 1]
         * Recv block into tmprecv: [0..remote_tree_root - 1], [remote_tree_root + mask, p - 1]
         */

        /* Send type */
        blocklens[0] = rcount * cur_tree_root;
        blocklens[1] = (comm_size >= cur_tree_root + mask) ?
                       rcount * (comm_size - cur_tree_root - mask) : 0;
        displs[0] = 0;
        displs[1] = comm_size * rcount - blocklens[1];
        err = ompi_datatype_create_indexed(2, blocklens, displs, dtype, &dtypesend);
        if (MPI_SUCCESS != err) { goto cleanup_and_return; }
        err = ompi_datatype_commit(&dtypesend);
        if (MPI_SUCCESS != err) { goto cleanup_and_return; }

        /* Recv type */
        blocklens[0] = rcount * remote_tree_root;
        blocklens[1] = (comm_size >= remote_tree_root + mask) ?
                       rcount * (comm_size - remote_tree_root - mask) : 0;
        displs[0] = 0;
        displs[1] = comm_size * rcount - blocklens[1];
        err = ompi_datatype_create_indexed(2, blocklens, displs, dtype, &dtyperecv);
        if (MPI_SUCCESS != err) { goto cleanup_and_return; }
        err = ompi_datatype_commit(&dtyperecv);
        if (MPI_SUCCESS != err) { goto cleanup_and_return; }

        int is_block_received = 0;
        if (remote < comm_size) {
            err = ompi_coll_base_sendrecv(tmpbuf, 1, dtypesend, remote,
                                          MCA_COLL_BASE_TAG_REDUCE_SCATTER_BLOCK,
                                          tmprecv, 1, dtyperecv, remote,
                                          MCA_COLL_BASE_TAG_REDUCE_SCATTER_BLOCK,
                                          comm, MPI_STATUS_IGNORE, rank);
            if (MPI_SUCCESS != err) { goto cleanup_and_return; }
            is_block_received = 1;
        }
        /*
         * Non-power-of-two case: if process did not have destination process
         * to communicate with, we need to send him the current result.
         * Recursive halving algorithm is used for search of process.
         */
        if (remote_tree_root + mask > comm_size) {
            /*
             * Compute the number of processes in current subtree
             * that have all the data
             */
            int nprocs_alldata = comm_size - cur_tree_root - mask;
            for (int rhalving_mask = mask >> 1; rhalving_mask > 0; rhalving_mask >>= 1) {
                remote = rank ^ rhalving_mask;
                int tree_root = ompi_rounddown(rank, rhalving_mask << 1);
                /*
                 * Send only if:
                 * 1) current process has data: (remote > rank) && (rank < tree_root + nprocs_alldata)
                 * 2) remote process does not have data at any step: remote >= tree_root + nprocs_alldata
                 */
                if ((remote > rank) && (rank < tree_root + nprocs_alldata)
                    && (remote >= tree_root + nprocs_alldata)) {
                    err = MCA_PML_CALL(send(tmprecv, 1, dtyperecv, remote,
                                            MCA_COLL_BASE_TAG_REDUCE_SCATTER_BLOCK,
                                            MCA_PML_BASE_SEND_STANDARD, comm));
                    if (MPI_SUCCESS != err) { goto cleanup_and_return; }

                } else if ((remote < rank) && (remote < tree_root + nprocs_alldata) &&
                           (rank >= tree_root + nprocs_alldata)) {
                    err = MCA_PML_CALL(recv(tmprecv, 1, dtyperecv, remote,
                                            MCA_COLL_BASE_TAG_REDUCE_SCATTER_BLOCK,
                                            comm, MPI_STATUS_IGNORE));
                    if (MPI_SUCCESS != err) { goto cleanup_and_return; }
                    is_block_received = 1;
                }
            }
        }

        if (is_block_received) {
            /* After reduction the result must be in tmpbuf */
            if (is_commutative || (remote_tree_root < cur_tree_root)) {
                ompi_op_reduce(op, tmprecv, tmpbuf, blocklens[0], dtype);
                ompi_op_reduce(op, tmprecv + (ptrdiff_t)displs[1] * extent,
                               tmpbuf + (ptrdiff_t)displs[1] * extent,
                               blocklens[1], dtype);
            } else {
                ompi_op_reduce(op, tmpbuf, tmprecv, blocklens[0], dtype);
                ompi_op_reduce(op, tmpbuf + (ptrdiff_t)displs[1] * extent,
                               tmprecv + (ptrdiff_t)displs[1] * extent,
                               blocklens[1], dtype);
                err = ompi_datatype_copy_content_same_ddt(dtyperecv, 1,
                                                          tmpbuf, tmprecv);
                if (MPI_SUCCESS != err) { goto cleanup_and_return; }
            }
        }
        rdoubling_step++;
        err = ompi_datatype_destroy(&dtypesend);
        if (MPI_SUCCESS != err) { goto cleanup_and_return; }
        err = ompi_datatype_destroy(&dtyperecv);
        if (MPI_SUCCESS != err) { goto cleanup_and_return; }
    }
    err = ompi_datatype_copy_content_same_ddt(dtype, rcount, rbuf,
                                              tmpbuf + (ptrdiff_t)rank * rcount * extent);
    if (MPI_SUCCESS != err) { goto cleanup_and_return; }

cleanup_and_return:
    if (dtypesend)
        ompi_datatype_destroy(&dtypesend);
    if (dtyperecv)
        ompi_datatype_destroy(&dtyperecv);
    if (tmpbuf_raw)
        free(tmpbuf_raw);
    if (tmprecv_raw)
        free(tmprecv_raw);
    return err;
}

/*
 * ompi_range_sum: Returns sum of elems in intersection of [a, b] and [0, r]
 *   index: 0 1 2 3 4 ... r r+1 r+2 ... nproc_pof2
 *   value: 2 2 2 2 2 ... 2  1   1  ... 1
 */
static int ompi_range_sum(int a, int b, int r)
{
    if (r < a)
        return b - a + 1;
    else if (r > b)
        return 2 * (b - a + 1);
    return 2 * (r - a + 1) + b - r;
}

/*
 * ompi_coll_base_reduce_scatter_block_intra_recursivehalving
 *
 * Function:  Recursive halving algorithm for reduce_scatter_block
 * Accepts:   Same as MPI_Reduce_scatter_block
 * Returns:   MPI_SUCCESS or error code
 *
 * Description:  Implements recursive halving algorithm for MPI_Reduce_scatter_block.
 *               The algorithm can be used by commutative operations only.
 *
 * Limitations:  commutative operations only
 * Memory requirements (per process): 2 * rcount * comm_size * typesize
 */
int
ompi_coll_base_reduce_scatter_block_intra_recursivehalving(
    const void *sbuf, void *rbuf, int rcount, struct ompi_datatype_t *dtype,
    struct ompi_op_t *op, struct ompi_communicator_t *comm,
    mca_coll_base_module_t *module)
{
    char *tmprecv_raw = NULL, *tmpbuf_raw = NULL, *tmprecv, *tmpbuf;
    ptrdiff_t span, gap, totalcount, extent;
    int err = MPI_SUCCESS;
    int comm_size = ompi_comm_size(comm);
    int rank = ompi_comm_rank(comm);

    OPAL_OUTPUT((ompi_coll_base_framework.framework_output,
                 "coll:base:reduce_scatter_block_intra_recursivehalving: rank %d/%d",
                 rank, comm_size));
    if (rcount == 0 || comm_size < 2)
        return MPI_SUCCESS;

    if (!ompi_op_is_commute(op)) {
        OPAL_OUTPUT((ompi_coll_base_framework.framework_output,
                     "coll:base:reduce_scatter_block_intra_recursivehalving: rank %d/%d "
                     "switching to basic reduce_scatter_block", rank, comm_size));
        return ompi_coll_base_reduce_scatter_block_basic(sbuf, rbuf, rcount, dtype,
                                                         op, comm, module);
    }
    totalcount = comm_size * rcount;
    ompi_datatype_type_extent(dtype, &extent);
    span = opal_datatype_span(&dtype->super, totalcount, &gap);
    tmpbuf_raw = malloc(span);
    tmprecv_raw = malloc(span);
    if (NULL == tmpbuf_raw || NULL == tmprecv_raw) {
        err = OMPI_ERR_OUT_OF_RESOURCE;
        goto cleanup_and_return;
    }
    tmpbuf = tmpbuf_raw - gap;
    tmprecv = tmprecv_raw - gap;

    if (sbuf != MPI_IN_PLACE) {
        err = ompi_datatype_copy_content_same_ddt(dtype, totalcount, tmpbuf, (char *)sbuf);
        if (MPI_SUCCESS != err) { goto cleanup_and_return; }
    } else {
        err = ompi_datatype_copy_content_same_ddt(dtype, totalcount, tmpbuf, rbuf);
        if (MPI_SUCCESS != err) { goto cleanup_and_return; }
    }

    /*
     * Step 1. Reduce the number of processes to the nearest lower power of two
     * p' = 2^{\floor{\log_2 p}} by removing r = p - p' processes.
     * In the first 2r processes (ranks 0 to 2r - 1), all the even ranks send
     * the input vector to their neighbor (rank + 1) and all the odd ranks recv
     * the input vector and perform local reduction.
     * The odd ranks (0 to 2r - 1) contain the reduction with the input
     * vector on their neighbors (the even ranks). The first r odd
     * processes and the p - 2r last processes are renumbered from
     * 0 to 2^{\floor{\log_2 p}} - 1. Even ranks do not participate in the
     * rest of the algorithm.
     */

    /* Find nearest power-of-two less than or equal to comm_size */
    int nprocs_pof2 = opal_next_poweroftwo(comm_size);
    nprocs_pof2 >>= 1;
    int nprocs_rem = comm_size - nprocs_pof2;

    int vrank = -1;
    if (rank < 2 * nprocs_rem) {
        if ((rank % 2) == 0) {
            /* Even process */
            err = MCA_PML_CALL(send(tmpbuf, totalcount, dtype, rank + 1,
                                    MCA_COLL_BASE_TAG_REDUCE_SCATTER_BLOCK,
                                    MCA_PML_BASE_SEND_STANDARD, comm));
            if (OMPI_SUCCESS != err) { goto cleanup_and_return; }
            /* This process does not pariticipate in the rest of the algorithm */
            vrank = -1;
        } else {
            /* Odd process */
            err = MCA_PML_CALL(recv(tmprecv, totalcount, dtype, rank - 1,
                                    MCA_COLL_BASE_TAG_REDUCE_SCATTER_BLOCK,
                                    comm, MPI_STATUS_IGNORE));
            if (OMPI_SUCCESS != err) { goto cleanup_and_return; }
            ompi_op_reduce(op, tmprecv, tmpbuf, totalcount, dtype);
            /* Adjust rank to be the bottom "remain" ranks */
            vrank = rank / 2;
        }
    } else {
        /* Adjust rank to show that the bottom "even remain" ranks dropped out */
        vrank = rank - nprocs_rem;
    }

    if (vrank != -1) {
        /*
         * Step 2. Recursive vector halving. We have p' = 2^{\floor{\log_2 p}}
         * power-of-two number of processes with new ranks (vrank) and partial
         * result in tmpbuf.
         * All processes then compute the reduction between the local
         * buffer and the received buffer. In the next \log_2(p') - 1 steps, the
         * buffers are recursively halved. At the end, each of the p' processes
         * has 1 / p' of the total reduction result.
         */
        int send_index = 0, recv_index = 0, last_index = nprocs_pof2;
        for (int mask = nprocs_pof2 >> 1; mask > 0; mask >>= 1) {
            int vpeer = vrank ^ mask;
            int peer = (vpeer < nprocs_rem) ? vpeer * 2 + 1 : vpeer + nprocs_rem;

            /*
             * Calculate the recv_count and send_count because the
             * even-numbered processes who no longer participate will
             * have their result calculated by the process to their
             * right (rank + 1).
             */
            int send_count = 0, recv_count = 0;
            if (vrank < vpeer) {
                /* Send the right half of the buffer, recv the left half */
                send_index = recv_index + mask;
                send_count = rcount * ompi_range_sum(send_index, last_index - 1, nprocs_rem - 1);
                recv_count = rcount * ompi_range_sum(recv_index, send_index - 1, nprocs_rem - 1);
            } else {
                /* Send the left half of the buffer, recv the right half */
                recv_index = send_index + mask;
                send_count = rcount * ompi_range_sum(send_index, recv_index - 1, nprocs_rem - 1);
                recv_count = rcount * ompi_range_sum(recv_index, last_index - 1, nprocs_rem - 1);
            }
            ptrdiff_t rdispl = rcount * ((recv_index <= nprocs_rem - 1) ?
                                         2 * recv_index : nprocs_rem + recv_index);
            ptrdiff_t sdispl = rcount * ((send_index <= nprocs_rem - 1) ?
                                         2 * send_index : nprocs_rem + send_index);
            struct ompi_request_t *request = NULL;

            if (recv_count > 0) {
                err = MCA_PML_CALL(irecv(tmprecv + rdispl * extent, recv_count,
                                         dtype, peer, MCA_COLL_BASE_TAG_REDUCE_SCATTER_BLOCK,
                                         comm, &request));
                if (OMPI_SUCCESS != err) { goto cleanup_and_return; }
            }
            if (send_count > 0) {
                err = MCA_PML_CALL(send(tmpbuf + sdispl * extent, send_count,
                                        dtype, peer, MCA_COLL_BASE_TAG_REDUCE_SCATTER_BLOCK,
                                        MCA_PML_BASE_SEND_STANDARD,
                                        comm));
                if (OMPI_SUCCESS != err) { goto cleanup_and_return; }
            }
            if (recv_count > 0) {
                err = ompi_request_wait(&request, MPI_STATUS_IGNORE);
                if (OMPI_SUCCESS != err) { goto cleanup_and_return; }
                ompi_op_reduce(op, tmprecv + rdispl * extent,
                               tmpbuf + rdispl * extent, recv_count, dtype);
            }
            send_index = recv_index;
            last_index = recv_index + mask;
        }
        err = ompi_datatype_copy_content_same_ddt(dtype, rcount, rbuf,
                                                  tmpbuf + (ptrdiff_t)rank * rcount * extent);
        if (MPI_SUCCESS != err) { goto cleanup_and_return; }
    }

    /* Step 3. Send the result to excluded even ranks */
    if (rank < 2 * nprocs_rem) {
        if ((rank % 2) == 0) {
            /* Even process */
            err = MCA_PML_CALL(recv(rbuf, rcount, dtype, rank + 1,
                                    MCA_COLL_BASE_TAG_REDUCE_SCATTER_BLOCK, comm,
                                    MPI_STATUS_IGNORE));
            if (OMPI_SUCCESS != err) { goto cleanup_and_return; }
        } else {
            /* Odd process */
            err = MCA_PML_CALL(send(tmpbuf + (ptrdiff_t)(rank - 1) * rcount * extent,
                                    rcount, dtype, rank - 1,
                                    MCA_COLL_BASE_TAG_REDUCE_SCATTER_BLOCK,
                                    MCA_PML_BASE_SEND_STANDARD, comm));
            if (MPI_SUCCESS != err) { goto cleanup_and_return; }
        }
    }

cleanup_and_return:
    if (tmpbuf_raw)
        free(tmpbuf_raw);
    if (tmprecv_raw)
        free(tmprecv_raw);
    return err;
}