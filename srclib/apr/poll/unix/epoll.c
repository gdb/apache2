/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "apr_arch_poll_private.h"
#include "apr_arch_inherit.h"

#ifdef POLLSET_USES_EPOLL

static apr_int16_t get_epoll_event(apr_int16_t event)
{
    apr_int16_t rv = 0;

    if (event & APR_POLLIN)
        rv |= EPOLLIN;
    if (event & APR_POLLPRI)
        rv |= EPOLLPRI;
    if (event & APR_POLLOUT)
        rv |= EPOLLOUT;
    if (event & APR_POLLERR)
        rv |= EPOLLERR;
    if (event & APR_POLLHUP)
        rv |= EPOLLHUP;
    /* APR_POLLNVAL is not handled by epoll. */

    return rv;
}

static apr_int16_t get_epoll_revent(apr_int16_t event)
{
    apr_int16_t rv = 0;

    if (event & EPOLLIN)
        rv |= APR_POLLIN;
    if (event & EPOLLPRI)
        rv |= APR_POLLPRI;
    if (event & EPOLLOUT)
        rv |= APR_POLLOUT;
    if (event & EPOLLERR)
        rv |= APR_POLLERR;
    if (event & EPOLLHUP)
        rv |= APR_POLLHUP;
    /* APR_POLLNVAL is not handled by epoll. */

    return rv;
}

struct apr_pollset_t
{
    apr_pool_t *pool;
    apr_uint32_t nelts;
    apr_uint32_t nalloc;
    int epoll_fd;
    struct epoll_event *pollset;
    apr_pollfd_t *result_set;
    apr_uint32_t flags;
#if APR_HAS_THREADS
    /* A thread mutex to protect operations on the rings */
    apr_thread_mutex_t *ring_lock;
#endif
    /* A ring containing all of the pollfd_t that are active */
    APR_RING_HEAD(pfd_query_ring_t, pfd_elem_t) query_ring;
    /* A ring of pollfd_t that have been used, and then _remove()'d */
    APR_RING_HEAD(pfd_free_ring_t, pfd_elem_t) free_ring;
    /* A ring of pollfd_t where rings that have been _remove()`ed but
        might still be inside a _poll() */
    APR_RING_HEAD(pfd_dead_ring_t, pfd_elem_t) dead_ring;
};

static apr_status_t backend_cleanup(void *p_)
{
    apr_pollset_t *pollset = (apr_pollset_t *) p_;
    close(pollset->epoll_fd);
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_pollset_create(apr_pollset_t **pollset,
                                             apr_uint32_t size,
                                             apr_pool_t *p,
                                             apr_uint32_t flags)
{
#if APR_HAS_THREADS
    apr_status_t rv;
#endif
    int fd;

#ifdef HAVE_EPOLL_CREATE1
    fd = epoll_create1(EPOLL_CLOEXEC);
#else
    fd = epoll_create(size);
#endif
    if (fd < 0) {
        *pollset = NULL;
        return errno;
    }

#ifndef HAVE_EPOLL_CREATE1
    {
        int flags;

        if ((flags = fcntl(fd, F_GETFD)) == -1)
            return errno;

        flags |= FD_CLOEXEC;
        if (fcntl(fd, F_SETFD, flags) == -1)
            return errno;
    }
#endif

    *pollset = apr_palloc(p, sizeof(**pollset));
#if APR_HAS_THREADS
    if ((flags & APR_POLLSET_THREADSAFE) &&
        !(flags & APR_POLLSET_NOCOPY) &&
        ((rv = apr_thread_mutex_create(&(*pollset)->ring_lock,
                                       APR_THREAD_MUTEX_DEFAULT,
                                       p)) != APR_SUCCESS)) {
        *pollset = NULL;
        return rv;
    }
#else
    if (flags & APR_POLLSET_THREADSAFE) {
        *pollset = NULL;
        return APR_ENOTIMPL;
    }
#endif
    (*pollset)->nelts = 0;
    (*pollset)->nalloc = size;
    (*pollset)->flags = flags;
    (*pollset)->pool = p;
    (*pollset)->epoll_fd = fd;
    (*pollset)->pollset = apr_palloc(p, size * sizeof(struct epoll_event));
    apr_pool_cleanup_register(p, *pollset, backend_cleanup, backend_cleanup);
    (*pollset)->result_set = apr_palloc(p, size * sizeof(apr_pollfd_t));

    if (!(flags & APR_POLLSET_NOCOPY)) {
        APR_RING_INIT(&(*pollset)->query_ring, pfd_elem_t, link);
        APR_RING_INIT(&(*pollset)->free_ring, pfd_elem_t, link);
        APR_RING_INIT(&(*pollset)->dead_ring, pfd_elem_t, link);
    }
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_pollset_destroy(apr_pollset_t *pollset)
{
    return apr_pool_cleanup_run(pollset->pool, pollset, backend_cleanup);
}

APR_DECLARE(apr_status_t) apr_pollset_add(apr_pollset_t *pollset,
                                          const apr_pollfd_t *descriptor)
{
    struct epoll_event ev = {0};
    int ret = -1;
    pfd_elem_t *elem = NULL;
    apr_status_t rv = APR_SUCCESS;

    ev.events = get_epoll_event(descriptor->reqevents);

    if (pollset->flags & APR_POLLSET_NOCOPY) {
        ev.data.ptr = (void *)descriptor;
    }
    else {
        pollset_lock_rings();

        if (!APR_RING_EMPTY(&(pollset->free_ring), pfd_elem_t, link)) {
            elem = APR_RING_FIRST(&(pollset->free_ring));
            APR_RING_REMOVE(elem, link);
        }
        else {
            elem = (pfd_elem_t *) apr_palloc(pollset->pool, sizeof(pfd_elem_t));
            APR_RING_ELEM_INIT(elem, link);
        }
        elem->pfd = *descriptor;
        ev.data.ptr = elem;
    }
    if (descriptor->desc_type == APR_POLL_SOCKET) {
        ret = epoll_ctl(pollset->epoll_fd, EPOLL_CTL_ADD,
                        descriptor->desc.s->socketdes, &ev);
    }
    else {
        ret = epoll_ctl(pollset->epoll_fd, EPOLL_CTL_ADD,
                        descriptor->desc.f->filedes, &ev);
    }

    if (pollset->flags & APR_POLLSET_NOCOPY) {
        if (0 != ret) {
            rv = APR_EBADF;
        }
    }
    else {
        if (0 != ret) {
            rv = APR_EBADF;
            APR_RING_INSERT_TAIL(&(pollset->free_ring), elem, pfd_elem_t, link);
        }
        else {
            pollset->nelts++;
            APR_RING_INSERT_TAIL(&(pollset->query_ring), elem, pfd_elem_t, link);
        }
        pollset_unlock_rings();
    }

    return rv;
}

APR_DECLARE(apr_status_t) apr_pollset_remove(apr_pollset_t *pollset,
                                             const apr_pollfd_t *descriptor)
{
    pfd_elem_t *ep;
    apr_status_t rv = APR_SUCCESS;
    struct epoll_event ev;
    int ret = -1;

    ev.events = get_epoll_event(descriptor->reqevents);

    if (descriptor->desc_type == APR_POLL_SOCKET) {
        ret = epoll_ctl(pollset->epoll_fd, EPOLL_CTL_DEL,
                        descriptor->desc.s->socketdes, &ev);
    }
    else {
        ret = epoll_ctl(pollset->epoll_fd, EPOLL_CTL_DEL,
                        descriptor->desc.f->filedes, &ev);
    }
    if (ret < 0) {
        rv = APR_NOTFOUND;
    }

    if (!(pollset->flags & APR_POLLSET_NOCOPY)) {
        pollset_lock_rings();

        if (!APR_RING_EMPTY(&(pollset->query_ring), pfd_elem_t, link)) {
            for (ep = APR_RING_FIRST(&(pollset->query_ring));
                 ep != APR_RING_SENTINEL(&(pollset->query_ring),
                                         pfd_elem_t, link);
                 ep = APR_RING_NEXT(ep, link)) {
                
                if (descriptor->desc.s == ep->pfd.desc.s) {
                    APR_RING_REMOVE(ep, link);
                    APR_RING_INSERT_TAIL(&(pollset->dead_ring),
                                         ep, pfd_elem_t, link);
                    break;
                }
            }
        }

        pollset_unlock_rings();
    }

    return rv;
}

APR_DECLARE(apr_status_t) apr_pollset_poll(apr_pollset_t *pollset,
                                           apr_interval_time_t timeout,
                                           apr_int32_t *num,
                                           const apr_pollfd_t **descriptors)
{
    int ret, i;
    apr_status_t rv = APR_SUCCESS;

    if (timeout > 0) {
        timeout /= 1000;
    }

    ret = epoll_wait(pollset->epoll_fd, pollset->pollset, pollset->nalloc,
                     timeout);
    (*num) = ret;

    if (ret < 0) {
        rv = apr_get_netos_error();
    }
    else if (ret == 0) {
        rv = APR_TIMEUP;
    }
    else {
        if (pollset->flags & APR_POLLSET_NOCOPY) {
            for (i = 0; i < ret; i++) {
                pollset->result_set[i] =
                    *((apr_pollfd_t *) (pollset->pollset[i].data.ptr));
                pollset->result_set[i].rtnevents =
                    get_epoll_revent(pollset->pollset[i].events);
            }
        }
        else {
            for (i = 0; i < ret; i++) {
                pollset->result_set[i] =
                    (((pfd_elem_t *) (pollset->pollset[i].data.ptr))->pfd);
                pollset->result_set[i].rtnevents =
                    get_epoll_revent(pollset->pollset[i].events);
            }
        }

        if (descriptors) {
            *descriptors = pollset->result_set;
        }
    }

    if (!(pollset->flags & APR_POLLSET_NOCOPY)) {
        pollset_lock_rings();

        /* Shift all PFDs in the Dead Ring to be Free Ring */
        APR_RING_CONCAT(&(pollset->free_ring), &(pollset->dead_ring), pfd_elem_t, link);

        pollset_unlock_rings();
    }

    return rv;
}

struct apr_pollcb_t {
    apr_pool_t *pool;
    apr_uint32_t nalloc;
    struct epoll_event *pollset;
    int epoll_fd;
};

static apr_status_t cb_cleanup(void *p_)
{
    apr_pollcb_t *pollcb = (apr_pollcb_t *) p_;
    close(pollcb->epoll_fd);
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_pollcb_create(apr_pollcb_t **pollcb,
                                            apr_uint32_t size,
                                            apr_pool_t *p,
                                            apr_uint32_t flags)
{
    int fd;
    
#ifdef HAVE_EPOLL_CREATE1
    fd = epoll_create1(EPOLL_CLOEXEC);
#else
    fd = epoll_create(size);
#endif
    
    if (fd < 0) {
        *pollcb = NULL;
        return apr_get_netos_error();
    }

#ifndef HAVE_EPOLL_CREATE1
    {
        int flags;

        if ((flags = fcntl(fd, F_GETFD)) == -1)
            return errno;

        flags |= FD_CLOEXEC;
        if (fcntl(fd, F_SETFD, flags) == -1)
            return errno;
    }
#endif
    
    *pollcb = apr_palloc(p, sizeof(**pollcb));
    (*pollcb)->nalloc = size;
    (*pollcb)->pool = p;
    (*pollcb)->epoll_fd = fd;
    (*pollcb)->pollset = apr_palloc(p, size * sizeof(struct epoll_event));
    apr_pool_cleanup_register(p, *pollcb, cb_cleanup, cb_cleanup);

    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_pollcb_add(apr_pollcb_t *pollcb,
                                         apr_pollfd_t *descriptor)
{
    struct epoll_event ev;
    int ret;
    
    ev.events = get_epoll_event(descriptor->reqevents);
    ev.data.ptr = (void *)descriptor;

    if (descriptor->desc_type == APR_POLL_SOCKET) {
        ret = epoll_ctl(pollcb->epoll_fd, EPOLL_CTL_ADD,
                        descriptor->desc.s->socketdes, &ev);
    }
    else {
        ret = epoll_ctl(pollcb->epoll_fd, EPOLL_CTL_ADD,
                        descriptor->desc.f->filedes, &ev);
    }
    
    if (ret == -1) {
        return apr_get_netos_error();
    }
    
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_pollcb_remove(apr_pollcb_t *pollcb,
                                            apr_pollfd_t *descriptor)
{
    apr_status_t rv = APR_SUCCESS;
    struct epoll_event ev;
    int ret = -1;
    
    ev.events = get_epoll_event(descriptor->reqevents);
    
    if (descriptor->desc_type == APR_POLL_SOCKET) {
        ret = epoll_ctl(pollcb->epoll_fd, EPOLL_CTL_DEL,
                        descriptor->desc.s->socketdes, &ev);
    }
    else {
        ret = epoll_ctl(pollcb->epoll_fd, EPOLL_CTL_DEL,
                        descriptor->desc.f->filedes, &ev);
    }
    
    if (ret < 0) {
        rv = APR_NOTFOUND;
    }
    
    return rv;
}


APR_DECLARE(apr_status_t) apr_pollcb_poll(apr_pollcb_t *pollcb,
                                          apr_interval_time_t timeout,
                                          apr_pollcb_cb_t func,
                                          void *baton)
{
    int ret, i;
    apr_status_t rv = APR_SUCCESS;
    
    if (timeout > 0) {
        timeout /= 1000;
    }
    
    ret = epoll_wait(pollcb->epoll_fd, pollcb->pollset, pollcb->nalloc,
                     timeout);
    if (ret < 0) {
        rv = apr_get_netos_error();
    }
    else if (ret == 0) {
        rv = APR_TIMEUP;
    }
    else {
        for (i = 0; i < ret; i++) {
            apr_pollfd_t *pollfd = (apr_pollfd_t *)(pollcb->pollset[i].data.ptr);
            pollfd->rtnevents = get_epoll_revent(pollcb->pollset[i].events);

            rv = func(baton, pollfd);
            if (rv) {
                return rv;
            }
        }
    }
    
    return rv;
}

#endif /* POLLSET_USES_EPOLL */
