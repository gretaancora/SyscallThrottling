#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/syscalls.h>
#include <asm/processor-flags.h>
#include <linux/nospec.h>
#include "monitor.h"
#include "../include/vtpmo.h" 

#define INST_LEN 5
unsigned char original_inst[INST_LEN];
unsigned char jump_inst[INST_LEN];
unsigned long dispatcher_addr = 0;


// INIZIO CODICE DI DISCOVERY DELLA SYSCALL TABLE

extern int sys_vtpmo(unsigned long vaddr);

#define ADDRESS_MASK 0xfffffffffffff000//to migrate

#define START             0xffffffff00000000ULL        // use this as starting address
#define MAX_ADDR        0xfffffffffff00000ULL
//entry deprecate
#define FIRST_NI_SYSCALL    134
#define SECOND_NI_SYSCALL    174
#define THIRD_NI_SYSCALL    182 
#define FOURTH_NI_SYSCALL    183
#define FIFTH_NI_SYSCALL    214    
#define SIXTH_NI_SYSCALL    215    
#define SEVENTH_NI_SYSCALL    236    

#define ENTRIES_TO_EXPLORE 256

int good_area(unsigned long *);
int validate_page(unsigned long *);
void syscall_table_finder(void);

unsigned long **hacked_syscall_tbl = NULL;
unsigned long *hacked_ni_syscall = NULL;

extern void monitor_trampoline(void);

unsigned long sys_call_table_address = 0x0;
module_param(sys_call_table_address, ulong, 0660);

unsigned long sys_ni_syscall_address = 0x0;
module_param(sys_ni_syscall_address, ulong, 0660);

int good_area(unsigned long * addr){
    int i;
    for(i=1;i<FIRST_NI_SYSCALL;i++){
        if(addr[i] == addr[FIRST_NI_SYSCALL]) goto bad_area;
    }    
    return 1;
bad_area:
    return 0;
}

int validate_page(unsigned long *addr){
    int i = 0;
    unsigned long page     = (unsigned long) addr;
    unsigned long new_page     = (unsigned long) addr;
    for(; i < PAGE_SIZE; i+=sizeof(void*)){        
        new_page = page+i+SEVENTH_NI_SYSCALL*sizeof(void*);
            
        if( 
            ( (page+PAGE_SIZE) == (new_page & ADDRESS_MASK) )
            && sys_vtpmo(new_page) == NO_MAP
        ) 
            break;
        addr = (unsigned long*) (page+i);
        if(
               ( (addr[FIRST_NI_SYSCALL] & 0x3  ) == 0 )        
               && (addr[FIRST_NI_SYSCALL] != 0x0 )            
               && (addr[FIRST_NI_SYSCALL] > 0xffffffff00000000 )    
            &&   ( addr[FIRST_NI_SYSCALL] == addr[SECOND_NI_SYSCALL] )
            &&   ( addr[FIRST_NI_SYSCALL] == addr[THIRD_NI_SYSCALL]     )    
            &&   ( addr[FIRST_NI_SYSCALL] == addr[FOURTH_NI_SYSCALL] )
            &&   ( addr[FIRST_NI_SYSCALL] == addr[FIFTH_NI_SYSCALL] )    
            &&   ( addr[FIRST_NI_SYSCALL] == addr[SIXTH_NI_SYSCALL] )
            &&   ( addr[FIRST_NI_SYSCALL] == addr[SEVENTH_NI_SYSCALL] )    
            &&   (good_area(addr))
        ){
            hacked_ni_syscall = (void*)(addr[FIRST_NI_SYSCALL]);                
            sys_ni_syscall_address = (unsigned long)hacked_ni_syscall;
            hacked_syscall_tbl = (void*)(addr);                
            sys_call_table_address = (unsigned long) hacked_syscall_tbl;
            return 1;
        }
    }
    return 0;
}

void syscall_table_finder(void){
    unsigned long k; 
    unsigned long candidate; 

    for(k=START; k < MAX_ADDR; k+=4096){ 
        candidate = k;
        if((sys_vtpmo(candidate) != NO_MAP)){ 
            if(validate_page( (unsigned long *)(candidate)) ){ 
                printk("%s: syscall table found at %px\n",MODNAME,(void*)(hacked_syscall_tbl));
                printk("%s: sys_ni_syscall found at %px\n",MODNAME,(void*)(hacked_ni_syscall));
                break;
            }
        }
    }
}

unsigned long cr0, cr4;

static inline void write_cr0_forced(unsigned long val){
        unsigned long __force_order;
        asm volatile("mov %0, %%cr0" : "+r"(val), "+m"(__force_order));
}

static inline void protect_memory(void){
        write_cr0_forced(cr0);
}

static inline void unprotect_memory(void){
        write_cr0_forced(cr0 & ~X86_CR0_WP);
}

static inline void write_cr4_forced(unsigned long val){
        unsigned long __force_order;
        asm volatile("mov %0, %%cr4" : "+r"(val), "+m"(__force_order));
}

static inline void conditional_cet_disable(void){
#ifdef X86_CR4_CET
        if (cr4 & X86_CR4_CET)
                write_cr4_forced(cr4 & ~X86_CR4_CET);
#endif
}

static inline void conditional_cet_enable(void){
#ifdef X86_CR4_CET
        if (cr4 & X86_CR4_CET)
                write_cr4_forced(cr4);
#endif
}

static inline void begin_syscall_table_hack(void){
        preempt_disable();
        cr0 = read_cr0(); 
        cr4 = native_read_cr4(); 
        conditional_cet_disable(); 
        unprotect_memory(); 
}

static inline void end_syscall_table_hack(void){
        protect_memory(); 
        conditional_cet_enable(); 
        preempt_enable();
}

// FINE CODICE DI DISCOVERY DELLA SYSCALL TABLE


// TRAMPOLINO ASSEMBLY (con mitigazione Spectre v1)
//naked: non fa introdurre prologo, lascia puliti i registri
__attribute__((naked)) void monitor_trampoline(void) {
    asm volatile(
        "movq 120(%%rdi), %%rax\n\t"        // estrae nr syscall
        
        "cmpq $512, %%rax\n\t"              
        "jae handle_invalid_syscall\n\t"    
        
        //mitigazione Spectre v1
        "sbbq %%rcx, %%rcx\n\t"             
        "andq %%rcx, %%rax\n\t"             

        "leaq shadow_syscall_tbl(%%rip), %%rcx\n\t" // mettiamo la tabella in %rcx
        "movq (%%rcx, %%rax, 8), %%rax\n\t"         // %rax = shadow_syscall_tbl[orig_ax]
        "jmp __x86_indirect_thunk_rax\n\t"          // JUMP

        "handle_invalid_syscall:\n\t"
        "movq sys_ni_syscall_address(%%rip), %%rax\n\t"
        "jmp __x86_indirect_thunk_rax\n\t"
        
        //diciamo al compilatore che stiamo sporcando solo rax e rcx
        ::: "rax", "rcx" 
    );
}


unsigned long shadow_syscall_tbl[512];
EXPORT_SYMBOL(shadow_syscall_tbl);

// MONITOR (puntato dalle entry relative alle syscall critiche)

asmlinkage long targeted_monitor_entry(struct pt_regs *regs) {
    long ret;
    int sys_num = regs->orig_ax;
    
    if (unlikely(sys_num < 0 || sys_num > 511)) return -ENOSYS;
    //mitigazione Spectre v1
    sys_num = array_index_nospec(sys_num, 512);
    
    typedef asmlinkage long (*sys_call_ptr_t)(struct pt_regs *);
    sys_call_ptr_t orig_sys;

    this_cpu_inc(in_flight);

    //controlliamo l'interruttore, lo smontaggio e le regole su UID/Prog
    if (unlikely(READ_ONCE(module_is_unloading)) || !READ_ONCE(monitor_is_on) || !should_monitor_target()) {
        goto call_orig;
    }

    //se deve essere monitorata entra nel fast path
    if (wait_for_token() == -ERESTARTSYS) {
        this_cpu_dec(in_flight);
        return -ERESTARTSYS;
    }

call_orig:
    //eseguiamo la syscall originale
    orig_sys = (sys_call_ptr_t)hacked_syscall_tbl[sys_num];
    ret = orig_sys(regs);

    this_cpu_dec(in_flight);
    return ret;
}


//  GESTIONE DINAMICA DELLE SYSCALL 

int hook_sys(int sys_num) {
    if (sys_num < 0 || sys_num > 511) return -EINVAL;
    //mitigazione Spectre v1
    sys_num = array_index_nospec(sys_num, 512);
    //dirottiamo la entry della shadow table verso il gestore 
    WRITE_ONCE(shadow_syscall_tbl[sys_num], (unsigned long)targeted_monitor_entry);
    printk(KERN_INFO "%s: Shadow Table dirottata con successo per syscall %d\n", MODNAME, sys_num);
    return 0;
}

int unhook_sys(int sys_num) {
    if (sys_num < 0 || sys_num > 511) return -EINVAL;
    //mitigazione Spectre v1
    sys_num = array_index_nospec(sys_num, 512);
    //ripristiniamo la entry della shadow table facendola puntare al puntatore originale del kernel
    WRITE_ONCE(shadow_syscall_tbl[sys_num], (unsigned long)hacked_syscall_tbl[sys_num]);
    return 0;
}

//ESPORTAZIONE DELLE INFORMAZIONI SULLE SYSCALL REGISTRATE PER IL DUMP DELLE CONFIGURAZIONI IN config.c

int get_hooked_syscalls_dump(char *buf, size_t size) {
    int len = 0;
    int i;
    if (!hacked_syscall_tbl) return 0;
    
    for (i = 0; i < 512; i++) {
        if (shadow_syscall_tbl[i] != (unsigned long)hacked_syscall_tbl[i]) {
            len += scnprintf(buf + len, size - len, "[%d] ", i);
        }
    }
    return len;
}


// INIT & CLEANUP DELL'HOOK

int init_hook(void) {
    int offset, i;
    static struct kprobe kp_x64 = { .symbol_name = "x64_sys_call" };

    syscall_table_finder();
    if (!hacked_syscall_tbl) {
        printk(KERN_ERR "%s: Syscall table non trovata!\n", MODNAME);
        return -1;
    }

    //inizializzazione shadow system call table
    for (i = 0; i < 512; i++) {
        shadow_syscall_tbl[i] = (unsigned long)hacked_syscall_tbl[i];
    }

    //otteniamo indirizzo dispatcher
    if (register_kprobe(&kp_x64)) {
        printk(KERN_ERR "%s: x64_sys_call non trovata!\n", MODNAME);
        return -1;
    }
    dispatcher_addr = (unsigned long)kp_x64.addr;
    unregister_kprobe(&kp_x64);
    
    //copiamo la prima istruzione del dispatcher per il successivo ripristino
    memcpy(original_inst, (void *)dispatcher_addr, INST_LEN);
    
    //iniettiamo il trampolino assembly
    jump_inst[0] = 0xE9;
    offset = (unsigned long)monitor_trampoline - dispatcher_addr - INST_LEN;
    memcpy(jump_inst + 1, &offset, sizeof(int));

    begin_syscall_table_hack();
    memcpy((void *)dispatcher_addr, jump_inst, INST_LEN);
    end_syscall_table_hack();

    printk(KERN_INFO "%s: Architettura Shadow Table attivata!\n", MODNAME);
    return 0;
}

void cleanup_hook(void) {
    if (!dispatcher_addr) return;
    
    //rispristiniamo il dispatcher
    begin_syscall_table_hack();
    memcpy((void *)dispatcher_addr, original_inst, INST_LEN);
    end_syscall_table_hack();

    printk(KERN_INFO "%s: Hook rimosso, memoria ripristinata.\n", MODNAME);
}
