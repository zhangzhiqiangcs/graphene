/* Copyright (C) 2014 Stony Brook University
   This file is part of Graphene Library OS.

   Graphene Library OS is free software: you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public License
   as published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   Graphene Library OS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/*
 * db_memory.c
 *
 * This files contains APIs that allocate, free or protect virtual memory.
 */

#include "pal_defs.h"
#include "pal_linux_defs.h"
#include "pal.h"
#include "pal_internal.h"
#include "pal_linux.h"
#include "pal_security.h"
#include "pal_error.h"
#include "pal_debug.h"
#include "spinlock.h"
#include "api.h"

#include <asm/mman.h>

#include "enclave_pages.h"

/* TODO: Having VMAs in an array is extremely inefficient */
#define PAL_VMA_MAX 64
static struct pal_vma {
    void * top, * bottom;
} pal_vmas[PAL_VMA_MAX];

static uint32_t pal_nvmas = 0;
static spinlock_t pal_vma_lock = INIT_SPINLOCK_UNLOCKED;

bool _DkCheckMemoryMappable (const void * addr, size_t size)
{
    if (addr < DATA_END && addr + size > TEXT_START) {
        printf("address %p-%p is not mappable\n", addr, addr + size);
        return true;
    }

    spinlock_lock(&pal_vma_lock);

    for (uint32_t i = 0 ; i < pal_nvmas ; i++)
        if (addr < pal_vmas[i].top && addr + size > pal_vmas[i].bottom) {
            spinlock_unlock(&pal_vma_lock);
            printf("address %p-%p is not mappable\n", addr, addr + size);
            return true;
        }

    spinlock_unlock(&pal_vma_lock);
    return false;
}

int _DkVirtualMemoryAlloc (void ** paddr, uint64_t size, int alloc_type, int prot)
{
    if (!WITHIN_MASK(prot, PAL_PROT_MASK))
        return -PAL_ERROR_INVAL;

    void * addr = *paddr, * mem;

    if ((alloc_type & PAL_ALLOC_INTERNAL) && addr)
        return -PAL_ERROR_INVAL;

    if (size == 0)
        __asm__ volatile ("int $3");

    mem = get_reserved_pages(addr, size);
    if (!mem)
        return addr ? -PAL_ERROR_DENIED : -PAL_ERROR_NOMEM;
    if (addr && mem != addr) {
        // TODO: This case should be made impossible by fixing
        // `get_reserved_pages` semantics.
        free_pages(mem, size);
        return -PAL_ERROR_INVAL; // `addr` was unaligned.
    }

    if (alloc_type & PAL_ALLOC_INTERNAL) {
        spinlock_lock(&pal_vma_lock);
        if (pal_nvmas >= PAL_VMA_MAX) {
            spinlock_unlock(&pal_vma_lock);
            SGX_DBG(DBG_E, "Pal is out of VMAs (current limit on VMAs PAL_VMA_MAX = %d)!\n",
                    PAL_VMA_MAX);
            free_pages(mem, size);
            return -PAL_ERROR_NOMEM;
        }

        pal_vmas[pal_nvmas].bottom = mem;
        pal_vmas[pal_nvmas].top = mem + size;
        pal_nvmas++;
        spinlock_unlock(&pal_vma_lock);

        SGX_DBG(DBG_M, "pal allocated %p-%p for internal use\n", mem, mem + size);
    }

    memset(mem, 0, size);

    *paddr = mem;
    return 0;
}

int _DkVirtualMemoryFree (void * addr, uint64_t size)
{
    if (sgx_is_completely_within_enclave(addr, size)) {
        free_pages(addr, size);

        /* check if it is internal PAL memory and remove this VMA from pal_vmas if yes */
        spinlock_lock(&pal_vma_lock);
        for (uint32_t i = 0; i < pal_nvmas; i++) {
            if (addr == pal_vmas[i].bottom) {
                /* TODO: currently assume that internal PAL memory is freed at same granularity as
                 *       was allocated in _DkVirtualMemoryAlloc(); may be false in general case */
                assert(addr + size == pal_vmas[i].top);

                for (uint32_t j = i; j < pal_nvmas - 1; j++) {
                    pal_vmas[j].bottom = pal_vmas[j + 1].bottom;
                    pal_vmas[j].top    = pal_vmas[j + 1].top;
                }

                pal_nvmas--;
                break;
            }
        }
        spinlock_unlock(&pal_vma_lock);
    } else {
        /* Possible to have untrusted mapping. Simply unmap
           the memory outside the enclave */
        ocall_munmap_untrusted(addr, size);
    }
    return 0;
}

int _DkVirtualMemoryProtect (void * addr, uint64_t size, int prot)
{
    static struct atomic_int at_cnt = {.counter = 0};

    if (atomic_cmpxchg(&at_cnt, 0, 1) == 0)
        SGX_DBG(DBG_M, "[Warning] DkVirtualMemoryProtect (0x%p, %lu, %d) is unimplemented",
                addr, size, prot);
    return 0;
}

unsigned long _DkMemoryQuota (void)
{
    return pal_sec.heap_max - pal_sec.heap_min;
}

extern struct atomic_int alloced_pages;
extern unsigned int g_page_size;

unsigned long _DkMemoryAvailableQuota (void)
{
    return (pal_sec.heap_max - pal_sec.heap_min) -
        atomic_read(&alloced_pages) * g_page_size;
}
