#ifdef WARDEN_TEST
#include "ktest.h"
#include <abi/errno.h>
#include "cpu.h"
#include "kstring.h"
#include "pmm.h"
#include "slab.h"
#include "vmm.h"

/* The real memory map: frames come back usable through the HHDM and
 * every one of them is returned. */
int test_pmm_real_memory(void) {
    size_t before = pmm_free_count();
    KASSERT(before > 0);

    uint64_t frames[16];
    for (int i = 0; i < 16; i++) {
        frames[i] = pmm_alloc();
        KASSERT(frames[i] != 0);
        KASSERT(frames[i] % PAGE_SIZE == 0);
        uint64_t *p = phys_to_virt(frames[i]);
        for (size_t j = 0; j < PAGE_SIZE / 8; j++)
            p[j] = frames[i] ^ j;
    }
    for (int i = 0; i < 16; i++) {
        uint64_t *p = phys_to_virt(frames[i]);
        for (size_t j = 0; j < PAGE_SIZE / 8; j++)
            KASSERT(p[j] == (frames[i] ^ j)); /* no two frames overlap */
    }
    KASSERT(pmm_free_count() == before - 16);

    for (int i = 0; i < 16; i++)
        KASSERT(pmm_free(frames[i]) == 0);
    KASSERT(pmm_free(frames[0]) != 0); /* double free is caught */
    KASSERT(pmm_free_count() == before);
    return 0;
}

int test_vmm_active(void) {
    KASSERT(read_cr3() == vmm_kernel_pml4()); /* off Limine's tables */
    KASSERT(rdmsr(MSR_EFER) & EFER_NXE);
    KASSERT(read_cr0() & CR0_WP);
    return 0;
}

int test_vmm_map(void) {
    uint64_t frame = pmm_alloc();
    KASSERT(frame != 0);
    KASSERT(vmm_map(TEST_VIRT, frame, PTE_W | PTE_NX) == 0);
    KASSERT(vmm_map(TEST_VIRT, frame, PTE_W | PTE_NX) == -EEXIST);

    /* the same frame seen through two mappings */
    volatile uint64_t *v = (volatile uint64_t *)TEST_VIRT;
    volatile uint64_t *h = phys_to_virt(frame);
    *v = 0x1234567890abcdefUL;
    KASSERT(*h == 0x1234567890abcdefUL);
    *h = 42;
    KASSERT(*v == 42);

    KASSERT(vmm_unmap(TEST_VIRT) == frame);
    KASSERT(pmm_free(frame) == 0);
    return 0;
}

/* Every free frame, reclaimed bootloader memory included, is reachable
 * and writable through our HHDM. Frames are chained through their first
 * word, so this needs no extra memory. */
int test_pmm_all_frames(void) {
    size_t before = pmm_free_count();
    uint64_t head = 0;
    size_t n = 0;
    uint64_t f;
    while ((f = pmm_alloc()) != 0) {
        uint64_t *p = phys_to_virt(f);
        p[0] = head;
        p[PAGE_SIZE / 8 - 1] = ~f; /* touch both ends of the page */
        head = f;
        n++;
    }
    KASSERT(n == before);
    while (head != 0) {
        uint64_t *p = phys_to_virt(head);
        uint64_t next = p[0];
        KASSERT(p[PAGE_SIZE / 8 - 1] == ~head);
        KASSERT(pmm_free(head) == 0);
        head = next;
    }
    KASSERT(pmm_free_count() == before);
    return 0;
}

int test_kmalloc_kernel(void) {
    size_t before = pmm_free_count();

    /* small: slab objects in the HHDM, usable and distinct */
    uint64_t *a = kmalloc(sizeof(uint64_t) * 4);
    uint64_t *b = kzalloc(100);
    KASSERT(a != NULL && b != NULL && a != (void *)b);
    KASSERT(virt_to_page(a) != NULL);
    KASSERT(virt_to_page(a)->flags & PG_SLAB);
    for (int i = 0; i < 4; i++)
        a[i] = 0xa0a0 + i;
    for (size_t i = 0; i < 100 / 8; i++)
        KASSERT(b[i] == 0);
    KASSERT(a[3] == 0xa0a3);

    /* large: contiguous pages */
    uint8_t *big = kmalloc(5 * PAGE_SIZE);
    KASSERT(big != NULL && ksize(big) == 5 * PAGE_SIZE);
    memset(big, 0x5c, 5 * PAGE_SIZE);
    KASSERT(big[5 * PAGE_SIZE - 1] == 0x5c);

    kfree(a);
    kfree(b);
    kfree(big);
    kmalloc_shrink();
    KASSERT(pmm_free_count() == before);
    return 0;
}

/* Many objects of every class at once, then all freed. */
int test_kmalloc_churn(void) {
    size_t before = pmm_free_count();
    static void *p[1024];
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 1024; i++) {
            size_t size = (size_t)16 << (i % 8);
            p[i] = kmalloc(size);
            KASSERT(p[i] != NULL);
            memset(p[i], i & 0xff, size);
        }
        for (int i = 0; i < 1024; i++) {
            uint8_t *q = p[i];
            size_t size = (size_t)16 << (i % 8);
            KASSERT(q[0] == (i & 0xff) && q[size - 1] == (i & 0xff));
            kfree(q);
        }
    }
    kmalloc_shrink();
    KASSERT(pmm_free_count() == before);
    return 0;
}

#endif /* ifdef WARDEN_TEST */
