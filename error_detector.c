#include "include/instrument.h"

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
   u64 callee_thread_id;
   u64 memory_access_count;
} MemoryAccess;

typedef enum LockWritability {
    WriteHeld,    
    ReadHeld,
} LockWritability;

typedef struct LockState {
   LockWritability state;
   usize addr;
   u64 callee_thread_id;
   usize lock_count;
   usize unlock_count;
} LockState;


typedef struct LockAccess {
   LockWritability state;
   usize addr;
   u64 callee_thread_id;
   u64 memory_access_count;
} LockAccess;

typedef struct MemoryAllocation {
   usize addr;
   u64 size;
   u64 callee_thread_id;
   u64 memory_access_count;
} MemoryAllocation;

typedef struct ThreadState {
    u64 thread_id;
    MemoryAccess *mem_read_set;
    u64 mem_read_set_capacity;
    u64 mem_read_set_len;

    MemoryAccess *mem_write_set;
    u64 mem_write_set_capacity;
    u64 mem_write_set_len;


    LockState *lock_state_set;
    u64 lock_state_set_capacity;
    u64 lock_state_set_len;

    LockAccess *lock_write_held_set;
    u64 lock_write_held_set_capacity;
    u64 lock_write_held_set_len;

    LockAccess *lock_read_held_set;
    u64 lock_read_held_set_capacity;
    u64 lock_read_held_set_len;
} ThreadState;


// max allocations is seperate from thread state since it needs to be itreated thorugh on every error check
MemoryAllocation allocations[MAX_ALLOCS] = {};
u64 n_allocs = 0;

ThreadState threads[MAX_THREADS] = {};
u64 n_threads = 0;

u64 memory_access_counter = 0;

// util fns..
void *increase_set_capacity(void *set, u64 *set_capacity) {
    *set_capacity += linear_set_size_increment;
    printf("new set_capacity: %ld \n", *set_capacity);
    return realloc(set, *set_capacity);
}

i64 find_thread_by_tid(u64 tid) {
    u64 i;
    for (i = 0; i <= n_threads; i++) {
        if (threads[i].thread_id == tid) {
            return i;
        }
    }
    return -1;
}

i64 find_lockset_by_memory_access_count(LockAccess *lock_set, u64 lock_set_len, u64 access_count) {
    u64 i;
    for (i = 0; i <= lock_set_len; i++) {
        if (lock_set[i].memory_access_count == access_count) {
            return i;
        }
    }
    return -1;
}

u32 is_in_range(u64 num, u64 min, u64 max) {      
    return (min <= num && num <= max); 
}
// util fns..


void mem_analyse_exit() { 
    u64 j;
    for (j = 0; j < n_threads; j++) {
        printf("Thread id: %ld \n", threads[j].thread_id);
        printf("mem_write_set_len: %ld \n", threads[j].mem_write_set_len);
        printf("mem_read_set_len: %ld \n", threads[j].mem_read_set_len);
        printf("lock_state_set_len: %ld \n", threads[j].lock_state_set_len);
        
        u64 i;
        for (i = 0; i <= threads[j].lock_state_set_len; i++) {
            printf("lock %ld, state: %d \n", threads[j].lock_state_set[i].addr, threads[j].lock_state_set[i].state);
        }
        
        free(threads[j].mem_write_set);
        free(threads[j].mem_read_set);
        free(threads[j].lock_state_set);
        free(threads[j].lock_write_held_set);
        free(threads[j].lock_read_held_set);
    }
}

u32 mem_analyse_init() { 
        return 1;
}

u32 mem_analyse_new_thread_init(void *drcontext) {
    if (drcontext == NULL) return 0;
    u64 thread_id = dr_get_thread_id(drcontext);
    // printf("init: %ld\n", thread_id);
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

    threads[n_threads].lock_state_set = (LockState*)malloc(sizeof(LockState) * linear_set_size_increment);
    threads[n_threads].lock_state_set_capacity = linear_set_size_increment;
    if (threads[n_threads].lock_state_set == NULL) {        
        printf("set allocation error \n");
        return 0;
    }

    threads[n_threads].lock_write_held_set = (LockAccess*)malloc(sizeof(LockAccess) * linear_set_size_increment);
    threads[n_threads].lock_write_held_set_capacity = linear_set_size_increment;
    if (threads[n_threads].lock_write_held_set == NULL) {        
        printf("set allocation error \n");
        return 0;
    }

    threads[n_threads].lock_read_held_set = (LockAccess*)malloc(sizeof(LockAccess) * linear_set_size_increment);
    threads[n_threads].lock_read_held_set_capacity = linear_set_size_increment;
    if (threads[n_threads].lock_read_held_set == NULL) {        
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
    // printf("pre UNLOCK\n");
    void *addr = drwrap_get_arg(wrapcxt, 0);
    u64 thread_id = dr_get_thread_id(dr_get_current_drcontext());
    // printf("pthread_unlock called\n");
    i64 t_index = find_thread_by_tid(thread_id);
    if (t_index < 0) {
        // printf("error finding thread_id. %ld \n", thread_id);
        return;    
    }
    ThreadState *thread_accessed = &threads[t_index];
    int i;
    for (i = 0; i <= thread_accessed->lock_state_set_len; i++) {
        if (thread_accessed->lock_state_set[i].addr == (usize)addr) break;
        if (i >= thread_accessed->lock_state_set_len) {
            thread_accessed->lock_state_set[thread_accessed->lock_state_set_len].addr = (usize)addr;
            thread_accessed->lock_state_set[thread_accessed->lock_state_set_len].callee_thread_id = thread_id;
            thread_accessed->lock_state_set[thread_accessed->lock_state_set_len].state = ReadHeld;
            thread_accessed->lock_state_set[thread_accessed->lock_state_set_len].unlock_count += 1;
            thread_accessed->lock_state_set_len += 1;
            return;
        };
    }
    thread_accessed->lock_state_set[i].unlock_count += 1;
    if (thread_accessed->lock_state_set[i].unlock_count >= thread_accessed->lock_state_set[i].lock_count) {
        thread_accessed->lock_state_set[i].state = ReadHeld;
    }
}
// todo => handle post lock/unlock and check wether it was successfull!.
void wrap_pre_lock(void *wrapcxt, OUT void **user_data) {
    // printf("pre LOCK\n");
    void *addr = drwrap_get_arg(wrapcxt, 0);
    u64 thread_id = dr_get_thread_id(dr_get_current_drcontext());
    // printf("pthread_unlock called\n");
    i64 t_index = find_thread_by_tid(thread_id);
    if (t_index < 0) {
        // printf("error finding thread_id. %ld \n", thread_id);
        return;    
    }
    ThreadState *thread_accessed = &threads[t_index];
    int i;
    for (i = 0; i <= thread_accessed->lock_state_set_len; i++) {
        if (thread_accessed->lock_state_set[i].addr == (usize)addr) break;
        if (i >= thread_accessed->lock_state_set_len) {
            thread_accessed->lock_state_set[thread_accessed->lock_state_set_len].addr = (usize) addr;
            thread_accessed->lock_state_set[thread_accessed->lock_state_set_len].callee_thread_id = thread_id;
            thread_accessed->lock_state_set[thread_accessed->lock_state_set_len].state = WriteHeld;
            thread_accessed->lock_state_set[thread_accessed->lock_state_set_len].lock_count += 1;
            thread_accessed->lock_state_set_len += 1;
            return;
        };
    }
    thread_accessed->lock_state_set[i].lock_count += 1;
    if (thread_accessed->lock_state_set[i].lock_count >= thread_accessed->lock_state_set[i].lock_count) {
        // now the lock is write held
        thread_accessed->lock_write_held_set[thread_accessed->lock_write_held_set_len].addr = (usize) addr;
        thread_accessed->lock_write_held_set[thread_accessed->lock_write_held_set_len].callee_thread_id = thread_id;
        thread_accessed->lock_write_held_set[thread_accessed->lock_write_held_set_len].memory_access_count = memory_access_counter;
        thread_accessed->lock_write_held_set_len += 1;
    } else {
        // read held
        thread_accessed->lock_read_held_set[thread_accessed->lock_read_held_set_len].addr = (usize) addr;
        thread_accessed->lock_read_held_set[thread_accessed->lock_read_held_set_len].callee_thread_id = thread_id;
        thread_accessed->lock_read_held_set[thread_accessed->lock_read_held_set_len].memory_access_count = memory_access_counter;
        thread_accessed->lock_read_held_set_len += 1;
    }
}

void wrap_post_malloc(void *wrapcxt, void *user_data) {
    size_t size = (size_t)user_data;
    void *addr = drwrap_get_retval(wrapcxt);
    // must use dr_get_current_drcontext() instead of wrapcxt bc thread_id is corrupted otherwise
    u64 thread_id = dr_get_thread_id(dr_get_current_drcontext());
    // printf("malloc: %ld\n", thread_id);
    // printf("malloc called %p in tid %ld\n", (void*)0x0, thread_id);

    int j;
    for(j = 0; j < n_threads; j++) {
        // printf("%ld %ld \n", threads[j].thread_id, thread_id);
        if (threads[j].thread_id == thread_id) break;
        if (j == n_threads-1) return;
    }
    // printf("ADDED %ld \n", thread_id);
    allocations[n_allocs].addr = (usize)addr;
    allocations[n_allocs].callee_thread_id = thread_id;
    allocations[n_allocs].size = size;
    n_allocs += 1;
}
void wrap_pre_malloc(void *wrapcxt, OUT void **user_data) {
    size_t alloc_size = (size_t)drwrap_get_arg(wrapcxt, 0);
    *user_data = (void *)alloc_size;
}

void check_for_race(ThreadState *thread_state) {
    int write_set_i, read_set_i, lock_set_i1, lock_set_i2;
    for (write_set_i = 0; write_set_i <= thread_state->mem_write_set_len; write_set_i++) {
        for (read_set_i = 0; read_set_i <= thread_state->mem_read_set_len; read_set_i++) {
            if (thread_state->mem_write_set[write_set_i].memory_access_count > thread_state->mem_read_set[read_set_i].memory_access_count) {
                if (find_lockset_by_memory_access_count(thread_state->lock_write_held_set, thread_state->lock_write_held_set_len, thread_state->lock_write_held_set[write_set_i].memory_access_count) == -1 || find_lockset_by_memory_access_count(thread_state->lock_write_held_set, thread_state->lock_write_held_set_len, thread_state->mem_read_set[read_set_i].memory_access_count) == -1) {
                    printf("FOUND!\n");
                }
            }
        }
    }
}

// this is an event like fn that is envoked on every memory access (called by DynamRIO)
void memtrace(void *drcontext, u64 thread_id) {
    // printf("trace: %ld \n",thread_id );
    if (drcontext == NULL) return;
    per_thread_t *data;
    mem_ref_t *mem_ref, *buf_ptr;
    data = drmgr_get_tls_field(drcontext, tls_idx);
    buf_ptr = BUF_PTR(data->seg_base);
    
    i64 t_index = find_thread_by_tid(thread_id);
    if (t_index < 0) {
        BUF_PTR(data->seg_base) = data->buf_base;
        return;    
    }
    for (mem_ref = (mem_ref_t *)data->buf_base; mem_ref < buf_ptr; mem_ref++) {
        int j;
        for(j = 0; j < n_allocs; j++) {
            if (is_in_range((usize)mem_ref->addr, allocations[j].addr, allocations[j].addr + allocations[j].size)) break;
            
            if (j >= n_allocs-1) { 
                // commiting a sin but goto is the most simple way to continue an outer loop in C
                goto continue_outer_loop;
            }
        }
        // no allocations, no mem shared
        if (n_allocs <= 0) {
            BUF_PTR(data->seg_base) = data->buf_base;
            return;
        }
        memory_access_counter++;
        if (memory_access_counter >= LLONG_MAX) DR_ASSERT(false);
        u64 thread_id_owning_accessed_addr = allocations[j].callee_thread_id;
        // printf("COMING THROUGH %ld \n", thread_id_owning_accessed_addr);

        i64 thread_states_index_owning_accessed_addr = find_thread_by_tid(thread_id_owning_accessed_addr);
        if (thread_states_index_owning_accessed_addr < 0) {
            printf("error finding thread_id. %ld \n", thread_id);
            BUF_PTR(data->seg_base) = data->buf_base;
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
            thread_accessed->mem_write_set[thread_accessed->mem_write_set_len].memory_access_count = memory_access_counter;
            thread_accessed->mem_write_set_len += 1;
        } else if(mem_ref->type == 0 || mem_ref->type == 227 || mem_ref->type == 225 || mem_ref->type == 197 || mem_ref->type == 228 || mem_ref->type == 229 || mem_ref->type == 299 || mem_ref->type == 173) {
            // mem read
            if (thread_accessed->mem_read_set_len >= thread_accessed->mem_read_set_capacity) thread_accessed->mem_read_set = increase_set_capacity(thread_accessed->mem_read_set, &thread_accessed->mem_read_set_capacity);
            if (thread_accessed->mem_read_set == NULL) exit(1);
            thread_accessed->mem_read_set[thread_accessed->mem_read_set_len].address_accessed = (usize)mem_ref->addr;
            thread_accessed->mem_read_set[thread_accessed->mem_read_set_len].opcode = mem_ref->type;
            thread_accessed->mem_read_set[thread_accessed->mem_read_set_len].callee_thread_id = thread_id;
            thread_accessed->mem_read_set[thread_accessed->mem_read_set_len].size = mem_ref->size;
            thread_accessed->mem_read_set[thread_accessed->mem_read_set_len].memory_access_count = memory_access_counter;
            thread_accessed->mem_read_set_len += 1;
        }
        check_for_race(thread_accessed);
        continue_outer_loop:;
        data->num_refs++;
        BUF_PTR(data->seg_base) = data->buf_base;
    }
}

