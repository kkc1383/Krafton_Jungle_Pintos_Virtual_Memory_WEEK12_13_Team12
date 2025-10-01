/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"

#include <string.h>

#include "include/threads/vaddr.h"
#include "lib/kernel/hash.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "userprog/process.h"
#include "vm/inspect.h"

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
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page *page) {
  int ty = VM_TYPE(page->operations->type);
  switch (ty) {
    case VM_UNINIT:
      return VM_TYPE(page->uninit.type);
    case VM_ANON:
      return VM_TYPE(page->anon.type);
    case VM_FILE:
      return VM_TYPE(page->file.type);
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
  // hash_delete(&spt->hash_table, &page->hash_elem);  // hash에서 제거
  vm_dealloc_page(page);  // page 타입에 맞게 destroy 후 free
  // return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *vm_get_victim(void) {
  struct frame *victim = NULL;
  /* TODO: The policy for eviction is up to you. */

  return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame(void) {
  struct frame *victim UNUSED = vm_get_victim();
  /* TODO: swap out the victim and return the evicted frame. */

  return NULL;
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
    PANIC("vm_get_frame: palloc_get_page(PAL_USER) failed (todo: eviction)");
  }
  if ((frame = malloc(sizeof *frame)) == NULL) {
    PANIC("vm_get_frame:  malloc(sizeof *frame) failed");
  }

  frame->kva = kaddr;
  frame->page = NULL;

  ASSERT(frame != NULL);
  ASSERT(frame->page == NULL);
  return frame;
}

/* Growing the stack. */
static bool vm_stack_growth(void *addr) {
  void *stack_bottom = pg_round_down(addr);

  struct thread *curr = thread_current();
  // 범위 확인
  if (addr > USER_STACK || addr < USER_STACK - (1 << 20)) return false;

  // page 생성, spt 등록 해줌
  if (!vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, true)) {  // VM_MARKER_0은 나중에
    return false;
  }
  // frame 생성, 페이지 테이블 등록 해줌
  if (!vm_claim_page(stack_bottom)) {
    return false;
  }
  curr->tf.rsp = stack_bottom;  //일단 페이지 하나만큼만 빈공간 벌려놓고, 그 다음부터 자율적으로 싣는것
  return true;
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page UNUSED) {}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr, bool user UNUSED, bool write UNUSED,
                         bool not_present UNUSED) {
  struct supplemental_page_table *spt UNUSED = &thread_current()->spt;
  struct page *page = NULL;
  /* TODO: Validate the fault */
  /* TODO: Your code goes here */
  page = spt_find_page(spt, addr);  // 알아서 pg_round_down 해줌
  if (!page) {
    if (not_present) {
      // void* rsp= user ? f->rsp : ;
    }
    return false;  // spt에 없는 주소(invalid 주소)
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
  bool succ = pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable);  //(디버깅 pml4)

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
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED,
                                  struct supplemental_page_table *src UNUSED) {
  struct hash_iterator i;
  hash_first(&i, src);
  while (hash_next(&i)) {
    struct page *page = hash_entry(hash_cur(&i), struct page, hash_elem);  //부모 spt의 페이지
    switch (page->operations->type) {
      case VM_UNINIT:
        struct page *init_new_page = malloc(sizeof(struct page));
        if (page->uninit.init) {
          struct lazy_load_arg *parent_aux = page->uninit.aux;
          struct lazy_load_arg *child_aux = malloc(sizeof(struct lazy_load_arg));
          child_aux->file = file_duplicate(parent_aux->file);
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
        // page 할당하고 spt에 집어넣음
        if (!vm_alloc_page(VM_ANON, page->va, page->writable)) return false;
        // 방금 할당한 page 가져오기
        struct page *anon_new_page = spt_find_page(dst, page->va);
        if (!anon_new_page) return false;

        // 페이지를 페이지 테이블에 등록하고 프레임에 올림
        if (!vm_do_claim_page(anon_new_page)) return false;

        //부모 데이터 복사
        if (page->frame != NULL) {
          memcpy(anon_new_page->frame->kva, page->frame->kva, PGSIZE);
        } else {  // swap_out 된 페이지
          // page->frame = vm_get_frame();
          // if (!page->frame) return false;
          // if (!swap_in(page, page->frame->kva)) return false;
          // memcpy(anon_new_page->frame->kva, page->frame->kva, PGSIZE);
        }
        break;
      case VM_FILE:
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