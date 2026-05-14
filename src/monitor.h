#ifndef MONITOR_H
#define MONITOR_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#define MODNAME "SysMonitor"

// VARIABILI GLOBALI ESPORTATE

extern wait_queue_head_t monitor_wq;
extern atomic_t available_tokens;
extern int max_limit;
extern bool monitor_is_on; 
extern atomic_t global_peak_blocked;
extern atomic_t global_currently_blocked;
extern u64 monitor_start_time_ns;
DECLARE_PER_CPU(int, in_flight); 
extern bool module_is_unloading;

// Struttura per la telemetria
struct monitor_telemetry {
    u64 peak_delay_ns;       
    int peak_uid;            
    char peak_comm[16];      
    
    u64 total_blocked;       
    int currently_blocked;   
    int max_blocked_peak;    
}____cacheline_aligned;

DECLARE_PER_CPU(struct monitor_telemetry, monitor_stats);

// PROTOTIPI DELLE FUNZIONI

// Da gatekeeper.c
int init_gatekeeper(void);
void cleanup_gatekeeper(void);
int __slow_path_gatekeeper(int ticket);
void release_all_blocked_tasks(void);

static inline int wait_for_token(void) {
    int remaining;
    if (unlikely(READ_ONCE(module_is_unloading)) || !READ_ONCE(monitor_is_on)) return 0;
    
    //tentiamo di acquisire un gettone
    remaining = atomic_dec_if_positive(&available_tokens);
    
    if (likely(remaining >= 0)) {
        
        if (unlikely(remaining == READ_ONCE(max_limit) - 1)) {
            //devo riarmare il timer
            return __slow_path_gatekeeper(remaining); 
        }
        
        return 0;
    }
    
    return __slow_path_gatekeeper(remaining);
}


// Dal config.c
int init_config(void);
void cleanup_config(void);
int add_prog_rcu(const char *comm);
int del_prog_rcu(const char *comm);
int add_uid_rcu(int uid);
int del_uid_rcu(int uid);
bool should_monitor_target(void);
int dump_config_state(char *buf, size_t size);

// Dal driver.c
int init_driver(void);
void cleanup_driver(void);

// Da hook.c
int init_hook(void);
void cleanup_hook(void);
int hook_sys(int sys_num);
int unhook_sys(int sys_num);
asmlinkage long targeted_monitor_entry(struct pt_regs *regs);
int get_hooked_syscalls_dump(char *buf, size_t size);

#endif
