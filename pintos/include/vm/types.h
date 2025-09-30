#ifndef VM_TYPES_H
#define VM_TYPES_H
#include <stdbool.h>

/* Forward declaration */
struct page;

/* VM 페이지 타입 정의 */
enum vm_type {
  VM_UNINIT = 0,
  VM_ANON = 1,
  VM_FILE = 2,
  VM_PAGE_CACHE = 3,
  VM_MARKER_0 = (1 << 3),
  VM_MARKER_1 = (1 << 4),
  VM_MARKER_END = (1 << 31),
};

/* VM initializer 타입 정의 */
typedef bool vm_initializer(struct page *, void *aux);

#endif /* VM_TYPES_H */