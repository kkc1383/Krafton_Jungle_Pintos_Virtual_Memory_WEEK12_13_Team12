# KAIST Pintos Project 3: Virtual Memory

본 프로젝트는 KAIST의 Pintos OS 교육용 프로젝트 중 **Virtual Memory(가상 메모리)** 시스템을 구현한 프로젝트입니다.

공식 문서: [https://casys-kaist.github.io/pintos-kaist/project3/introduction.html](https://casys-kaist.github.io/pintos-kaist/project3/introduction.html)

## 📎 관련 프로젝트
본 프로젝트는 KAIST PintOS 시리즈 중 **Project 3 (Virtual Memory)** 에 해당하며,  
이전 단계 프로젝트는 아래에서 확인할 수 있습니다.

- **Project 1 — Thread**  
  🔗 [https://github.com/kkc1383/Krafton_Jungle_Pintos_Thread_WEEK09_10_Team11  ](https://github.com/kkc1383/Krafton_Jungle_10th_Pintos_Thread_WEEK09_Team05)

- **Project 2 — User Program**  
  🔗 [https://github.com/kkc1383/Krafton_Jungle_10th_Pintos_User_Program_WEEK10_11_Team11  ](https://github.com/kkc1383/Krafton_Jungle_10th_Pintos_User_Program_WEEK10_11_Team11)


---

## 프로젝트 개요

Pintos의 가상 메모리 시스템을 완전히 구현하여, 프로세스가 물리 메모리 크기를 초과하는 메모리를 사용할 수 있도록 하고, 메모리 효율성을 극대화하는 다양한 기법들을 적용했습니다.

다음 6개의 주요 도전 과제를 구현했습니다:

1. **Memory Management** - 프레임 테이블과 SPT 기반 메모리 관리
2. **Anonymous Page** - 익명 페이지와 스왑 시스템
3. **Stack Growth** - 동적 스택 확장
4. **Memory Mapped Files** - 파일 메모리 매핑 (mmap/munmap)
5. **Swap In/Out** - Clock 알고리즘 기반 페이지 교체
6. **Copy-on-Write (COW)** - 프로세스 fork 시 메모리 복사 최적화

---

## 1. Memory Management

### 구현 내용

**핵심 자료 구조:**
- **Supplemental Page Table (SPT)**: Hash 테이블 기반으로 빠른 페이지 검색 (O(1))
- **Frame Table**: 모든 물리 프레임을 추적하고 페이지 교체 알고리즘 지원
- **Page 구조체**: 가상 주소, 프레임 참조, 쓰기 권한, COW 플래그 포함
- **Frame 구조체**: 물리 주소, 페이지 참조, 참조 카운트 (COW 지원)

**주요 함수:**
- `vm_init()` - 프레임 테이블 초기화, Clock 알고리즘 포인터 설정
- `vm_alloc_page_with_initializer()` - Lazy loading을 위한 페이지 할당
- `spt_find_page()` / `spt_insert_page()` - Hash 기반 SPT 연산
- `vm_get_frame()` - 물리 프레임 할당, 필요 시 페이지 교체

**설계 특징:**
- **Lazy Loading**: UNINIT 페이지로 할당 후 첫 접근 시 실제 메모리 할당
- **Hash 기반 SPT**: 선형 검색 대비 빠른 페이지 테이블 조회
- **동기화**: `frame_table_lock`으로 프레임 테이블 동시성 제어

**관련 파일:**
- [pintos/vm/vm.c](pintos/vm/vm.c)
- [pintos/include/vm/vm.h](pintos/include/vm/vm.h)
- [pintos/include/threads/thread.h](pintos/include/threads/thread.h)

---

## 2. Anonymous Page

### 구현 내용

익명 페이지는 파일과 연결되지 않은 메모리(힙, 스택 등)를 관리합니다.

**핵심 자료 구조:**
- `anon_page` 구조체: swap_index (스왑 디스크 위치), is_stack (스택 페이지 여부)
- `swap_table`: 각 스왑 슬롯의 참조 카운트 추적 (COW 지원)
- `swap_disk`: 디스크 장치 1,1을 스왑 디스크로 사용

**주요 함수:**
- `vm_anon_init()` - 스왑 디스크 초기화, 스왑 테이블 생성
- `anon_swap_in()` - 디스크에서 페이지 읽기 (8 섹터 = 1 페이지)
- `anon_swap_out()` - 페이지를 디스크에 쓰기, swap_index 업데이트
- `anon_destroy()` - COW 참조 카운트 관리하며 프레임 해제

**설계 특징:**
- **Swap Table 참조 카운트**: 여러 프로세스가 스왑 슬롯 공유 가능
- **Lazy Swap 할당**: 스왑 필요 시점에만 디스크 슬롯 할당
- **스택 페이지 마킹**: `VM_MARKER_0` 플래그로 스택 페이지 구분

**관련 파일:**
- [pintos/vm/anon.c](pintos/vm/anon.c)
- [pintos/include/vm/anon.h](pintos/include/vm/anon.h)

---

## 3. Stack Growth

### 구현 내용

사용자 스택이 동적으로 확장되어 최대 1MB까지 성장할 수 있도록 구현했습니다.

**주요 함수:**
- `vm_stack_growth()` - 스택 페이지 할당 (VM_ANON | VM_MARKER_0)
- `vm_try_handle_fault()` - Page fault 발생 시 스택 확장 판단

**스택 확장 조건:**
```c
// 1MB 제한 확인
if (addr > USER_STACK || addr < USER_STACK - (1 << 20)) return false;

// RSP 기반 검증 (PUSH 명령어 1페이지 선행 허용)
void *rsp = user ? f->rsp : thread_current()->rsp;
if (addr < rsp && addr != rsp - 8) return false;

// 스택 확장 수행
if (vm_stack_growth(addr)) {
    page = spt_find_page(spt, addr);
}
```

**설계 특징:**
- **1MB 스택 제한**: 무제한 스택 확장 방지
- **PUSH 명령어 지원**: `rsp - 8` 주소까지 허용
- **즉시 할당**: 스택 페이지는 lazy loading 없이 즉시 할당
- **항상 쓰기 가능**: 스택 페이지는 `writable = true`로 생성

**COW 예외 처리:**
- 스택 페이지는 Copy-on-Write 대상이 아님 (fork 시 즉시 복사)
- 자식 프로세스의 독립적인 스택 보장

**관련 파일:**
- [pintos/vm/vm.c](pintos/vm/vm.c) - `vm_stack_growth()`, `vm_try_handle_fault()`
- [pintos/userprog/exception.c](pintos/userprog/exception.c) - Page fault 핸들러

---

## 4. Memory Mapped Files

### 구현 내용

파일을 프로세스의 가상 주소 공간에 직접 매핑하여, 파일 I/O를 메모리 접근으로 수행할 수 있도록 구현했습니다.

**핵심 자료 구조:**
- `file_page` 구조체: 파일 포인터, 오프셋, 읽기/제로 바이트 수
- `mmap_file` 구조체: 매핑 시작 주소, 길이, 파일 포인터

**주요 함수:**
- `do_mmap()` - 파일을 가상 주소 공간에 매핑
  - 주소 정렬, 길이, 오프셋 검증
  - 독립적인 파일 포인터로 재오픈
  - VM_FILE 페이지를 lazy loading으로 생성

- `do_munmap()` - 매핑 해제
  - Dirty 페이지 writeback (dirty bit 확인)
  - SPT에서 페이지 제거
  - 파일 닫기

- `file_backed_swap_in()` - 파일에서 데이터 로드
- `file_backed_swap_out()` - Dirty 페이지만 파일에 쓰기

**설계 특징:**
- **Lazy Loading**: 파일 페이지는 첫 접근 시 로드
- **독립적인 파일 포인터**: 각 mmap이 별도 파일 포인터 사용
- **Writeback on Munmap**: Dirty 페이지는 unmap 시 파일에 저장
- **Overlap 감지**: 기존 페이지와 겹치는 매핑 방지

**관련 파일:**
- [pintos/vm/file.c](pintos/vm/file.c)
- [pintos/include/vm/file.h](pintos/include/vm/file.h)
- [pintos/userprog/syscall.c](pintos/userprog/syscall.c) - mmap/munmap 시스템 콜

---

## 5. Swap In/Out

### 구현 내용

물리 메모리가 부족할 때 페이지를 디스크로 교체하는 스왑 시스템을 구현했습니다.

**페이지 교체 알고리즘: Clock Algorithm (Second Chance)**

```c
static struct frame *vm_get_victim(void) {
    // Accessed bit를 활용한 Clock 알고리즘
    while (true) {
        if (pml4_is_accessed(pml4, frame->page->va)) {
            // Accessed bit가 1이면 0으로 변경하고 다음으로
            pml4_set_accessed(pml4, frame->page->va, false);
            clock_hand = list_next(clock_hand);
        } else {
            // Accessed bit가 0인 페이지를 희생양으로 선택
            victim = frame;
            break;
        }
    }
}
```

**익명 페이지 스왑:**
- **Swap Out**: 디스크에 쓰기 (8 섹터), swap_index 업데이트, 페이지 테이블 엔트리 제거
- **Swap In**: swap_index를 사용해 디스크에서 읽기, 참조 카운트 감소

**파일 페이지 스왑:**
- **Swap Out**: Dirty bit 확인 후 수정된 경우만 파일에 쓰기
- **Swap In**: 원본 파일 오프셋에서 재로드

**교체 시나리오:**
```c
// vm_get_frame() 내부
int8_t *kaddr = palloc_get_page(PAL_USER);
if (kaddr == NULL) {
    // 물리 메모리 부족 시 희생 프레임 선택
    frame = vm_evict_frame();
    // 희생 페이지의 swap_out() 호출
    // 프레임을 새 페이지에 재사용
}
```

**설계 특징:**
- **Clock 알고리즘**: LRU 근사 알고리즘으로 최근 사용 페이지 보호
- **Stateful Pointer**: clock_hand가 프레임 리스트를 순회하며 위치 유지
- **효율적인 교체**: 전체 페이지 테이블 스캔 없이 희생자 선택

**관련 파일:**
- [pintos/vm/vm.c](pintos/vm/vm.c) - `vm_get_victim()`, `vm_evict_frame()`
- [pintos/vm/anon.c](pintos/vm/anon.c) - 스왑 연산
- [pintos/vm/file.c](pintos/vm/file.c) - 파일 페이지 스왑

---

## 6. Copy-on-Write (COW)

### 구현 내용

프로세스 fork 시 부모와 자식이 메모리를 공유하다가, 쓰기 시도 시에만 실제 복사를 수행하는 최적화 기법을 구현했습니다.

**자료 구조 확장:**
```c
struct page {
    bool is_cow;          // COW 페이지 여부
};

struct frame {
    int ref_count;        // 이 프레임을 공유하는 프로세스 수
    struct lock lock;     // ref_count 동기화
};
```

**핵심 함수:**

**1. `supplemental_page_table_copy()` - fork 시 페이지 테이블 복사**
```c
// UNINIT 페이지: aux 구조체 복사 (파일 재오픈)
// 스택 페이지: 즉시 물리 복사 (COW 제외)
// ANON 페이지 (비스택):
//   - 프레임 공유, is_cow = true 설정
//   - ref_count 증가
//   - 부모/자식 모두 read-only로 재매핑
// 스왑된 ANON 페이지: swap_table 참조 카운트 증가
// FILE 페이지: aux 복사 (파일 재오픈)
```

**2. `vm_handle_wp()` - Write-protect 예외 처리**
```c
if (ref_count == 1) {
    // 마지막 사용자 - 쓰기 권한만 활성화
    page->is_cow = false;
    pml4_set_page(pml4, va, kva, true);
} else {
    // 다중 사용자 - 프레임 복사
    struct frame *new_frame = vm_get_frame();
    memcpy(new_frame->kva, old_frame->kva, PGSIZE);
    // 새 프레임으로 재매핑 (쓰기 가능)
    old_frame->ref_count--;
    // ref_count가 0이 되면 프레임 해제
}
```

**COW Write Protection 흐름:**
1. 자식이 공유 페이지에 쓰기 시도 (read-only 상태)
2. CPU가 write-protect exception 발생 (not_present=false, write=true)
3. `page_fault()`가 `vm_try_handle_fault()` 호출
4. `page->is_cow` 확인 (true)
5. `vm_handle_wp()` 호출:
   - ref_count=1: 쓰기 권한만 활성화
   - ref_count>1: 프레임 복사, 기존 ref_count 감소
6. 새 쓰기 가능 매핑으로 실행 재개

**특수 케이스 처리:**
- **스택 페이지**: COW 제외, fork 시 즉시 복사
- **스왑된 페이지**: fork 시 swap_table 참조 카운트 증가
- **페이지 삭제**: ref_count 감소, 0이 되면 프레임 해제
- **Write-protect 처리**: 부모/자식 페이지 테이블 동시 업데이트

**설계 특징:**
- **메모리 효율성**: 실제 쓰기 전까지 메모리 공유
- **투명한 구현**: 사용자 프로그램은 COW를 인지하지 못함
- **동기화**: Frame lock으로 ref_count 보호
- **스왑 지원**: 스왑된 페이지도 COW 참조 카운트 관리

**관련 파일:**
- [pintos/vm/vm.c](pintos/vm/vm.c) - `vm_handle_wp()`, `vm_do_claim_page()`
- [pintos/vm/anon.c](pintos/vm/anon.c) - `anon_destroy()` COW 지원
- [pintos/userprog/process.c](pintos/userprog/process.c) - `supplemental_page_table_copy()`

---

## 주요 설계 결정사항

### 1. Lazy Loading 아키텍처
- 모든 파일 백업 페이지와 일부 힙 페이지는 lazy loading
- UNINIT 페이지가 초기화 함수와 보조 데이터 포함
- 첫 fault 발생 시 특정 페이지 타입으로 변환

### 2. Hash 기반 SPT
- O(1) 페이지 조회 성능
- 선형 검색 대비 확장성 우수

### 3. Clock Algorithm
- 비용이 큰 페이지 테이블 전체 스캔 회피
- Accessed bit로 최근 사용 정보 제공
- Stateful pointer로 효율적인 순회

### 4. COW 복잡도 처리
- fork 시 부모 페이지 테이블도 read-only로 변경
- 프로세스 간 스왑 슬롯 공유 (참조 카운트)
- 스택 페이지는 COW 제외로 복잡도 감소

### 5. 메모리 누수 방지
- 프로세스 종료 시 모든 mmap 영역 해제
- SPT 소멸자가 모든 페이지 해제
- 프레임 교체 시 프레임 테이블 정리

### 6. Page Fault 검증
- 스택 확장은 USER_STACK부터 1MB로 제한
- PUSH 명령어를 위한 1페이지 선행 허용
- SPT 조회 전 사용자 주소 검증

---

## 파일 구조

### 핵심 구현 파일

| 컴포넌트 | 주요 파일 | 핵심 함수 |
|---------|---------|----------|
| **Memory Management** | [vm/vm.c](pintos/vm/vm.c), [vm/vm.h](pintos/include/vm/vm.h) | vm_init, vm_alloc, vm_get_frame, spt_* |
| **Anonymous Pages** | [vm/anon.c](pintos/vm/anon.c), [vm/anon.h](pintos/include/vm/anon.h) | vm_anon_init, anon_swap_in/out |
| **Stack Growth** | [vm/vm.c](pintos/vm/vm.c), [userprog/exception.c](pintos/userprog/exception.c) | vm_stack_growth, page_fault |
| **Memory Mapping** | [vm/file.c](pintos/vm/file.c), [userprog/syscall.c](pintos/userprog/syscall.c) | do_mmap, do_munmap, file_backed_* |
| **Swap System** | [vm/anon.c](pintos/vm/anon.c), [vm/vm.c](pintos/vm/vm.c) | vm_get_victim, disk read/write |
| **Copy-on-Write** | [vm/vm.c](pintos/vm/vm.c), [userprog/process.c](pintos/userprog/process.c) | vm_handle_wp, spt_copy, ref_count |

---

## 빌드 및 실행

### 빌드
```bash
cd pintos/vm
make
```

### 테스트 실행
```bash
# 모든 테스트 실행
make check

# 특정 테스트 실행
make tests/vm/pt-grow-stack.result
```

### 디버깅
```bash
# GDB 디버깅
pintos --gdb -- run [program]

# 별도 터미널에서
gdb kernel.o
target remote localhost:1234
```

---

## 참고 자료

- [KAIST Pintos 공식 문서](https://casys-kaist.github.io/pintos-kaist/)
- [Pintos Project 3: Virtual Memory](https://casys-kaist.github.io/pintos-kaist/project3/introduction.html)
- Clock Algorithm (Second Chance Page Replacement)
- Copy-on-Write in Modern Operating Systems


