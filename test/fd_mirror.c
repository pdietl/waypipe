/*
 * Copyright © 2019 Manuel Stoeckl
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define _GNU_SOURCE
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>

struct compression_settings {
	enum compression_mode mode;
	int level;
};

static const struct compression_settings comp_modes[] = {
		{COMP_NONE, 0},
#ifdef HAS_LZ4
		{COMP_LZ4, 1},
#endif
#ifdef HAS_ZSTD
		{COMP_ZSTD, 5},
#endif
};

#ifdef HAS_DMABUF
#include <gbm.h>
#define TEST_2CPP_FORMAT GBM_FORMAT_GR88
#else
#define TEST_2CPP_FORMAT 0
#endif

static int update_file(int file_fd, struct gbm_bo *bo, size_t sz, int seqno)
{
	(void)bo;
	if (rand() % 11 == 0) {
		/* no change */
		return 0;
	}

	void *data = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, file_fd,
			0);
	if (data == MAP_FAILED) {
		return -1;
	}

	size_t start = (size_t)rand() % sz;
	size_t end = (size_t)rand() % sz;
	if (start > end) {
		size_t tmp = start;
		start = end;
		end = tmp;
	}
	memset((char *)data + start, seqno, end - start);

	munmap(data, sz);
	return (int)(end - start);
}

static int update_dmabuf(int file_fd, struct gbm_bo *bo, size_t sz, int seqno)
{
	(void)file_fd;
	if (rand() % 11 == 0) {
		/* no change */
		return 0;
	}

	void *map_handle = NULL;
	void *data = map_dmabuf(bo, true, &map_handle);
	if (data == MAP_FAILED) {
		return -1;
	}

	size_t start = (size_t)rand() % sz;
	size_t end = (size_t)rand() % sz;
	if (start > end) {
		size_t tmp = start;
		start = end;
		end = tmp;
	}
	memset((char *)data + start, seqno, end - start);

	unmap_dmabuf(bo, map_handle);
	return (int)(end - start);
}

static struct bytebuf combine_transfer_blocks(struct transfer_data *td)
{
	size_t net_size = 0;
	for (int i = td->start; i < td->end; i++) {
		net_size += td->data[i].iov_len;
	}

	struct bytebuf ret_block;
	ret_block.size = net_size;
	ret_block.data = malloc(net_size);
	size_t pos = 0;
	for (int i = td->start; i < td->end; i++) {
		memcpy(ret_block.data + pos, td->data[i].iov_base,
				td->data[i].iov_len);
		pos += td->data[i].iov_len;
	}
	return ret_block;
}

static bool check_match(int orig_fd, int copy_fd, struct gbm_bo *orig_bo,
		struct gbm_bo *copy_bo)
{
	size_t csz = 0, osz = 0;
	fdcat_t ctype = get_fd_type(copy_fd, &csz);
	fdcat_t otype = get_fd_type(orig_fd, &osz);
	if (ctype != otype || csz != osz) {
		wp_error("Mirrored file descriptor has different type or size: ot=%d ct=%d | os=%d cs=%d",
				otype, ctype, (int)osz, (int)csz);
		return false;
	}

	void *ohandle = NULL, *chandle = NULL;
	void *cdata = NULL, *odata = NULL;
	if (otype == FDC_FILE) {
		cdata = mmap(NULL, csz, PROT_READ, MAP_SHARED, copy_fd, 0);
		if (cdata == MAP_FAILED) {
			return false;
		}
		odata = mmap(NULL, osz, PROT_READ, MAP_SHARED, orig_fd, 0);
		if (odata == MAP_FAILED) {
			munmap(cdata, csz);
			return false;
		}
	} else if (otype == FDC_DMABUF) {
		cdata = map_dmabuf(copy_bo, false, &chandle);
		if (cdata == MAP_FAILED) {
			return false;
		}
		odata = map_dmabuf(orig_bo, false, &ohandle);
		if (odata == MAP_FAILED) {
			unmap_dmabuf(copy_bo, chandle);
			return false;
		}
	}

	bool pass = memcmp(cdata, odata, csz) == 0;

	if (otype == FDC_FILE) {
		munmap(odata, osz);
		munmap(cdata, csz);
	} else if (otype == FDC_DMABUF) {
		unmap_dmabuf(orig_bo, ohandle);
		unmap_dmabuf(copy_bo, chandle);
	}

	if (!pass) {
		wp_error("Mirrored file descriptor contents differ");
	}

	return pass;
}

static void cleanup_transfer(struct transfer_data *td)
{
	pthread_mutex_destroy(&td->lock);
	for (int i = td->start; i < td->end; i++) {
		if (td->data[i].iov_base != &td->zeros) {
			free(td->data[i].iov_base);
		}
	}
	free(td->data);
	free(td->msgno);
}

static void wait_for_thread_pool(struct thread_pool *pool)
{
	bool done = false;
	while (!done) {
		uint8_t flush[64];
		(void)read(pool->selfpipe_r, flush, sizeof(flush));

		/* Also run tasks on main thread, just like the real version */
		// TODO: create a 'threadpool.c'
		struct task_data task;
		task.type = TASK_STOP;
		pthread_mutex_lock(&pool->work_mutex);
		done = pool->queue_end == pool->queue_start &&
		       pool->queue_in_progress == 0;
		if (pool->queue_start < pool->queue_end) {
			int i = pool->queue_start;
			if (pool->queue[i].type != TASK_STOP) {
				task = pool->queue[i];
				pool->queue_start++;
				pool->queue_in_progress++;
			}
		}
		pthread_mutex_unlock(&pool->work_mutex);

		if (task.type != TASK_STOP) {
			run_task(&task, &pool->threads[0]);

			pthread_mutex_lock(&pool->work_mutex);
			pool->queue_in_progress--;
			pthread_mutex_unlock(&pool->work_mutex);
			/* To skip the next poll */
		} else {
			/* Wait a short amount */
			usleep(100);
		}
	}
}

static bool test_transfer(struct fd_translation_map *src_map,
		struct fd_translation_map *dst_map,
		struct thread_pool *src_pool, struct thread_pool *dst_pool,
		int rid, int ndiff, struct render_data *render_data)
{
	struct transfer_data transfer_data;
	memset(&transfer_data, 0, sizeof(struct transfer_data));
	pthread_mutex_init(&transfer_data.lock, NULL);

	struct shadow_fd *src_shadow = get_shadow_for_rid(src_map, rid);
	collect_update(src_map, src_pool, src_shadow, &transfer_data);
	wait_for_thread_pool(src_pool);
	finish_update(src_map, src_shadow);

	if (ndiff == 0) {
		long ns = 0;
		for (int i = transfer_data.start; i < transfer_data.end; i++) {
			ns += transfer_data.data[i].iov_len;
		}
		cleanup_transfer(&transfer_data);
		if (transfer_data.end > 0) {
			wp_error("Collecting updates gave a transfer (%ld bytes, %d blocks) when none was expected",
					ns, transfer_data.end);
			return false;
		}
		return true;
	}
	if (transfer_data.end == 0) {
		wp_error("Collecting updates gave a unexpected number (%d) of transfers",
				transfer_data.end);
		cleanup_transfer(&transfer_data);
		return false;
	}
	struct bytebuf res = combine_transfer_blocks(&transfer_data);
	cleanup_transfer(&transfer_data);

	size_t start = 0;
	while (start < res.size) {
		struct bytebuf tmp;
		tmp.data = &res.data[start];
		uint32_t hb = ((uint32_t *)tmp.data)[0];
		int32_t xid = ((int32_t *)tmp.data)[1];
		tmp.size = alignu(transfer_size(hb), 16);
		apply_update(dst_map, dst_pool, render_data, transfer_type(hb),
				xid, &tmp);
		start += tmp.size;
	}
	free(res.data);

	/* first round, this only exists after the transfer */
	struct shadow_fd *dst_shadow = get_shadow_for_rid(dst_map, rid);

	return check_match(src_shadow->fd_local, dst_shadow->fd_local,
			src_shadow->dmabuf_bo, dst_shadow->dmabuf_bo);
}

/* This test closes the provided file fd */
static bool test_mirror(int new_file_fd, size_t sz,
		int (*update)(int fd, struct gbm_bo *bo, size_t sz, int seqno),
		struct compression_settings comp_mode, int n_src_threads,
		int n_dst_threads, struct render_data *rd,
		const struct dmabuf_slice_data *slice_data)
{
	struct fd_translation_map src_map;
	setup_translation_map(&src_map, false);

	struct thread_pool src_pool;
	setup_thread_pool(&src_pool, comp_mode.mode, comp_mode.level,
			n_src_threads);

	struct fd_translation_map dst_map;
	setup_translation_map(&dst_map, true);

	struct thread_pool dst_pool;
	setup_thread_pool(&dst_pool, comp_mode.mode, comp_mode.level,
			n_dst_threads);

	size_t fdsz = 0;
	fdcat_t fdtype = get_fd_type(new_file_fd, &fdsz);
	struct shadow_fd *src_shadow = translate_fd(
			&src_map, rd, new_file_fd, fdtype, fdsz, slice_data);
	struct shadow_fd *dst_shadow = NULL;
	int rid = src_shadow->remote_id;

	bool pass = true;
	for (int i = 0; i < 5; i++) {
		bool fwd = i == 0 || i % 2;

		int target_fd = fwd ? src_shadow->fd_local
				    : dst_shadow->fd_local;
		struct gbm_bo *target_bo = fwd ? src_shadow->dmabuf_bo
					       : dst_shadow->dmabuf_bo;
		int ndiff = i > 0 ? (*update)(target_fd, target_bo, sz, i)
				  : (int)sz;
		if (ndiff == -1) {
			pass = false;
			break;
		}
		bool subpass;
		if (fwd) {
			src_shadow->is_dirty = true;
			damage_everything(&src_shadow->damage);
			subpass = test_transfer(&src_map, &dst_map, &src_pool,
					&dst_pool, rid, ndiff, rd);
		} else {
			dst_shadow->is_dirty = true;
			damage_everything(&dst_shadow->damage);
			subpass = test_transfer(&dst_map, &src_map, &dst_pool,
					&dst_pool, rid, ndiff, rd);
		}
		pass &= subpass;
		if (!pass) {
			break;
		}

		dst_shadow = get_shadow_for_rid(&dst_map, rid);
	}

	cleanup_translation_map(&src_map);
	cleanup_translation_map(&dst_map);
	cleanup_thread_pool(&src_pool);
	cleanup_thread_pool(&dst_pool);
	return pass;
}

log_handler_func_t log_funcs[2] = {NULL, test_log_handler};
int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	if (mkdir("test", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == -1 &&
			errno != EEXIST) {
		wp_error("Not allowed to create test directory, cannot run tests.");
		return EXIT_FAILURE;
	}

	/* to avoid warnings when the driver dmabuf size constraints require
	 * significant alignment, the width/height are already 64 aligned */
	const size_t test_width = 256;
	const size_t test_height = 320;
	const size_t test_cpp = 2;
	const size_t test_size = test_width * test_height * test_cpp;
	const struct dmabuf_slice_data slice_data = {
			.width = (uint32_t)test_width,
			.height = (uint32_t)test_height,
			.format = TEST_2CPP_FORMAT,
			.num_planes = 1,
			.modifier = 0,
			.offsets = {0, 0, 0, 0},
			.strides = {(uint32_t)(test_width * test_cpp), 0, 0, 0},
			.using_planes = {true, false, false, false},
	};

	uint8_t *test_pattern = malloc(test_size);
	for (size_t i = 0; i < test_size; i++) {
		test_pattern[i] = (uint8_t)i;
	}

	struct render_data *rd = calloc(1, sizeof(struct render_data));
	rd->drm_fd = -1;
	rd->av_disabled = true;

	bool has_dmabuf = TEST_2CPP_FORMAT != 0;
	if (has_dmabuf && init_render_data(rd) == -1) {
		has_dmabuf = false;
	}

	bool all_success = true;
	srand(0);
	for (size_t c = 0; c < sizeof(comp_modes) / sizeof(comp_modes[0]);
			c++) {
		for (int gt = 1; gt <= 5; gt++) {
			for (int rt = 1; rt <= 5; rt++) {
				int file_fd = open("test/file",
						O_CREAT | O_RDWR | O_TRUNC,
						0644);
				if (file_fd == -1) {
					wp_error("Failed to create test file: %s",
							strerror(errno));
					continue;
				}
				if (write(file_fd, test_pattern, test_size) !=
						(ssize_t)test_size) {
					wp_error("Failed to write to test file: %s",
							strerror(errno));
					close(file_fd);
					continue;
				}

				bool pass = test_mirror(file_fd, test_size,
						update_file, comp_modes[c], gt,
						rt, rd, &slice_data);

				printf("  FILE comp=%d src_thread=%d dst_thread=%d, %s\n",
						(int)c, gt, rt,
						pass ? "pass" : "FAIL");
				all_success &= pass;

				if (has_dmabuf) {
					struct gbm_bo *bo = make_dmabuf(rd,
							test_size, &slice_data);
					if (!bo) {
						has_dmabuf = false;
						continue;
					}

					void *map_handle = NULL;
					void *data = map_dmabuf(
							bo, true, &map_handle);
					if (!data) {
						destroy_dmabuf(bo);
						has_dmabuf = false;
						continue;
					}
					memcpy(data, test_pattern, test_size);
					unmap_dmabuf(bo, map_handle);

					int dmafd = export_dmabuf(bo);
					if (dmafd == -1) {
						has_dmabuf = false;
						continue;
					}
					destroy_dmabuf(bo);

					bool dpass = test_mirror(dmafd,
							test_size,
							update_dmabuf,
							comp_modes[c], gt, rt,
							rd, &slice_data);

					printf("DMABUF comp=%d src_thread=%d dst_thread=%d, %s\n",
							(int)c, gt, rt,
							dpass ? "pass"
							      : "FAIL");
					all_success &= dpass;
				}
			}
		}
	}

	cleanup_render_data(rd);
	free(rd);
	free(test_pattern);

	printf("All pass: %c\n", all_success ? 'Y' : 'n');
	return all_success ? EXIT_SUCCESS : EXIT_FAILURE;
}
