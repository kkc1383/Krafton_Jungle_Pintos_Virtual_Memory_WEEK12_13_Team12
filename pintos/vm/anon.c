/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include <bitmap.h>

#include "devices/disk.h"
#include "threads/mmu.h"
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

int *swap_table;  // swap table, 각 페이지 별 사용 가능한 지 확인용
static size_t swap_slot_cnt; //총 swap slot 갯수
struct lock swap_lock;      // swap table 접근 시 동기화를 위해 사용

/* Initialize the data for anonymous pages */
void vm_anon_init(void) {
  /* TODO: Set up the swap_disk. */
  swap_disk = disk_get(1, 1);
  if (!swap_disk) PANIC("SWAP DISK NOT FOUND");
  // swap table도 만들어야 함
  swap_slot_cnt = disk_size(swap_disk) * DISK_SECTOR_SIZE / PGSIZE;  // disk에 들어갈 총 page 갯수
  swap_table = calloc(swap_slot_cnt,sizeof(int));
  if (!swap_table) PANIC("CANNOT CREATE SWAP TABLE");  // bitmap 생성 실패 시
  lock_init(&swap_lock);                               // swap table 접근 시 동기화 용 락
}

/* Initialize the file mapping */
bool anon_initializer(struct page *page, enum vm_type type, void *kva UNUSED) {
  /* Set up the handler */
  page->operations = &anon_ops;

  struct anon_page *anon_page = &page->anon;
  anon_page->swap_index = -1;  //아직 swap_table에 들어가지 않으니 -1로 초기화
  anon_page->is_stack=type&VM_MARKER_0; //스택인지 아닌지 확인
  return true;  //어느 기점에서 false를 반환 시켜야할 지 모르겠다.
}

/* Swap in the page by read contents from the swap disk. */
static bool anon_swap_in(struct page *page, void *kva) {
  struct anon_page *anon_page = &page->anon;

  // swap_index 확인
  if (anon_page->swap_index == -1) {
    return false;  // swap_out 된 적 없음
  }

  // disk에서 페이지 읽기(8 sectors)
  size_t slot = anon_page->swap_index;
  for (int i = 0; i < 8; i++) {
    disk_read(swap_disk, slot * 8 + i, kva + i * DISK_SECTOR_SIZE);
  }
  // swap table에서 slot해제
  lock_acquire(&swap_lock);
  swap_table[slot]--;

  if(swap_table[slot]==0){
    //slot이 완전히 해제됨
  }
  lock_release(&swap_lock);

  // swap_index 초기화
  anon_page->swap_index = -1;

  return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool anon_swap_out(struct page *page) {
  struct anon_page *anon_page = &page->anon;

  // swap table에서 빈 slot 찾기
  lock_acquire(&swap_lock);
  size_t slot = BITMAP_ERROR; //SIZE_MAX와 같음
  for(size_t i=0;i<swap_slot_cnt;i++){
    if(swap_table[i]==0){
      slot=i;
      swap_table[i]=1;// 참조 카운트 1로 설정
      break;
    }
  }

  if (slot == BITMAP_ERROR) {
    lock_release(&swap_lock);
    return false;  // swap disk가 가득 참
  }
  lock_release(&swap_lock);

  // disk에 페이지 쓰기( 한 페이지 = 8 sector)
  for (int i = 0; i < 8; i++) {
    disk_write(swap_disk, slot * 8 + i, page->frame->kva + i * DISK_SECTOR_SIZE);
  }
  // swap_index 저장
  anon_page->swap_index = slot;

  //페이지 테이블에서 매핑 제거
  pml4_clear_page(thread_current()->pml4, page->va);

  // frame 연결 해제
  page->frame = NULL;

  return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy(struct page *page) {
  struct anon_page *anon_page = &page->anon;
  if (page->frame) {
    struct frame *frame=page->frame;

    //참조 카운터 감소
    lock_acquire(&frame->lock);
    frame->ref_count--;
    int ref_count=frame->ref_count;
    lock_release(&frame->lock);

    //참조 카운터가 0이면 프레임 해제
    if(ref_count==0){
      //frame table에서 제거
      lock_acquire(&frame_table_lock);
      list_remove(&frame->elem);
      lock_release(&frame_table_lock);

      //물리 메모리 해제
      palloc_free_page(frame->kva);
      free(frame);
    }

    //페이지 테이블에서 매핑 제거
    pml4_clear_page(thread_current()->pml4,page->va);
  }
  //swap out 되어 있다면 swap slot 해제
  if (anon_page->swap_index != -1) {
    lock_acquire(&swap_lock);
    swap_table[anon_page->swap_index]--;
    //참조 카운트가 0이 되면 자동으로 해제됨 (다음 swap_out에서 재사용 가능)
    lock_release(&swap_lock);
  }
}
