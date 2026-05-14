#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/percpu.h>
#include <linux/sched/signal.h>
#include <linux/workqueue.h>
#include "monitor.h"
#define DEFAULT_MAX_LIMIT 100

atomic_t global_currently_blocked = ATOMIC_INIT(0);
atomic_t global_peak_blocked = ATOMIC_INIT(0);
u64 monitor_start_time_ns = 0;

DEFINE_PER_CPU(struct monitor_telemetry, monitor_stats);
DECLARE_WAIT_QUEUE_HEAD(monitor_wq);

//lockless token bucket
atomic_t available_tokens = ATOMIC_INIT(DEFAULT_MAX_LIMIT);
int max_limit = DEFAULT_MAX_LIMIT;
//htimer che scandisce le epoche
static struct hrtimer monitor_timer;
//worqueue scandita dal bottom half del hrtimer
static struct work_struct wake_up_work;

// BOTTOM HALF 

static void timer_bottom_half(struct work_struct *work) {
    //riempiamo il secchio per la nuova epoca
    atomic_set(&available_tokens, READ_ONCE(max_limit)); 
    //svegliamo fino a max thread dalla coda
    wake_up_nr(&monitor_wq, READ_ONCE(max_limit));
}

// TOP HALF 

static enum hrtimer_restart monitor_timer_callback(struct hrtimer *timer) {
    schedule_work(&wake_up_work);
    return HRTIMER_NORESTART;
}

// SLOW-PATH

int __slow_path_gatekeeper(int remaining) {
    DEFINE_WAIT(wait);
    u64 sleep_start = 0, sleep_end = 0, delay = 0;
    struct monitor_telemetry *stats;
    int current_blocked, current_peak;

    //dobbiamo avviare il timer
    if (unlikely(remaining == READ_ONCE(max_limit) - 1)) {
        if (likely(!READ_ONCE(module_is_unloading)) && READ_ONCE(monitor_is_on)) {
            hrtimer_start(&monitor_timer, ktime_set(1, 0), HRTIMER_MODE_REL);
        }
        return 0; 
    }

    stats = this_cpu_ptr(&monitor_stats);
    sleep_start = ktime_get_ns();

    
    current_blocked = atomic_inc_return(&global_currently_blocked);
    current_peak = atomic_read(&global_peak_blocked);
    while (current_blocked > current_peak) {
        int old_peak = atomic_cmpxchg(&global_peak_blocked, current_peak, current_blocked);
        if (old_peak == current_peak) break;
        current_peak = old_peak;
    }

    stats->total_blocked++;

    for (;;) {
        int taken;
        
        prepare_to_wait_exclusive(&monitor_wq, &wait, TASK_INTERRUPTIBLE);

        if (unlikely(READ_ONCE(module_is_unloading)) || !READ_ONCE(monitor_is_on)) {
            break;
        }
        
        taken = atomic_dec_if_positive(&available_tokens);
        if (taken >= 0) {
            if (unlikely(taken == READ_ONCE(max_limit) - 1)) {
                if (likely(!READ_ONCE(module_is_unloading))) {
                    hrtimer_start(&monitor_timer, ktime_set(1, 0), HRTIMER_MODE_REL);
                }
            }
            break; 
        }

        schedule();
        
        if (signal_pending(current)) {
            atomic_dec(&global_currently_blocked);
            //evita il problema di Lost Wakeup nel caso in cui tutti i thread svegliati vengano colpiti da segnale
            wake_up_nr(&monitor_wq, 1);
            finish_wait(&monitor_wq, &wait);
            return -ERESTARTSYS; 
        }
    }

    atomic_dec(&global_currently_blocked);
    finish_wait(&monitor_wq, &wait);

    sleep_end = ktime_get_ns();
    delay = sleep_end - sleep_start; 
    if (delay > stats->peak_delay_ns) {
        stats->peak_delay_ns = delay;
        stats->peak_uid = current_euid().val;
        memcpy(stats->peak_comm, current->comm, TASK_COMM_LEN);
    }
    
    return 0;
}

//RISVEGLIO DI TUTTI I THREAD IN CODA (usato in caso di spegnimento del monitor)

void release_all_blocked_tasks(void) {
    wake_up_all(&monitor_wq);
}

//INIT & CLEANUP DEL GATEKEEPER

int init_gatekeeper(void) {
    //inizializzazione del timer
    hrtimer_setup(&monitor_timer, monitor_timer_callback, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    
    //inizializzazione della workqueue
    INIT_WORK(&wake_up_work, timer_bottom_half);

    monitor_start_time_ns = ktime_get_ns();
    printk(KERN_INFO "%s: Gatekeeper inizializzato.\n", MODNAME);
    return 0;
}

void cleanup_gatekeeper(void) {
    WRITE_ONCE(module_is_unloading, true);
    
    // Inondiamo il secchio di token per far defluire tutti
    atomic_set(&available_tokens, INT_MAX); 
    max_limit = INT_MAX; 
    
    // Svegliamo tutti quelli rimasti in coda
    wake_up_all(&monitor_wq);

    // cancelliamo il timer
    hrtimer_cancel(&monitor_timer);
    //aspettiamo che il Bottom Half finisca se era in esecuzione
    cancel_work_sync(&wake_up_work);
    
    printk(KERN_INFO "%s: Gatekeeper smontato in sicurezza.\n", MODNAME);
}
