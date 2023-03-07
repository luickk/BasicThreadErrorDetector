#include <stdio.h>
#include <stddef.h> /* for offsetof */
#include <string.h>
#include <sys/syscall.h>
#include "dr_api.h"
#include "drmgr.h"
#include "drreg.h"
#include "drutil.h"
#include "drx.h"
#include "drwrap.h"
#include "drcallstack.h"
#include "drsyms.h"

#include "types.h"

#define SYS_MAX_ARGS 3
#define TLS_SLOT(tls_base, enum_val) (void **)((byte *)(tls_base) + tls_offs + (enum_val))
#define BUF_PTR(tls_base) *(mem_ref_t **)TLS_SLOT(tls_base, MEMTRACE_TLS_OFFS_BUF_PTR)

enum {
    REF_TYPE_READ = 0,
    REF_TYPE_WRITE = 1,
};
/* Each mem_ref_t is a <type, size, addr> entry representing a memory reference
 * instruction or the reference information, e.g.:
 * - mem ref instr: { type = 42 (call), size = 5, addr = 0x7f59c2d002d3 }
 * - mem ref info:  { type = 1 (write), size = 8, addr = 0x7ffeacab0ec8 }
 */
typedef struct _mem_ref_t {
    ushort type; /* r(0), w(1), or opcode (assuming 0/1 are invalid opcode) */
    ushort size; /* mem ref size or instr length */
    app_pc addr; /* mem ref addr or instr pc */
} mem_ref_t;

/* Max number of mem_ref a buffer can have. It should be big enough
 * to hold all entries between clean calls.
 */
#define MAX_NUM_MEM_REFS 4096
/* The maximum size of buffer for holding mem_refs. */
#define MEM_BUF_SIZE (sizeof(mem_ref_t) * MAX_NUM_MEM_REFS)

/* thread private log file and counter */
typedef struct {
    byte *seg_base;
    mem_ref_t *buf_base;
    file_t log;
    FILE *logf;
    uint64 num_refs;

    reg_t param[SYS_MAX_ARGS];
    bool repeat;
} per_thread_t;

/* Allocated TLS slot offsets */
enum {
    MEMTRACE_TLS_OFFS_BUF_PTR,
    MEMTRACE_TLS_COUNT, /* total number of TLS slots allocated */
};

int tls_idx;
uint tls_offs;

int num_syscalls;

extern void memtrace(void *drcontext, u64 thread_id);
extern u32 mem_analyse_init();
extern void mem_analyse_exit();
extern void mem_analyse_thread_exit();
extern u32 mem_analyse_new_thread_init(void *drcontext);
extern void wrap_pre_unlock(void *wrapcxt, OUT void **user_data);
extern void wrap_pre_lock(void *wrapcxt, OUT void **user_data);
extern void wrap_post_malloc(void *wrapcxt, void *user_data);
extern void wrap_pre_malloc(void *wrapcxt, OUT void **user_data);