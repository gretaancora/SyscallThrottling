#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/percpu.h>
#include <linux/cpumask.h>
#include "../include/monitor_ioctl.h"
#include "monitor.h"

//AGGIORNAMENTO DELLE CONFIGURAZIONI

static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    int val;
    struct monitor_prog_config prog_conf;
    int is_update_cmd;

    //identifichiamo se il comando altera le regole
    is_update_cmd = (cmd == MONITOR_IOC_SET_MAX ||
                     cmd == MONITOR_IOC_ADD_PROG || cmd == MONITOR_IOC_DEL_PROG ||
                     cmd == MONITOR_IOC_ADD_UID || cmd == MONITOR_IOC_DEL_UID ||
                     cmd == MONITOR_IOC_ADD_SYS || cmd == MONITOR_IOC_DEL_SYS);

    //controllo di sicurezza sui permessi
    if (is_update_cmd && current_euid().val != 0) {
        printk(KERN_WARNING "%s: Accesso negato. Solo root (EUID 0) puo' aggiornare le configurazioni.\n", MODNAME);
        return -EPERM;
    }

    switch (cmd) {
        case MONITOR_IOC_SET_MAX:
            if (copy_from_user(&val, (int __user *)arg, sizeof(int))) return -EFAULT;
            if (val <= 0) return -EINVAL;
            WRITE_ONCE(max_limit, val);
            atomic_set(&available_tokens, val);
            wake_up_nr(&monitor_wq, val);
            
            printk(KERN_INFO "%s: Limite MAX globale settato a %d\n", MODNAME, val);
            break;

        case MONITOR_IOC_ADD_PROG:
            if (copy_from_user(&prog_conf, (void __user *)arg, sizeof(prog_conf))) return -EFAULT;
            printk(KERN_INFO "%s: Prog '%s' aggiunto.\n", MODNAME, prog_conf.comm);
            return add_prog_rcu(prog_conf.comm);

        case MONITOR_IOC_DEL_PROG:
            if (copy_from_user(&prog_conf, (void __user *)arg, sizeof(prog_conf))) return -EFAULT;
            printk(KERN_INFO "%s: Prog '%s' rimosso.\n", MODNAME, prog_conf.comm);
            return del_prog_rcu(prog_conf.comm);

        case MONITOR_IOC_ADD_UID:
            if (copy_from_user(&val, (int __user *)arg, sizeof(int))) return -EFAULT;
            printk(KERN_INFO "%s: UID '%d' aggiunto.\n", MODNAME, val);
            return add_uid_rcu(val);

        case MONITOR_IOC_DEL_UID:
            if (copy_from_user(&val, (int __user *)arg, sizeof(int))) return -EFAULT;
            printk(KERN_INFO "%s: UID '%d' rimosso.\n", MODNAME, val);
            return del_uid_rcu(val);

        case MONITOR_IOC_ADD_SYS:
            if (copy_from_user(&val, (int __user *)arg, sizeof(int))) return -EFAULT;
            printk(KERN_INFO "%s: Syscall '%d' aggiunta.\n", MODNAME, val);
            return hook_sys(val);

        case MONITOR_IOC_DEL_SYS:
            if (copy_from_user(&val, (int __user *)arg, sizeof(int))) return -EFAULT;
            printk(KERN_INFO "%s: Syscall '%d' rimossa.\n", MODNAME, val);
            return unhook_sys(val);

        case MONITOR_IOC_SET_STATE:
            if (copy_from_user(&val, (int __user *)arg, sizeof(int))) return -EFAULT;
            WRITE_ONCE(monitor_is_on, (val != 0));
            printk(KERN_INFO "%s: Monitor impostato su %s\n", MODNAME, val ? "ON" : "OFF");
            if (val == 0) release_all_blocked_tasks();
            break;

        default:
            return -EINVAL;
    }
    return 0;
}


//READ DELLE STATISTICHE

static ssize_t monitor_read(struct file *file, char __user *user_buf, size_t size, loff_t *ppos) {
    char *buf;
    int len = 0;
    int cpu;
    ssize_t ret;

    //variabili per l'aggregazione Map-Reduce
    u64 global_max_delay = 0;
    int global_max_uid = -1;
    char global_max_comm[MAX_COMM_LEN] = "Nessuno";
    u64 global_total_blocked = 0;
    
    //tempo trascorso in secondi per la media
    u64 uptime_sec = (ktime_get_ns() - monitor_start_time_ns) / 1000000000ULL;
    u64 average_int = 0;
    u64 average_frac = 0; // Per i due "falsi" decimali

    if (*ppos > 0) return 0;
    buf = kmalloc(4096, GFP_KERNEL);
    if (!buf) return -ENOMEM;

    len += scnprintf(buf + len, 4096 - len, "\n========================================\n");
    len += scnprintf(buf + len, 4096 - len, "      SYSCALL MONITOR - REPORT GLOBALE  \n");
    len += scnprintf(buf + len, 4096 - len, "========================================\n");
    len += scnprintf(buf + len, 4096 - len, "Stato Globale : %s\n", READ_ONCE(monitor_is_on) ? "ON" : "OFF");
    len += scnprintf(buf + len, 4096 - len, "Limite MAX    : %d syscall/sec\n\n", max_limit);

    len += dump_config_state(buf + len, 4096 - len);
    len += scnprintf(buf + len, 4096 - len, "\n");

    
    //fase di reduce
    for_each_possible_cpu(cpu) {
        struct monitor_telemetry *stats = &per_cpu(monitor_stats, cpu);
        
        //sommiamo il totale dei thread bloccati su tutte le CPU
        global_total_blocked += stats->total_blocked;

        //troviamo il picco assoluto tra tutte le CPU
        if (stats->peak_delay_ns > global_max_delay) {
            global_max_delay = stats->peak_delay_ns;
            global_max_uid = stats->peak_uid;
            strncpy(global_max_comm, stats->peak_comm, 16);
        }
    }

    //calcolo della media matematica con "falsi decimali"
    if (uptime_sec > 0) {
        average_int = global_total_blocked / uptime_sec;
        average_frac = ((global_total_blocked % uptime_sec) * 100) / uptime_sec;
    }

    len += scnprintf(buf + len, 4096 - len, "--- STATISTICHE RALLENTAMENTO ---\n");
    len += scnprintf(buf + len, 4096 - len, "[1] Ritardo Massimo (Peak Delay):\n");
    len += scnprintf(buf + len, 4096 - len, "    -> %llu nanosecondi\n", global_max_delay);
    len += scnprintf(buf + len, 4096 - len, "    -> Programma : %s\n", global_max_comm);
    len += scnprintf(buf + len, 4096 - len, "    -> User ID   : %d\n\n", global_max_uid);
    len += scnprintf(buf + len, 4096 - len, "[2] Volumi di Blocco:\n");
    len += scnprintf(buf + len, 4096 - len, "    -> Totale Thread bloccati : %llu\n", global_total_blocked);
    len += scnprintf(buf + len, 4096 - len, "    -> Media Thread bloccati  : %llu.%02llu al secondo\n", average_int, average_frac);
    len += scnprintf(buf + len, 4096 - len, "    -> Picco in coda (Peak)   : %d thread simultanei\n", atomic_read(&global_peak_blocked));
    
    len += scnprintf(buf + len, 4096 - len, "========================================\n\n");

    if (len > size) len = size;
    if (copy_to_user(user_buf, buf, len)) ret = -EFAULT;
    else { ret = len; *ppos += len; }

    kfree(buf);
    return ret;
}

// FILE OPERATIONS ESPORTATE DAL DRIVER

static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .read           = monitor_read,     
    .unlocked_ioctl = monitor_ioctl,
};

//DEFINIZIONE DEL NUOVO CHARACTER DEVICE

static struct miscdevice monitor_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "syscall_monitor",
    .fops  = &monitor_fops,
    .mode  = 0666, // (rw-rw-rw-) tutti possono leggere e mandare comandi base
};

//INIT & CLEANUP DEL DRIVER

int init_driver(void) {
    int ret = misc_register(&monitor_miscdev);
    if (ret) {
        printk(KERN_ERR "%s: Fallita registrazione misc device\n", MODNAME);
        return ret;
    }
    printk(KERN_INFO "%s: Interfaccia ioctl creata su /dev/syscall_monitor\n", MODNAME);
    return 0;
}

void cleanup_driver(void) {
    misc_deregister(&monitor_miscdev);
    printk(KERN_INFO "%s: Interfaccia ioctl rimossa\n", MODNAME);
}
