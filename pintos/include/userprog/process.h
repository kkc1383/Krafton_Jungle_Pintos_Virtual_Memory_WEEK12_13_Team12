#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

struct lazy_load_arg {
  struct file *file;    // 어떤 파일인지 링크
  off_t ofs;            // 해당 파일의 어디부터 읽어야 하는지 오프셋
  uint32_t read_bytes;  // 몇 바이트를 읽어야 하는지
  uint32_t zero_bytes;  // 몇 바이트를 0으로 채워야 하는지
  struct mmap_file *mmap;  // mmap에서만 사용 나머지는 null
};

tid_t process_create_initd(const char *file_name);
tid_t process_fork(const char *name, struct intr_frame *if_);
int process_exec(void *f_name);
int process_wait(tid_t);
void process_exit(void);
void process_activate(struct thread *next);

extern bool lazy_load_segment(struct page *page, void *aux);
#endif /* userprog/process.h */
