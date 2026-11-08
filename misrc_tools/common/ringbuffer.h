/*
* MISRC tools
* Copyright (C) 2024  vrunk11, stefan_o
* 
* based on:
* http://web.archive.org/web/20171026083549/https://lo.calho.st/quick-hacks/employing-black-magic-in-the-linux-page-table/
* Copyright (C) 2017 by Travis Mick
* 
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef RINGBUFFER_H
#define RINGBUFFER_H

/* TEMPORARY (iOS alloc diagnosis 2026-11-08): rb_init failure logging to
 * the app's Documents dir; pulled over devicectl. */
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#include <stdio.h>
#include <stdlib.h>
#define RB_LOG_FAIL(code_, sz_) do { \
    const char *h_ = getenv("HOME"); \
    if (h_) { \
        char p_[1024]; \
        snprintf(p_, sizeof(p_), "%s/Documents/misrc_rb.log", h_); \
        FILE *f_ = fopen(p_, "a"); \
        if (f_) { fprintf(f_, "rb_init FAIL code=%d size=%zu (%.1f MB)\n", \
                          (code_), (size_t)(sz_), (double)(sz_)/1048576.0); \
                  fclose(f_); } \
    } \
} while(0)
#endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

typedef struct {
	uint8_t      *buffer;
#ifdef _WIN32
	uint8_t      *_buffer2;
#endif
#if defined(__APPLE__) && !defined(_WIN32)
	/* iOS: the backing is a vm_allocate region (no memfd/shm in the iOS
	 * sandbox); the entry is mapped twice for the mirror. rb_close needs
	 * the storage address to vm_deallocate it. */
	void         *_storage;
#endif
	size_t        buffer_size;
	int           fd;
	atomic_size_t head;
	atomic_size_t tail;
} ringbuffer_t;

int   rb_init(ringbuffer_t *rb, char *name, size_t size);
int   rb_put(ringbuffer_t *rb, void *data, size_t size);
void* rb_read_ptr(ringbuffer_t *rb, size_t size);
int   rb_read_finished(ringbuffer_t *rb, size_t size);
void* rb_write_ptr(ringbuffer_t *rb, size_t size);
int   rb_write_finished(ringbuffer_t *rb, size_t size);
void  rb_close(ringbuffer_t *rb);

#endif // RINGBUFFER_H