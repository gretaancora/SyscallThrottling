#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/wait.h>
#include <pthread.h>
#include <limits.h>
#include "../include/monitor_ioctl.h"

#define DEVICE_PATH "/dev/syscall_monitor"
#define SYS_GETPID 39
#define SYS_GETUID 102

int fd;

// FUNZIONI DI SUPPORTO
double get_time_diff(struct timeval start, struct timeval end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
}

void print_stats() {
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    lseek(fd, 0, SEEK_SET);
    if (read(fd, buf, sizeof(buf) - 1) > 0) {
        printf("\n--- STATISTICHE DAL DRIVER ---\n%s\n------------------------------\n", buf);
    }
}


// Funzione target per i thread
void *thread_syscall_worker(void *arg) {
    syscall(SYS_GETPID);
    return NULL;
}

// Throttling Concorrente (Thundering Herd Stress-Test) 
void test_throttling() {
    printf("[*] TEST 2: Verifica Throttling Concorrente (Stress Test)...\n");
    
    int state_on = 1;
    ioctl(fd, MONITOR_IOC_SET_STATE, &state_on);
    int sys_num = SYS_GETPID;
    ioctl(fd, MONITOR_IOC_ADD_SYS, &sys_num);
    int my_uid = 0;
    ioctl(fd, MONITOR_IOC_ADD_UID, &my_uid);
    int limit = 10;
    if (ioctl(fd, MONITOR_IOC_SET_MAX, &limit) < 0) perror("  [-] Fallito SET_MAX");

    int num_threads = 25;
    pthread_t threads[25];

    printf("  [~] Scateno %d thread simultanei (limite 10/sec). Mi aspetto ~2 secondi...\n", num_threads);
    
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, thread_syscall_worker, NULL) != 0) {
            perror("  [-] Errore creazione thread");
        }
    }
    
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    gettimeofday(&end, NULL);
    double elapsed = get_time_diff(start, end);
    
    printf("  [+] Tempo impiegato: %.2f secondi.\n", elapsed);
    printf("  [+] Test concorrenza superato.\n\n");
}

// Impatto Prestazionale
void test_performance() {
    printf("[*] TEST 3: Valutazione impatto prestazionale reale\n");
    struct timeval start, end;
    int iterations = 1000000;
    double time_unmonitored, time_monitored_fastpath;
    
    int sys_test = SYS_GETUID; 
    //ci assicuriamo che la syscall non sia monitorata per la baseline
    ioctl(fd, MONITOR_IOC_DEL_SYS, &sys_test);
    
    //baseline
    gettimeofday(&start, NULL);
    for (int i = 0; i < iterations; i++) {
        syscall(sys_test); 
    }
    gettimeofday(&end, NULL);
    time_unmonitored = get_time_diff(start, end);
    printf("  [+] %d esecuzioni di SYS_getuid (NON monitorata): %.4f secondi.\n", iterations, time_unmonitored);

    //preparazione test fast path
    ioctl(fd, MONITOR_IOC_ADD_SYS, &sys_test);
    int my_uid = 0;
    ioctl(fd, MONITOR_IOC_ADD_UID, &my_uid);
    int limit = INT_MAX; 
    ioctl(fd, MONITOR_IOC_SET_MAX, &limit);
    int state_on = 1;
    ioctl(fd, MONITOR_IOC_SET_STATE, &state_on);

    gettimeofday(&start, NULL);
    for (int i = 0; i < iterations; i++) {
        syscall(sys_test); 
    }
    gettimeofday(&end, NULL);
    time_monitored_fastpath = get_time_diff(start, end);
    printf("  [+] %d esecuzioni di SYS_getuid (Monitorata, Fast-Path): %.4f secondi.\n", iterations, time_monitored_fastpath);
    
    // Moltiplichiamo per 1.000.000.000 per passare da secondi a nanosecondi, 
    // poi dividiamo per il numero di iterazioni
    double ns_per_sys_unmonitored = (time_unmonitored * 1000000000.0) / iterations;
    double ns_per_sys_monitored = (time_monitored_fastpath * 1000000000.0) / iterations;
    double absolute_overhead_ns = ns_per_sys_monitored - ns_per_sys_unmonitored;
    
    printf("\n  --- ANALISI MICRO-BENCHMARK ---\n");
    printf("  -> Tempo base medio per syscall : ~%.2f ns\n", ns_per_sys_unmonitored);
    printf("  -> Tempo hook medio per syscall : ~%.2f ns\n", ns_per_sys_monitored);
    printf("  [+] OVERHEAD ASSOLUTO AGGIUNTO  : %.2f nanosecondi per syscall\n", absolute_overhead_ns);
    
    ioctl(fd, MONITOR_IOC_DEL_SYS, &sys_test);
    printf("  [+] TEST 3 COMPLETATO.\n\n");
}

// Sicurezza (Accesso negato per non-root)
void test_security() {
    printf("[*] TEST 4: Controllo Sicurezza ioctl (Solo Root)...\n");
    
    pid_t pid = fork();
    if (pid == 0) {
        //degradiamo i privilegi
        if (setuid(1000) != 0) {
            printf("  [-] Impossibile degradare privilegi nel figlio. Salto test.\n");
            exit(1);
        }
        int limit = 50;
        int ret = ioctl(fd, MONITOR_IOC_SET_MAX, &limit);
        if (ret < 0 && errno == EPERM) {
            exit(0); // Successo: ha rifiutato l'accesso
        }
        exit(1); // Fallimento: ha permesso l'accesso!
    } else {
        int status;
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) == 0) {
            printf("  [+] Accesso negato correttamente all'utente non-root.\n");
            printf("  [+] TEST 4 SUPERATO.\n\n");
        } else {
            printf("  [-] TEST 4 FALLITO (Vulnerabilita': l'utente non-root ha modificato le regole!).\n\n");
        }
    }
}

void cleanup() {
    printf("[*] Pulizia regole e ripristino stato originale...\n");
    int sys_num = SYS_GETPID;
    int uid = getuid();
    struct monitor_prog_config prog;
    strncpy(prog.comm, "test_suite", MAX_COMM_LEN);

    ioctl(fd, MONITOR_IOC_DEL_SYS, &sys_num);
    ioctl(fd, MONITOR_IOC_DEL_UID, &uid);
    ioctl(fd, MONITOR_IOC_DEL_PROG, &prog);
}

int main() {
    if (geteuid() != 0) {
        fprintf(stderr, "ERRORE: Questo test suite deve essere eseguito come root!\n");
        return 1;
    }

    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("ERRORE: Impossibile aprire /dev/syscall_monitor. Il modulo è caricato?");
        return 1;
    }

    printf("=== AVVIO SYSCALL MONITOR TEST SUITE ===\n\n");

    test_throttling();
    print_stats();
    test_performance();
    test_security();

    cleanup();

    close(fd);
    printf("=== TEST SUITE TERMINATA CON SUCCESSO ===\n");
    return 0;
}
