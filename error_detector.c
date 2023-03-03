#include "include/memtrace.h"

#define MAX_THREADS 100
#define MAX_ALLOCS 10000
const u64 linear_set_size_increment = 1000000;

// todo => shrink to 8 bytes. Should be possible if memory address access/accessing only store 
// bytes of the virtual address that define the memory locations relative to the process pages)
// reference: https://developer.arm.com/documentation/den0024/a/The-Memory-Management-Unit/Translating-a-Virtual-Address-to-a-Physical-Address
// abstract: only the last 28 bits of an virtual address encode the actual loation(or index) of the address, the rest is context
typedef struct MemoryAccess {
   usize address_accessed;
   u16 opcode;
   u64 size;
   u32 callee_thread_id;
} MemoryAccess;

typedef struct LockAccess {
   usize lock_address;
   u32 callee_thread_id;
} LockAccess;

typedef struct MemoryAllocation {
   usize addr;
   u64 size;
   u32 callee_thread_id;
} MemoryAllocation;

typedef struct ThreadState {
    u64 thread_id;
    MemoryAccess *mem_read_set;
    u64 mem_read_set_capacity;
    u64 mem_read_set_len;

    MemoryAccess *mem_write_set;
    u64 mem_write_set_capacity;
    u64 mem_write_set_len;

    LockAccess *lock_unlock_set;
    u64 lock_unlock_set_capacity;
    u64 lock_unlock_set_len;

    LockAccess *lock_lock_set;
    u64 lock_lock_set_capacity;
    u64 lock_lock_set_len;
} ThreadState;


// max allocations is seperate from thread state since it needs to be itreated thorugh on every error check
MemoryAllocation allocations[MAX_ALLOCS] = {};
u64 n_allocs = 0;

ThreadState threads[MAX_THREADS] = {};
u64 n_threads = 0;


// util fns..
void *increase_set_capacity(void *set, u64 *set_capacity) {
    *set_capacity += linear_set_size_increment;
    printf("new set_capacity: %ld \n", *set_capacity);
    return realloc(set, *set_capacity);
}

i64 findThreadByTId(u64 tid) {
    u64 i;
    for (i = 0; i <= n_threads; i++) {
        if (threads[i].thread_id == tid) {
            return i;
        }
    }
    return -1;
}
// util fns..


void mem_analyse_exit() { 
    u64 j;
    for (j = 0; j < n_threads; j++) {
        u64 i;
        printf("Thread id: %ld \n", threads[j].thread_id);
        printf("mem_write_set_len: %ld \n", threads[j].mem_write_set_len);
        printf("mem_read_set_len: %ld \n", threads[j].mem_read_set_len);
        printf("unlocks: %ld \n", threads[j].lock_unlock_set_len);
        printf("locks: %ld \n", threads[j].lock_lock_set_len);
        // for (i = 0; i < threads[j].mem_write_set_len; i++) {
        //     printf("[%d]tid write access to address: %ld, size: %ld, opcode: %s \n", threads[j].mem_write_set[i].thread_id, threads[j].mem_write_set[i].address_accessed, threads[j].mem_write_set[i].size, (threads[j].mem_read_set[i].opcode > REF_TYPE_WRITE) ? decode_opcode_name(threads[j].mem_read_set[i].opcode) /* opcode for instr */ : (threads[j].mem_read_set[i].opcode == REF_TYPE_WRITE ? "w" : "r"));        
        // }
        // for (i = 0; i < threads[j].mem_read_set_len; i++) {
        //     printf("[%d]tid read access to address: %ld, size: %ld, opcode: %s \n", threads[j].mem_write_set[i].thread_id, threads[j].mem_read_set[i].address_accessed, threads[j].mem_read_set[i].size, (threads[j].mem_read_set[i].opcode > REF_TYPE_WRITE) ? decode_opcode_name(threads[j].mem_read_set[i].opcode) /* opcode for instr */ : (threads[j].mem_read_set[i].opcode == REF_TYPE_WRITE ? "w" : "r"));
        // }

        free(threads[j].mem_write_set);
        free(threads[j].mem_read_set);
        free(threads[j].lock_lock_set);
        free(threads[j].lock_unlock_set);
    }
}

u32 mem_analyse_init() { 
        return 1;
}

u32 mem_analyse_new_thread_init(u64 thread_id) {
    if (n_threads >= MAX_THREADS) return 0;
    threads[n_threads].thread_id = thread_id;

    threads[n_threads].mem_read_set = (MemoryAccess*)malloc(sizeof(MemoryAccess) * linear_set_size_increment);
    threads[n_threads].mem_read_set_capacity = linear_set_size_increment;
    if (threads[n_threads].mem_read_set == NULL) {
        printf("set allocation error \n");
        return 0;    
    }
    threads[n_threads].mem_write_set = (MemoryAccess*)malloc(sizeof(MemoryAccess) * linear_set_size_increment);
    threads[n_threads].mem_write_set_capacity = linear_set_size_increment;
    if (threads[n_threads].mem_write_set == NULL) {
        printf("set allocation error \n");
        return 0;
    }

    threads[n_threads].lock_lock_set = (LockAccess*)malloc(sizeof(LockAccess) * linear_set_size_increment);
    threads[n_threads].lock_lock_set_capacity = linear_set_size_increment;
    if (threads[n_threads].lock_lock_set == NULL) {
        printf("set allocation error \n");
        return 0;
    }
    threads[n_threads].lock_unlock_set = (LockAccess*)malloc(sizeof(LockAccess) * linear_set_size_increment);
    threads[n_threads].lock_unlock_set_capacity = linear_set_size_increment;    
    if (threads[n_threads].lock_unlock_set == NULL) {
        printf("set allocation error \n");
        return 0;
    }

    n_threads += 1;
    return 1;
}

void mem_analyse_thread_exit() {
    // printf("thread exit \n");
}


void wrap_pre_unlock(void *wrapcxt, OUT void **user_data) {
    void *addr = drwrap_get_arg(wrapcxt, 0);
    u64 thread_id = dr_get_thread_id(wrapcxt);
    // printf("pthread_unlock called\n");
    i64 t_index = findThreadByTId(thread_id);
    if (t_index < 0) {
        // printf("error finding thread_id. %ld \n", thread_id);
        return;    
    }
    ThreadState *thread_accessed = &threads[t_index];
    thread_accessed->lock_unlock_set[thread_accessed->lock_unlock_set_len].lock_address = (u64)addr;
    thread_accessed->lock_unlock_set[thread_accessed->lock_unlock_set_len].callee_thread_id = thread_id;
    thread_accessed->lock_unlock_set_len += 1;
}

void wrap_pre_lock(void *wrapcxt, OUT void **user_data) {
    void *addr = drwrap_get_arg(wrapcxt, 0);
    // todo => wrong id
    u64 thread_id = dr_get_thread_id(wrapcxt);
    // printf("pthread_lock called %ld \n", thread_id);
    i64 t_index = findThreadByTId(thread_id);
    if (t_index < 0) {
        // printf("error finding thread_id. %ld \n", thread_id);
        return;        }
    ThreadState *thread_accessed = &threads[t_index];
    thread_accessed->lock_lock_set[thread_accessed->lock_lock_set_len].lock_address = (u64)addr;
    thread_accessed->lock_lock_set[thread_accessed->lock_lock_set_len].callee_thread_id = thread_id;
    thread_accessed->lock_lock_set_len += 1;
}
void wrap_post_malloc(void *wrapcxt, void *user_data) {
    void *addr = drwrap_get_retval(wrapcxt);
    // todo => wrong id
    u64 thread_id = dr_get_thread_id(wrapcxt);
    // printf("malloc called %p in tid %ld\n", (void*)0x0, thread_id);

    int j;
    for(j = 0; j < n_threads; j++) {
        if (threads[j].thread_id == thread_id) break;
        if (j == n_threads-1) return;
    }
    printf("%d \n", thread_id);
    allocations[n_allocs].addr = (usize)addr;
    allocations[n_allocs].callee_thread_id = 0;
    n_allocs += 1;
}

// this is an event like fn that is envoked on every memory access (called by DynamRIO)
void memtrace(void *drcontext, u64 thread_id) {
    per_thread_t *data;
    mem_ref_t *mem_ref, *buf_ptr;
    data = drmgr_get_tls_field(drcontext, tls_idx);
    buf_ptr = BUF_PTR(data->seg_base);
    
    i64 t_index = findThreadByTId(thread_id);
    if (t_index < 0) {
        // printf("error finding thread_id. %ld \n", thread_id);
        return;    
    }
    for (mem_ref = (mem_ref_t *)data->buf_base; mem_ref < buf_ptr; mem_ref++) {
        int j;
        for(j = 0; j < n_allocs; j++) {
            if ((usize)mem_ref->addr == allocations[j].addr) break;
            if (j == n_allocs-1) return;
        }
        // no allocations, no mem shared
        if (n_allocs <= 0) return;
        u64 thread_id_owning_accessed_addr = allocations[j].callee_thread_id;

        u64 thread_states_index_owning_accessed_addr = findThreadByTId(thread_id_owning_accessed_addr);
        if (thread_states_index_owning_accessed_addr < 0) {
            printf("error finding thread_id. %ld \n", thread_id);
            return;    
        }
        ThreadState *thread_accessed = &threads[thread_states_index_owning_accessed_addr];
        // printf("adding %d \n", thread_states_index_owning_accessed_addr);
        if (mem_ref->type == 1 || mem_ref->type == 457 || mem_ref->type == 458 || mem_ref->type == 456 || mem_ref->type == 568) {
            // mem write
            if (thread_accessed->mem_write_set_len >= thread_accessed->mem_write_set_capacity) thread_accessed->mem_write_set = increase_set_capacity(thread_accessed->mem_write_set, &thread_accessed->mem_write_set_capacity);
            if (thread_accessed->mem_write_set == NULL) exit(1);
            thread_accessed->mem_write_set[thread_accessed->mem_write_set_len].address_accessed = (usize)mem_ref->addr;
            thread_accessed->mem_write_set[thread_accessed->mem_write_set_len].opcode = mem_ref->type;
            thread_accessed->mem_write_set[thread_accessed->mem_write_set_len].callee_thread_id = thread_id;
            thread_accessed->mem_write_set[thread_accessed->mem_write_set_len].size = mem_ref->size;
            thread_accessed->mem_write_set_len += 1;
        } else if(mem_ref->type == 0 || mem_ref->type == 227 || mem_ref->type == 225 || mem_ref->type == 197 || mem_ref->type == 228 || mem_ref->type == 229 || mem_ref->type == 299 || mem_ref->type == 173) {
            // mem read
            if (thread_accessed->mem_read_set_len >= thread_accessed->mem_read_set_capacity) thread_accessed->mem_read_set = increase_set_capacity(thread_accessed->mem_read_set, &thread_accessed->mem_read_set_capacity);
            if (thread_accessed->mem_read_set == NULL) exit(1);
            thread_accessed->mem_read_set[thread_accessed->mem_read_set_len].address_accessed = (usize)mem_ref->addr;
            thread_accessed->mem_read_set[thread_accessed->mem_read_set_len].opcode = mem_ref->type;
            thread_accessed->mem_read_set[thread_accessed->mem_read_set_len].callee_thread_id = thread_id;
            thread_accessed->mem_read_set[thread_accessed->mem_read_set_len].size = mem_ref->size;
            thread_accessed->mem_read_set_len += 1;
        }
        //  else {
        //     printf("missed %d \n", mem_ref->type);        
        // }
        // // /* We use PIFX to avoid leading zeroes and shrink the resulting file. */
        // fprintf(data->logf, "" PIFX ": %2d, %s (%d)\n", (ptr_uint_t)mem_ref->addr, mem_ref->size, (mem_ref->type > REF_TYPE_WRITE) ? decode_opcode_name(mem_ref->type) /* opcode for instr */ : (mem_ref->type == REF_TYPE_WRITE ? "w" : "r"), mem_ref->type);
        data->num_refs++;
    }
    BUF_PTR(data->seg_base) = data->buf_base;
}

