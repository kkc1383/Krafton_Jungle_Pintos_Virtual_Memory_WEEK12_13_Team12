#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/types.h"
struct page;
enum vm_type;

struct anon_page {
  int swap_index; /* swap table에서의 bitmap index*/
};

void vm_anon_init(void);
bool anon_initializer(struct page *page, enum vm_type type, void *kva);

#endif
