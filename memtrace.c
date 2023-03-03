#include "include/memtrace.h"

static client_id_t client_id;
static void *mutex;        /* for multithread support */
static uint64 num_refs;    /* keep a global memory reference count */

static reg_id_t tls_seg;

#define MINSERT instrlist_meta_preinsert

static void
print_qualified_function_name(app_pc pc)
{
    module_data_t *mod = dr_lookup_module(pc);
    if (mod == NULL) {
        // If we end up in assembly code or generated code we'll likely never
        // get out again without stack scanning or frame pointer walking or
        // other strategies not yet part of drcallstack.
        printf("  <unknown module> @%p\n", pc);
        return;
    }
    drsym_info_t sym_info;
#define MAX_FUNC_LEN 1024
    char name[MAX_FUNC_LEN];
    char file[MAXIMUM_PATH];
    sym_info.struct_size = sizeof(sym_info);
    sym_info.name = name;
    sym_info.name_size = MAX_FUNC_LEN;
    sym_info.file = file;
    sym_info.file_size = MAXIMUM_PATH;
    const char *func = "<unknown>";
    drsym_error_t sym_res =
        drsym_lookup_address(mod->full_path, pc - mod->start, &sym_info, DRSYM_DEMANGLE);
    if (sym_res == DRSYM_SUCCESS)
        func = sym_info.name;
    dr_fprintf(STDERR, "  %s!%s\n", dr_module_preferred_name(mod), func);
    dr_free_module_data(mod);
}

static void
module_load_event(void *drcontext, const module_data_t *mod, bool loaded)
{
    size_t modoffs_lock;
    drsym_error_t sym_res_lock = drsym_lookup_symbol(mod->full_path, "pthread_mutex_lock", &modoffs_lock, DRSYM_DEMANGLE);
    if (sym_res_lock == DRSYM_SUCCESS) {
        app_pc towrap_lock = mod->start + modoffs_lock;
        bool ok = drwrap_wrap(towrap_lock, wrap_pre_lock, NULL);
        DR_ASSERT(ok);
    }

    size_t modoffs_unlock;
    drsym_error_t sym_res_unlock = drsym_lookup_symbol(mod->full_path, "pthread_mutex_unlock", &modoffs_unlock, DRSYM_DEMANGLE);
    if (sym_res_unlock == DRSYM_SUCCESS) {
        app_pc towrap_unlock = mod->start + modoffs_unlock;
        bool ok = drwrap_wrap(towrap_unlock, wrap_pre_unlock, NULL);
        DR_ASSERT(ok);
    }
    size_t modoffs_malloc;
    drsym_error_t sym_res_malloc = drsym_lookup_symbol(mod->full_path, "malloc", &modoffs_malloc, DRSYM_DEMANGLE);
    if (sym_res_malloc == DRSYM_SUCCESS) {
        app_pc towrap_malloc = mod->start + modoffs_malloc;
        bool ok = drwrap_wrap(towrap_malloc, NULL, wrap_post_malloc);
        DR_ASSERT(ok);
    }
}


/* clean_call dumps the memory reference info to the log file */
static void clean_call(void) {
    void *drcontext = dr_get_current_drcontext();
    u64 thread_id = dr_get_thread_id(drcontext);
    memtrace(drcontext, thread_id);
}

static void insert_load_buf_ptr(void *drcontext, instrlist_t *ilist, instr_t *where, reg_id_t reg_ptr) {
    dr_insert_read_raw_tls(drcontext, ilist, where, tls_seg,
                           tls_offs + MEMTRACE_TLS_OFFS_BUF_PTR, reg_ptr);
}

static void insert_update_buf_ptr(void *drcontext, instrlist_t *ilist, instr_t *where,
                      reg_id_t reg_ptr, int adjust) {
    MINSERT(
        ilist, where,
        XINST_CREATE_add(drcontext, opnd_create_reg(reg_ptr), OPND_CREATE_INT16(adjust)));
    dr_insert_write_raw_tls(drcontext, ilist, where, tls_seg,
                            tls_offs + MEMTRACE_TLS_OFFS_BUF_PTR, reg_ptr);
}

static void insert_save_type(void *drcontext, instrlist_t *ilist, instr_t *where, reg_id_t base,
                 reg_id_t scratch, ushort type) {
    scratch = reg_resize_to_opsz(scratch, OPSZ_2);
    MINSERT(ilist, where,
            XINST_CREATE_load_int(drcontext, opnd_create_reg(scratch),
                                  OPND_CREATE_INT16(type)));
    MINSERT(ilist, where,
            XINST_CREATE_store_2bytes(drcontext,
                                      OPND_CREATE_MEM16(base, offsetof(mem_ref_t, type)),
                                      opnd_create_reg(scratch)));
}

static void insert_save_size(void *drcontext, instrlist_t *ilist, instr_t *where, reg_id_t base,
                 reg_id_t scratch, ushort size) {
    scratch = reg_resize_to_opsz(scratch, OPSZ_2);
    MINSERT(ilist, where,
            XINST_CREATE_load_int(drcontext, opnd_create_reg(scratch),
                                  OPND_CREATE_INT16(size)));
    MINSERT(ilist, where,
            XINST_CREATE_store_2bytes(drcontext,
                                      OPND_CREATE_MEM16(base, offsetof(mem_ref_t, size)),
                                      opnd_create_reg(scratch)));
}

static void insert_save_pc(void *drcontext, instrlist_t *ilist, instr_t *where, reg_id_t base,
               reg_id_t scratch, app_pc pc) {
    instrlist_insert_mov_immed_ptrsz(drcontext, (ptr_int_t)pc, opnd_create_reg(scratch),
                                     ilist, where, NULL, NULL);
    MINSERT(ilist, where,
            XINST_CREATE_store(drcontext,
                               OPND_CREATE_MEMPTR(base, offsetof(mem_ref_t, addr)),
                               opnd_create_reg(scratch)));
}

static void insert_save_addr(void *drcontext, instrlist_t *ilist, instr_t *where, opnd_t ref,
                 reg_id_t reg_ptr, reg_id_t reg_addr) {
    bool ok;
    /* we use reg_ptr as scratch to get addr */
    ok = drutil_insert_get_mem_addr(drcontext, ilist, where, ref, reg_addr, reg_ptr);
    DR_ASSERT(ok);
    insert_load_buf_ptr(drcontext, ilist, where, reg_ptr);
    MINSERT(ilist, where,
            XINST_CREATE_store(drcontext,
                               OPND_CREATE_MEMPTR(reg_ptr, offsetof(mem_ref_t, addr)),
                               opnd_create_reg(reg_addr)));
}

/* insert inline code to add an instruction entry into the buffer */
static void instrument_instr(void *drcontext, instrlist_t *ilist, instr_t *where, instr_t *instr) {
    /* We need two scratch registers */
    reg_id_t reg_ptr, reg_tmp;
    /* we don't want to predicate this, because an instruction fetch always occurs */
    instrlist_set_auto_predicate(ilist, DR_PRED_NONE);
    if (drreg_reserve_register(drcontext, ilist, where, NULL, &reg_ptr) !=
            DRREG_SUCCESS ||
        drreg_reserve_register(drcontext, ilist, where, NULL, &reg_tmp) !=
            DRREG_SUCCESS) {
        DR_ASSERT(false); /* cannot recover */
        return;
    }
    insert_load_buf_ptr(drcontext, ilist, where, reg_ptr);
    insert_save_type(drcontext, ilist, where, reg_ptr, reg_tmp,
                     (ushort)instr_get_opcode(instr));
    insert_save_size(drcontext, ilist, where, reg_ptr, reg_tmp,
                     (ushort)instr_length(drcontext, instr));
    insert_save_pc(drcontext, ilist, where, reg_ptr, reg_tmp, instr_get_app_pc(instr));
    insert_update_buf_ptr(drcontext, ilist, where, reg_ptr, sizeof(mem_ref_t));
    /* Restore scratch registers */
    if (drreg_unreserve_register(drcontext, ilist, where, reg_ptr) != DRREG_SUCCESS ||
        drreg_unreserve_register(drcontext, ilist, where, reg_tmp) != DRREG_SUCCESS)
        DR_ASSERT(false);
    instrlist_set_auto_predicate(ilist, instr_get_predicate(where));
}

/* insert inline code to add a memory reference info entry into the buffer */
static void instrument_mem(void *drcontext, instrlist_t *ilist, instr_t *where, opnd_t ref,
               bool write) {
    /* We need two scratch registers */
    reg_id_t reg_ptr, reg_tmp;
    if (drreg_reserve_register(drcontext, ilist, where, NULL, &reg_ptr) !=
            DRREG_SUCCESS ||
        drreg_reserve_register(drcontext, ilist, where, NULL, &reg_tmp) !=
            DRREG_SUCCESS) {
        DR_ASSERT(false); /* cannot recover */
        return;
    }
    /* save_addr should be called first as reg_ptr or reg_tmp maybe used in ref */
    insert_save_addr(drcontext, ilist, where, ref, reg_ptr, reg_tmp);
    insert_save_type(drcontext, ilist, where, reg_ptr, reg_tmp,
                     write ? REF_TYPE_WRITE : REF_TYPE_READ);
    insert_save_size(drcontext, ilist, where, reg_ptr, reg_tmp,
                     (ushort)drutil_opnd_mem_size_in_bytes(ref, where));
    insert_update_buf_ptr(drcontext, ilist, where, reg_ptr, sizeof(mem_ref_t));
    /* Restore scratch registers */
    if (drreg_unreserve_register(drcontext, ilist, where, reg_ptr) != DRREG_SUCCESS ||
        drreg_unreserve_register(drcontext, ilist, where, reg_tmp) != DRREG_SUCCESS)
        DR_ASSERT(false);
}

/* For each memory reference app instr, we insert inline code to fill the buffer
 * with an instruction entry and memory reference entries.
 */
static dr_emit_flags_t event_app_instruction(void *drcontext, void *tag, instrlist_t *bb, instr_t *where,
                      bool for_trace, bool translating, void *user_data) {
    int i;

    /* Insert code to add an entry for each app instruction. */
    /* Use the drmgr_orig_app_instr_* interface to properly handle our own use
     * of drutil_expand_rep_string() and drx_expand_scatter_gather() (as well
     * as another client/library emulating the instruction stream).
     */
    instr_t *instr_fetch = drmgr_orig_app_instr_for_fetch(drcontext);
    if (instr_fetch != NULL &&
        (instr_reads_memory(instr_fetch) || instr_writes_memory(instr_fetch))) {
        DR_ASSERT(instr_is_app(instr_fetch));
        instrument_instr(drcontext, bb, where, instr_fetch);
    }

    /* Insert code to add an entry for each memory reference opnd. */
    instr_t *instr_operands = drmgr_orig_app_instr_for_operands(drcontext);
    if (instr_operands == NULL ||
        (!instr_reads_memory(instr_operands) && !instr_writes_memory(instr_operands)))
        return DR_EMIT_DEFAULT;
    DR_ASSERT(instr_is_app(instr_operands));

    for (i = 0; i < instr_num_srcs(instr_operands); i++) {
        if (opnd_is_memory_reference(instr_get_src(instr_operands, i)))
            instrument_mem(drcontext, bb, where, instr_get_src(instr_operands, i), false);
    }

    for (i = 0; i < instr_num_dsts(instr_operands); i++) {
        if (opnd_is_memory_reference(instr_get_dst(instr_operands, i)))
            instrument_mem(drcontext, bb, where, instr_get_dst(instr_operands, i), true);
    }

    /* insert code to call clean_call for processing the buffer */
    if (/* XXX i#1698: there are constraints for code between ldrex/strex pairs,
         * so we minimize the instrumentation in between by skipping the clean call.
         * As we're only inserting instrumentation on a memory reference, and the
         * app should be avoiding memory accesses in between the ldrex...strex,
         * the only problematic point should be before the strex.
         * However, there is still a chance that the instrumentation code may clear the
         * exclusive monitor state.
         * Using a fault to handle a full buffer should be more robust, and the
         * forthcoming buffer filling API (i#513) will provide that.
         */
        IF_AARCHXX_ELSE(!instr_is_exclusive_store(instr_operands), true))
        dr_insert_clean_call(drcontext, bb, where, (void *)clean_call, false, 0);

    return DR_EMIT_DEFAULT;
}

/* We transform string loops into regular loops so we can more easily
 * monitor every memory reference they make.
 */
dr_emit_flags_t event_bb_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace,
                 bool translating) {
    if (!drutil_expand_rep_string(drcontext, bb)) {
        DR_ASSERT(false);
        /* in release build, carry on: we'll just miss per-iter refs */
    }
    if (!drx_expand_scatter_gather(drcontext, bb, NULL)) {
        DR_ASSERT(false);
    }
    return DR_EMIT_DEFAULT;
}

static bool
event_pre_syscall(void *drcontext, int sysnum)
{
    printf("PRE SYSCALL %d %d \n", sysnum, SYS_write);
    if (sysnum == SYS_waitid) printf("mutex related syscall \n");
    bool modify_write = (sysnum == SYS_write);
    dr_atomic_add32_return_sum(&num_syscalls, 1);

    if (modify_write) {
        printf("is modifying...\n");
        /* store params for access post-syscall */
        int i;
        per_thread_t *data = (per_thread_t *)drmgr_get_cls_field(drcontext, tls_idx);
        for (i = 0; i < SYS_MAX_ARGS; i++)
            data->param[i] = dr_syscall_get_param(drcontext, i);
        /* suppress stderr */
        if (dr_syscall_get_param(drcontext, 0) == (reg_t)STDERR) {
            /* pretend it succeeded */
            /* return the #bytes == 3rd param */
            dr_syscall_result_info_t info = {
                sizeof(info),
            };
            info.succeeded = true;
            info.value = dr_syscall_get_param(drcontext, 2);
            dr_syscall_set_result_ex(drcontext, &info);

            return false; /* skip syscall */
        } else if (dr_syscall_get_param(drcontext, 0) == (reg_t)STDOUT) {
            if (!data->repeat) {
                /* redirect stdout to stderr (unless it's our repeat) */
                dr_syscall_set_param(drcontext, 0, (reg_t)STDERR);
            }
            /* we're going to repeat this syscall once */
            data->repeat = !data->repeat;
        }
    }
    return true; /* execute normally */
}

static void event_thread_init(void *drcontext) {
    u64 thread_id = dr_get_thread_id(drcontext);
    if(!mem_analyse_new_thread_init(thread_id)) DR_ASSERT(false);
    per_thread_t *data = dr_thread_alloc(drcontext, sizeof(per_thread_t));
    DR_ASSERT(data != NULL);
    drmgr_set_tls_field(drcontext, tls_idx, data);

    /* Keep seg_base in a per-thread data structure so we can get the TLS
     * slot and find where the pointer points to in the buffer.
     */
    data->seg_base = dr_get_dr_segment_base(tls_seg);
    data->buf_base = dr_raw_mem_alloc(MEM_BUF_SIZE, DR_MEMPROT_READ | DR_MEMPROT_WRITE, NULL);
    DR_ASSERT(data->seg_base != NULL && data->buf_base != NULL);
    /* put buf_base to TLS as starting buf_ptr */
    BUF_PTR(data->seg_base) = data->buf_base;
    data->num_refs = 0;
    data->logf = stderr;
}

static void event_thread_exit(void *drcontext) {
    u64 thread_id = dr_get_thread_id(drcontext);
    mem_analyse_thread_exit();
    per_thread_t *data;
    memtrace(drcontext, thread_id); /* dump any remaining buffer entries */
    data = drmgr_get_tls_field(drcontext, tls_idx);
    dr_mutex_lock(mutex);
    num_refs += data->num_refs;
    dr_mutex_unlock(mutex);
    dr_raw_mem_free(data->buf_base, MEM_BUF_SIZE);
    dr_thread_free(drcontext, data, sizeof(per_thread_t));
}

static void event_exit(void) {
    mem_analyse_exit();

    if (!dr_raw_tls_cfree(tls_offs, MEMTRACE_TLS_COUNT))
        DR_ASSERT(false);

    if (!drmgr_unregister_tls_field(tls_idx) ||
        !drmgr_unregister_thread_init_event(event_thread_init) ||
        !drmgr_unregister_thread_exit_event(event_thread_exit) ||
        // !drmgr_unregister_pre_syscall_event(event_pre_syscall) ||
        !drmgr_unregister_bb_app2app_event(event_bb_app2app) ||
        !drmgr_unregister_bb_insertion_event(event_app_instruction) ||
        drreg_exit() != DRREG_SUCCESS)
        DR_ASSERT(false);

    dr_mutex_destroy(mutex);
    drutil_exit();
    drmgr_exit();
    drx_exit();    drcallstack_exit();
    drwrap_exit();
    drsym_exit();
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
    /* We need 2 reg slots beyond drreg's eflags slots => 3 slots */
    drreg_options_t drreg_ops = { sizeof(drreg_ops), 3, false };
    drcallstack_options_t callstack_ops = {
        sizeof(callstack_ops),
    };
    
    if (!mem_analyse_init()) DR_ASSERT(false);

    if (!drmgr_init() || drreg_init(&drreg_ops) != DRREG_SUCCESS || !drutil_init() ||
        !drx_init())
        DR_ASSERT(false);

    /* register events */
    dr_register_exit_event(event_exit);
    if (!drmgr_register_thread_init_event(event_thread_init) ||
        !drmgr_register_thread_exit_event(event_thread_exit) ||
        // !drmgr_register_pre_syscall_event(event_pre_syscall) ||
        !drmgr_register_bb_app2app_event(event_bb_app2app, NULL) ||
        !drmgr_register_bb_instrumentation_event(NULL /*analysis_func*/, event_app_instruction, NULL) ||
        !drwrap_init() || 
        drcallstack_init(&callstack_ops) != DRCALLSTACK_SUCCESS ||
        drsym_init(0) != DRSYM_SUCCESS ||
        !drmgr_register_module_load_event(module_load_event))
        DR_ASSERT(false);

    client_id = id;
    mutex = dr_mutex_create();

    tls_idx = drmgr_register_tls_field();
    DR_ASSERT(tls_idx != -1);
    /* The TLS field provided by DR cannot be directly accessed from the code cache.
     * For better performance, we allocate raw TLS so that we can directly
     * access and update it with a single instruction.
     */
    if (!dr_raw_tls_calloc(&tls_seg, &tls_offs, MEMTRACE_TLS_COUNT, 0))
        DR_ASSERT(false);
}