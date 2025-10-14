#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/types.h"

struct page;
enum vm_type;

struct file_page {
  struct file *file;
  off_t ofs;
  size_t read_bytes;
  size_t zero_bytes;
  struct mmap_file *mmap;  //해당 페이지와 매핑된 mmap_file 구조체
};

struct mmap_file {
  void *addr;             // 매핑 시작 주소
  size_t length;          // 총 매핑 길이(바이트)
  struct file *file;      // mmap 된 파일
  struct list_elem elem;  // thread->mmap_list에 들어갈 때 사용
};
void vm_file_init(void);
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable, struct file *file, off_t offset);
void do_munmap(void *va);
#endif
