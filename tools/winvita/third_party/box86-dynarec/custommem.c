#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <signal.h>
#include <errno.h>
#include <sched.h>
#include <time.h>

#include "box86context.h"
#include "elfloader.h"
#include "debug.h"
#include "x86trace.h"
#include "x86emu.h"
#include "librarian.h"
#include "bridge.h"
#include "library.h"
#include "callback.h"
#include "wrapper.h"
#include "myfts.h"
#include "threads.h"
#include "x86trace.h"
#include "signals.h"
#include <sys/mman.h>
#include "custommem.h"
#include "threads.h"
#include "rbtree.h"
#ifdef DYNAREC
#include "dynablock.h"
#include "dynarec/dynablock_private.h"
#include "dynarec/arm_lock_helper.h"
#include "khash.h"

/* build.sh passe deja -DUSE_MMAP : sans le garde, chaque compilation de ce
 * fichier sort un avertissement de redefinition. */
#ifndef USE_MMAP
#define USE_MMAP
#endif
//#define USE_MMAP_MORE

// init inside dynablocks.c
static mmaplist_t          *mmaplist = NULL;
#ifdef TRACE_MEMSTAT
static size_t jmptbl_allocated = 0, jmptbl_allocated1 = 0;
#endif
static int                 mmapsize = 0;
static uintptr_t           *box86_jmptbl[JMPTABL_SIZE];
#ifndef WX86_JT3
static uintptr_t           box86_jmptbl_default[1<<JMPTABL_SHIFT];
#endif
// lock addresses
KHASH_SET_INIT_INT(lockaddress)
static kh_lockaddress_t    *lockaddress = NULL;
#endif
#ifdef USE_CUSTOM_MUTEX
static uint32_t            mutex_prot;
static uint32_t            mutex_blocks;
static uint32_t            mutex_dynarec;
#else
static pthread_mutex_t     mutex_prot;
static pthread_mutex_t     mutex_blocks;
/* D2Vita (audit T12 §11.6): dedicated lock for the dynarec JIT arena
 * (mmaplist chunks + their blockmark chains). Upstream box86 leaves
 * AllocDynarecMap/FreeDynarecMap lockless and relies on EVERY caller
 * reaching them under my_context->mutex_dyndump (internalDBGetBlock's
 * need_lock discipline, threaded through 3 files) — an IMPLICIT invariant
 * that nothing asserts. Under D2SCHED=native, guest runners translate
 * GIL-free; the arena must be self-consistent regardless of the caller's
 * lock state. Static initializer as belt-and-braces (pte handles the
 * lazy-init sentinel; a ZEROED pte mutex would deref NULL — see the
 * init_custommem_helper comment); init_mutexes() re-inits it properly.
 * Lock order: strict LEAF — nothing else is taken while holding it
 * (setProtection of a fresh chunk happens AFTER unlock, same shape as
 * customMalloc), so it can never join the pre-existing
 * mutex_prot<->mutex_dyndump ordering tangle. */
static pthread_mutex_t     mutex_dynarec = PTHREAD_MUTEX_INITIALIZER;
#endif
#define MEMPROT_SHIFT 12
#define MEMPROT_SIZE (1<<(32-MEMPROT_SHIFT))
//#define TRACE_MEMSTAT
rbtree* memprot = NULL;
static int inited = 0;

static rbtree*  mapallmem = NULL;
static rbtree*  mmapmem = NULL;

typedef struct blocklist_s {
    void*               block;
    size_t              maxfree;
    size_t              size;
    void*               first;
} blocklist_t;

#define MMAPSIZE (64*1024)      // allocate 64kb sized blocks
#define DYNMMAPSZ (2*1024*1024) // allocate 2Mb block for dynarec

static int                 n_blocks = 0;       // number of blocks for custom malloc
static int                 c_blocks = 0;       // capacity of blocks for custom malloc
static blocklist_t*        p_blocks = NULL;    // actual blocks for custom malloc

typedef union mark_s {
    struct {
        unsigned int    size:31;
        unsigned int    fill:1;
    };
    uint32_t            x32;
} mark_t;
typedef struct blockmark_s {
    mark_t  prev;
    mark_t  next;
} blockmark_t;

#define NEXT_BLOCK(b) (blockmark_t*)((uintptr_t)(b) + (b)->next.size + sizeof(blockmark_t))
#define PREV_BLOCK(b) (blockmark_t*)(((uintptr_t)(b) - (b)->prev.size) - sizeof(blockmark_t))
#define LAST_BLOCK(b, s) (blockmark_t*)(((uintptr_t)(b)+(s))-sizeof(blockmark_t))

void printBlock(blockmark_t* b, void* start)
{
    printf_log(LOG_NONE, "========== Block is:\n");
    do {
        printf_log(LOG_NONE, "%c%p, fill=%d, size=0x%x (prev=%d/0x%x)\n", b==start?'*':' ', b, b->next.fill, b->next.size, b->prev.fill, b->prev.size);
        b = NEXT_BLOCK(b);
    } while(b->next.x32);
    printf_log(LOG_NONE, "===================\n");
}

// get first subblock free in block. Return NULL if no block, else first subblock free (mark included), filling size
static void* getFirstBlock(void* block, size_t maxsize, size_t* size, void* start)
{
    // get start of block
    blockmark_t *m = (blockmark_t*)((start)?start:block);
    while(m->next.x32) {    // while there is a subblock
        if(!m->next.fill && m->next.size>=maxsize) {
            *size = m->next.size;
            return m;
        }
        m = NEXT_BLOCK(m);
    }

    return NULL;
}

static void* getNextFreeBlock(void* block)
{
    blockmark_t *m = (blockmark_t*)block;
    while (m->next.fill) {
         m = NEXT_BLOCK(m);
    };
    return m;
}
static void* getPrevFreeBlock(void* block)
{
    blockmark_t *m = (blockmark_t*)block;
    do {
         m = PREV_BLOCK(m);
    } while (m->next.fill);
    return m;
}

static size_t getMaxFreeBlock(void* block, size_t block_size, void* start)
{
    // get start of block
    if(start) {
        blockmark_t *m = (blockmark_t*)start;
        int maxsize = 0;
        while(m->next.x32) {    // while there is a subblock
            if(!m->next.fill && m->next.size>maxsize) {
                maxsize = m->next.size;
            }
            m = NEXT_BLOCK(m);
        }
        return (maxsize>=sizeof(blockmark_t))?maxsize:0;
    } else {
        blockmark_t *m = LAST_BLOCK(block, block_size); // start with the end
        int maxsize = 0;
        while(m->prev.x32) {    // while there is a subblock
            if(!m->prev.fill && m->prev.size>maxsize) {
                maxsize = m->prev.size;
                if((uintptr_t)block+maxsize>(uintptr_t)m)
                    return (maxsize>=sizeof(blockmark_t))?maxsize:0; // no block large enough left...
            }
            m = PREV_BLOCK(m);
        }
        return (maxsize>=sizeof(blockmark_t))?maxsize:0;
    }
}

#define THRESHOLD   (128-2*sizeof(blockmark_t))

static void* allocBlock(void* block, void *sub, size_t size, void** pstart)
{
    (void)block;

    blockmark_t *s = (blockmark_t*)sub;
    blockmark_t *n = NEXT_BLOCK(s);

    s->next.fill = 1;
    // check if a new mark is worth it
    if(s->next.size>size+2*sizeof(blockmark_t)+THRESHOLD) {
        size_t old_size = s->next.size;
        s->next.size = size;
        blockmark_t *m = NEXT_BLOCK(s);
        m->prev.fill = 1;
        m->prev.size = s->next.size;
        m->next.fill = 0;
        m->next.size = old_size - (size + sizeof(blockmark_t));
        n->prev.fill = 0;
        n->prev.size = m->next.size;
        n = m;
    } else {
        n->prev.fill = 1;
    }

    if(pstart && sub==*pstart) {
        // get the next free block
        while(n->next.fill)
            n = NEXT_BLOCK(n);
        *pstart = (void*)n;
    }
    return (void*)((uintptr_t)sub + sizeof(blockmark_t));
}
static size_t freeBlock(void *block, void* sub, void** pstart)
{
    blockmark_t *m = (blockmark_t*)block;
    blockmark_t *s = (blockmark_t*)sub;
    blockmark_t *n = NEXT_BLOCK(s);
    if(block!=sub)
        m = PREV_BLOCK(s);
    s->next.fill = 0;
    n->prev.fill = 0;
    // check if merge with previous
    if (m!=s && s->prev.x32 && !s->prev.fill) {
        // remove s...
        m->next.size += s->next.size + sizeof(blockmark_t);
        n->prev.size = m->next.size;
        s = m;
    }
    // check if merge with next
    if(n->next.x32 && !n->next.fill) {
        blockmark_t *n2 = NEXT_BLOCK(n);
        //remove n
        s->next.size += n->next.size + sizeof(blockmark_t);
        n2->prev.size = s->next.size;
    }
    if(pstart && (uintptr_t)*pstart>(uintptr_t)s) {
        *pstart = (void*)s;
    }
    // return free size at current block (might be bigger)
    return s->next.size;
}
// return 1 if block has been expanded to new size, 0 if not
static int expandBlock(void* block, void* sub, size_t newsize)
{
    (void)block;

    newsize = (newsize+3)&~3;
    blockmark_t *s = (blockmark_t*)sub;
    blockmark_t *n = NEXT_BLOCK(s);
    if(s->next.size>=newsize)
        // big enough, no shrinking...
        return 1;
    if(s->next.fill)
        return 0;   // next block is filled
    // unsigned bitfield of this length gets "promoted" to *signed* int...
    if((size_t)(s->next.size + n->next.size + sizeof(blockmark_t)) < newsize)
        return 0;   // free space too short
    // ok, doing the alloc!
    if((s->next.size+n->next.size+sizeof(blockmark_t))-newsize<THRESHOLD+2*sizeof(blockmark_t))
        s->next.size += n->next.size+sizeof(blockmark_t);
    else
        s->next.size = newsize+sizeof(blockmark_t);
    blockmark_t *m = NEXT_BLOCK(s);   // this is new n
    m->prev.fill = 1;
    m->prev.size = s->next.size;
    if(n!=m) {
        // new mark
        m->prev.fill = 1;
        m->prev.size = s->next.size;
        m->next.fill = 0;
        m->next.size = (uintptr_t)n - (uintptr_t)m;
        n->prev.fill = 0;
        n->prev.size = m->next.size;
    }
    return 1;
}
// return size of block
static size_t sizeBlock(void* sub)
{
    blockmark_t *s = (blockmark_t*)sub;
    return s->next.size;
}

// return 1 if block is coherent, 0 if not (and printf the issues)
int printBlockCoherent(int i)
{
    if(i<0 || i>=n_blocks) {
        printf_log(LOG_NONE, "Error, %d should be between 0 and %d\n", i, n_blocks);
        return 0;
    }
    int ret = 1;
    blockmark_t* m = (blockmark_t*)p_blocks[i].block;
    // check if first is correct
    blockmark_t* first = getNextFreeBlock(m);
    if(p_blocks[i].first && p_blocks[i].first!=first) {printf_log(LOG_NONE, "First %p and stored first %p differs for block %d\n", first, p_blocks[i].first, i); ret = 0;}
    // check if maxfree is correct, with no hint
    size_t maxfree = getMaxFreeBlock(m, p_blocks[i].size, NULL);
    if(maxfree != p_blocks[i].maxfree) {printf_log(LOG_NONE, "Maxfree without hint %zd and stored maxfree %zd differs for block %d\n", maxfree, p_blocks[i].maxfree, i); ret = 0;}
    // check if maxfree from first is correct
    maxfree = getMaxFreeBlock(m, p_blocks[i].size, p_blocks[i].first);
    if(maxfree != p_blocks[i].maxfree) {printf_log(LOG_NONE, "Maxfree with hint %zd and stored maxfree %zd differs for block %d\n", maxfree, p_blocks[i].maxfree, i); ret = 0;}
    // check chain
    blockmark_t* last = (blockmark_t*)(((uintptr_t)m)+p_blocks[i].size-sizeof(blockmark_t));
    while(m<last) {
        blockmark_t* n = NEXT_BLOCK(m);
        if(!m->next.fill && !n->next.fill && n!=last) {
            printf_log(LOG_NONE, "Chain contains 2 subsequent free blocks %p (%d) and %p (%d) for block %d\n", m, m->next.size, n, n->next.size, i);
            ret = 0;
        }
        m = n;
    }
    if(m!=last) {
        printf_log(LOG_NONE, "Last block %p is behond expexted block %p for block %d\n", m, last, i);
        ret = 0;
    }

    return ret;
}

void testAllBlocks()
{
    size_t total = 0;
    size_t fragmented_free = 0;
    size_t max_free = 0;
    for(int i=0; i<n_blocks; ++i) {
        printBlockCoherent(i);
        total += p_blocks[i].size;
        if(max_free<p_blocks[i].maxfree)
            max_free = p_blocks[i].maxfree;
        blockmark_t* m = (blockmark_t*)p_blocks[i].block;
        while(m->next.x32) {
            if(!m->next.fill)
                fragmented_free += m->next.size;
            m = NEXT_BLOCK(m);
        }
    }
    printf_log(LOG_NONE, "Total %d blocks, for %zd allocated memory, max_free %zd, toatal fragmented free %zd\n", n_blocks, total, max_free, fragmented_free);
}

static size_t roundSize(size_t size)
{
    if(!size)
        return size;
    size = (size+7)&~7LL;   // 8 bytes align in size

    if(size<THRESHOLD)
        size = THRESHOLD;

    return size;
}

#ifdef TRACE_MEMSTAT
static size_t customMalloc_allocated = 0;
#endif
/* ⚡ 08/09 : EXPOSER LES POSTES D'ALLOCATION DE BOX86. mallinfo() donne le tas
 * newlib EN BLOC ; quand il grimpe de 22 a 30 Mio a l'entree en jeu, on ne sait
 * pas si c'est box86 (metadonnees de blocs, table de sauts) ou le reste du
 * runtime. Ces compteurs EXISTAIENT deja mais (a) etaient derriere
 * TRACE_MEMSTAT, non defini — donc pas meme compiles — et (b) n'etaient
 * publies que dans un printf_log de fin, jamais atteint sur console. Deux
 * facons pour un compteur de ne servir a rien.
 * Le surcout de TRACE_MEMSTAT est nul en pratique : les increments sont sur
 * des chemins FROIDS (nouveau bloc de 64 Kio, table de sauts), pas dans le
 * malloc chaud. */
size_t d2rt_box86_custommalloc_kb(void){
#ifdef TRACE_MEMSTAT
    return customMalloc_allocated >> 10;
#else
    return 0;
#endif
}
size_t d2rt_box86_jmptbl_kb(void){
#ifdef TRACE_MEMSTAT
    return jmptbl_allocated >> 10;
#else
    return 0;
#endif
}
void* customMalloc(size_t size)
{
    size = roundSize(size);
    // look for free space
    void* sub = NULL;
    size_t fullsize = size+2*sizeof(blockmark_t);
    mutex_lock(&mutex_blocks);
    for(int i=0; i<n_blocks; ++i) {
        if(p_blocks[i].maxfree>=size) {
            size_t rsize = 0;
            sub = getFirstBlock(p_blocks[i].block, size, &rsize, p_blocks[i].first);
            if(sub) {
                if(rsize-size<THRESHOLD)
                    size = rsize;
                void* ret = allocBlock(p_blocks[i].block, sub, size, &p_blocks[i].first);
                if(rsize==p_blocks[i].maxfree)
                    p_blocks[i].maxfree = getMaxFreeBlock(p_blocks[i].block, p_blocks[i].size, p_blocks[i].first);
                mutex_unlock(&mutex_blocks);
                return ret;
            }
        }
    }
    // add a new block
    int i = n_blocks++;
    if(n_blocks>c_blocks) {
        c_blocks += 4;
        p_blocks = (blocklist_t*)box_realloc(p_blocks, c_blocks*sizeof(blocklist_t));
    }
    size_t allocsize = (fullsize>MMAPSIZE)?fullsize:MMAPSIZE;
    /* D2Vita — INJECTION DE PENURIE (D2_CUSTOMFAILAFTER=N). Le refus du noyau
     * sur ce tas est arrive en production mais ne se produit JAMAIS sous qemu,
     * ou mmap ne refuse rien : sans de quoi le fabriquer, le traitement du
     * refus ci-dessous resterait du code jamais execute, donc une reparation
     * non verifiable. Meme intention que D2_JITFAILAFTER pour l'arene JIT.
     * Compteur sous mutex_blocks, deja tenu ici. */
    static int cm_failafter = -1;
    static unsigned cm_blocks = 0;
    if(cm_failafter < 0) {
        const char* e = getenv("D2_CUSTOMFAILAFTER");
        cm_failafter = (e && e[0]) ? atoi(e) : 0;
        if(cm_failafter > 0)
            printf_log(LOG_NONE, "[custom] INJECTION: tas de metadonnees borne a %d blocs de %u Kio\n",
                       cm_failafter, (unsigned)(MMAPSIZE>>10));
    }
    #ifdef USE_MMAP
    void* p;
    if(cm_failafter > 0 && (int)cm_blocks >= cm_failafter) {
        p = MAP_FAILED;
    } else {
        ++cm_blocks;
        p = mmap(NULL, allocsize, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
    }
    /* D2Vita: l'amont N'ETAIT PAS TESTE. mmap rend (void*)-1, pas NULL, et le
     * memset qui suivait ecrivait 64 Kio a partir de 0xFFFFFFFF : sur Vita
     * c'est la faute hote signature hfault_sys|SceLibKernel|0x120 (memset de
     * la newlib y est un saut de queue vers sceClibMemset, d'ou un pc dans le
     * module systeme et un lr dans customMalloc). La RAM utilisateur de la
     * console est engagee a 100% au boot : ce refus n'est pas theorique, il
     * est arrive en 0.1.7 comme en 0.1.9, sur cinq consoles. Le refus doit
     * remonter comme un refus. */
    if(p==MAP_FAILED) p = NULL; else memset(p, 0, allocsize);
    #else
    void* p = box_calloc(1, allocsize);
    #endif
    if(!p) {
        /* Rendre le numero de bloc pris plus haut : sans ca, p_blocks[i] reste
         * un trou que la boucle de recherche du prochain appel dereference. */
        --n_blocks;
        mutex_unlock(&mutex_blocks);
        return NULL;
    }
#ifdef TRACE_MEMSTAT
    customMalloc_allocated += allocsize;
#endif
    p_blocks[i].block = p;
    p_blocks[i].first = p;
    p_blocks[i].size = allocsize;
    // setup marks
    blockmark_t* m = (blockmark_t*)p;
    m->prev.x32 = 0;
    m->next.fill = 0;
    m->next.size = allocsize-2*sizeof(blockmark_t);
    blockmark_t* n = NEXT_BLOCK(m);
    n->next.x32 = 0;
    n->prev.fill = 0;
    n->prev.size = m->next.size;
    // alloc 1st block
    void* ret  = allocBlock(p_blocks[i].block, p, size, &p_blocks[i].first);
    p_blocks[i].maxfree = getMaxFreeBlock(p_blocks[i].block, p_blocks[i].size, p_blocks[i].first);
    mutex_unlock(&mutex_blocks);
    if(mapallmem)
        setProtection((uintptr_t)p, allocsize, PROT_READ | PROT_WRITE);
    return ret;
}
void* customCalloc(size_t n, size_t size)
{
    size_t newsize = roundSize(n*size);
    void* ret = customMalloc(newsize);
    if(!ret) return NULL;   /* D2Vita: meme defaut un cran plus haut */
    memset(ret, 0, newsize);
    return ret;
}
void* customRealloc(void* p, size_t size)
{
    if(!p)
        return customMalloc(size);
    size = roundSize(size);
    uintptr_t addr = (uintptr_t)p;
    mutex_lock(&mutex_blocks);
    for(int i=0; i<n_blocks; ++i) {
        if ((addr>(uintptr_t)p_blocks[i].block) 
         && (addr<((uintptr_t)p_blocks[i].block+p_blocks[i].size))) {
            void* sub = (void*)(addr-sizeof(blockmark_t));
            if(expandBlock(p_blocks[i].block, sub, size)) {
                if(sub<p_blocks[i].first && p+size+sizeof(blockmark_t)>=p_blocks[i].first)
                    p_blocks[i].first = getNextFreeBlock(sub);
                p_blocks[i].maxfree = getMaxFreeBlock(p_blocks[i].block, p_blocks[i].size, p_blocks[i].first);
                mutex_unlock(&mutex_blocks);
                return p;
            }
            mutex_unlock(&mutex_blocks);
            void* newp = customMalloc(size);
            /* D2Vita: meme trou que dans customMalloc, atteignable par
             * krealloc (kh_resize de la table lockaddress). Un refus doit
             * rendre NULL SANS liberer l'ancien bloc — semantique realloc :
             * l'appelant garde une reference valide et peut renoncer. */
            if(!newp) return NULL;
            memcpy(newp, p, sizeBlock(sub));
            customFree(p);
            return newp;
        }
    }
    mutex_unlock(&mutex_blocks);
    if(n_blocks)
        dynarec_log(LOG_NONE, "Warning, block %p not found in p_blocks for realloc, malloc'ing again without free\n", (void*)addr);
    return customMalloc(size);
}
void customFree(void* p)
{
    if(!p)
        return;
    uintptr_t addr = (uintptr_t)p;
    mutex_lock(&mutex_blocks);
    for(int i=0; i<n_blocks; ++i) {
        if ((addr>(uintptr_t)p_blocks[i].block) 
         && (addr<((uintptr_t)p_blocks[i].block+p_blocks[i].size))) {
            void* sub = (void*)(addr-sizeof(blockmark_t));
            size_t newfree = freeBlock(p_blocks[i].block, sub, &p_blocks[i].first);
            if(p_blocks[i].maxfree < newfree) p_blocks[i].maxfree = newfree;
            mutex_unlock(&mutex_blocks);
            return;
        }
    }
    mutex_unlock(&mutex_blocks);
    if(n_blocks)
        dynarec_log(LOG_NONE, "Warning, block %p not found in p_blocks for Free\n", (void*)addr);
}

#ifdef DYNAREC
void arm_next(void);

#define NCHUNK          64
typedef struct mmaplist_s {
    blocklist_t         chunks[NCHUNK];
    mmaplist_t*         next;
} mmaplist_t;

dynablock_t* FindDynablockFromNativeAddress(void* p)
{
    if(!p)
        return NULL;
    
    uintptr_t addr = (uintptr_t)p;

    int i= 0;
    mmaplist_t* list = mmaplist;
    if(!list)
        return NULL;
    while(list) {
        if ((addr>(uintptr_t)list->chunks[i].block) 
         && (addr<((uintptr_t)list->chunks[i].block+list->chunks[i].size))) {
            blockmark_t* sub = (blockmark_t*)list->chunks[i].block;
            while((uintptr_t)sub<addr) {
                blockmark_t* n = NEXT_BLOCK(sub);
                if((uintptr_t)n>addr) {
                    // found it!
                    // self is the field of a block
                    return *(dynablock_t**)((uintptr_t)sub+sizeof(blockmark_t));
                }
                sub = n;
            }
            return NULL;
        }
        ++i;
        if(i==NCHUNK) {
            i = 0;
            list = list->next;
        }
    }
    return NULL;
}

#ifdef TRACE_MEMSTAT
static uint32_t dynarec_allocated = 0;
#endif

/* D2Vita (lot eviction JIT) — pourquoi ces deux symboles vivent ICI.
 *
 * dyn86_jit_allocfail : compte les refus de CE allocateur, et lui seul. Le
 * site d'eviction (dynablock.c) doit distinguer « FillBlock a echoue faute de
 * MEMOIRE » de « FillBlock a echoue parce que l'opcode n'est pas implemente,
 * ou parce que la passe 2/3 a abandonne ». Evincer sur la seconde famille
 * serait du vandalisme pur : on jetterait du code chaud sans que ca change
 * quoi que ce soit au resultat. Comparer ce compteur avant/apres l'appel
 * donne la reponse EXACTE, sans deviner.
 *
 * D2_JITFAILAFTER=<n> : injection de panne. A partir du n-ieme chunk, cet
 * allocateur cesse d'en ouvrir de nouveaux et ne sert plus que depuis les
 * chunks existants. C'est EXACTEMENT le comportement de la piscine Vita une
 * fois pleine (mman_vita.c : la piscine est un allocateur a butee, box86 ne
 * rend jamais ses chunks), et c'est le seul moyen de reproduire le defaut
 * sous qemu-arm — ou mman_vita.c n'est pas compile du tout, le mmap de Linux
 * etant lie a sa place.
 *
 * D2_JITCHUNK_KB=<n> : taille de chunk (defaut DYNMMAPSZ = 2048 Kio). Sans
 * lui, la borne ci-dessus n'a que 2 Mio de resolution, et pour un jeu de code
 * chaud de 3 Mio le seul reglage disponible saute de « aucun effet » (2
 * chunks) a « 60 % de sur-reservation » (1 chunk). Or ce n'est PAS le defaut
 * du parc : sur console le jeu chaud fait ~14 Mio pour une piscine de 16 Mio
 * — une piscine LEGEREMENT trop petite, decoupee en 8 chunks. Un banc qui ne
 * sait reproduire que l'emballement total prouverait autre chose que ce qu'on
 * corrige. Baisser la taille de chunk rend le MEME rapport chunks/besoin, et
 * donc la meme situation.
 *
 * Ce sont des BOUTONS D'ENVIRONNEMENT, pas des #ifdef de test : le code
 * eprouve doit etre le code livre. Absents => zero surcout (des entiers deja
 * charges, testes une fois par CREATION DE CHUNK, jamais par bloc traduit). */
uint32_t dyn86_jit_allocfail = 0;
static int      dyn86_failafter = -1;   /* -1 = pas encore lu ; 0 = inerte */
static uint32_t dyn86_chunks = 0;       /* chunks ouverts depuis le boot */
static size_t   dyn86_chunksz = 0;      /* 0 = pas encore lu => DYNMMAPSZ */

uintptr_t AllocDynarecMap(size_t size)
{
    if(!size)
        return 0;

    size = roundSize(size);

    /* D2Vita (audit T12 §11.6): the whole walk+mutate is under mutex_dynarec.
     * "Leaf" is not absolute — the edges taken while holding it, all sane:
     *  - box_calloc: plain newlib/glibc calloc in the D2Vita shim (debug.h),
     *    i.e. the malloc-internal lock only. If box_* is ever remapped to the
     *    custom allocator (upstream box64 shape), that becomes an explicit
     *    mutex_dynarec -> mutex_blocks edge — sane ONLY while never inverted
     *    (nothing under mutex_blocks may reach the dynarec arena; customMalloc
     *    does not today — keep it that way).
     *  - mmap: kernel on Linux; on Vita the mman_vita facade takes its leaf
     *    g_blk_mx (also reached under mutex_blocks via customMalloc — leaf on
     *    both paths, never inverted).
     *  - dynarec_log: the stdio lock.
     * setProtection (mutex_prot) is deliberately kept OUTSIDE the lock —
     * see the success-path below. Uncontended cost under coop: one
     * lock/unlock pair per TRANSLATION (not per executed block) — noise. */
    mutex_lock(&mutex_dynarec);
    mmaplist_t* list = mmaplist;
    if(!list)
        list = mmaplist = (mmaplist_t*)box_calloc(1, sizeof(mmaplist_t));
    // check if there is space in current open ones
    int i = 0;
    uintptr_t sz = size + 2*sizeof(blockmark_t);
    while(1) {
        if(list->chunks[i].maxfree>=size) {
            // looks free, try to alloc!
            size_t rsize = 0;
            void* sub = getFirstBlock(list->chunks[i].block, size, &rsize, list->chunks[i].first);
            if(sub) {
                void* ret = allocBlock(list->chunks[i].block, sub, size, NULL);
                if(sub==list->chunks[i].first)
                    list->chunks[i].first = getNextFreeBlock(sub);
                if(rsize==list->chunks[i].maxfree)
                    list->chunks[i].maxfree = getMaxFreeBlock(list->chunks[i].block, list->chunks[i].size, list->chunks[i].first);
                mutex_unlock(&mutex_dynarec);
                return (uintptr_t)ret;
            }
        }
        // check if new
        if(!list->chunks[i].size) {
            /* Injection de panne (D2_JITFAILAFTER) — voir l'en-tete plus haut.
             * Place ICI, sur la CREATION DE CHUNK, et pas sur le service depuis
             * un chunk existant : c'est la seule facon de modeliser une arene
             * FINIE, donc de rendre l'espace libere par l'eviction reellement
             * utile. Un refus place plus bas refuserait aussi les allocations
             * que l'eviction vient de rendre possibles, et le banc ne
             * prouverait rien. */
            if(dyn86_failafter < 0) {
                const char* e = getenv("WX86_JITFAILAFTER");
                if(!e) e = getenv("D2_JITFAILAFTER");
                dyn86_failafter = e ? atoi(e) : 0;
                const char* c = getenv("WX86_JITCHUNK_KB");
                if(!c) c = getenv("D2_JITCHUNK_KB");
                dyn86_chunksz = c ? ((size_t)atoi(c) << 10) : (size_t)DYNMMAPSZ;
                if(!dyn86_chunksz) dyn86_chunksz = DYNMMAPSZ;
                if(dyn86_failafter > 0 || c)
                    fprintf(stderr, "[jit] INJECTION: arene bornee a %d chunks de %zu Ko (= %zu Ko)\n",
                               dyn86_failafter, dyn86_chunksz>>10,
                               (size_t)dyn86_failafter*(dyn86_chunksz>>10));
            }
            if(dyn86_failafter > 0 && (int)dyn86_chunks >= dyn86_failafter) {
                ++dyn86_jit_allocfail;
                mutex_unlock(&mutex_dynarec);
                return 0;
            }
            // alloc a new block, aversized or not, we are at the end of the list
            size_t allocsize = (sz>dyn86_chunksz)?sz:dyn86_chunksz;
            // allign sz with pagesize
            allocsize = (allocsize+(box86_pagesize-1))&~(box86_pagesize-1);
            #ifndef USE_MMAP
            void *p = NULL;
            if(!(p=box_memalign(box86_pagesize, allocsize))) {
                dynarec_log(LOG_INFO, "Cannot create dynamic map of %zu bytes\n", allocsize);
                ++dyn86_jit_allocfail;
                mutex_unlock(&mutex_dynarec);
                return 0;
            }
            mprotect(p, allocsize, PROT_READ | PROT_WRITE | PROT_EXEC);
            #else
            void* p = mmap(NULL, allocsize, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
            if(p==MAP_FAILED) {
                dynarec_log(LOG_INFO, "Cannot create dynamic map of %zu bytes (%s)\n", allocsize, strerror(errno));
                ++dyn86_jit_allocfail;
                mutex_unlock(&mutex_dynarec);
                return 0;
            }
            #ifdef MADV_HUGEPAGE
            madvise(p, allocsize, MADV_HUGEPAGE);
            #endif
            #endif
#ifdef TRACE_MEMSTAT
            dynarec_allocated += allocsize;
#endif
            ++dyn86_chunks;
            /* Trace du banc, UNIQUEMENT quand une injection est armee : c'est
             * elle qui donne le nombre de chunks reellement consommes par un
             * scenario, donc la valeur a donner a D2_JITFAILAFTER pour placer
             * la borne juste en dessous. Sans elle il faudrait chercher ce
             * reglage a l'aveugle. */
            if(dyn86_failafter > 0)
                fprintf(stderr, "[jit] arene: chunk %u ouvert (%zu Ko au total)\n",
                           dyn86_chunks, (size_t)dyn86_chunks*(dyn86_chunksz>>10));
            list->chunks[i].block = p;
            list->chunks[i].first = p;
            list->chunks[i].size = allocsize;
            // setup marks
            blockmark_t* m = (blockmark_t*)p;
            m->prev.x32 = 0;
            m->next.fill = 0;
            m->next.size = allocsize-2*sizeof(blockmark_t);
            blockmark_t* n = NEXT_BLOCK(m);
            n->next.x32 = 0;
            n->prev.fill = 0;
            n->prev.size = m->next.size;
            // alloc 1st block
            void* ret  = allocBlock(list->chunks[i].block, p, size, NULL);
            list->chunks[i].maxfree = getMaxFreeBlock(list->chunks[i].block, list->chunks[i].size, NULL);
            if(list->chunks[i].maxfree)
                list->chunks[i].first = getNextFreeBlock(m);
            mutex_unlock(&mutex_dynarec);
            /* D2Vita: setProtection AFTER unlock — the exact shape customMalloc
             * already uses in this file. Taking mutex_prot while holding
             * mutex_dynarec would close a deadlock cycle with the free side
             * (unprotectDB holds mutex_prot -> MarkDynablock ->
             * FreeInvalidDynablock takes mutex_dyndump -> FreeDynarecMap takes
             * mutex_dynarec). Bookkeeping-only (rbtree), the real permissions
             * came from the mmap flags; nothing reads memprot for chain ops. */
            setProtection((uintptr_t)p, allocsize, PROT_READ | PROT_WRITE | PROT_EXEC);
            return (uintptr_t)ret;
        }
        // next chunk...
        ++i;
        if(i==NCHUNK) {
            i = 0;
            if(!list->next)
                list->next = (mmaplist_t*)box_calloc(1, sizeof(mmaplist_t));
            list = list->next;
        }
    }
}

void FreeDynarecMap(uintptr_t addr)
{
    if(!addr)
        return;

    int i= 0;
    mutex_lock(&mutex_dynarec);     // D2Vita (audit T12 §11.6) — leaf, see AllocDynarecMap
    mmaplist_t* list = mmaplist;

    while(list) {
        if ((addr>(uintptr_t)list->chunks[i].block)
         && (addr<((uintptr_t)list->chunks[i].block+list->chunks[i].size))) {
            void* sub = (void*)(addr-sizeof(blockmark_t));
            size_t newfree = freeBlock(list->chunks[i].block, sub, &list->chunks[i].first);
            if(list->chunks[i].maxfree < newfree)
                list->chunks[i].maxfree = newfree;
            mutex_unlock(&mutex_dynarec);
            return;
        }
        ++i;
        if(i==NCHUNK) {
            i = 0;
            list = list->next;
        }
    }
    mutex_unlock(&mutex_dynarec);
}

/* D2Vita (lot eviction JIT) — enumerateur de victimes.
 *
 * Il n'existe AUCUN registre des dynablocks : getDB parcourt la table de
 * sauts (indexee par adresse INVITE, donc inutilisable pour « donne-moi des
 * blocs a jeter »), et rien d'autre ne les liste. Mais l'arene elle-meme les
 * contient tous : chaque sous-bloc ALLOUE commence par le pointeur `self`
 * que FillBlock/CreateEmptyBlock y ecrivent (dynarec_arm.c). C'est exactement
 * la marche que FindDynablockFromNativeAddress fait deja, a ceci pres qu'elle
 * cherche une adresse et que celle-ci ramasse.
 *
 * `next.fill` est teste, lui. FindDynablockFromNativeAddress ne le teste pas
 * et rend donc le `self` RANCE d'un sous-bloc libere (defaut latent signale
 * mais hors perimetre ici) ; un enumerateur qui ferait pareil rendrait des
 * pointeurs pendants a la pelle des la premiere eviction.
 *
 * CURSEUR TOURNANT persistant : sans donnee de chaleur — et en ajouter une
 * demanderait d'emettre du code, ce qu'un mecanisme de survie n'a pas le droit
 * de faire — le choix des victimes est une horloge sans bit de reference,
 * c'est-a-dire du remplacement quasi aleatoire. C'est le compromis assume :
 * un bloc chaud evince par erreur coute UNE retraduction, pas une session.
 *
 * VERROU : prend mutex_dynarec (l'arene bouge sous Alloc/FreeDynarecMap).
 * L'appelant (dynablock.c) tient deja mutex_dyndump, donc l'arete empruntee
 * est dyndump -> dynarec, celle que FreeDynablock -> FreeDynarecMap utilise
 * depuis toujours. Aucun ordre nouveau. Les champs de dynablock_t lus ici
 * (`gone`) sont ecrits sous dyndump, que l'appelant tient : coherent. */
/* CURSEUR : (maillon, indice de chunk) — et surtout PAS un offset d'octet
 * dans le chunk. La premiere version en gardait un, et c'etait faux de facon
 * instructive : la recuperation qu'on vient de declencher appelle freeBlock,
 * qui COALESCE les marqueurs voisins. L'offset memorise tombait alors au
 * milieu d'une zone fusionnee, donc sur un blockmark_t qui n'existe plus ;
 * NEXT_BLOCK y lisait des tailles arbitraires et la marche partait dans le
 * decor — au mieux elle ne rendait plus aucune victime (mesure : 64 blocs
 * retires en tout et pour tout sur 250 tours, soit 51 Kio rendus sur une
 * arene de 2304 Kio, c'est-a-dire une eviction qui ne servait a rien), au
 * pire elle aurait rendu des dynablock_t* fabriques a partir d'octets
 * quelconques. Un chunk se parcourt donc TOUJOURS depuis son debut, ou la
 * chaine de marqueurs est valide par construction ; seul l'indice de chunk
 * tourne. */
/* JIT CLOCK (WarCraft III build 11). The victim walk above has no idea
 * which blocks are hot, so once the pool is full each eviction throws out
 * random blocks, hot ones included; they come straight back, need room, and
 * evict more: K's build 9 battle log shows 20 000 retranslations in 10 s.
 * A second-chance clock fixes that without emitting a single instruction:
 * dyn86_jit_clock_tick() (called once per presented frame) walks a slice of
 * the arena and UNLINKS each block it passes (its jump-table entry goes to
 * the block's own jmpnext stub, exactly box86's "dirty" mark), setting
 * db->probed. Any transfer to it then goes through LinkNext -> DBGetBlock,
 * which clears probed and, the hash being unchanged, links it back. A block
 * still probed one lap later was not reached for a whole lap: cold. Blocks
 * returned to by D2_CALLRET's native return addresses are not seen by this
 * (they bypass the table); that only costs a retranslation, the grace period
 * keeps it safe. WX86_JITCLOCK=0 = the old walk. */
int      dyn86_clock_live = 0;       /* the tick has run at least once */
uint32_t dyn86_clock_fallbacks = 0;  /* walks that found no cold block */
uint32_t dyn86_clock_probes = 0;     /* unlinks made by the tick */
static mmaplist_t* dyn86_tick_list = NULL;
static int         dyn86_tick_chunk = 0;
uintptr_t getJumpAddress64(uintptr_t addr);
static int dyn86_clock_cold(dynablock_t* db)
{
    return db->probed && getJumpAddress64((uintptr_t)db->x86_addr) != (uintptr_t)db->block;
}
static uint64_t dyn86_clock_now_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000u + (uint64_t)ts.tv_nsec/1000u;
}
static mmaplist_t* dyn86_walk_list = NULL;
static int         dyn86_walk_chunk = 0;

int dyn86_jit_collect_victims(dynablock_t** out, int max)
{
    if(!out || max<=0)
        return 0;
    int n = 0;
    mutex_lock(&mutex_dynarec);
    if(!mmaplist) { mutex_unlock(&mutex_dynarec); return 0; }
    /* Revalidation du maillon : l'arene peut avoir change entre deux appels,
     * et un pointeur garde tel quel serait un pari. */
    mmaplist_t* list = mmaplist; int ci = 0;
    if(dyn86_walk_list) {
        for(mmaplist_t* v = mmaplist; v; v = v->next)
            if(v==dyn86_walk_list) { list = v; ci = dyn86_walk_chunk; break; }
    }
    if(ci < 0 || ci >= NCHUNK) ci = 0;
    /* AU PLUS UN TOUR, et la borne doit etre EXACTEMENT le nombre de creneaux
     * existants — pas un multiple « genereux ».
     *
     * La version precedente bornait a NCHUNK*8 creneaux. Quand l'arene a
     * moins de blocs vivants que `max`, la marche faisait alors PLUSIEURS
     * TOURS et ramassait les memes blocs plusieurs fois. La vague contenait
     * des doublons, donc la recuperation liberait deux fois le meme bloc, et
     * empoisonnait de la memoire deja reattribuee a une autre traduction.
     * Mesure : inoffensif avec 9 chunks (la vague de 64 tenait dans un tour),
     * 3 plantages sur 4 avec 3 chunks — un usage-apres-liberation bien reel,
     * attrape par l'empoisonnement, pas par le raisonnement. */
    int nslots = 0;
    for(mmaplist_t* v = mmaplist; v; v = v->next) nslots += NCHUNK;
    { static int on = -1;
      if(on < 0) { const char* e = getenv("WX86_JITCLOCK"); on = (e && *e=='0') ? 0 : 1; }
      if(on) dyn86_clock_live = 1; }
    mmaplist_t* list0 = list; int ci0 = ci;
    for(int pass = 0; pass < 2 && n == 0; ++pass) {
    if(pass) { list = list0; ci = ci0; if(!dyn86_clock_live) break; ++dyn86_clock_fallbacks; }
    int visited = 0;
    const int maxvisit = nslots;
    while(n < max && visited < maxvisit) {
        blocklist_t* ch = &list->chunks[ci];
        ++visited;
        if(ch->size && ch->block) {
            blockmark_t* m = (blockmark_t*)ch->block;
            blockmark_t* last = LAST_BLOCK(ch->block, ch->size);
            while(m < last && n < max) {
                if(m->next.fill) {
                    dynablock_t* db = *(dynablock_t**)((uintptr_t)m + sizeof(blockmark_t));
                    /* `gone` : deja retire (il appartient a une chaine
                     * `previous` ou a la vague en cours). Le reprendre le
                     * mettrait deux fois dans la file, donc le libererait
                     * deux fois. */
                    if(db && !db->gone && db->actual_block) {
                        /* JIT clock (dyn86_jit_clock_tick): once the tick
                         * runs, only blocks it unlinked and that nobody
                         * reached since are taken; a fallback pass takes
                         * anything if that finds none (progress first). */
                        if(!dyn86_clock_live || pass || dyn86_clock_cold(db))
                            out[n++] = db;
                    }
                }
                m = NEXT_BLOCK(m);
            }
        }
        ++ci;
        if(ci >= NCHUNK) { ci = 0; list = list->next ? list->next : mmaplist; }
    }
    }
    dyn86_walk_list = list; dyn86_walk_chunk = ci;
    mutex_unlock(&mutex_dynarec);
    return n;
}

extern int dyn86_evict_armed(void);
extern uint32_t dyn86_ev_rounds;
void dyn86_jit_clock_tick_locked(int chunks);
/* One slice of the clock: `chunks` arena chunks per call, probing every live
 * block in them. Inert until the first eviction round (the pool is full from
 * then on, on the Vita for good) and with WX86_JITCLOCK=0. */
void dyn86_jit_clock_tick(int chunks)
{
    static int on = -1;
    if(on < 0) { const char* e = getenv("WX86_JITCLOCK"); on = (e && *e=='0') ? 0 : 1; }
    (void)chunks;
    if(!on || !dyn86_ev_rounds || !dyn86_evict_armed())
        return;
    mutex_lock(&my_context->mutex_dyndump);
    dyn86_jit_clock_tick_locked(0);
    mutex_unlock(&my_context->mutex_dyndump);
}
/* Same, with mutex_dyndump already held (the eviction round itself: the clock
 * must keep turning while the game translates without presenting frames,
 * i.e. during loads; K's build 11 log: probes froze at the first load and
 * every later eviction fell back to random victims). */
/* Build 13: the hand turns on a TIMER, WX86_JITCLOCK_RATE chunks per second
 * (default 10: one lap of the 16 MiB pool in ~26 s; build 11/12 did 15 per
 * second at a steady 15 fps), whoever calls it: the presented frame or the
 * eviction round. Build 11 ticked per frame only, so loads froze it; build 12
 * let the eviction walk probe whenever frames were 0.5 s apart, and on K's
 * Vita a slow battle after a save load turned that into a spiral (fast hand
 * -> warm code evicted -> slower frames -> hand stays fast; 4-6k
 * retranslations per 10 s for minutes). `chunks` > 0 forces that many. */
void dyn86_jit_clock_tick_locked(int chunks)
{
    static int on = -1, rate = 10;
    static uint64_t last = 0;
    if(on < 0) {
        const char* e = getenv("WX86_JITCLOCK"); on = (e && *e=='0') ? 0 : 1;
        const char* r = getenv("WX86_JITCLOCK_RATE"); if(r && atoi(r) > 0) rate = atoi(r);
    }
    if(!on)
        return;
    if(chunks <= 0) {
        const uint64_t now = dyn86_clock_now_us();
        const uint64_t period = 1000000u / (uint64_t)rate;
        if(!last) last = now - period;
        if(now - last < period)
            return;
        uint64_t n = (now - last) / period;
        if(n > 32) { n = 32; last = now; } else last += n * period;
        chunks = (int)n;
    }
    mutex_lock(&mutex_dynarec);
    if(mmaplist) {
        mmaplist_t* list = mmaplist; int ci = 0;
        if(dyn86_tick_list)
            for(mmaplist_t* v = mmaplist; v; v = v->next)
                if(v==dyn86_tick_list) { list = v; ci = dyn86_tick_chunk; break; }
        if(ci < 0 || ci >= NCHUNK) ci = 0;
        for(int k = 0; k < chunks; ++k) {
            blocklist_t* ch = &list->chunks[ci];
            if(ch->size && ch->block) {
                blockmark_t* m = (blockmark_t*)ch->block;
                blockmark_t* last = LAST_BLOCK(ch->block, ch->size);
                while(m < last) {
                    if(m->next.fill) {
                        dynablock_t* db = *(dynablock_t**)((uintptr_t)m + sizeof(blockmark_t));
                        if(db && !db->gone && db->done && db->block && db->jmpnext && !db->probed
                           && db->actual_block == (void*)((uintptr_t)m + sizeof(blockmark_t))) {
                            db->probed = 1;
                            if(setJumpTableIfRef(db->x86_addr, db->jmpnext, db->block)) ++dyn86_clock_probes;
                        }
                    }
                    m = NEXT_BLOCK(m);
                }
            }
            ++ci;
            if(ci >= NCHUNK) { ci = 0; list = list->next ? list->next : mmaplist; }
        }
        dyn86_tick_list = list; dyn86_tick_chunk = ci;
        dyn86_clock_live = 1;
    }
    mutex_unlock(&mutex_dynarec);
}

/* Recensement de l'arene (diagnostic d'eviction, build 16) : ou va la
 * place quand l'eviction ne trouve que quelques dizaines de victimes ? */
void dyn86_arena_census(char* out, int n)
{
    size_t tot=0, fre=0, live=0, gone=0, noself=0, maxfr=0; int nlive=0, ngone=0, nfree=0;
    mutex_lock(&mutex_dynarec);
    for(mmaplist_t* list = mmaplist; list; list = list->next)
        for(int ci=0; ci<NCHUNK; ++ci) {
            blocklist_t* ch = &list->chunks[ci];
            if(!ch->size || !ch->block) continue;
            tot += ch->size;
            blockmark_t* m = (blockmark_t*)ch->block;
            blockmark_t* last = LAST_BLOCK(ch->block, ch->size);
            while(m < last) {
                size_t sz = m->next.size;
                if(!m->next.fill) { fre += sz; ++nfree; if(sz>maxfr) maxfr=sz; }
                else {
                    dynablock_t* db = *(dynablock_t**)((uintptr_t)m + sizeof(blockmark_t));
                    if(!db || db->actual_block != (void*)((uintptr_t)m + sizeof(blockmark_t))) noself += sz;
                    else if(db->gone) { gone += sz; ++ngone; }
                    else { live += sz; ++nlive; }
                }
                m = NEXT_BLOCK(m);
            }
        }
    mutex_unlock(&mutex_dynarec);
    /* %u et non %zu : le printf de newlib sur Vita ne connait pas %zu (build 16 : « zuKo »). */
    snprintf(out, n, "arene %uKo: vivants %d/%uKo, gone %d/%uKo, sans-self %uKo, libre %d trous/%uKo (max %uKo)",
             (unsigned)(tot>>10), nlive, (unsigned)(live>>10), ngone, (unsigned)(gone>>10), (unsigned)(noself>>10), nfree, (unsigned)(fre>>10), (unsigned)(maxfr>>10));
}

extern void wx86_vita_progress_c(const char* msg);   /* src/platform/vita_host.cpp */
/* Out of host heap for a jump table: said on stderr and in the Vita progress
 * file, since the code at that address can then not be run. */
static void jt_oom_note(uintptr_t addr, unsigned kb)
{
    char m[160];
    snprintf(m, sizeof m, "dynarec: no host memory for the jump table of guest address %p (%u KiB of tables so far)", (void*)addr, kb);
    fprintf(stderr, "%s\n", m);
    wx86_vita_progress_c(m);
}

#ifndef WX86_JT3
static uintptr_t getDBSize(uintptr_t addr, size_t maxsize, dynablock_t** db)
{
    uintptr_t idx1 = addr>>JMPTABL_SHIFT;
    uintptr_t idx0 = addr&JMPTABLE_MASK;
    *db = *(dynablock_t**)(box86_jmptbl[idx1][idx0]- sizeof(void*));
    if(*db)
        return addr+1;
    uintptr_t* block = box86_jmptbl[idx1];
    if(block == box86_jmptbl_default)
        return ((idx1+1)<<JMPTABL_SHIFT);
    maxsize+=idx0;  // need to adjust maxsize to "end in current block"
    if (maxsize>JMPTABLE_MASK)
        maxsize = JMPTABLE_MASK;
    while(block[idx0]==(uintptr_t)arm_next) {
        ++idx0;
        if(idx0>maxsize)
            return (addr&~JMPTABLE_MASK)+idx0;
    }
    *db = *(dynablock_t**)(block[idx0]- sizeof(void*));
    return (addr&~JMPTABLE_MASK)+idx0+1;
}

// each dynmap is 64k of size

void addDBFromAddressRange(uintptr_t addr, size_t size)
{
    dynarec_log(LOG_DEBUG, "addDBFromAddressRange %p -> %p\n", (void*)addr, (void*)(addr+size-1));
    // do nothing, dynablock are allowed based on memory protection flags
}

int isJmpTableEmpty(uintptr_t *tbl)
{
    if(tbl == box86_jmptbl_default)
        return 0;
    for(int i=0; i<1<<JMPTABL_SHIFT; ++i)
        if(tbl[i]!=(uintptr_t)arm_next)
            return 0;
    return 1;
}

void cleanDBFromAddressRange(uintptr_t addr, size_t size, int destroy)
{
    uintptr_t start_addr = my_context?((addr<my_context->max_db_size)?0:(addr-my_context->max_db_size)):addr;
    dynarec_log(LOG_DEBUG, "cleanDBFromAddressRange %p/%p -> %p %s\n", (void*)addr, (void*)start_addr, (void*)(addr+size-1), destroy?"destroy":"mark");
    dynablock_t* db = NULL;
    uintptr_t end = addr+size;
    while (start_addr<end) {
        start_addr = getDBSize(start_addr, end-start_addr, &db);
        if(db) {
            if(destroy)
                FreeRangeDynablock(db, addr, size);
            else
                // need_lock=1 : ce chemin tient mutex_prot, JAMAIS mutex_dyndump.
                MarkRangeDynablock(db, addr, size, 1);
        }
    }
    if(destroy) {
        // remove only if the page is destroyed
        start_addr = addr;
        end = addr+size;
        while (start_addr<end) {
            uintptr_t* tbl;
            if(isJmpTableEmpty((tbl=box86_jmptbl[start_addr>>JMPTABL_SHIFT]))) {
                if(arm_lock_storeifref(&box86_jmptbl[start_addr>>JMPTABL_SHIFT], box86_jmptbl_default, tbl)==box86_jmptbl_default) {
                    box_free(tbl);
                    #ifdef TRACE_MEMSTAT
                    jmptbl_allocated -= (1<<JMPTABL_SHIFT)*sizeof(uintptr_t);
                    #endif
                }
            }
            start_addr += 1<<JMPTABL_SHIFT;
        }
    }
}

/* WC3 Vita build 4: level-2 jump tables (256 KiB each, one per 64 KiB guest
 * page holding translated code) come out of the host newlib heap. Counted
 * always, so the Vita log can say how many exist and how many failed.
 * WX86_JMPTBL_MAX=<n> refuses table n+1 onwards (host reproduction of a
 * console that ran out of heap). */
static volatile unsigned jmptbl_lv2 = 0, jmptbl_oom = 0;
static unsigned jmptbl_cap = 0;   /* 0 = not read yet */
unsigned d2rt_box86_jmptbl_tables(void) { return jmptbl_lv2; }
unsigned d2rt_box86_jmptbl_fails(void)  { return jmptbl_oom; }
unsigned d2rt_box86_jmptbl_heap_kb(void) { return jmptbl_lv2 * (((1u<<JMPTABL_SHIFT)*sizeof(uintptr_t)) >> 10); }

/* Returns the slot for (idx1, idx0) in a page-private level-2 table, or NULL
 * when that table could not be allocated.
 *
 * NEVER return a pointer into box86_jmptbl_default here: every page without
 * its own table shares that one array, and its entries all equal arm_next, so
 * a caller's store-if-ref(arm_next) SUCCEEDS on it. The block for guest
 * address A then answers for A + k*64 KiB on every other table-less page. On
 * the Vita (build 3) that sent `call 0x0105ffa0` into the code translated for
 * 0x0104ffa0 -- WC3's terrain-table constructor -- which recursed forever,
 * reloading TerrainArt\Terrain.slk each time until guest memory ran out
 * (Storm "SFile.cpp(4072) error 8"). */
static uintptr_t *create_jmptbl(uintptr_t idx0, uintptr_t idx1)
{
    if(box86_jmptbl[idx1] == box86_jmptbl_default) {
        if(!jmptbl_cap) {
            const char* e = getenv("WX86_JMPTBL_MAX");
            jmptbl_cap = (e && atoi(e) > 0) ? (unsigned)atoi(e) : 0xffffffffu;
        }
        uintptr_t* tbl = (jmptbl_lv2 < jmptbl_cap)
            ? (uintptr_t*)box_malloc((1<<JMPTABL_SHIFT)*sizeof(uintptr_t)) : NULL;
        if(!tbl) {
            if(__atomic_add_fetch(&jmptbl_oom, 1, __ATOMIC_RELAXED) <= 4)
                jt_oom_note((uintptr_t)(idx1<<JMPTABL_SHIFT), jmptbl_lv2 * 256);
            return NULL;
        }
        for(int i=0; i<(1<<JMPTABL_SHIFT); ++i)
            tbl[i] = (uintptr_t)arm_next;
        if(arm_lock_storeifref(&box86_jmptbl[idx1], tbl, box86_jmptbl_default)!=tbl)
            box_free(tbl);
        else {
            __atomic_add_fetch(&jmptbl_lv2, 1, __ATOMIC_RELAXED);
#ifdef TRACE_MEMSTAT
            jmptbl_allocated += (1<<JMPTABL_SHIFT)*sizeof(uintptr_t);
            ++jmptbl_allocated1;
#endif
        }
    }
    return &box86_jmptbl[idx1][idx0];
}

int addJumpTableIfDefault(void* addr, void* jmp)
{
    uintptr_t idx1, idx0;
    idx1 = (((uintptr_t)addr)>>JMPTABL_SHIFT)&JMPTABLE_MASK;
    idx0 = (((uintptr_t)addr)               )&JMPTABLE_MASK;
    uintptr_t* slot = create_jmptbl(idx0, idx1);
    if(!slot) return 0;   /* not published: the caller frees the block */
    return (arm_lock_storeifref(slot, jmp, arm_next)==jmp)?1:0;
}
void setJumpTableDefault(void* addr)
{
    uintptr_t idx1, idx0;
    idx1 = (((uintptr_t)addr)>>JMPTABL_SHIFT)&JMPTABLE_MASK;
    if(box86_jmptbl[idx1] == box86_jmptbl_default)
        return;
    idx0 = (((uintptr_t)addr)    )&JMPTABLE_MASK;
    arm_lock_stored(&box86_jmptbl[idx1][idx0], (uintptr_t)arm_next);
}
void setJumpTableDefaultRef(void* addr, void* jmp)
{
    uintptr_t idx1, idx0;
    idx1 = (((uintptr_t)addr)>>JMPTABL_SHIFT)&JMPTABLE_MASK;
    if(box86_jmptbl[idx1] == box86_jmptbl_default)
        return;
    idx0 = (((uintptr_t)addr)    )&JMPTABLE_MASK;
    arm_lock_storeifref(&box86_jmptbl[idx1][idx0], arm_next, jmp);
}
int setJumpTableIfRef(void* addr, void* jmp, void* ref)
{
    uintptr_t idx1, idx0;
    idx1 = (((uintptr_t)addr)>>JMPTABL_SHIFT)&JMPTABLE_MASK;
    idx0 = (((uintptr_t)addr)               )&JMPTABLE_MASK;
    uintptr_t* slot = create_jmptbl(idx0, idx1);
    if(!slot) return 0;
    return (arm_lock_storeifref(slot, jmp, ref)==jmp)?1:0;
}
int isJumpTableDefault(void* addr)
{
    uintptr_t idx1, idx0;
    idx1 = (((uintptr_t)addr)>>JMPTABL_SHIFT)&JMPTABLE_MASK;
    if(box86_jmptbl[idx1] == box86_jmptbl_default)
        return 1;
    idx0 = (((uintptr_t)addr)               )&JMPTABLE_MASK;
    return (box86_jmptbl[idx1][idx0]==(uintptr_t)arm_next)?1:0;
}
uintptr_t getJumpTable()
{
    return (uintptr_t)box86_jmptbl;
}

uintptr_t getJumpTableAddress(uintptr_t addr)
{
    uintptr_t idx1, idx0;
    idx1 = ((addr)>>JMPTABL_SHIFT)&JMPTABLE_MASK;
    idx0 = ((addr)               )&JMPTABLE_MASK;
    uintptr_t* slot = create_jmptbl(idx0, idx1);
    /* No table: hand the emitted code the shared default slot. It only ever
     * READS it (always arm_next, so the jump goes through LinkNext and finds
     * the right block); nothing may write there, see create_jmptbl. */
    return slot ? (uintptr_t)slot : (uintptr_t)&box86_jmptbl_default[idx0];
}
/* Diagnostic census (slow: walks every allocated table). pages = level-2
 * tables, sub4k = 4 KiB sub-pages holding at least one entry, entries = slots
 * not on arm_next. */
void d2rt_box86_jmptbl_census(unsigned* pages, unsigned* chunks, unsigned* entries)
{
    unsigned np = 0, ne = 0, nc[5] = {0};   /* chunks of 64 B .. 1 KiB */
    for(size_t i1 = 0; i1 < JMPTABL_SIZE; ++i1) {
        uintptr_t* t = box86_jmptbl[i1];
        if(t == box86_jmptbl_default) continue;
        ++np;
        for(int g = 0; g < 5; ++g) {
            const size_t cs = (size_t)64 << g;
            for(size_t s0 = 0; s0 < (1<<JMPTABL_SHIFT); s0 += cs) {
                unsigned here = 0;
                for(size_t i0 = s0; i0 < s0 + cs; ++i0)
                    if(t[i0] != (uintptr_t)arm_next) { ++here; if(g==0) ++ne; }
                if(here) ++nc[g];
            }
        }
    }
    if(pages) *pages = np;
    if(chunks) for(int g = 0; g < 5; ++g) chunks[g] = nc[g];
    if(entries) *entries = ne;
}
int isJumpTableSharedSlot(uintptr_t p)
{
    return p >= (uintptr_t)box86_jmptbl_default
        && p <  (uintptr_t)(box86_jmptbl_default + (1<<JMPTABL_SHIFT));
}

dynablock_t* getDB(uintptr_t addr)
{
    uintptr_t idx1, idx0;
    idx1 = ((addr)>>JMPTABL_SHIFT)&JMPTABLE_MASK;
    idx0 = ((addr)               )&JMPTABLE_MASK;
    uintptr_t ret = (uintptr_t)box86_jmptbl[idx1][idx0];

    return *(dynablock_t**)(ret - sizeof(void*));
}

extern int dyn86_link_direct(void);   // D2Vita scheduler quantum mode (src/dynarec86/dyn86.c)
int getNeedTest(uintptr_t addr)
{
    uintptr_t idx1, idx0;
    idx1 = ((addr)>>JMPTABL_SHIFT)&JMPTABLE_MASK;
    idx0 = ((addr)               )&JMPTABLE_MASK;
    uintptr_t ret = (uintptr_t)box86_jmptbl[idx1][idx0];
    dynablock_t* db = *(dynablock_t**)(ret - sizeof(void*));
    // D2Vita quantum mode: entries deliberately point at the block's jmpnext
    // stub so every chain funnels through LinkNext (periodic preemption).
    // That is NOT the dirty marker — don't re-hash on every chain.
    if(db && !dyn86_link_direct() && ret==(uintptr_t)db->jmpnext && !db->dirty)
        return 0;
    return db?((ret!=(uintptr_t)db->block)?1:0):0;
}

uintptr_t getJumpAddress64(uintptr_t addr)
{
    uintptr_t idx1, idx0;
    idx1 = ((addr)>>JMPTABL_SHIFT)&JMPTABLE_MASK;
    idx0 = ((addr)               )&JMPTABLE_MASK;
    return (uintptr_t)box86_jmptbl[idx1][idx0];
}

#else /* WX86_JT3 */
/* ---- three-level jump table (WarCraft III Vita build 4) ---------------------
 * L1[addr>>16] -> L2[(addr>>6) & 1023] -> L3[addr & 63] = ARM code to run.
 *
 * The stock two-level table spends a 256 KiB level-2 array on every 64 KiB
 * guest page that holds ANY translated code: 171 of them (43 MiB of host
 * heap) by WC3's main menu, for 47 000 live entries (185 KiB of payload).
 * The Vita's whole newlib heap is 48 MiB. Here a page costs 4 KiB (L2) and
 * each 64-byte run of guest code that has an entry point costs 256 B (L3):
 * 5.5 MiB for the same menu. Measured on the host with the census below.
 *
 * Pages and runs without a table of their own point at jt_l2_default /
 * jt_l3_default, which are READ-ONLY: nothing may ever be stored into them
 * (that sharing is what aliased guest pages on the Vita in build 3, see
 * create_jmptbl). Tables are never freed: emitted code embeds L3 slot
 * addresses (getJumpTableAddress), so freeing one would leave dangling
 * pointers in other blocks; the memory involved is small.
 * The emitted lookup (dynarec_arm_helper.c) is one UBFX + one LDR longer. */
#define JT_L3_MASK  ((1<<JT_L3_SHIFT)-1)
#define JT_L2_SIZE  (1<<(JMPTABL_SHIFT-JT_L3_SHIFT))
#define JT_L3_SIZE  (1<<JT_L3_SHIFT)
static uintptr_t* jt_l2_default[JT_L2_SIZE];
static uintptr_t  jt_l3_default[JT_L3_SIZE];
static volatile unsigned char jt_lock = 0;   /* leaf spinlock: only malloc runs under it */
static uint8_t*   jt_slab = NULL;
static size_t     jt_slab_left = 0;
static volatile unsigned jt_n2 = 0, jt_n3 = 0, jt_oom = 0, jt_slab_kb = 0;
static unsigned   jt_cap = 0;   /* WX86_JMPTBL_MAX: cap on L3 tables (host test) */
unsigned d2rt_box86_jmptbl_tables(void) { return jt_n3; }
unsigned d2rt_box86_jmptbl_fails(void)  { return jt_oom; }
unsigned d2rt_box86_jmptbl_heap_kb(void) { return jt_slab_kb; }

#define JT_L2(a)  (((uintptr_t**)box86_jmptbl[(uintptr_t)(a)>>JMPTABL_SHIFT]))
#define JT_I2(a)  ((((uintptr_t)(a))>>JT_L3_SHIFT)&(JT_L2_SIZE-1))
#define JT_I0(a)  (((uintptr_t)(a))&JT_L3_MASK)
static inline uintptr_t* jt_l3_of(uintptr_t a) { return JT_L2(a)[JT_I2(a)]; }
static inline uintptr_t  jt_get(uintptr_t a)   { return jt_l3_of(a)[JT_I0(a)]; }

/* 64 KiB slabs, carved; never freed. Called with jt_lock held. */
static void* jt_alloc(size_t n)
{
    if(jt_slab_left < n) {
        uint8_t* sl = (uint8_t*)box_malloc(64<<10);
        if(!sl) return NULL;
        jt_slab = sl; jt_slab_left = 64<<10; jt_slab_kb += 64;
    }
    void* r = jt_slab; jt_slab += n; jt_slab_left -= n;
    return r;
}

/* The page-private slot for addr, or NULL when no memory was left for it. */
static uintptr_t *create_jmptbl(uintptr_t addr)
{
    uintptr_t* l3 = jt_l3_of(addr);
    if(l3 != jt_l3_default)
        return &l3[JT_I0(addr)];
    while(__atomic_test_and_set(&jt_lock, __ATOMIC_ACQUIRE)) sched_yield();
    if(!jt_cap) {
        const char* e = getenv("WX86_JMPTBL_MAX");
        jt_cap = (e && atoi(e) > 0) ? (unsigned)atoi(e) : 0xffffffffu;
    }
    uintptr_t** l2 = JT_L2(addr);
    if(l2 == jt_l2_default) {
        uintptr_t** n2 = (uintptr_t**)jt_alloc(JT_L2_SIZE*sizeof(uintptr_t*));
        if(!n2) goto oom;
        for(int i=0; i<JT_L2_SIZE; ++i) n2[i] = jt_l3_default;
        __atomic_store_n(&box86_jmptbl[addr>>JMPTABL_SHIFT], (uintptr_t*)n2, __ATOMIC_RELEASE);
        l2 = n2; ++jt_n2;
    }
    l3 = l2[JT_I2(addr)];
    if(l3 == jt_l3_default) {
        uintptr_t* n3 = (jt_n3 < jt_cap) ? (uintptr_t*)jt_alloc(JT_L3_SIZE*sizeof(uintptr_t)) : NULL;
        if(!n3) goto oom;
        for(int i=0; i<JT_L3_SIZE; ++i) n3[i] = (uintptr_t)arm_next;
        __atomic_store_n(&l2[JT_I2(addr)], n3, __ATOMIC_RELEASE);
        l3 = n3; ++jt_n3;
    }
    __atomic_clear(&jt_lock, __ATOMIC_RELEASE);
    return &l3[JT_I0(addr)];
oom:
    __atomic_clear(&jt_lock, __ATOMIC_RELEASE);
    if(__atomic_add_fetch(&jt_oom, 1, __ATOMIC_RELAXED) <= 4)
        jt_oom_note(addr, jt_slab_kb);
    return NULL;
}

/* Next address to look at from addr (bounded by maxsize), with *db set when
 * a published block starts right before the returned address. */
static uintptr_t getDBSize(uintptr_t addr, size_t maxsize, dynablock_t** db)
{
    *db = NULL;
    uintptr_t** l2 = JT_L2(addr);
    if(l2 == jt_l2_default)
        return ((addr>>JMPTABL_SHIFT)+1)<<JMPTABL_SHIFT;
    uintptr_t* l3 = l2[JT_I2(addr)];
    if(l3 == jt_l3_default)
        return (addr|JT_L3_MASK)+1;
    uintptr_t i0 = JT_I0(addr);
    uintptr_t last = (maxsize >= JT_L3_MASK) ? JT_L3_MASK : i0 + maxsize;   /* no wrap */
    if(last > JT_L3_MASK) last = JT_L3_MASK;
    for(; i0 <= last; ++i0)
        if(l3[i0] != (uintptr_t)arm_next) {
            *db = *(dynablock_t**)(l3[i0] - sizeof(void*));
            return (addr&~(uintptr_t)JT_L3_MASK)+i0+1;
        }
    return (addr&~(uintptr_t)JT_L3_MASK)+i0;
}

void addDBFromAddressRange(uintptr_t addr, size_t size)
{
    dynarec_log(LOG_DEBUG, "addDBFromAddressRange %p -> %p\n", (void*)addr, (void*)(addr+size-1));
}

void cleanDBFromAddressRange(uintptr_t addr, size_t size, int destroy)
{
    uintptr_t start_addr = my_context?((addr<my_context->max_db_size)?0:(addr-my_context->max_db_size)):addr;
    dynarec_log(LOG_DEBUG, "cleanDBFromAddressRange %p/%p -> %p %s\n", (void*)addr, (void*)start_addr, (void*)(addr+size-1), destroy?"destroy":"mark");
    dynablock_t* db = NULL;
    uintptr_t end = addr+size;
    while (start_addr<end) {
        uintptr_t next = getDBSize(start_addr, end-start_addr, &db);
        if(next <= start_addr) break;   /* wrapped past 4 GiB */
        start_addr = next;
        if(db) {
            if(destroy)
                FreeRangeDynablock(db, addr, size);
            else
                MarkRangeDynablock(db, addr, size, 1);
        }
    }
    /* Tables stay allocated (see the header comment). */
}

int addJumpTableIfDefault(void* addr, void* jmp)
{
    uintptr_t* slot = create_jmptbl((uintptr_t)addr);
    if(!slot) return 0;   /* not published: the caller frees the block */
    return (arm_lock_storeifref(slot, jmp, arm_next)==jmp)?1:0;
}
void setJumpTableDefault(void* addr)
{
    uintptr_t* l3 = jt_l3_of((uintptr_t)addr);
    if(l3 == jt_l3_default) return;
    arm_lock_stored(&l3[JT_I0(addr)], (uintptr_t)arm_next);
}
void setJumpTableDefaultRef(void* addr, void* jmp)
{
    uintptr_t* l3 = jt_l3_of((uintptr_t)addr);
    if(l3 == jt_l3_default) return;
    arm_lock_storeifref(&l3[JT_I0(addr)], arm_next, jmp);
}
int setJumpTableIfRef(void* addr, void* jmp, void* ref)
{
    uintptr_t* slot = create_jmptbl((uintptr_t)addr);
    if(!slot) return 0;
    return (arm_lock_storeifref(slot, jmp, ref)==jmp)?1:0;
}
int isJumpTableDefault(void* addr)
{
    return (jt_get((uintptr_t)addr)==(uintptr_t)arm_next)?1:0;
}
uintptr_t getJumpTable()
{
    return (uintptr_t)box86_jmptbl;
}
uintptr_t getJumpTableAddress(uintptr_t addr)
{
    uintptr_t* slot = create_jmptbl(addr);
    /* No memory: the shared default slot, which emitted code only reads
     * (always arm_next, so the jump goes through LinkNext). */
    return slot ? (uintptr_t)slot : (uintptr_t)&jt_l3_default[JT_I0(addr)];
}
int isJumpTableSharedSlot(uintptr_t p)
{
    return p >= (uintptr_t)jt_l3_default && p < (uintptr_t)(jt_l3_default + JT_L3_SIZE);
}
/* Census: pages = L2 tables, chunks[0] = L3 tables (64 B runs), chunks[1..4]
 * = runs of 128 B .. 1 KiB holding an entry, entries = live slots. */
void d2rt_box86_jmptbl_census(unsigned* pages, unsigned* chunks, unsigned* entries)
{
    unsigned np = 0, ne = 0, nc[5] = {0};
    for(size_t i1 = 0; i1 < JMPTABL_SIZE; ++i1) {
        uintptr_t** l2 = (uintptr_t**)box86_jmptbl[i1];
        if(l2 == jt_l2_default) continue;
        ++np;
        unsigned run[5] = {0};
        for(size_t i2 = 0; i2 < JT_L2_SIZE; ++i2) {
            unsigned here = 0;
            if(l2[i2] != jt_l3_default)
                for(int i0 = 0; i0 < JT_L3_SIZE; ++i0)
                    if(l2[i2][i0] != (uintptr_t)arm_next) ++here;
            ne += here;
            for(int g = 0; g < 5; ++g) {
                if(here) run[g] = 1;
                if(((i2+1) & ((1u<<g)-1)) == 0) { nc[g] += run[g]; run[g] = 0; }
            }
        }
    }
    if(pages) *pages = np;
    if(chunks) for(int g = 0; g < 5; ++g) chunks[g] = nc[g];
    if(entries) *entries = ne;
}

dynablock_t* getDB(uintptr_t addr)
{
    return *(dynablock_t**)(jt_get(addr) - sizeof(void*));
}

extern int dyn86_link_direct(void);   // D2Vita scheduler quantum mode (src/dynarec86/dyn86.c)
int getNeedTest(uintptr_t addr)
{
    uintptr_t ret = jt_get(addr);
    dynablock_t* db = *(dynablock_t**)(ret - sizeof(void*));
    if(db && !dyn86_link_direct() && ret==(uintptr_t)db->jmpnext && !db->dirty)
        return 0;
    return db?((ret!=(uintptr_t)db->block)?1:0):0;
}

uintptr_t getJumpAddress64(uintptr_t addr)
{
    return jt_get(addr);
}
#endif /* WX86_JT3 */

// Remove the Write flag from an adress range, so DB can be executed
extern int dyn86_protectdb(void);     // D2Vita SMC write-protection switch (src/dynarec86/dyn86.c)
void protectDB(uintptr_t addr, uintptr_t size)
{
    dynarec_log(LOG_DEBUG, "protectDB %p -> %p\n", (void*)addr, (void*)(addr+size-1));

    uintptr_t cur = addr&~(box86_pagesize-1);
    uintptr_t end = ALIGN(addr+size);

    mutex_lock(&mutex_prot);
    while(cur!=end) {
        uint32_t prot = 0, oprot;
        uintptr_t bend = 0;
        rb_get_end(memprot, cur, &prot, &bend);
        if(bend>end)
            bend = end;
        oprot = prot;
        uint32_t dyn = prot&PROT_DYN;
        if(!prot)
            prot = PROT_READ | PROT_WRITE | PROT_EXEC;
        if(!(dyn&PROT_NEVERPROT)) {
            prot&=~PROT_CUSTOM;
            if(prot&PROT_WRITE) {
                // D2Vita: keep ALL the PROT_DYNAREC bookkeeping (FillBlock
                // requires isprotectedDB) but skip the actual host mprotect
                // unless SMC write-protection is enabled — with no SIGSEGV
                // handler ported, a protected page would turn a guest
                // self-write into a hard crash. SMC goes undetected instead.
                if(!dyn && dyn86_protectdb())
                    mprotect((void*)DYN86_G2H(cur), bend-cur, prot&~PROT_WRITE);
                prot |= PROT_DYNAREC;
            } else 
                prot |= PROT_DYNAREC_R;
        }
        if (prot != oprot) // If the node doesn't exist, then prot != 0
            rb_set(memprot, cur, bend, prot);
        cur = bend;
    }
    mutex_unlock(&mutex_prot);
}

// Add the Write flag from an adress range, and mark all block as dirty
// no log, as it can be executed inside a signal handler
void unprotectDB(uintptr_t addr, uintptr_t size, int mark)
{
    dynarec_log(LOG_DEBUG, "unprotectDB %p -> %p (mark=%d)\n", (void*)addr, (void*)(addr+size-1), mark);

    uintptr_t cur = addr&~(box86_pagesize-1);
    uintptr_t end = ALIGN(addr+size);

    mutex_lock(&mutex_prot);
    while(cur!=end) {
        uint32_t prot = 0, oprot;
        uintptr_t bend = 0;
        if (!rb_get_end(memprot, cur, &prot, &bend)) {
            if(bend>=end) break;
            else {
                cur = bend;
                continue;
            }
        }
        oprot = prot;
        if(bend>end)
            bend = end;
        if(!(prot&PROT_NEVERPROT)) {
            if(prot&PROT_DYNAREC) {
                prot&=~PROT_DYN;
                if(mark)
                    cleanDBFromAddressRange(cur, bend-cur, 0);
                if(dyn86_protectdb())   // D2Vita: page was never mprotect'ed otherwise
                    mprotect((void*)DYN86_G2H(cur), bend-cur, prot);
            } else if(prot&PROT_DYNAREC_R)
                prot &= ~PROT_CUSTOM;
        }
        if (prot != oprot)
            rb_set(memprot, cur, bend, prot);
        cur = bend;
    }
    mutex_unlock(&mutex_prot);
}

int isprotectedDB(uintptr_t addr, size_t size)
{
    dynarec_log(LOG_DEBUG, "isprotectedDB %p -> %p => ", (void*)addr, (void*)(addr+size-1));
    uintptr_t end = ALIGN(addr+size);
    addr &=~(box86_pagesize-1);
    mutex_lock(&mutex_prot);
    while (addr < end) {
        uint32_t prot;
        uintptr_t bend;
        if (!rb_get_end(memprot, addr, &prot, &bend) || !(prot&PROT_DYN)) {
            dynarec_log(LOG_DEBUG, "0\n");
            mutex_unlock(&mutex_prot);
            return 0;
        } else {
            addr = bend;
        }
    }
    mutex_unlock(&mutex_prot);
    dynarec_log(LOG_DEBUG, "1\n");
    return 1;
}

uintptr_t hotpage = 0;
int hotpage_cnt = 0;
#define HOTPAGE_MARK 64
void SetHotPage(uintptr_t addr)
{
    hotpage = addr&~(box86_pagesize-1);
    hotpage_cnt = HOTPAGE_MARK;
}
int isInHotPage(uintptr_t addr)
{
    if(!hotpage_cnt)
        return 0;
    --hotpage_cnt;
    return (addr>=hotpage) && (addr<hotpage+box86_pagesize);
}
int checkInHotPage(uintptr_t addr)
{
    return hotpage_cnt && (addr>=hotpage) && (addr<hotpage+box86_pagesize);
}

#endif

// Port addition (StarCraft/Storm): mark [addr, addr+size) as code that the
// guest rewrites in place (runtime-generated blitters in PAGE_EXECUTE_* memory).
// With no SIGSEGV-based SMC detection, such pages get PROT_NEVERCLEAN: blocks
// translated there are always_test (re-hashed on every lookup, never
// direct-linked), so a rewrite is picked up on the next entry.
int dyn86_smc_single(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("WX86_SMC_SINGLE"); v = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0; }
    return v;
}

int dyn86_smc_stop_after(uintptr_t ip, uintptr_t next)
{
    if (dyn86_smc_single() == 2) return 1;
    for (uintptr_t p = ip; p + 4 <= next; ++p) {
        uint32_t v; memcpy(&v, (const void*)DYN86_G2H(p), 4);
        if (v >= 0x10000 && (getProtection(v) & PROT_NEVERCLEAN)) return 1;
    }
    return 0;
}

void dyn86_mark_volatile_code(uintptr_t addr, uintptr_t size)
{
    uintptr_t cur = addr & ~(box86_pagesize-1);
    uintptr_t end = ALIGN(addr+size);
    mutex_lock(&mutex_prot);
    while (cur < end) {
        uint32_t prot = 0; uintptr_t bend = 0;
        rb_get_end(memprot, cur, &prot, &bend);
        if (bend > end || bend <= cur) bend = end;
        uint32_t np = prot ? (prot & ~(PROT_DYNAREC | PROT_DYNAREC_R)) : (PROT_READ | PROT_WRITE | PROT_EXEC);
        rb_set(memprot, cur, bend, np | PROT_NEVERCLEAN);
        cur = bend;
    }
    mutex_unlock(&mutex_prot);
    cleanDBFromAddressRange(addr & ~(box86_pagesize-1), end - (addr & ~(box86_pagesize-1)), 1);
}

void updateProtection(uintptr_t addr, uintptr_t size, uint32_t prot)
{
    //dynarec_log(LOG_DEBUG, "updateProtection %p -> %p to 0x%02x\n", (void*)addr, (void*)(addr+size-1), prot);
    mutex_lock(&mutex_prot);
    uintptr_t cur = addr & ~(box86_pagesize-1);
    uintptr_t end = ALIGN(cur+size);
    rb_set(mapallmem, cur, cur+size, 1);
    while (cur < end) {
        uintptr_t bend;
        uint32_t oprot;
        rb_get_end(memprot, cur, &oprot, &bend);
        if(bend>end) bend = end;
        uint32_t dyn=(oprot&PROT_DYN);
        if(!(dyn&PROT_NEVERPROT)) {
            if(dyn && (prot&PROT_WRITE)) {   // need to remove the write protection from this block
                dyn = PROT_DYNAREC;
                mprotect((void*)DYN86_G2H(cur), bend-cur, prot&~PROT_WRITE);
            } else if(dyn && !(prot&PROT_WRITE)) {
                dyn = PROT_DYNAREC_R;
            }
        }
        if ((prot|dyn) != oprot)
            rb_set(memprot, cur, bend, prot|dyn);
        cur = bend;
    }
    mutex_unlock(&mutex_prot);
}

void setProtection(uintptr_t addr, uintptr_t size, uint32_t prot)
{
    //dynarec_log(LOG_DEBUG, "setProtection %p -> %p to 0x%02x\n", (void*)addr, (void*)(addr+size-1), prot);
    size = ALIGN(size);
    mutex_lock(&mutex_prot);
    uintptr_t cur = addr & ~(box86_pagesize-1);
    uintptr_t end = ALIGN(cur+size);
    rb_set(mapallmem, cur, end, 1);
    rb_set(memprot, cur, end, prot);
    mutex_unlock(&mutex_prot);
}

void setProtection_mmap(uintptr_t addr, size_t size, uint32_t prot)
{
    if(!size)
        return;
    addr &= ~(box86_pagesize-1);
    size = ALIGN(size);
    mutex_lock(&mutex_prot);
    rb_set(mmapmem, addr, addr+size, 1);
    mutex_unlock(&mutex_prot);
    if(prot)
        setProtection(addr, size, prot);
    else {
        mutex_lock(&mutex_prot);
        rb_set(mapallmem, addr, addr+size, 1);
        rb_unset(memprot, addr, addr+size);
        mutex_unlock(&mutex_prot);
    }
}

void setProtection_elf(uintptr_t addr, size_t size, uint32_t prot)
{
    size = ALIGN(size);
    addr &= ~(box86_pagesize-1);
    if(prot)
        setProtection(addr, size, prot);
    else {
        mutex_lock(&mutex_prot);
        rb_set(mapallmem, addr, addr+size, 1);
        rb_unset(memprot, addr, addr+size);
        mutex_unlock(&mutex_prot);
    }
}

void refreshProtection(uintptr_t addr)
{
    mutex_lock(&mutex_prot);
    uint32_t prot;
    uintptr_t bend;
    if (rb_get_end(memprot, addr, &prot, &bend)) {
        int ret = mprotect((void*)DYN86_G2H(addr&~(box86_pagesize-1)), box86_pagesize, prot&~PROT_CUSTOM);
        dynarec_log(LOG_DEBUG, "refreshProtection(%p): %p/0x%x (ret=%d/%s)\n", (void*)addr, (void*)(addr&~(box86_pagesize-1)), prot, ret, ret?strerror(errno):"ok");
    }
    mutex_unlock(&mutex_prot);
}

void allocProtection(uintptr_t addr, size_t size, uint32_t prot)
{
    dynarec_log(LOG_DEBUG, "allocProtection %p:%p 0x%x\n", (void*)addr, (void*)(addr+size-1), prot);
    size = ALIGN(size);
    addr &= ~(box86_pagesize-1);
    mutex_lock(&mutex_prot);
    rb_set(mapallmem, addr, addr+size, 1);
    mutex_unlock(&mutex_prot);
    // don't need to add precise tracking probably
}

uintptr_t pbrk = 0;
uintptr_t old_brk = 0;
uintptr_t* cur_brk = NULL;
void loadProtectionFromMap()
{
    if(box86_mapclean)
        return;
    char buf[500];
    FILE *f = fopen("/proc/self/maps", "r");
    if(!f)
        return;
    while(!feof(f)) {
        char* ret = fgets(buf, sizeof(buf), f);
        (void)ret;
        char r, w, x;
        uintptr_t s, e;
        if(sscanf(buf, "%x-%x %c%c%c", &s, &e, &r, &w, &x)==5) {
            int prot = ((r=='r')?PROT_READ:0)|((w=='w')?PROT_WRITE:0)|((x=='x')?PROT_EXEC:0);
            allocProtection(s, e-s, prot);
            if(!pbrk && strstr(buf, "[heap]"))
                pbrk = s;
        }
    }
    if(!pbrk) {
        printf_log(LOG_INFO, "BOX86: Warning, program break not found\n");
        if(cur_brk) pbrk = *cur_brk;    // approximate is better than nothing
    }
    fclose(f);
    box86_mapclean = 1;
}

void freeProtection(uintptr_t addr, uintptr_t size)
{
    size = ALIGN(size);
    addr &= ~(box86_pagesize-1);
    dynarec_log(LOG_DEBUG, "freeProtection %p:%p\n", (void*)addr, (void*)(addr+size-1));
    mutex_lock(&mutex_prot);
    rb_unset(mapallmem, addr, addr+size);
    rb_unset(mmapmem, addr, addr+size);
    rb_unset(memprot, addr, addr+size);
    mutex_unlock(&mutex_prot);
}

uint32_t getProtection(uintptr_t addr)
{
    mutex_lock(&mutex_prot);
    uint32_t ret = rb_get(memprot, addr);
    mutex_unlock(&mutex_prot);
    return ret;
}

int getMmapped(uintptr_t addr)
{
    return rb_get(mmapmem, addr);
}


#define LOWEST (void*)0x10000
#define MEDIAN (void*)0x40000000
static void* findBlockHinted(void* hint, size_t size, uintptr_t mask)
{
    // first, check if program break as changed
    if(pbrk && cur_brk && *cur_brk!=old_brk) {
        old_brk = *cur_brk;
        setProtection(pbrk, old_brk-pbrk, PROT_READ|PROT_WRITE);
    }
    int prot;
    if(hint<LOWEST) hint = LOWEST;
    uintptr_t bend = 0;
    uintptr_t cur = (uintptr_t)hint;
    if(!mask) mask = 0xffff;
    uintptr_t end_mem = 0xffffffffLL - mask;
    while(bend<end_mem) {
        if(!rb_get_end(mapallmem, cur, &prot, &bend)) {
            if(bend-cur>=size)
                return (void*)cur;
            if(bend>=0xc0000000 && (uintptr_t)hint<0xc0000000)
                return NULL;
        }
        // granularity 0x10000
        cur = (bend+mask)&~mask;
    }
    return NULL;
}
void* findBlockNearHint(void* hint, size_t size, uintptr_t mask)
{   void* ret = findBlockHinted(hint, size, mask);
    return ret?ret:hint;
}
void* find32bitBlock(size_t size)
{
    void* ret = findBlockHinted(MEDIAN, size, 0);
    if(!ret) ret = findBlockHinted(LOWEST, size, 0);
    return ret;
}

void* find32bitBlockElf(size_t size, int mainbin, uintptr_t mask)
{
    static void* startingpoint = (void*)0x60000000;
    void* mainaddr = (void*)0x30000000;
    void* ret = findBlockHinted(mainbin?mainaddr:startingpoint, size, mask);
    if(!ret) ret = findBlockHinted(LOWEST, size, mask);
    if(!mainbin)
        startingpoint = (void*)(((uintptr_t)startingpoint+size+0x200000)&~0xfffff);
    return ret;
}

int isBlockFree(void* hint, size_t size)
{
    int prot;
    uintptr_t bend = 0;
    uintptr_t cur = (uintptr_t)hint;
    if(!rb_get_end(mapallmem, cur, &prot, &bend)) {
        if(bend-cur>=size)
            return 1;
    }
    return 0;
}

int unlockCustommemMutex()
{
    int ret = 0;
    int i = 0;
    #ifdef USE_CUSTOM_MUTEX
    void* tid = (void*)GetTID();
    #define GO(A, B)                    \
        i = (arm_lock_storeifref2(&A, NULL, (void*)tid)==tid);  \
        if(i) {                         \
            ret|=(1<<B);                \
        }
    #else
    #define GO(A, B)                    \
        i = checkUnlockMutex(&A);       \
        if(i) {                         \
            ret|=(1<<B);                \
        }
    #endif
    GO(mutex_blocks, 0)
    GO(mutex_prot, 1)
    GO(mutex_dynarec, 2)
    #undef GO
    return ret;
}

void relockCustommemMutex(int locks)
{
    #define GO(A, B)                    \
        if(locks&(1<<B))                \
            mutex_lock(&A);             \

    GO(mutex_blocks, 0)
    GO(mutex_prot, 1)
    GO(mutex_dynarec, 2)
    #undef GO
}

static void init_mutexes(void)
{
    #ifdef USE_CUSTOM_MUTEX
    arm_lock_stored(&mutex_blocks, 0);
    arm_lock_stored(&mutex_prot, 0);
    arm_lock_stored(&mutex_dynarec, 0);
    #else
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&mutex_blocks, &attr);
    pthread_mutex_init(&mutex_prot, &attr);
    pthread_mutex_init(&mutex_dynarec, &attr);

    pthread_mutexattr_destroy(&attr);
    #endif
}

static void atfork_child_custommem(void)
{
    // (re))init mutex if it was lock before the fork
    init_mutexes();
}

void init_custommem_helper(box86context_t* ctx)
{
    (void)ctx;
    if(inited) // already initialized
        return;
    inited = 1;
    // Mutexes FIRST: init_rbtree/kh_init allocate through customMalloc, which
    // locks mutex_blocks. glibc tolerates locking a zeroed mutex (its static
    // initializer is all-zeros) but pte (PS Vita pthreads) dereferences the
    // NULL handle and spins forever in pte_osAtomicExchange.
    init_mutexes();
    cur_brk = dlsym(RTLD_NEXT, "__curbrk");
    memprot = init_rbtree();
#ifdef DYNAREC
#ifdef ARM
#ifdef WX86_JT3
    for(int i=0; i<JT_L3_SIZE; ++i)
        jt_l3_default[i] = (uintptr_t)arm_next;
    for(int i=0; i<JT_L2_SIZE; ++i)
        jt_l2_default[i] = jt_l3_default;
    for(int i=0; i<JMPTABL_SIZE; ++i)
        box86_jmptbl[i] = (uintptr_t*)jt_l2_default;
#else
    for(int i=0; i<(1<<JMPTABL_SHIFT); ++i)
        box86_jmptbl_default[i] = (uintptr_t)arm_next;
    for(int i=0; i<JMPTABL_SIZE; ++i)
        box86_jmptbl[i] = box86_jmptbl_default;
#endif
#else
#error Unsupported architecture!
#endif
    lockaddress = kh_init(lockaddress);
#endif
    pthread_atfork(NULL, NULL, atfork_child_custommem);
    // init mapallmem list
    mapallmem = init_rbtree();
    // init mmapmem list
    mmapmem = init_rbtree();
    loadProtectionFromMap();
}

void fini_custommem_helper(box86context_t *ctx)
{
    (void)ctx;
#if defined(TRACE_MEMSTAT) && !defined(WX86_JT3)
    size_t njmps = 0, njmps_in_lv1_max = 0;
    for (size_t idx1 = 0; idx1 < JMPTABL_SIZE; ++idx1) {
        if (box86_jmptbl[idx1] == box86_jmptbl_default) continue;
        size_t njmps_in_cur_lv1 = 0;
        for (size_t idx0 = 0; idx0 < (1 << JMPTABL_SHIFT); ++idx0) {
            if (box86_jmptbl[idx1][idx0] == (uintptr_t)arm_next) continue;
            ++njmps;
            ++njmps_in_cur_lv1;
        }
        if (njmps_in_cur_lv1 > njmps_in_lv1_max) njmps_in_lv1_max = njmps_in_cur_lv1;
    }
    printf_log(LOG_INFO, "Allocation:\n- dynarec: %lu kio\n- customMalloc: %zu kio\n- jump table: %zu kio (%zu level 1 table allocated, for %zu jumps, with at most %zu per level 1)\n", dynarec_allocated / 1024, customMalloc_allocated / 1024, jmptbl_allocated / 1024, jmptbl_allocated1, njmps, njmps_in_lv1_max);
#endif
    if(!inited)
        return;
    inited = 0;
#ifdef DYNAREC
    if(box86_dynarec) {
        dynarec_log(LOG_DEBUG, "Free global Dynarecblocks\n");
        mmaplist_t* head = mmaplist;
        mmaplist = NULL;
        while(head) {
            for (int i=0; i<NCHUNK; ++i) {
                if(head->chunks[i].block)
                    #ifdef USE_MMAP
                    munmap(head->chunks[i].block, head->chunks[i].size);
                    #else
                    box_free(head->chunks[i].block);
                    #endif
            }
            mmaplist_t *old = head;
            head = head->next;
            free(old);
        }
        mmapsize = 0;
        dynarec_log(LOG_DEBUG, "Free dynamic Dynarecblocks\n");
        uintptr_t idx = 0;
        uintptr_t end = ((0xFFFFFFFF)>>DYNAMAP_SHIFT);
        box_free(mmaplist);
#ifndef WX86_JT3   /* JT3 tables live in slabs that are never freed */
        for (int i=0; i<DYNAMAP_SIZE; ++i)
            if(box86_jmptbl[i]!=box86_jmptbl_default)
                box_free(box86_jmptbl[i]);
#endif
    }
    kh_destroy(lockaddress, lockaddress);
    lockaddress = NULL;
#endif
    delete_rbtree(memprot);
    memprot = NULL;
    delete_rbtree(mmapmem);
    mmapmem = NULL;
    delete_rbtree(mapallmem);
    mapallmem = NULL;

    for(int i=0; i<n_blocks; ++i)
        #ifdef USE_MMAP
        munmap(p_blocks[i].block, p_blocks[i].size);
        #else
        box_free(p_blocks[i].block);
        #endif
    box_free(p_blocks);
    #ifndef USE_CUSTOM_MUTEX
    pthread_mutex_destroy(&mutex_blocks);
    pthread_mutex_destroy(&mutex_prot);
    pthread_mutex_destroy(&mutex_dynarec);
    #endif
}

#ifdef DYNAREC
// add an address to the list of "LOCK"able
void addLockAddress(uintptr_t addr)
{
    int ret;
    kh_put(lockaddress, lockaddress, addr, &ret);
}

// return 1 is the address is used as a LOCK, 0 else
int isLockAddress(uintptr_t addr)
{
    khint_t k = kh_get(lockaddress, lockaddress, addr);
    return (k==kh_end(lockaddress))?0:1;
}

#endif
