#include "include/instrument.h"

#define MAX_THREADS 100
#define MAX_ALLOCS 10000
#define MAX_LOCKS 10000
const u64 linear_set_size_increment = 1000000;



typedef enum LockWritability {    
    ReadHeld = 0,
    WriteHeld = 1,
} LockWritability;

typedef struct LockAccess {
   LockWritability state;
   usize addr;
   u64 callee_thread_id;
} LockAccess;


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
   u32 has_lock;
   LockAccess lock_access;
} MemoryAccess;


typedef struct LockState {
   LockWritability state;
   usize addr;
   u64 callee_thread_id;
   usize lock_count;
   usize unlock_count;
} LockState;


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

    isize last_locked_mutex_addr;
} ThreadState;

usize checked_but_ok_races_counter = 0;
usize detected_races_counter = 0;


// max program_allocations is seperate from thread state since it needs to be itreated thorugh on every error check
MemoryAllocation program_allocations[MAX_ALLOCS] = {};
pthread_mutex_t mutex_program_allocs = PTHREAD_MUTEX_INITIALIZER;
u64 n_program_allocs = 0;

ThreadState program_threads[MAX_THREADS] = {};
pthread_mutex_t mutex_program_threads = PTHREAD_MUTEX_INITIALIZER;
u64 n_program_threads = 0;

LockState program_locks[MAX_LOCKS] = {};
pthread_mutex_t mutex_program_locks = PTHREAD_MUTEX_INITIALIZER;
u64 n_program_locks;

u64 memory_access_counter = 0;

// util fns..
void *increase_set_capacity(void *set, u64 *set_capacity) {
    *set_capacity += linear_set_size_increment;
    printf("new set_capacity: %ld \n", *set_capacity);
    return realloc(set, *set_capacity);
}

i64 find_thread_by_tid(u64 tid) {
    u64 i;
    for (i = 0; i <= n_program_threads; i++) {
        if (program_threads[i].thread_id == tid) {
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
    printf("------ results ------ \n");
    // u64 j;
    // for (j = 0; j < n_program_threads; j++) {
    //     printf("Thread id: %ld \n", program_threads[j].thread_id);
    //     printf("mem_write_set_len: %ld \n", program_threads[j].mem_write_set_len);
    //     printf("mem_read_set_len: %ld \n", program_threads[j].mem_read_set_len);
    //     printf("lock_state_set_len: %ld \n", program_threads[j].lock_state_set_len);
            
    //     u64 i;
    //     printf("program_locks: \n");
    //     for (i = 0; i < program_threads[j].lock_state_set_len; i++) {
    //         printf("lock addr: %ld, (end)state: %d \n", program_threads[j].lock_state_set[i].addr, program_threads[j].lock_state_set[i].state);
    //     }
        
    //     free(program_threads[j].mem_write_set);
    //     free(program_threads[j].mem_read_set);
    //     free(program_threads[j].lock_state_set);
    // }
    printf("detected_races_counter: %ld, checked_but_ok_races_counter: %ld \n", detected_races_counter, checked_but_ok_races_counter);
}

u32 mem_analyse_init() { 
        return 1;
}

u32 mem_analyse_new_thread_init(void *drcontext) {
    if (drcontext == NULL) return 0;
    u64 thread_id = dr_get_thread_id(drcontext);
    // printf("init: %ld\n", thread_id);
    if (n_program_threads >= MAX_THREADS) return 0;
    program_threads[n_program_threads].thread_id = thread_id;

    program_threads[n_program_threads].mem_read_set = (MemoryAccess*)malloc(sizeof(MemoryAccess) * linear_set_size_increment);
    program_threads[n_program_threads].mem_read_set_capacity = linear_set_size_increment;
    if (program_threads[n_program_threads].mem_read_set == NULL) {
        printf("set allocation error \n");
        return 0;    
    }
    program_threads[n_program_threads].mem_write_set = (MemoryAccess*)malloc(sizeof(MemoryAccess) * linear_set_size_increment);
    program_threads[n_program_threads].mem_write_set_capacity = linear_set_size_increment;
    if (program_threads[n_program_threads].mem_write_set == NULL) {
        printf("set allocation error \n");
        return 0;
    }

    n_program_threads += 1;
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
    ThreadState *thread_accessed = &program_threads[t_index];
    int i;
    for (i = 0; i <= n_program_locks; i++) {
        if (program_locks[i].addr == (usize)addr) break;
        if (i >= n_program_locks) {
            pthread_mutex_lock(&mutex_program_locks);
            program_locks[n_program_locks].addr = (usize)addr;
            program_locks[n_program_locks].callee_thread_id = thread_id;
            program_locks[n_program_locks].state = ReadHeld;
            program_locks[n_program_locks].unlock_count += 1;
            // printf("new addr: %ld \n", program_locks[n_program_locks].addr);
            n_program_locks += 1;
            pthread_mutex_unlock(&mutex_program_locks);
            return;
        };
    }

    pthread_mutex_lock(&mutex_program_locks);
    program_locks[i].unlock_count += 1;
    if (program_locks[i].unlock_count >= program_locks[i].lock_count) {
        program_locks[i].state = ReadHeld;
    }
    pthread_mutex_unlock(&mutex_program_locks);
    thread_accessed->last_locked_mutex_addr = -1;
}
// todo => handle post lock/unlock and check wether it was successfull!.
void wrap_pre_lock(void *wrapcxt, OUT void **user_data) {
    // printf("pre LOCK\n");
    void *addr = drwrap_get_arg(wrapcxt, 0);
    // printf("locking: %ld \n", addr);
    u64 thread_id = dr_get_thread_id(dr_get_current_drcontext());
    // printf("pthread_unlock called\n");
    i64 t_index = find_thread_by_tid(thread_id);
    if (t_index < 0) {
        // printf("error finding thread_id. %ld \n", thread_id);
        return;    
    }
    ThreadState *thread_accessed = &program_threads[t_index];
    pthread_mutex_lock(&mutex_program_threads);
    thread_accessed->last_locked_mutex_addr = (usize)addr;
    pthread_mutex_unlock(&mutex_program_threads);
    int i;
    for (i = 0; i <= n_program_locks; i++) {
        if (program_locks[i].addr == (usize)addr) break;
        if (i >= n_program_locks) {
            pthread_mutex_lock(&mutex_program_locks);
            program_locks[n_program_locks].addr = (usize) addr;
            program_locks[n_program_locks].callee_thread_id = thread_id;
            program_locks[n_program_locks].state = WriteHeld;
            program_locks[n_program_locks].lock_count += 1;

            n_program_locks += 1;
            pthread_mutex_unlock(&mutex_program_locks);
            return;
        };
    }

    pthread_mutex_lock(&mutex_program_locks);
    program_locks[i].lock_count += 1;
    // todo => check wether it has to be < or <=
    if (program_locks[i].unlock_count <= program_locks[i].lock_count) {
        program_locks[i].state = WriteHeld;
    }
    pthread_mutex_unlock(&mutex_program_locks);
}

void wrap_post_malloc(void *wrapcxt, void *user_data) {
    size_t size = (size_t)user_data;
    void *addr = drwrap_get_retval(wrapcxt);
    // must use dr_get_current_drcontext() instead of wrapcxt bc thread_id is corrupted otherwise
    u64 thread_id = dr_get_thread_id(dr_get_current_drcontext());
    // printf("malloc: %ld\n", thread_id);
    // printf("malloc called %p in tid %ld\n", (void*)0x0, thread_id);

    int j;
    for(j = 0; j < n_program_threads; j++) {
        // printf("%ld %ld \n", program_threads[j].thread_id, thread_id);
        if (program_threads[j].thread_id == thread_id) break;
        if (j == n_program_threads-1) return;
    }
    pthread_mutex_lock(&mutex_program_allocs);
    program_allocations[n_program_allocs].addr = (usize)addr;
    program_allocations[n_program_allocs].callee_thread_id = thread_id;
    program_allocations[n_program_allocs].size = size;
    n_program_allocs += 1;
    pthread_mutex_unlock(&mutex_program_allocs);
}
void wrap_pre_malloc(void *wrapcxt, OUT void **user_data) {
    size_t alloc_size = (size_t)drwrap_get_arg(wrapcxt, 0);
    *user_data = (void *)alloc_size;
}

void check_for_race(ThreadState *thread_state) {
    int thread_i, write_set_i, write_set_i_plus1, read_set_i;
    for (write_set_i = 0; write_set_i < thread_state->mem_write_set_len; write_set_i++) {
        for (thread_i = 0; thread_i < n_program_threads; thread_i++) {
            ThreadState *iterated_thread = &program_threads[thread_i];
            // check write-read pairs
            for (read_set_i = 0; read_set_i < iterated_thread->mem_read_set_len; read_set_i++) {
                if (thread_state->mem_write_set[write_set_i].address_accessed == iterated_thread->mem_read_set[read_set_i].address_accessed) {
                    if (thread_state->mem_write_set[write_set_i].callee_thread_id != iterated_thread->mem_write_set[write_set_i_plus1].callee_thread_id) {
                        if (thread_state->mem_write_set[write_set_i].memory_access_count > iterated_thread->mem_read_set[read_set_i].memory_access_count) {
                            if(thread_state->mem_write_set[write_set_i].lock_access.state != WriteHeld && iterated_thread->mem_read_set[read_set_i].lock_access.state != ReadHeld) {
                                detected_races_counter += 1;
                                break;
                            }
                        }
                    }
                }
            }   
            // write write-read pairs
            for (write_set_i_plus1 = 0; write_set_i_plus1 < iterated_thread->mem_write_set_len; write_set_i_plus1++) {
                if (thread_state->mem_write_set[write_set_i].address_accessed == iterated_thread->mem_write_set[write_set_i_plus1].address_accessed) {
                    if (thread_state->mem_write_set[write_set_i].callee_thread_id != iterated_thread->mem_write_set[write_set_i_plus1].callee_thread_id) {
                        // printf("addr match(not smae thread id) %ld, %ld \n", thread_state->mem_write_set[write_set_i].address_accessed, iterated_thread->mem_write_set[write_set_i_plus1].address_accessed);
                        if(thread_state->mem_write_set[write_set_i].lock_access.state != WriteHeld && iterated_thread->mem_write_set[write_set_i_plus1].lock_access.state != WriteHeld) {
                            detected_races_counter += 1;
                            printf("race on: %ld \n", thread_state->mem_write_set[write_set_i].address_accessed);
                            // printf("lol\n");
                            // printf("state match %d, %d \n", thread_state->mem_write_set[write_set_i].lock_access.state, iterated_thread->mem_write_set[write_set_i_plus1].lock_access.state);
                            break;
                        }
                    }
                }
            }   
        }
        checked_but_ok_races_counter += 1;
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

        // no program_allocations, no mem shared
        if (n_program_allocs <= 0) {
            BUF_PTR(data->seg_base) = data->buf_base;
            return;
        }
        for(j = 0; j < n_program_allocs; j++) {
            if (is_in_range((usize)mem_ref->addr, program_allocations[j].addr, program_allocations[j].addr + program_allocations[j].size)) break;
            
            if (j >= n_program_allocs-1) { 
                // commiting a sin but goto is the most simple way to continue an outer loop in C
                goto continue_outer_loop;
            }
        }
        memory_access_counter++;
        if (memory_access_counter >= LLONG_MAX) DR_ASSERT(false);
        u64 thread_id_owning_accessed_addr = program_allocations[j].callee_thread_id;
        // printf("COMING THROUGH %ld \n", thread_id_owning_accessed_addr);

        i64 thread_states_index_owning_accessed_addr = find_thread_by_tid(thread_id_owning_accessed_addr);
        if (thread_states_index_owning_accessed_addr < 0) {
            printf("error finding thread_id. %ld \n", thread_id);
            BUF_PTR(data->seg_base) = data->buf_base;
            return;    
        }
        ThreadState *thread_accessed = &program_threads[thread_states_index_owning_accessed_addr];
        pthread_mutex_lock(&mutex_program_threads);
        i32 lock_state_i;
        if (thread_accessed->last_locked_mutex_addr != -1) {
            for (lock_state_i = 0; lock_state_i <= n_program_locks; lock_state_i++) {
                if (program_locks[lock_state_i].addr == thread_accessed->last_locked_mutex_addr) {
                    // printf("found: %ld %ld \n", program_locks[lock_state_i].addr, thread_accessed->last_locked_mutex_addr);
                    break;
                }
                if (lock_state_i >= n_program_locks) lock_state_i = -1;
            }
        } else {
            lock_state_i = thread_accessed->last_locked_mutex_addr;
            printf("asdassa \n");
        }
        if (mem_ref->type == 1 || mem_ref->type == 457 || mem_ref->type == 458 || mem_ref->type == 456 || mem_ref->type == 568) {
            // mem write
            if (thread_accessed->mem_write_set_len >= thread_accessed->mem_write_set_capacity) thread_accessed->mem_write_set = increase_set_capacity(thread_accessed->mem_write_set, &thread_accessed->mem_write_set_capacity);
            if (thread_accessed->mem_write_set == NULL) exit(1);
            thread_accessed->mem_write_set[thread_accessed->mem_write_set_len].address_accessed = (usize)mem_ref->addr;
            // printf("added to write set: %ld \n", (usize)mem_ref->addr);
            thread_accessed->mem_write_set[thread_accessed->mem_write_set_len].opcode = mem_ref->type;
            thread_accessed->mem_write_set[thread_accessed->mem_write_set_len].callee_thread_id = thread_id;
            thread_accessed->mem_write_set[thread_accessed->mem_write_set_len].size = mem_ref->size;
            thread_accessed->mem_write_set[thread_accessed->mem_write_set_len].memory_access_count = memory_access_counter;
            if (lock_state_i != -1) {
                LockAccess la = {program_locks[lock_state_i].state, program_locks[lock_state_i].addr, program_locks[lock_state_i].callee_thread_id};
                thread_accessed->mem_write_set[thread_accessed->mem_read_set_len].lock_access = la;
                thread_accessed->mem_write_set[thread_accessed->mem_read_set_len].has_lock = 1;
            } else {
                // todo => should not be invoked
                printf("no lock for addr %ld \n", mem_ref->addr);
            }
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
            if (lock_state_i != -1) {
                LockAccess la = {program_locks[lock_state_i].state, program_locks[lock_state_i].addr, program_locks[lock_state_i].callee_thread_id};
                thread_accessed->mem_read_set[thread_accessed->mem_read_set_len].lock_access = la;
                thread_accessed->mem_read_set[thread_accessed->mem_read_set_len].has_lock = 1;
            } else {
                printf("no lock for addr %ld \n", mem_ref->addr);
            }
            thread_accessed->mem_read_set_len += 1;
        }
        pthread_mutex_unlock(&mutex_program_threads);
        check_for_race(thread_accessed);
        continue_outer_loop:;
        data->num_refs++;
        BUF_PTR(data->seg_base) = data->buf_base;
    }
}

