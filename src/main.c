#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/percpu.h>
#include "monitor.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Greta");
MODULE_DESCRIPTION("Syscall Rate-Limiter Monitor");


DEFINE_PER_CPU(int, in_flight);
bool module_is_unloading = false;


static int __init monitor_init(void) {
    int ret;

    printk(KERN_INFO "=======================================\n");
    printk(KERN_INFO "%s: Inizializzazione Modulo...\n", MODNAME);

    // 1. Inizializza le strutture dati RCU
    ret = init_config();
    if (ret) {
        printk(KERN_ERR "%s: Fallita inizializzazione Configurazione\n", MODNAME);
        return ret;
    }

    // 2. Inizializza il Gatekeeper
    ret = init_gatekeeper();
    if (ret) {
        printk(KERN_ERR "%s: Fallita inizializzazione Gatekeeper\n", MODNAME);
        goto fail_gatekeeper;
    }

    // 3. Inizializza il Driver
    ret = init_driver();
    if (ret) {
        printk(KERN_ERR "%s: Fallita inizializzazione Driver\n", MODNAME);
        goto fail_driver;
    }

    // 4. Inizializza Hook + Shadow Table
    ret = init_hook();
    if (ret) {
        printk(KERN_ERR "%s: Fallita inizializzazione Hook\n", MODNAME);
        goto fail_hook;
    }

    printk(KERN_INFO "%s: Modulo caricato e pronto all'uso.\n", MODNAME);
    return 0;

fail_hook:
    cleanup_driver();
fail_driver:
    cleanup_gatekeeper();
fail_gatekeeper:
    cleanup_config();
    return ret;
}

static void __exit monitor_exit(void) {
    int total_inflight;
    int cpu;

    printk(KERN_INFO "%s: Inizio procedura di smontaggio...\n", MODNAME);

    // 1. Avvisiamo globalmente dello smontaggio
    WRITE_ONCE(module_is_unloading, true);
    
    // 2. Tagliamo i ponti con lo User-Space
    cleanup_driver();

    // 3. Ripristiniamo il Kernel
    cleanup_hook(); 

    // 4. Liberiamo i threads in coda
    cleanup_gatekeeper();

    // 5. Aspettiamo che l'ultimo thread esca dal modulo
    do {
        total_inflight = 0;
        for_each_possible_cpu(cpu) {
            total_inflight += per_cpu(in_flight, cpu);
        }
        if (total_inflight > 0) {
            msleep(10);
        }
    } while (total_inflight > 0);

    // 6. Deallochiamo le strutture RCU
    cleanup_config();

    printk(KERN_INFO "%s: Modulo rimosso con successo.\n", MODNAME);
    printk(KERN_INFO "=======================================\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
