#ifndef __CUSTOM_MEM__H_
#define __CUSTOM_MEM__H_
#include <unistd.h>
#include <stdint.h>


typedef struct box86context_s box86context_t;

void* customMalloc(size_t size);
void* customCalloc(size_t n, size_t size);
void* customRealloc(void* p, size_t size);
void customFree(void* p);

#define kcalloc     customCalloc
#define kmalloc     customMalloc
#define krealloc    customRealloc
#define kfree       customFree

#define ALIGN(p) (((p)+box86_pagesize-1)&~(box86_pagesize-1))

#ifdef DYNAREC
typedef struct dynablock_s dynablock_t;
// custom protection flag to mark Page that are Write protected for Dynarec purpose
uintptr_t AllocDynarecMap(size_t size);
void FreeDynarecMap(uintptr_t addr);
/* D2Vita (lot eviction JIT) — contrat complet au-dessus des definitions,
 * custommem.c. dyn86_jit_allocfail ne compte QUE les refus d'AllocDynarecMap,
 * ce qui permet au site d'eviction de distinguer une panne de memoire d'un
 * opcode non implemente. dyn86_jit_collect_victims remplit `out` avec au plus
 * `max` blocs vivants pris a un curseur tournant sur l'arene, et rend combien
 * il en a mis ; l'appelant DOIT tenir mutex_dyndump. */
extern uint32_t dyn86_jit_allocfail;
int dyn86_jit_collect_victims(dynablock_t** out, int max);

void addDBFromAddressRange(uintptr_t addr, size_t size);
void cleanDBFromAddressRange(uintptr_t addr, size_t size, int destroy);

dynablock_t* getDB(uintptr_t idx);
int getNeedTest(uintptr_t idx);
int addJumpTableIfDefault(void* addr, void* jmp); // return 1 if write was succesfull
int setJumpTableIfRef(void* addr, void* jmp, void* ref); // return 1 if write was succesfull
void setJumpTableDefault(void* addr);
void setJumpTableDefaultRef(void* addr, void* jmp);
int isJumpTableDefault(void* addr);
uintptr_t getJumpTable();
uintptr_t getJumpTableAddress(uintptr_t addr);
int isJumpTableSharedSlot(uintptr_t p); // 1 = the read-only default slot (page has no table)
unsigned d2rt_box86_jmptbl_tables(void);   // level-2 tables allocated
unsigned d2rt_box86_jmptbl_fails(void);    // level-2 allocations refused
unsigned d2rt_box86_jmptbl_heap_kb(void);  // host heap the tables take
void d2rt_box86_jmptbl_census(unsigned* pages, unsigned* chunks /*[5]: 256B..4K*/, unsigned* entries);
uintptr_t getJumpAddress(uintptr_t addr);
#endif

#define PROT_NEVERCLEAN 0x100
#define PROT_DYNAREC    0x80
#define PROT_DYNAREC_R  0x40
#define PROT_NOPROT     0x20
#define PROT_DYN        (PROT_DYNAREC | PROT_DYNAREC_R | PROT_NOPROT | PROT_NEVERCLEAN)
#define PROT_CUSTOM     (PROT_DYNAREC | PROT_DYNAREC_R | PROT_NOPROT | PROT_NEVERCLEAN)
#define PROT_NEVERPROT  (PROT_NOPROT | PROT_NEVERCLEAN)

void updateProtection(uintptr_t addr, size_t size, uint32_t prot);
void setProtection(uintptr_t addr, size_t size, uint32_t prot);
void setProtection_mmap(uintptr_t addr, size_t size, uint32_t prot);
void setProtection_elf(uintptr_t addr, size_t size, uint32_t prot);
void freeProtection(uintptr_t addr, size_t size);
void refreshProtection(uintptr_t addr);
uint32_t getProtection(uintptr_t addr);
int getMmapped(uintptr_t addr);
void loadProtectionFromMap();
#ifdef DYNAREC
void protectDB(uintptr_t addr, size_t size);
void unprotectDB(uintptr_t addr, size_t size, int mark);    // if mark==0, the blocks are not marked as potentially dirty
void dyn86_mark_volatile_code(uintptr_t addr, uintptr_t size);
// Port addition (Deus Ex): WX86_SMC_SINGLE=1 makes every block translated in
// volatile (PROT_NEVERCLEAN) memory one instruction long, so code that
// rewrites an instruction further down its own block (Fire.dll's water and
// fire painters) sees the new bytes, as a real x86 does.
int dyn86_smc_single(void);
// In volatile memory: should the block end after the instruction [ip, next)?
// WX86_SMC_SINGLE=2: always (one instruction per block). =1: only when the
// instruction carries a 32-bit address that lands in volatile memory, i.e. a
// possible patch of nearby code (Unreal's self-patching painters write their
// own displacements through absolute addresses).
int dyn86_smc_stop_after(uintptr_t ip, uintptr_t next);   // port addition: always re-validate code here
int isprotectedDB(uintptr_t addr, size_t size);
#endif
void* find32bitBlock(size_t size);
void* findBlockNearHint(void* hint, size_t size, uintptr_t mask);
void* find32bitBlockElf(size_t size, int mainbin, uintptr_t mask);
int isBlockFree(void* hint, size_t size);

// unlock mutex that are locked by current thread (for signal handling). Return a mask of unlock mutex
int unlockCustommemMutex();
// relock the muxtex that were unlocked
void relockCustommemMutex(int locks);

void init_custommem_helper(box86context_t* ctx);
void fini_custommem_helper(box86context_t* ctx);

#ifdef DYNAREC
// ---- StrongMemoryModel
void addLockAddress(uintptr_t addr);    // add an address to the list of "LOCK"able
int isLockAddress(uintptr_t addr);  // return 1 is the address is used as a LOCK, 0 else

void SetHotPage(uintptr_t addr);
int isInHotPage(uintptr_t addr);
int checkInHotPage(uintptr_t addr);
#endif

#endif //__CUSTOM_MEM__H_