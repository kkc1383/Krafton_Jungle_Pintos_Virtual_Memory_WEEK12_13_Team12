/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"

#include <string.h>

#include "include/threads/vaddr.h"
#include "lib/kernel/hash.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "userprog/process.h"
#include "vm/inspect.h"
#include "threads/thread.h"

static struct list frame_table;       //모든 frame들의 리스트
struct lock frame_table_lock;  // frame_table 동기화용 (COW : static 제거, extern 접근 목적)
static struct list_elem *clock_hand;  // Clock algorithm용 포인터

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void) {
  vm_anon_init();
  vm_file_init();
#ifdef EFILESYS /* For project 4 */
  pagecache_init();
#endif
  register_inspect_intr();
  /* DO NOT MODIFY UPPER LINES. */
  /* TODO: Your code goes here. */
  list_init(&frame_table);
  lock_init(&frame_table_lock);
  clock_hand = NULL;
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page *page) {
  int ty = VM_TYPE(page->operations->type);
  switch (ty) {
    case VM_UNINIT:
      return VM_TYPE(page->uninit.type);
    default:
      return ty;
  }
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);
static void spt_destructor(struct hash_elem *e, void *aux);
static void spt_copy_page(struct hash_elem *h, void *aux UNUSED);
static bool vm_stack_growth(void *addr);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable, vm_initializer *init, void *aux) {
  ASSERT(VM_TYPE(type) != VM_UNINIT);

  struct supplemental_page_table *spt = &thread_current()->spt;

  /* Check wheter the upage is already occupied or not. */
  if (spt_find_page(spt, upage) == NULL) {  //일단 새로 만들 페이지인데, spt에 이미 있어서는 안됨.
    /* TODO: Create the page, fetch the initialier according to the VM type,
     * TODO: and then create "uninit" page struct by calling uninit_new. You
     * TODO: should modify the field after calling the uni3nit_new. */

    struct page *page = (struct page *)malloc(sizeof(struct page));  // page 크기만큼만 할당하면 된다.
    if (!page) goto err;

    // page 필드 채우는건 아래 uninit_new에서 해줌
    switch (VM_TYPE(type)) {  // type에 맞게 uninit 페이지를 생성
      case VM_ANON:
        uninit_new(page, upage, init, type, aux, anon_initializer);
        break;
      case VM_FILE:
        uninit_new(page, upage, init, type, aux, file_backed_initializer);
        break;
      default:
        goto err;
    }
    page->writable = writable;  // writable 필드 채우기
    page->is_cow=false;

    /* TODO: Insert the page into the spt. */
    if (!spt_insert_page(spt, page)) {  // (디버깅 추가됨)
      free(page);
      goto err;
    }
    return true;  // (디버깅 추가됨)
  }
err:
  return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *spt_find_page(struct supplemental_page_table *spt, void *va) {
  /* TODO: Fill this function. */
  if (!is_user_vaddr(va)) return NULL;

  struct page key;

  key.va = pg_round_down(va);
  struct hash_elem *he = hash_find(&spt->hash_table, &key.hash_elem);
  return (he != NULL) ? hash_entry(he, struct page, hash_elem) : NULL;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt UNUSED, struct page *page UNUSED) {
  bool succ = false;
  /* TODO: Fill this function. */
  ASSERT(spt != NULL && page != NULL);
  ASSERT(page->va == pg_round_down(page->va));

  succ = (hash_insert(&spt->hash_table, &page->hash_elem) == NULL);
  return succ;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page) {
  hash_delete(&spt->hash_table, &page->hash_elem);  // hash에서 제거
  vm_dealloc_page(page);                            // page 타입에 맞게 destroy 후 free
  
}

/* Get the struct frame, that will be evicted. */
static struct frame *vm_get_victim(void) {
  /* TODO: The policy for eviction is up to you. */
  lock_acquire(&frame_table_lock);

  if (list_empty(&frame_table)) {
    lock_release(&frame_table_lock);
    return NULL;
  }

  // clock algorithm(second chance)
  if (clock_hand == NULL || clock_hand == list_end(&frame_table)) {
    clock_hand = list_begin(&frame_table);
  }
  struct frame *victim = NULL;
  struct list_elem *start = clock_hand;

  while (true) {
    struct frame *f = list_entry(clock_hand, struct frame, elem);

    if (f->page == NULL) {
      //페이지가 없는 frame은 스킵
      clock_hand = list_next(clock_hand);
      if (clock_hand == list_end(&frame_table)) {
        clock_hand = list_begin(&frame_table);
      }
      continue;
    }

    // accessed bit 확인
    if (pml4_is_accessed(thread_current()->pml4, f->page->va)) {
      // accessed bit이 1이면 0으로 바꾸고 다음으로
      pml4_set_accessed(thread_current()->pml4, f->page->va, false);
      clock_hand = list_next(clock_hand);
      if (clock_hand == list_end(&frame_table)) {
        clock_hand = list_begin(&frame_table);
      }
    } else {
      // accessed bit이 0이면 victim 선정
      victim = f;
      clock_hand = list_next(clock_hand);
      break;
    }
    //한 바퀴 돌았으면 첫 번째 것 선택
    if (clock_hand == start) {
      victim = f;
      break;
    }
  }
  lock_release(&frame_table_lock);
  return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame(void) {
  struct frame *victim = vm_get_victim();
  /* TODO: swap out the victim and return the evicted frame. */
  if (victim == NULL) {
    return NULL;
  }
  struct page *page = victim->page;

  // swap out 호출
  if (!swap_out(page)) {
    return NULL;  // swap_out 실패
  }
  // frame과 page 연결 해제(swap_out에서 이미 했지만 확인)
  victim->page = NULL;
  return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *vm_get_frame(void) {
  struct frame *frame = NULL;
  /* TODO: Fill this function. */

  int8_t *kaddr = palloc_get_page(PAL_USER);
  if (kaddr == NULL) {
    frame = vm_evict_frame();  // evict 하고 frame 재사용
    if (frame == NULL) {
      PANIC("vm_get_frame: eviction failed");
    }
    return frame;
  }

  if ((frame = malloc(sizeof *frame)) == NULL) {
    palloc_free_page(kaddr);
    PANIC("vm_get_frame:  malloc(sizeof *frame) failed");
  }

  frame->kva = kaddr;
  frame->page = NULL;

  /* cow용 추가 */
  frame->ref_count=1;
  lock_init(&frame->lock);

  // frame_table에 추가
  lock_acquire(&frame_table_lock);
  list_push_back(&frame_table, &frame->elem);
  lock_release(&frame_table_lock);

  ASSERT(frame != NULL);
  ASSERT(frame->page == NULL);
  return frame;
}

/* Growing the stack. */
static bool vm_stack_growth(void *addr) {
  void *stack_bottom = pg_round_down(addr);
  return vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, true);
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page) {
  struct frame *old_frame=page->frame;

  //참조 카운터 확인
  lock_acquire(&old_frame->lock);
  int ref_count=old_frame->ref_count;

  //마지막 참조자라면 복사 불필요
  if(ref_count==1){
    page->is_cow=false;
    pml4_clear_page(thread_current()->pml4,page->va);
    bool result= pml4_set_page(thread_current()->pml4,page->va,old_frame->kva,page->writable);
    lock_release(&old_frame->lock);
    return result;
  }
  lock_release(&old_frame->lock);

  // 참조자가 아직 있다면
  // 새 프레임 할당
  struct frame *new_frame=vm_get_frame();
  if(!new_frame) return false;

  //기존 프레임에서 데이터 복사
  memcpy(new_frame->kva,old_frame->kva,PGSIZE);

  //페이지를 새 프레임에 연결
  new_frame->page=page;
  page->frame=new_frame;

  //페이지 테이블 업데이트 (쓰기 가능으로)
  pml4_clear_page(thread_current()->pml4,page->va); 
  if(!pml4_set_page(thread_current()->pml4,page->va,new_frame->kva,page->writable))
    return false;

  //COW 플래그 해제
  page->is_cow=false;

  //기존 프레임의 참조 카운터 감소
  lock_acquire(&old_frame->lock);
  old_frame->ref_count--;
  ref_count=old_frame->ref_count;
  lock_release(&old_frame->lock);

  if(ref_count==0){
    lock_acquire(&frame_table_lock);
    list_remove(&old_frame->elem);
    lock_release(&frame_table_lock);

    palloc_free_page(old_frame->kva);
    free(old_frame);
  }
  return true;
}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f , void *addr, bool user UNUSED, bool write,
                         bool not_present ) {
  struct supplemental_page_table *spt  = &thread_current()->spt;
  struct page *page = NULL;
  /* TODO: Validate the fault */
  /* TODO: Your code goes here */

  page = spt_find_page(spt, addr);  // 알아서 pg_round_down 해줌
  if (!page) {                      // spt에 페이지가 없을 때
    // 메모리에 매핑되어 있지 않다면 (not_present가 false라면 readonly 페이지에 쓰려고 할때)
    // addr이 스택 성장인지 범위 확인
    if (addr > USER_STACK || addr < USER_STACK - (1 << 20)) return false;
    void *rsp = user ? f->rsp : thread_current()->rsp;
    if (addr < rsp && addr != rsp - 8) return false;
    if (vm_stack_growth(addr)) {
      page = spt_find_page(spt, addr);  // 알아서 pg_round_down 해줌
    } else
      return false;
  } else {                        // page!=NULL 일 때
    if (!not_present && write) {  // read-only 페이지에 쓰려고 했을 때
      if(page->is_cow){
        //COW 페이지라면 vm_handle_wp 호출
        return vm_handle_wp(page);
      } else {
        return false; // 알아서 page_fault 나고 종료될거라
      }
    }
  }

  return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page) {
  destroy(page);
  free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void *va) {
  struct page *page = NULL;
  /* TODO: Fill this function */
  if ((page = spt_find_page(&thread_current()->spt, va)) == NULL) {
    return false;
  }

  return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool vm_do_claim_page(struct page *page) {
  struct frame *frame = vm_get_frame();

  /* Set links */
  frame->page = page;
  page->frame = frame;

  /* TODO: Insert page table entry to map page's VA to frame's PA. */
  // COW 페이지는 read-only로 매핑해야함
  bool writable = page->is_cow ? false : page->writable;
  bool succ = pml4_set_page(thread_current()->pml4, page->va, frame->kva, writable);

  if (!succ) {
    frame->page = NULL;
    page->frame = NULL;
    palloc_free_page(frame->kva);
    free(frame);  // 디버깅 추가됨
    return false;
  }

  return swap_in(page, frame->kva);
}

static uint64_t hash_func(const struct hash_elem *e, void *aux) {
  struct page *p = hash_entry(e, struct page, hash_elem);
  void *key = p->va;
  return hash_bytes(&key, sizeof key);
}

static bool less_func(const struct hash_elem *a, const struct hash_elem *b, void *aux) {
  const struct page *pa = hash_entry(a, struct page, hash_elem);
  const struct page *pb = hash_entry(b, struct page, hash_elem);
  return pa->va < pb->va;  // less
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt) {
  hash_init(&spt->hash_table, hash_func, less_func, NULL);  //(디버깅 변경됨 & 추가)
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst ,
                                  struct supplemental_page_table *src , struct thread* parent) {
  struct hash_iterator i;
  hash_first(&i, src);
  while (hash_next(&i)) {
    struct page *page = hash_entry(hash_cur(&i), struct page, hash_elem);  //부모 spt의 페이지
    switch (page->operations->type) {
      case VM_UNINIT:
        struct page *init_new_page = malloc(sizeof(struct page));
        if(!init_new_page) return false;
        if (page->uninit.init) {
          struct lazy_load_arg *parent_aux = page->uninit.aux;
          struct lazy_load_arg *child_aux = malloc(sizeof(struct lazy_load_arg));
          if(!child_aux){
            free(init_new_page);
            return false;
          }

          child_aux->file = file_reopen(parent_aux->file);
          child_aux->ofs = parent_aux->ofs;
          child_aux->read_bytes = parent_aux->read_bytes;
          child_aux->zero_bytes = parent_aux->zero_bytes;

          if (VM_TYPE(page->uninit.type) == VM_ANON)
            uninit_new(init_new_page, page->va, page->uninit.init, page->uninit.type, child_aux, anon_initializer);
          else if (VM_TYPE(page->uninit.type) == VM_FILE)
            uninit_new(init_new_page, page->va, page->uninit.init, page->uninit.type, child_aux,
                       file_backed_initializer);
          init_new_page->writable = page->writable;
        }
        if (!spt_insert_page(dst, init_new_page)) {
          return false;
        }
        break;
      case VM_ANON:
        // Stack 페이지는 COW 적용하지 않고 즉시 복사
        if(page->anon.is_stack){
          //기존 방식대로 복사
          if(!vm_alloc_page(VM_ANON|VM_MARKER_0, page->va, page->writable))
            return false;
          struct page *new_page = spt_find_page(dst,page->va);
          if(!vm_do_claim_page(new_page)) return false;
          if(page->frame!=NULL){
            memcpy(new_page->frame->kva,page->frame->kva,PGSIZE);
          }
        }
        else{ //Stack 페이지가 아닐 경우
          //COW 적용: 프레임 공유
          if(!vm_alloc_page(VM_ANON, page->va, page->writable))
            return false;
          struct page *new_page= spt_find_page(dst, page->va);
          if(page->frame!=NULL){
            //부모 페이지가 이미 메모리에 있는 경우
            //자식도 같은 프레임을 가리키도록 설정 (이게 핵심)
            new_page->frame=page->frame;

            // anon_page로 만들어줌(fork-recursive 디버깅)
            anon_initializer(new_page,VM_ANON,new_page->frame->kva);

            //프레임 참조 카운터 증가
            lock_acquire(&page->frame->lock);
            page->frame->ref_count++;
            lock_release(&page->frame->lock);

            //양쪽 모두 COW 플래그 설정
            page->is_cow=true;
            new_page->is_cow=true;

            //양쪽 모두 read-only로 설정
            pml4_clear_page(thread_current()->pml4,new_page->va);
            if(!pml4_set_page(thread_current()->pml4, new_page->va,new_page->frame->kva, false))
              return false;
            //부모 페이지도 read-only로 변경
            pml4_clear_page(parent->pml4,page->va);
            if(!pml4_set_page(parent->pml4,page->va,page->frame->kva,false))
              return false;
          }
          else if(page->anon.swap_index!=-1){ //page->frame==NULL인 경우는 swap out 된 상태
            //swap out 된 페이지의 경우
            if(!vm_alloc_page(VM_ANON,page->va,page->writable))
              return false;
            struct page *new_page =spt_find_page(dst,page->va);

            //swap_index 복사(같은 swap slot을 가리킴)
            new_page->anon.swap_index=page->anon.swap_index;

            //COW 설정
            new_page->is_cow=true;
            page->is_cow=true;

            lock_acquire(&swap_lock);
            swap_table[page->anon.swap_index]++;
            lock_release(&swap_lock);
          }
        }
        break;
      case VM_FILE:
        if(page->file.file){
          //lazy_load_arg 생성
          struct lazy_load_arg * child_aux=malloc(sizeof(struct lazy_load_arg));
          if(!child_aux) return false;

          child_aux->file=file_reopen(page->file.file);
          if(!child_aux->file){
            free(child_aux);
            return false;
          }
          child_aux->ofs=page->file.ofs;
          child_aux->read_bytes=page->file.read_bytes;
          child_aux->zero_bytes=page->file.zero_bytes;
          child_aux->mmap=NULL; //fork에서는 mmap 공유하지 않음

          //VM_FILE 타입으로 lazy loading 페이지 생성
          if(!vm_alloc_page_with_initializer(VM_FILE,page->va,page->writable,lazy_load_segment,child_aux)){
            file_close(child_aux->file);
            free(child_aux);
            return false;
          }
        }
        break;
    }
  }
  return true;
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt) {
  /* TODO: Destroy all the supplemental_page_table hold by thread and
   * TODO: writeback all the modified contents to the storage. */
  hash_clear(&spt->hash_table, spt_destructor);
}

static void spt_destructor(struct hash_elem *e, void *aux) {
  struct page *page = hash_entry(e, struct page, hash_elem);
  vm_dealloc_page(page);
}