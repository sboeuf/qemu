/*
 * virtio-fs glue for FUSE
 * Copyright (C) 2018 Red Hat, Inc. and/or its affiliates
 *
 * Authors:
 *   Dave Gilbert  <dgilbert@redhat.com>
 *
 * Implements the glue between libfuse and libvhost-user
 *
 * This program can be distributed under the terms of the GNU LGPLv2.
 * See the file COPYING.LIB
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "fuse_virtio.h"
#include "fuse_i.h"
#include "standard-headers/linux/fuse.h"
#include "fuse_misc.h"
#include "fuse_opt.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "contrib/libvhost-user/libvhost-user.h"

struct fv_VuDev;
struct fv_QueueInfo {
    pthread_t thread;
    struct fv_VuDev *virtio_dev;

    /* Our queue index, corresponds to array position */
    int qidx;
    int kick_fd;
};

/*
 * We pass the dev element into libvhost-user
 * and then use it to get back to the outer
 * container for other data.
 */
struct fv_VuDev {
    VuDev dev;
    struct fuse_session *se;

    /*
     * The following pair of fields are only accessed in the main
     * virtio_loop
     */
    size_t nqueues;
    struct fv_QueueInfo **qi;
};

/* From spec */
struct virtio_fs_config {
    char tag[36];
    uint32_t num_queues;
};

/* Callback from libvhost-user */
static uint64_t fv_get_features(VuDev *dev)
{
    return 1ULL << VIRTIO_F_VERSION_1;
}

/* Callback from libvhost-user */
static void fv_set_features(VuDev *dev, uint64_t features)
{
}

/*
 * Callback from libvhost-user if there's a new fd we're supposed to listen
 * to, typically a queue kick?
 */
static void fv_set_watch(VuDev *dev, int fd, int condition, vu_watch_cb cb,
                         void *data)
{
    fuse_log(FUSE_LOG_WARNING, "%s: TODO! fd=%d\n", __func__, fd);
}

/*
 * Callback from libvhost-user if we're no longer supposed to listen on an fd
 */
static void fv_remove_watch(VuDev *dev, int fd)
{
    fuse_log(FUSE_LOG_WARNING, "%s: TODO! fd=%d\n", __func__, fd);
}

/* Callback from libvhost-user to panic */
static void fv_panic(VuDev *dev, const char *err)
{
    fuse_log(FUSE_LOG_ERR, "%s: libvhost-user: %s\n", __func__, err);
    /* TODO: Allow reconnects?? */
    exit(EXIT_FAILURE);
}

/*
 * Copy from an iovec into a fuse_buf (memory only)
 * Caller must ensure there is space
 */
static void copy_from_iov(struct fuse_buf *buf, size_t out_num,
                          const struct iovec *out_sg)
{
    void *dest = buf->mem;

    while (out_num) {
        size_t onelen = out_sg->iov_len;
        memcpy(dest, out_sg->iov_base, onelen);
        dest += onelen;
        out_sg++;
        out_num--;
    }
}

/* Thread function for individual queues, created when a queue is 'started' */
static void *fv_queue_thread(void *opaque)
{
    struct fv_QueueInfo *qi = opaque;
    struct VuDev *dev = &qi->virtio_dev->dev;
    struct VuVirtq *q = vu_get_queue(dev, qi->qidx);
    struct fuse_session *se = qi->virtio_dev->se;
    struct fuse_chan ch;
    struct fuse_buf fbuf;

    fbuf.mem = NULL;
    fbuf.flags = 0;

    fuse_mutex_init(&ch.lock);
    ch.fd = (int)0xdaff0d111;
    ch.ctr = 1;
    ch.qi = qi;

    fuse_log(FUSE_LOG_INFO, "%s: Start for queue %d kick_fd %d\n", __func__,
             qi->qidx, qi->kick_fd);
    while (1) {
        struct pollfd pf[1];
        pf[0].fd = qi->kick_fd;
        pf[0].events = POLLIN;
        pf[0].revents = 0;

        fuse_log(FUSE_LOG_DEBUG, "%s: Waiting for Queue %d event\n", __func__,
                 qi->qidx);
        int poll_res = ppoll(pf, 1, NULL, NULL);

        if (poll_res == -1) {
            if (errno == EINTR) {
                fuse_log(FUSE_LOG_INFO, "%s: ppoll interrupted, going around\n",
                         __func__);
                continue;
            }
            fuse_log(FUSE_LOG_ERR, "fv_queue_thread ppoll: %m\n");
            break;
        }
        assert(poll_res == 1);
        if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fuse_log(FUSE_LOG_ERR, "%s: Unexpected poll revents %x Queue %d\n",
                     __func__, pf[0].revents, qi->qidx);
            break;
        }
        assert(pf[0].revents & POLLIN);
        fuse_log(FUSE_LOG_DEBUG, "%s: Got queue event on Queue %d\n", __func__,
                 qi->qidx);

        eventfd_t evalue;
        if (eventfd_read(qi->kick_fd, &evalue)) {
            fuse_log(FUSE_LOG_ERR, "Eventfd_read for queue: %m\n");
            break;
        }
        /* out is from guest, in is too guest */
        unsigned int in_bytes, out_bytes;
        vu_queue_get_avail_bytes(dev, q, &in_bytes, &out_bytes, ~0, ~0);

        fuse_log(FUSE_LOG_DEBUG,
                 "%s: Queue %d gave evalue: %zx available: in: %u out: %u\n",
                 __func__, qi->qidx, (size_t)evalue, in_bytes, out_bytes);

        while (1) {
            /*
             * An element contains one request and the space to send our
             * response They're spread over multiple descriptors in a
             * scatter/gather set and we can't trust the guest to keep them
             * still; so copy in/out.
             */
            VuVirtqElement *elem = vu_queue_pop(dev, q, sizeof(VuVirtqElement));
            if (!elem) {
                break;
            }

            if (!fbuf.mem) {
                fbuf.mem = malloc(se->bufsize);
                assert(fbuf.mem);
                assert(se->bufsize > sizeof(struct fuse_in_header));
            }
            /* The 'out' part of the elem is from qemu */
            unsigned int out_num = elem->out_num;
            struct iovec *out_sg = elem->out_sg;
            size_t out_len = iov_size(out_sg, out_num);
            fuse_log(FUSE_LOG_DEBUG,
                     "%s: elem %d: with %d out desc of length %zd\n", __func__,
                     elem->index, out_num, out_len);

            /*
             * The elem should contain a 'fuse_in_header' (in to fuse)
             * plus the data based on the len in the header.
             */
            if (out_len < sizeof(struct fuse_in_header)) {
                fuse_log(FUSE_LOG_ERR, "%s: elem %d too short for in_header\n",
                         __func__, elem->index);
                assert(0); /* TODO */
            }
            if (out_len > se->bufsize) {
                fuse_log(FUSE_LOG_ERR, "%s: elem %d too large for buffer\n",
                         __func__, elem->index);
                assert(0); /* TODO */
            }
            copy_from_iov(&fbuf, out_num, out_sg);
            fbuf.size = out_len;

            /* TODO! Endianness of header */

            /* TODO: Fixup fuse_send_msg */
            /* TODO: Add checks for fuse_session_exited */
            fuse_session_process_buf_int(se, &fbuf, &ch);

            /* TODO: vu_queue_push(dev, q, elem, qi->write_count); */
            vu_queue_notify(dev, q);

            free(elem);
            elem = NULL;
        }
    }
    pthread_mutex_destroy(&ch.lock);
    free(fbuf.mem);

    return NULL;
}

/* Callback from libvhost-user on start or stop of a queue */
static void fv_queue_set_started(VuDev *dev, int qidx, bool started)
{
    struct fv_VuDev *vud = container_of(dev, struct fv_VuDev, dev);
    struct fv_QueueInfo *ourqi;

    fuse_log(FUSE_LOG_INFO, "%s: qidx=%d started=%d\n", __func__, qidx,
             started);
    assert(qidx >= 0);

    /*
     * Ignore additional request queues for now.  passthrough_ll.c must be
     * audited for thread-safety issues first.  It was written with a
     * well-behaved client in mind and may not protect against all types of
     * races yet.
     */
    if (qidx > 1) {
        fuse_log(FUSE_LOG_ERR,
                 "%s: multiple request queues not yet implemented, please only "
                 "configure 1 request queue\n",
                 __func__);
        exit(EXIT_FAILURE);
    }

    if (started) {
        /* Fire up a thread to watch this queue */
        if (qidx >= vud->nqueues) {
            vud->qi = realloc(vud->qi, (qidx + 1) * sizeof(vud->qi[0]));
            assert(vud->qi);
            memset(vud->qi + vud->nqueues, 0,
                   sizeof(vud->qi[0]) * (1 + (qidx - vud->nqueues)));
            vud->nqueues = qidx + 1;
        }
        if (!vud->qi[qidx]) {
            vud->qi[qidx] = calloc(sizeof(struct fv_QueueInfo), 1);
            assert(vud->qi[qidx]);
            vud->qi[qidx]->virtio_dev = vud;
            vud->qi[qidx]->qidx = qidx;
        } else {
            /* Shouldn't have been started */
            assert(vud->qi[qidx]->kick_fd == -1);
        }
        ourqi = vud->qi[qidx];
        ourqi->kick_fd = dev->vq[qidx].kick_fd;
        if (pthread_create(&ourqi->thread, NULL, fv_queue_thread, ourqi)) {
            fuse_log(FUSE_LOG_ERR, "%s: Failed to create thread for queue %d\n",
                     __func__, qidx);
            assert(0);
        }
    } else {
        /* TODO: Kill the thread */
        assert(qidx < vud->nqueues);
        ourqi = vud->qi[qidx];
        ourqi->kick_fd = -1;
    }
}

static bool fv_queue_order(VuDev *dev, int qidx)
{
    return false;
}

static const VuDevIface fv_iface = {
    .get_features = fv_get_features,
    .set_features = fv_set_features,

    /* Don't need process message, we've not got any at vhost-user level */
    .queue_set_started = fv_queue_set_started,

    .queue_is_processed_in_order = fv_queue_order,
};

/*
 * Main loop; this mostly deals with events on the vhost-user
 * socket itself, and not actual fuse data.
 */
int virtio_loop(struct fuse_session *se)
{
    fuse_log(FUSE_LOG_INFO, "%s: Entry\n", __func__);

    while (!fuse_session_exited(se)) {
        struct pollfd pf[1];
        pf[0].fd = se->vu_socketfd;
        pf[0].events = POLLIN;
        pf[0].revents = 0;

        fuse_log(FUSE_LOG_DEBUG, "%s: Waiting for VU event\n", __func__);
        int poll_res = ppoll(pf, 1, NULL, NULL);

        if (poll_res == -1) {
            if (errno == EINTR) {
                fuse_log(FUSE_LOG_INFO, "%s: ppoll interrupted, going around\n",
                         __func__);
                continue;
            }
            fuse_log(FUSE_LOG_ERR, "virtio_loop ppoll: %m\n");
            break;
        }
        assert(poll_res == 1);
        if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fuse_log(FUSE_LOG_ERR, "%s: Unexpected poll revents %x\n", __func__,
                     pf[0].revents);
            break;
        }
        assert(pf[0].revents & POLLIN);
        fuse_log(FUSE_LOG_DEBUG, "%s: Got VU event\n", __func__);
        if (!vu_dispatch(&se->virtio_dev->dev)) {
            fuse_log(FUSE_LOG_ERR, "%s: vu_dispatch failed\n", __func__);
            break;
        }
    }

    fuse_log(FUSE_LOG_INFO, "%s: Exit\n", __func__);

    return 0;
}

int virtio_session_mount(struct fuse_session *se)
{
    struct sockaddr_un un;
    mode_t old_umask;

    if (strlen(se->vu_socket_path) >= sizeof(un.sun_path)) {
        fuse_log(FUSE_LOG_ERR, "Socket path too long\n");
        return -1;
    }

    /*
     * Poison the fuse FD so we spot if we accidentally use it;
     * DO NOT check for this value, check for fuse_lowlevel_is_virtio()
     */
    se->fd = 0xdaff0d11;

    /*
     * Create the Unix socket to communicate with qemu
     * based on QEMU's vhost-user-bridge
     */
    unlink(se->vu_socket_path);
    strcpy(un.sun_path, se->vu_socket_path);
    size_t addr_len = sizeof(un);

    int listen_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_sock == -1) {
        fuse_log(FUSE_LOG_ERR, "vhost socket creation: %m\n");
        return -1;
    }
    un.sun_family = AF_UNIX;

    /*
     * Unfortunately bind doesn't let you set the mask on the socket,
     * so set umask to 077 and restore it later.
     */
    old_umask = umask(0077);
    if (bind(listen_sock, (struct sockaddr *)&un, addr_len) == -1) {
        fuse_log(FUSE_LOG_ERR, "vhost socket bind: %m\n");
        umask(old_umask);
        return -1;
    }
    umask(old_umask);

    if (listen(listen_sock, 1) == -1) {
        fuse_log(FUSE_LOG_ERR, "vhost socket listen: %m\n");
        return -1;
    }

    fuse_log(FUSE_LOG_INFO, "%s: Waiting for vhost-user socket connection...\n",
             __func__);
    int data_sock = accept(listen_sock, NULL, NULL);
    if (data_sock == -1) {
        fuse_log(FUSE_LOG_ERR, "vhost socket accept: %m\n");
        close(listen_sock);
        return -1;
    }
    close(listen_sock);
    fuse_log(FUSE_LOG_INFO, "%s: Received vhost-user socket connection\n",
             __func__);

    /* TODO: Some cleanup/deallocation! */
    se->virtio_dev = calloc(sizeof(struct fv_VuDev), 1);
    if (!se->virtio_dev) {
        fuse_log(FUSE_LOG_ERR, "%s: virtio_dev calloc failed\n", __func__);
        close(data_sock);
        return -1;
    }

    se->vu_socketfd = data_sock;
    se->virtio_dev->se = se;
    vu_init(&se->virtio_dev->dev, 2, se->vu_socketfd, fv_panic, fv_set_watch,
            fv_remove_watch, &fv_iface);

    return 0;
}
