/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include <bitmap.h>

#include "devices/disk.h"
#include "threads/vaddr.h"
#include "vm/vm.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in(struct page *page, void *kva);
static bool anon_swap_out(struct page *page);
static void anon_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
    .swap_in = anon_swap_in,
    .swap_out = anon_swap_out,
    .destroy = anon_destroy,
    .type = VM_ANON,
};

static struct bitmap *swap_table;  // swap table, 각 페이지 별 사용 가능한 지 확인용
static struct lock swap_lock;      // swap table 접근 시 동기화를 위해 사용

/* Initialize the data for anonymous pages */
void vm_anon_init(void) {
  /* TODO: Set up the swap_disk. */
  swap_disk = disk_get(1, 1);
  if (!swap_disk) PANIC("SWAP DISK NOT FOUND");
  // swap table도 만들어야 함
  size_t swap_page_cnt =
      disk_size(swap_disk) * DISK_SECTOR_SIZE / PGSIZE;  // disk에 들어갈 총 page 갯수
  swap_table = bitmap_create(swap_page_cnt);
  if (!swap_table) PANIC("CANNOT CREATE SWAP TABLE");  // bitmap 생성 실패 시
  lock_init(&swap_lock);                               // swap table 접근 시 동기화 용 락
}

/* Initialize the file mapping */
bool anon_initializer(struct page *page, enum vm_type type UNUSED, void *kva UNUSED) {
  /* Set up the handler */
  page->operations = &anon_ops;

  struct anon_page *anon_page = &page->anon;
  anon_page->swap_index = -1;  //아직 swap_table에 들어가지 않으니 -1로 초기화

  return true;  //어느 기점에서 false를 반환 시켜야할 지 모르겠다.
}

/* Swap in the page by read contents from the swap disk. */
static bool anon_swap_in(struct page *page, void *kva) {
  struct anon_page *anon_page = &page->anon;
}

/* Swap out the page by writing contents to the swap disk. */
static bool anon_swap_out(struct page *page) { struct anon_page *anon_page = &page->anon; }

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy(struct page *page) { struct anon_page *anon_page = &page->anon; }
