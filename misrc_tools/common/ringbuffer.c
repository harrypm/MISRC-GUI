/*
* MISRC tools
* Copyright (C) 2024-2025  vrunk11, stefan_o
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

#ifdef _WIN32
#include <windows.h>
#else
#include "shm_anon.h"
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#include <mach/mach.h>
/* iOS: no memfd_create/shm_open in the app sandbox (POSIX shared memory
 * is unavailable), so the double-mirror ringbuffer needs a different
 * backing: a Mach memory entry over an anonymous vm_allocate region.
 * The entry maps the SAME pages at both mirror addresses — identical
 * semantics to the memfd double-map, entirely in-RAM. */
#define RB_IOS_MACH_ENTRY 1
#endif
#endif
#endif
#include "ringbuffer.h"
#include <string.h>


int rb_init(ringbuffer_t *rb, char *name, size_t size) {

#ifdef _WIN32

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#define nullptr NULL
#endif

	SYSTEM_INFO sysInfo;
	HANDLE h = nullptr;
	void* maparea = nullptr;

	// First, make sure the size is a multiple of the page size
	GetSystemInfo (&sysInfo);
	if((size % sysInfo.dwAllocationGranularity) != 0) {
		return 1;
	}

	if((maparea = (PCHAR)VirtualAlloc2(nullptr, nullptr, 2*size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0)) == nullptr) {
		return 2;
	}

	if(VirtualFree(maparea, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == FALSE) {
		return 3;
	}

	if((h = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, nullptr)) == nullptr) {
		VirtualFree(maparea, 0, MEM_RELEASE);
		VirtualFree(maparea+size, 0, MEM_RELEASE);
		return 4;
	} 

	if((rb->buffer = MapViewOfFile3(h, nullptr, maparea, 0, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) == nullptr) {
		CloseHandle(h);
		VirtualFree(maparea, 0, MEM_RELEASE);
		VirtualFree(maparea+size, 0, MEM_RELEASE);
		return 5;
	}

	if((rb->_buffer2 = MapViewOfFile3(h, nullptr, maparea+size, 0, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) == nullptr) {
		CloseHandle(h);
		VirtualFree(maparea+size, 0, MEM_RELEASE);
		UnmapViewOfFileEx(rb->buffer, 0);
		return 6;
	}
	CloseHandle(h);
#else
	// First, make sure the size is a multiple of the page size
	long page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		page_size = 4096;
	}
	if(size % (size_t)page_size != 0) {
		return 1;
	}

#if defined(RB_IOS_MACH_ENTRY)
	/* iOS path: vm_allocate the real storage, wrap it in a Mach memory
	 * entry, and map that entry twice into a reserved 2x range — the
	 * mirror trick without POSIX shm. */
	task_t task = mach_task_self();
	vm_address_t storage = 0;
	kern_return_t kr = vm_allocate(task, &storage, size, VM_FLAGS_ANYWHERE);
	if(kr != KERN_SUCCESS) {
		return 2;
	}
	memory_object_size_t entry_size = size;
	mach_port_t entry = MACH_PORT_NULL;
	kr = mach_make_memory_entry_64(task, &entry_size, (memory_object_offset_t)storage,
	                               VM_PROT_READ | VM_PROT_WRITE, &entry, MACH_PORT_NULL);
	if(kr != KERN_SUCCESS || entry_size != size) {
		vm_deallocate(task, storage, size);
		return 3;
	}
	void *reserve = mmap(NULL, 2 * size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(reserve == MAP_FAILED) {
		mach_port_deallocate(task, entry);
		vm_deallocate(task, storage, size);
		return 4;
	}
	vm_address_t mirror1 = (vm_address_t)reserve;
	kr = vm_map(task, &mirror1, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
	            entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
	            VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_DEFAULT);
	if(kr != KERN_SUCCESS) {
		munmap(reserve, 2 * size);
		mach_port_deallocate(task, entry);
		vm_deallocate(task, storage, size);
		return 5;
	}
	vm_address_t mirror2 = (vm_address_t)reserve + size;
	kr = vm_map(task, &mirror2, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
	            entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
	            VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_DEFAULT);
	if(kr != KERN_SUCCESS) {
		munmap(reserve, size);
		munmap((char *)reserve + size, size);
		mach_port_deallocate(task, entry);
		vm_deallocate(task, storage, size);
		return 6;
	}
	/* The two mappings hold the pages; the entry handle is not needed. */
	mach_port_deallocate(task, entry);
	rb->buffer = (uint8_t *)reserve;
	rb->_storage = (void *)(uintptr_t)storage;
	rb->fd = -1;
#else
	// Make an anonymous file and set its size
	if((rb->fd = memfd_create(name, 0)) == -1) {
		return 2;
	}

	if(ftruncate(rb->fd, size) == -1) {
		return 3;
	}

	// Ask mmap for an address at a location where we can put both virtual copies of the buffer
	if((rb->buffer = mmap(NULL, 2 * size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) == MAP_FAILED) {
		return 4;
	}

	// Map the buffer at that address
	if(mmap(rb->buffer, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, rb->fd, 0) == MAP_FAILED)  {
		return 5;
	}

	// Now map it again, in the next virtual page
	if(mmap(rb->buffer + size, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, rb->fd, 0) == MAP_FAILED)  {
		return 6;
	}
#endif /* RB_IOS_MACH_ENTRY */
#endif /* _WIN32 */

	// Initialize our buffer indices
	rb->buffer_size = size;
	rb->head = 0;
	rb->tail = 0;
	return 0;
}

int rb_put(ringbuffer_t *rb, void *data, size_t size) {
	size_t head = atomic_load(&rb->head);
	size_t tail = atomic_load(&rb->tail);
	size_t map_size = rb->buffer_size * 2;
	if(tail < head) {
		return 1;
	}
	if(rb->buffer_size - (tail - head) < size) {
		return 1;
	}
	if(tail >= map_size || size > (map_size - tail)) {
		return 1;
	}
	memcpy(&rb->buffer[tail], data, size);
	rb->tail += size;
	return 0;
}

void* rb_write_ptr(ringbuffer_t *rb, size_t size) {
	size_t head = atomic_load(&rb->head);
	size_t tail = atomic_load(&rb->tail);
	size_t map_size = rb->buffer_size * 2;
	if(tail < head) {
		return NULL;
	}
	if(rb->buffer_size - (tail - head) < size) {
		return NULL;
	}
	if(tail >= map_size || size > (map_size - tail)) {
		return NULL;
	}
	return &rb->buffer[tail];
}

int rb_write_finished(ringbuffer_t *rb, size_t size) {
	size_t head = atomic_load(&rb->head);
	size_t tail = atomic_load(&rb->tail);
	size_t map_size = rb->buffer_size * 2;
	if(tail < head) {
		return 1;
	}
	if(rb->buffer_size - (tail - head) < size) {
		return 1;
	}
	if(tail >= map_size || size > (map_size - tail)) {
		return 1;
	}
	rb->tail += size;
	return 0;
}

void* rb_read_ptr(ringbuffer_t *rb, size_t size) {
	size_t head = atomic_load(&rb->head);
	size_t tail = atomic_load(&rb->tail);
	size_t map_size = rb->buffer_size * 2;
	if(tail < head){
		return NULL;
	}
	if(tail - head < size){
		return NULL;
	}
	if(head >= map_size || size > (map_size - head)) {
		return NULL;
	}
	return &rb->buffer[head];
}

int rb_read_finished(ringbuffer_t *rb, size_t size) {
	if(rb->tail - rb->head < size){
		return 1;
	}
	rb->head += size;
	if(rb->head >= rb->buffer_size) {
		rb->head -= rb->buffer_size;
		rb->tail -= rb->buffer_size;
	}
	return 0;
}

void rb_close(ringbuffer_t *rb) {
#ifdef _WIN32
	UnmapViewOfFile(rb->buffer);
	UnmapViewOfFile(rb->_buffer2);
#else
	munmap(rb->buffer, rb->buffer_size);
	munmap(rb->buffer+rb->buffer_size, rb->buffer_size);
#if defined(RB_IOS_MACH_ENTRY)
	/* Release the vm_allocate storage (the two mirror mappings are gone;
	 * without this the backing pages would leak). */
	if(rb->_storage) {
		vm_deallocate(mach_task_self(),
		              (vm_address_t)(uintptr_t)rb->_storage, rb->buffer_size);
		rb->_storage = NULL;
	}
#elif defined(__APPLE__)
    close(rb->fd); /* release anonymous backing object after both mappings */
    rb->fd = -1;
#endif
#endif
}
