#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/hashtable.h> 
#include <linux/jhash.h>     
#include "../include/monitor_ioctl.h"
#include "monitor.h"

//hashtable con 256 buckets
#define MONITOR_HASH_BITS 8

//necessario per safe hot unloading del modulo 
bool monitor_is_on = true;
//velocizza should_monitor_target() in caso di filtro unicamente su UID
atomic_t active_prog_rules = ATOMIC_INIT(0);

//nodi hash table con meccanismo RCU
struct prog_node {
    char comm[MAX_COMM_LEN];
    struct hlist_node node; 
    struct rcu_head rcu;
};

struct uid_node {
    int uid;
    struct hlist_node node;
    struct rcu_head rcu;
};

static DEFINE_HASHTABLE(prog_hash, MONITOR_HASH_BITS);
static DEFINE_HASHTABLE(uid_hash, MONITOR_HASH_BITS);
//mutex necessario per poter scrivere sulle struttue di configurazione
static DEFINE_MUTEX(config_mutex);

// slab allocator caches utilizzate per l'allocazione dei nodi delle hashtable RCU
static struct kmem_cache *prog_cache;
static struct kmem_cache *uid_cache;

//funzione di hash applicata sul nome dei programmi da registrare
//Jenkins hash support
static inline u32 hash_string(const char *str) {
    return jhash(str, strlen(str), 0);
}


//FILTRAGGIO

bool should_monitor_target(void) {
    struct prog_node *p_node;
    struct uid_node *u_node;
    bool target_match = false;
    int current_uid = current_euid().val;
    u32 comm_hash;
    
    rcu_read_lock(); //disabilita la preemption locale
    
    //controlliamo prima gli UID
    //hash_for_each_possible_rcu - iterate over all possible objects hashing to the same bucket in an rcu enabled hashtable
    hash_for_each_possible_rcu(uid_hash, u_node, node, current_uid) {
        if (u_node->uid == current_uid) {
            target_match = true;
            break;
        }
    }
    
    //lazy hashing: calcoliamo l'hash solo se l'UID non ha matchato e se c'è almeno un programma registrato nel sistema
    if (!target_match && atomic_read(&active_prog_rules) > 0) {
        comm_hash = hash_string(current->comm);
        hash_for_each_possible_rcu(prog_hash, p_node, node, comm_hash) {
            if (strncmp(current->comm, p_node->comm, TASK_COMM_LEN) == 0) {
                target_match = true;
                break;
            }
        }
    }
    
    rcu_read_unlock(); //riabilita la preemption locale
    return target_match; 
}


//FUNZIONI DI CALLBACK RCU PER LA LIBERAZIONE DELLA MEMORIA DELLE SLAB CACHE

static void free_prog_rcu(struct rcu_head *head) {
    struct prog_node *node = container_of(head, struct prog_node, rcu);
    kmem_cache_free(prog_cache, node);
}

static void free_uid_rcu(struct rcu_head *head) {
    struct uid_node *node = container_of(head, struct uid_node, rcu);
    kmem_cache_free(uid_cache, node);
}


//REGISTRAZIONE E DEREGISTRAZIONE

int add_prog_rcu(const char *comm) {
    struct prog_node *p, *new_node;
    u32 key = hash_string(comm);
    
    //allocazione del nuovo nodo, non necessario GFP_ATOMIC in quanto l'aggiunta delle regole tramite IOCTL 
    //avviene in contesto di processo, non abbiamo spinlock e non ci troviamo in un blocco di lettura
    new_node = kmem_cache_alloc(prog_cache, GFP_KERNEL);
    if (!new_node) return -ENOMEM;
    
    strncpy(new_node->comm, comm, MAX_COMM_LEN);
    new_node->comm[MAX_COMM_LEN - 1] = '\0';

    mutex_lock(&config_mutex);

    //controlliamo la presenza di doppioni
    hash_for_each_possible_rcu(prog_hash, p, node, key) {
        if (strncmp(p->comm, comm, MAX_COMM_LEN) == 0) {
            mutex_unlock(&config_mutex);
            //deallochiamo il nodo preallocato in caso di regola già presente
            kmem_cache_free(prog_cache, new_node);
            return 0; 
        }
    }

    hash_add_rcu(prog_hash, &new_node->node, key);
    
    //incrementiamo il contatore globale solo se abbiamo aggiunto una regola
    atomic_inc(&active_prog_rules);
    
    mutex_unlock(&config_mutex);
    
    return 0;
}

int del_prog_rcu(const char *comm) {
    struct prog_node *p;
    struct hlist_node *tmp;
    u32 key = hash_string(comm);
    int ret = -ENOENT;

    mutex_lock(&config_mutex);
    hash_for_each_possible_safe(prog_hash, p, tmp, node, key) {
        if (strncmp(p->comm, comm, MAX_COMM_LEN) == 0) {
            hash_del_rcu(&p->node);
            //chiamata di callback della free del maccanismo di RCU
            call_rcu(&p->rcu, free_prog_rcu); 
            atomic_dec(&active_prog_rules);
            ret = 0;
            break;
        }
    }
    mutex_unlock(&config_mutex);
    return ret;
}

int add_uid_rcu(int uid) {
    struct uid_node *u, *new_node;
    
    new_node = kmem_cache_alloc(uid_cache, GFP_KERNEL);
    if (!new_node) return -ENOMEM;
    new_node->uid = uid;

    mutex_lock(&config_mutex);

    
    //controlliamo la presenza di doppioni
    hash_for_each_possible_rcu(uid_hash, u, node, uid) {
        if (u->uid == uid) {
            mutex_unlock(&config_mutex);
            //deallochiamo il nodo preallocato in caso di regola già presente
            kmem_cache_free(uid_cache, new_node);
            return 0; 
        }
    }
    
    hash_add_rcu(uid_hash, &new_node->node, uid);
    mutex_unlock(&config_mutex);
    
    return 0;
}

int del_uid_rcu(int uid) {
    struct uid_node *u;
    struct hlist_node *tmp;
    int ret = -ENOENT;

    mutex_lock(&config_mutex);
    hash_for_each_possible_safe(uid_hash, u, tmp, node, uid) {
        if (u->uid == uid) {
            hash_del_rcu(&u->node);
            call_rcu(&u->rcu, free_uid_rcu);
            ret = 0;
            break;
        }
    }
    mutex_unlock(&config_mutex);
    return ret;
}

//DUMP DELLE CONFIGURAZIONI

int dump_config_state(char *buf, size_t size) {
    int len = 0;
    int bkt; 
    struct prog_node *p;
    struct uid_node *u;
    
    rcu_read_lock();
    len += scnprintf(buf + len, size - len, "--- REGOLE ATTIVE ---\nProgrammi : ");
    hash_for_each_rcu(prog_hash, bkt, p, node) {
        len += scnprintf(buf + len, size - len, "[%s] ", p->comm);
    }
    
    len += scnprintf(buf + len, size - len, "\nUIDs      : ");
    hash_for_each_rcu(uid_hash, bkt, u, node) {
        len += scnprintf(buf + len, size - len, "[%d] ", u->uid);
    }
    rcu_read_unlock();

    len += scnprintf(buf + len, size - len, "\nSyscalls  : ");
    len += get_hooked_syscalls_dump(buf + len, size - len);
    len += scnprintf(buf + len, size - len, "\n");
    return len;
}

//INIT & CLEANUP

int init_config(void) {
    //creazione delle slab caches
    prog_cache = kmem_cache_create("sysmon_prog", sizeof(struct prog_node), 0, SLAB_HWCACHE_ALIGN, NULL);
    uid_cache = kmem_cache_create("sysmon_uid", sizeof(struct uid_node), 0, SLAB_HWCACHE_ALIGN, NULL);
    
    if (!prog_cache || !uid_cache) return -ENOMEM;
    
    //inizializzazione delle hashtable
    hash_init(prog_hash);
    hash_init(uid_hash);
    
    printk(KERN_INFO "%s: Configurazione Slab Caches e Hash Table inizializzata.\\n", MODNAME);
    return 0;
}

void cleanup_config(void) {
    int bkt;
    struct prog_node *p_node;
    struct uid_node *u_node;
    struct hlist_node *tmp;

    mutex_lock(&config_mutex);
    hash_for_each_safe(prog_hash, bkt, tmp, p_node, node) {
        hash_del_rcu(&p_node->node);
        call_rcu(&p_node->rcu, free_prog_rcu);
    }
    hash_for_each_safe(uid_hash, bkt, tmp, u_node, node) {
        hash_del_rcu(&u_node->node);
        call_rcu(&u_node->rcu, free_uid_rcu);
    }
    mutex_unlock(&config_mutex);
    
    //aspettiamo che tutte le call_rcu abbiano finito di eseguire le free
    rcu_barrier(); 
    
    //deallocazione sicura delle slab cache
    kmem_cache_destroy(prog_cache);
    kmem_cache_destroy(uid_cache);
}
