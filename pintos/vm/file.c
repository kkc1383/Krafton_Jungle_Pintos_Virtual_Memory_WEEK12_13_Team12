/* file.c: Implementation of memory backed file object (mmaped object). */

#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "vm/vm.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

//헬퍼 함수
static void cleanup_mmap_pages(void *start_addr, void *end_addr, struct mmap_file *mmap, struct file *file);
static bool mmap_file_load(struct page *page, void *aux);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
    .swap_in = file_backed_swap_in,
    .swap_out = file_backed_swap_out,
    .destroy = file_backed_destroy,
    .type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void) {}

/* Initialize the file backed page */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva) {
  /* Set up the handler */
  page->operations = &file_ops;
  struct file_page *file_page = &page->file;

  /* uninit->aux에서 lazy_load_arg를 가져와서 file_page에 복사 */
  struct lazy_load_arg *aux = (struct lazy_load_arg *)page->uninit.aux;

  file_page->file = aux->file;
  file_page->ofs = aux->ofs;
  file_page->read_bytes = aux->read_bytes;
  file_page->zero_bytes = aux->zero_bytes;
  file_page->mmap = aux->mmap;  //이건 do_mmap에서 설정

  return true;
}

/* Swap in the page by read contents from the file. */
static bool file_backed_swap_in(struct page *page, void *kva) {
  struct file_page *file_page = &page->file;

  //파일의 offset 위치로 이동
  file_seek(file_page->file, file_page->ofs);

  // read_bytes만큼 파일에서 kva로 읽기
  if (file_read(file_page->file, kva, file_page->read_bytes) != (int)file_page->read_bytes) {
    return false;  //읽기 실패
  }
  //나머지 부분은 0으로 채우기
  memset(kva + file_page->read_bytes, 0, file_page->zero_bytes);

  return true;
}

/* Swap out the page by writeback contents to the file. */
static bool file_backed_swap_out(struct page *page) {
  struct file_page *file_page = &page->file;
  if (pml4_is_dirty(thread_current()->pml4, page->va) && page->frame) {
    //파일에 데이터 쓰기
    file_write_at(file_page->file, page->frame->kva, file_page->read_bytes, file_page->ofs);

    // dirty bit 클리어
    pml4_set_dirty(thread_current()->pml4, page->va, false);
  }
  // present bit 클리어 (페이지 테이블에서 매핑 제거)
  pml4_clear_page(thread_current()->pml4, page->va);

  // frame 연결 해제
  page->frame = NULL;  //물리 메모리는 재사용할 수 있으므로 연결만 끊기
  return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page *page) {
  struct file_page *file_page = &page->file;

  // dirty bit 확인 후 write back
  if (pml4_is_dirty(thread_current()->pml4, page->va) && page->frame) {
    file_write_at(file_page->file, page->frame->kva, file_page->read_bytes, file_page->ofs);
  }
  //페이지 테이블에서 매핑 제거
  pml4_clear_page(thread_current()->pml4, page->va);
  if (page->frame != NULL) {
    //물리 메모리 해제
    palloc_free_page(page->frame->kva);
    free(page->frame);
  }
}

/* Do the mmap */
void *do_mmap(void *addr, size_t length, int writable, struct file *file, off_t offset) {
  // 예외 사항 처리
  if (addr == NULL || addr != pg_round_down(addr))  // addr이 NULL이거나 페이지 정렬이 되어있지 않다면 실패
    return NULL;
  if (length == 0)  // 파일 길이가 0이면 실패
    return NULL;
  if (offset != pg_round_down(offset))  // offset이 페이지 정렬이 되어있지 않으면 실패
    return NULL;
  if (file == NULL)  // 파일이 없으면 실패
    return NULL;
  // 파일 재오픈(독립적인 offset 유지)
  struct file *reopend_file = file_reopen(file);
  if (reopend_file == NULL) return NULL;

  //파일 길이 확인
  off_t file_len = file_length(reopend_file);
  if (file_len == 0 || offset >= file_len) {  //전체 파일길이가 0이거나 offset보다 작다면 실패
    file_close(reopend_file);
    return NULL;
  }

  // mmap_file 구조체 할당
  struct mmap_file *mmap = malloc(sizeof(struct mmap_file));
  if (mmap == NULL) {
    file_close(reopend_file);
    return NULL;
  }
  // mmap_file 구조체 필드 초기화
  mmap->addr = addr;
  mmap->file = reopend_file;
  mmap->length = length;
  mmap->page_cnt = 0;

  //페이지 단위로 lazy_loading 설정
  //읽을 수 있는 남은 파일 길이가 length보다 작다면 남은 만큼만
  size_t read_bytes = length < file_len - offset ? length : file_len - offset;
  size_t zero_bytes = length - read_bytes;

  void *upage = addr;
  off_t ofs = offset;

  while (read_bytes > 0 || zero_bytes > 0) {
    //이미 매핑된 페이지인지 체크
    if (spt_find_page(&thread_current()->spt, upage) != NULL) {
      //실패, 이후 정리 필요
      cleanup_mmap_pages(addr, upage, mmap, reopend_file);
      return NULL;
    }
    // 페이지 크기 이하만 읽을 수 있음
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    // lazy_load_arg 할당
    struct lazy_load_arg *aux = malloc(sizeof(struct lazy_load_arg));
    if (aux == NULL) {
      //정리 필요
      cleanup_mmap_pages(addr, upage, mmap, reopend_file);
      return NULL;
    }
    aux->file = reopend_file;
    aux->ofs = ofs;
    aux->read_bytes = page_read_bytes;
    aux->zero_bytes = page_zero_bytes;
    aux->mmap = mmap;

    // VM_FILE 페이지 생성
    if (!vm_alloc_page_with_initializer(VM_FILE, upage, writable, mmap_file_load, aux)) {
      cleanup_mmap_pages(addr, upage, mmap, reopend_file);
      free(aux);
      return NULL;
    }
    mmap->page_cnt++;

    read_bytes -= page_read_bytes;
    zero_bytes = (zero_bytes < page_zero_bytes) ? 0 : zero_bytes - page_zero_bytes;
    upage += PGSIZE;
    ofs += page_read_bytes;
  }
  // thread의 mmap_list에 추가
  list_push_back(&thread_current()->mmap_list, &mmap->elem);
  return addr;
}
static bool mmap_file_load(struct page *page, void *aux) {
  struct lazy_load_arg *lla_aux = (struct lazy_load_arg *)aux;  //포인터 형 맞춰 주고

  // file에서 필요한 만큼만 읽는다. 읽어야 할 만큼 못 읽었으면 실패
  file_seek(lla_aux->file, lla_aux->ofs);  // offset 설정
  if (file_read(lla_aux->file, page->frame->kva, lla_aux->read_bytes) != (uint32_t)lla_aux->read_bytes) {
    free(aux);     // aux 구조체 반환
    return false;  // 실패했음을 알림
  }
  // page단위 이므로 남는 부분을 0으로 채움
  memset(page->frame->kva + lla_aux->read_bytes, 0, lla_aux->zero_bytes);

  // 할거 다 했으니 aux 반납
  free(lla_aux);
  return true;
}
static void cleanup_mmap_pages(void *start_addr, void *end_addr, struct mmap_file *mmap, struct file *file) {
  void *cleanup_addr = start_addr;
  while (cleanup_addr < end_addr) {
    struct page *p = spt_find_page(&thread_current()->spt, cleanup_addr);
    if (p) spt_remove_page(&thread_current()->spt, p);
    cleanup_addr += PGSIZE;
  }
  free(mmap);
  file_close(file);
}
/* Do the munmap */
void do_munmap(void *addr) {
  struct thread *curr = thread_current();

  // mmap_list에서 해당 주소의 mmap_file 찾기
  struct list_elem *e;
  struct mmap_file *mmap = NULL;
  for (e = list_begin(&curr->mmap_list); e != list_end(&curr->mmap_list); e = list_next(e)) {
    struct mmap_file *m = list_entry(e, struct mmap_file, elem);
    if (m->addr == addr) {
      mmap = m;
      break;
    }
  }
  if (mmap == NULL) return;

  //각 페이지에 대해 처리
  for (void *va = mmap->addr; va < mmap->addr + mmap->length; va += PGSIZE) {
    struct page *page = spt_find_page(&curr->spt, va);
    if (page == NULL) continue;

    if (VM_TYPE(page->operations->type) == VM_UNINIT) {  // uninit 페이지일 경우 spt에서만 제거
      spt_remove_page(&curr->spt, page);
      continue;
    }
    // dirty면 파일에 write back
    if (pml4_is_dirty(curr->pml4, va) && page->frame) {
      file_write_at(mmap->file, page->frame->kva, page->file.read_bytes, page->file.ofs);
    }
    // spt에서 페이지 제거
    spt_remove_page(&curr->spt, page);
  }
  // mmap_file을 리스트에서 제거
  list_remove(&mmap->elem);

  //파일 닫기 및 메모리 해제
  file_close(mmap->file);
  free(mmap);
}
