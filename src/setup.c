/* SPDX-License-Identifier: MIT */
#define _DEFAULT_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include "liburing/compat.h"
#include "liburing/io_uring.h"
#include "liburing.h"

#include "syscall.h"

static void io_uring_unmap_rings(struct io_uring_sq *sq, struct io_uring_cq *cq)
{
	munmap(sq->ring_ptr, sq->ring_sz);
	if (cq->ring_ptr && cq->ring_ptr != sq->ring_ptr)
		munmap(cq->ring_ptr, cq->ring_sz);
}

static int io_uring_mmap(int fd, struct io_uring_params *p,
			 struct io_uring_sq *sq, struct io_uring_cq *cq)
{
	size_t size;
	int ret;

	sq->ring_sz = p->sq_off.array + p->sq_entries * sizeof(unsigned);
	cq->ring_sz = p->cq_off.cqes + p->cq_entries * sizeof(struct io_uring_cqe);

	if (p->features & IORING_FEAT_SINGLE_MMAP) {
		if (cq->ring_sz > sq->ring_sz)
			sq->ring_sz = cq->ring_sz;
		cq->ring_sz = sq->ring_sz;
	}
	sq->ring_ptr = mmap(0, sq->ring_sz, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
	if (sq->ring_ptr == MAP_FAILED)
		return -errno;

	if (p->features & IORING_FEAT_SINGLE_MMAP) {
		cq->ring_ptr = sq->ring_ptr;
	} else {
		cq->ring_ptr = mmap(0, cq->ring_sz, PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
		if (cq->ring_ptr == MAP_FAILED) {
			cq->ring_ptr = NULL;
			ret = -errno;
			goto err;
		}
	}

	sq->khead = sq->ring_ptr + p->sq_off.head;
	sq->ktail = sq->ring_ptr + p->sq_off.tail;
	sq->kring_mask = sq->ring_ptr + p->sq_off.ring_mask;
	sq->kring_entries = sq->ring_ptr + p->sq_off.ring_entries;
	sq->kflags = sq->ring_ptr + p->sq_off.flags;
	sq->kdropped = sq->ring_ptr + p->sq_off.dropped;
	sq->array = sq->ring_ptr + p->sq_off.array;

	size = p->sq_entries * sizeof(struct io_uring_sqe);
	sq->sqes = mmap(0, size, PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_POPULATE, fd,
				IORING_OFF_SQES);
	if (sq->sqes == MAP_FAILED) {
		ret = -errno;
err:
		io_uring_unmap_rings(sq, cq);
		return ret;
	}

	cq->khead = cq->ring_ptr + p->cq_off.head;
	cq->ktail = cq->ring_ptr + p->cq_off.tail;
	cq->kring_mask = cq->ring_ptr + p->cq_off.ring_mask;
	cq->kring_entries = cq->ring_ptr + p->cq_off.ring_entries;
	cq->koverflow = cq->ring_ptr + p->cq_off.overflow;
	cq->cqes = cq->ring_ptr + p->cq_off.cqes;
	if (p->cq_off.flags)
		cq->kflags = cq->ring_ptr + p->cq_off.flags;
	return 0;
}

/*
 * For users that want to specify sq_thread_cpu or sq_thread_idle, this
 * interface is a convenient helper for mmap()ing the rings.
 * Returns -errno on error, or zero on success.  On success, 'ring'
 * contains the necessary information to read/write to the rings.
 */
int io_uring_queue_mmap(int fd, struct io_uring_params *p, struct io_uring *ring)
{
	int ret;

	memset(ring, 0, sizeof(*ring));
	ret = io_uring_mmap(fd, p, &ring->sq, &ring->cq);
	if (!ret) {
		ring->flags = p->flags;
		ring->ring_fd = fd;
	}
	return ret;
}

/*
 * Ensure that the mmap'ed rings aren't available to a child after a fork(2).
 * This uses madvise(..., MADV_DONTFORK) on the mmap'ed ranges.
 */
int io_uring_ring_dontfork(struct io_uring *ring)
{
	size_t len;
	int ret;

	if (!ring->sq.ring_ptr || !ring->sq.sqes || !ring->cq.ring_ptr)
		return -EINVAL;

	len = *ring->sq.kring_entries * sizeof(struct io_uring_sqe);
	ret = madvise(ring->sq.sqes, len, MADV_DONTFORK);
	if (ret == -1)
		return -errno;

	len = ring->sq.ring_sz;
	ret = madvise(ring->sq.ring_ptr, len, MADV_DONTFORK);
	if (ret == -1)
		return -errno;

	if (ring->cq.ring_ptr != ring->sq.ring_ptr) {
		len = ring->cq.ring_sz;
		ret = madvise(ring->cq.ring_ptr, len, MADV_DONTFORK);
		if (ret == -1)
			return -errno;
	}

	return 0;
}

int io_uring_queue_init_params(unsigned entries, struct io_uring *ring,
			       struct io_uring_params *p)
{
	int fd, ret;

	fd = __sys_io_uring_setup(entries, p);
	if (fd < 0)
		return -errno;

	ret = io_uring_queue_mmap(fd, p, ring);
	if (ret) {
		close(fd);
		return ret;
	}

	ring->features = p->features;
	return 0;
}

/*
 * Returns -errno on error, or zero on success. On success, 'ring'
 * contains the necessary information to read/write to the rings.
 */
int io_uring_queue_init(unsigned entries, struct io_uring *ring, unsigned flags)
{
	struct io_uring_params p;

	memset(&p, 0, sizeof(p));
	p.flags = flags;

	return io_uring_queue_init_params(entries, ring, &p);
}

void io_uring_queue_exit(struct io_uring *ring)
{
	struct io_uring_sq *sq = &ring->sq;
	struct io_uring_cq *cq = &ring->cq;

	munmap(sq->sqes, *sq->kring_entries * sizeof(struct io_uring_sqe));
	io_uring_unmap_rings(sq, cq);
	close(ring->ring_fd);
}

struct io_uring_probe *io_uring_get_probe_ring(struct io_uring *ring)
{
	struct io_uring_probe *probe;
	size_t len;
	int r;

	len = sizeof(*probe) + 256 * sizeof(struct io_uring_probe_op);
	probe = malloc(len);
	if (!probe)
		return NULL;
	memset(probe, 0, len);

	r = io_uring_register_probe(ring, probe, 256);
	if (r >= 0)
		return probe;

	free(probe);
	return NULL;
}

struct io_uring_probe *io_uring_get_probe(void)
{
	struct io_uring ring;
	struct io_uring_probe *probe;
	int r;

	r = io_uring_queue_init(2, &ring, 0);
	if (r < 0)
		return NULL;

	probe = io_uring_get_probe_ring(&ring);
	io_uring_queue_exit(&ring);
	return probe;
}

void io_uring_free_probe(struct io_uring_probe *probe)
{
	free(probe);
}

static int __fls(int x)
{
	int r = 32;

	if (!x)
		return 0;
	if (!(x & 0xffff0000u)) {
		x <<= 16;
		r -= 16;
	}
	if (!(x & 0xff000000u)) {
		x <<= 8;
		r -= 8;
	}
	if (!(x & 0xf0000000u)) {
		x <<= 4;
		r -= 4;
	}
	if (!(x & 0xc0000000u)) {
		x <<= 2;
		r -= 2;
	}
	if (!(x & 0x80000000u)) {
		x <<= 1;
		r -= 1;
	}
	return r;
}

static unsigned roundup_pow2(unsigned depth)
{
	return 1UL << __fls(depth - 1);
}

static size_t npages(size_t size, unsigned page_size)
{
	size--;
	size /= page_size;
	return __fls(size);
}

#define KRING_SIZE	320

static size_t rings_size(unsigned entries, unsigned cq_entries, unsigned page_size)
{
	size_t pages, sq_size, cq_size;

	cq_size = KRING_SIZE;
	cq_size += cq_entries * sizeof(struct io_uring_cqe);
	cq_size = (cq_size + 63) & ~63UL;
	pages = (size_t) 1 << npages(cq_size, page_size);

	sq_size = sizeof(struct io_uring_sqe) * entries;
	pages += (size_t) 1 << npages(sq_size, page_size);
	return pages * page_size;
}

#define KERN_MAX_ENTRIES	32768
#define KERN_MAX_CQ_ENTRIES	(2 * KERN_MAX_ENTRIES)

ssize_t io_uring_mlock_size_params(unsigned entries, struct io_uring_params *p)
{
	struct io_uring_params lp = { };
	struct io_uring ring;
	unsigned cq_entries;
	long page_size;
	ssize_t ret;

	ret = io_uring_queue_init_params(entries, &ring, &lp);
	if (ret < 0)
		return ret;

	io_uring_queue_exit(&ring);

	/*
	 * Native workers imply using cgroup memory accounting, and hence no
	 * memlock memory is needed for the ring allocations.
	 */
	if (lp.features & IORING_FEAT_NATIVE_WORKERS)
		return 0;

	if (!entries)
		return -EINVAL;
	if (entries > KERN_MAX_ENTRIES) {
		if (!(p->flags & IORING_SETUP_CLAMP))
			return -EINVAL;
		entries = KERN_MAX_ENTRIES;
	}

	entries = roundup_pow2(entries);
	if (p->flags & IORING_SETUP_CQSIZE) {
		if (!p->cq_entries)
			return -EINVAL;
		cq_entries = p->cq_entries;
		if (cq_entries > KERN_MAX_CQ_ENTRIES) {
			if (!(p->flags & IORING_SETUP_CLAMP))
				return -EINVAL;
			cq_entries = KERN_MAX_CQ_ENTRIES;
		}
		cq_entries = roundup_pow2(cq_entries);
		if (cq_entries < entries)
			return -EINVAL;
	} else {
		cq_entries = 2 * entries;
	}

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size < 0)
		page_size = 4096;

	return rings_size(entries, cq_entries, page_size);
}

ssize_t io_uring_mlock_size(unsigned entries, unsigned flags)
{
	struct io_uring_params p = { .flags = flags, };

	return io_uring_mlock_size_params(entries, &p);
}
