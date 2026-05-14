#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "../include/monitor_ioctl.h"

#define DEVICE_PATH "/dev/syscall_monitor"
#define MAX_MSG 256

void print_usage(const char *prog_name) {
    printf("Uso: %s [comando] [valore]\n", prog_name);
    printf("  --set-max <num>   Imposta limite MAX\n");
    printf("  --add-prog <nome> Aggiunge programma\n");
    printf("  --del-prog <nome> Rimuove programma\n");
    printf("  --add-uid <num>   Aggiunge User ID\n");
    printf("  --del-uid <num>   Rimuove User ID\n");
    printf("  --add-sys <num>   Aggiunge Syscall (es. 0 per read)\n");
    printf("  --del-sys <num>   Rimuove Syscall\n");
    printf("  --on              Accende il monitor\n");
    printf("  --off             Spegne il monitor\n");
}

// Funzione helper per gestire le stampe di successo o di errore
void print_result(int ret, const char *success_msg) {
    if (ret < 0) {
        if (errno == EPERM) {
            fprintf(stderr, "Errore: Permesso negato. Devi usare 'sudo' per modificare questa configurazione.\n");
        } else {
            fprintf(stderr, "Errore di sistema: %s\n", strerror(errno));
        }
    } else {
        printf("%s\n", success_msg);
    }
}

int main(int argc, char *argv[]) {
    int fd;
    int ret;
    char msg[MAX_MSG]; // Buffer per formattare i messaggi di successo
    
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("Errore apertura device /dev/syscall_monitor");
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "--on") == 0) {
        int val = 1;
        ret = ioctl(fd, MONITOR_IOC_SET_STATE, &val);
        print_result(ret, "Monitor acceso (ON).");
    } 
    else if (strcmp(argv[1], "--off") == 0) {
        int val = 0;
        ret = ioctl(fd, MONITOR_IOC_SET_STATE, &val);
        print_result(ret, "Monitor spento (OFF).");
    }
    // Comandi che richiedono un valore (argc == 3)
    else if (argc == 3) {
        if (strcmp(argv[1], "--set-max") == 0) {
            int val = atoi(argv[2]);
            ret = ioctl(fd, MONITOR_IOC_SET_MAX, &val);
            snprintf(msg, sizeof(msg), "Limite MAX globale settato a %d.", val);
            print_result(ret, msg);
            
        } else if (strcmp(argv[1], "--add-prog") == 0) {
            struct monitor_prog_config conf;
            strncpy(conf.comm, argv[2], MAX_COMM_LEN);
            ret = ioctl(fd, MONITOR_IOC_ADD_PROG, &conf);
            snprintf(msg, sizeof(msg), "Programma '%s' aggiunto.", conf.comm);
            print_result(ret, msg);
            
        } else if (strcmp(argv[1], "--del-prog") == 0) {
            struct monitor_prog_config conf;
            strncpy(conf.comm, argv[2], MAX_COMM_LEN);
            ret = ioctl(fd, MONITOR_IOC_DEL_PROG, &conf); // Corretto: era ADD_PROG
            snprintf(msg, sizeof(msg), "Programma '%s' rimosso.", conf.comm);
            print_result(ret, msg);
            
        } else if (strcmp(argv[1], "--add-uid") == 0) {
            int val = atoi(argv[2]);
            ret = ioctl(fd, MONITOR_IOC_ADD_UID, &val);
            snprintf(msg, sizeof(msg), "UID %d aggiunto.", val);
            print_result(ret, msg);
            
        } else if (strcmp(argv[1], "--del-uid") == 0) { // Aggiunto comando mancante!
            int val = atoi(argv[2]);
            ret = ioctl(fd, MONITOR_IOC_DEL_UID, &val);
            snprintf(msg, sizeof(msg), "UID %d rimosso.", val);
            print_result(ret, msg);
            
        } else if (strcmp(argv[1], "--add-sys") == 0) {
            int val = atoi(argv[2]);
            ret = ioctl(fd, MONITOR_IOC_ADD_SYS, &val);
            snprintf(msg, sizeof(msg), "Syscall %d aggiunta.", val);
            print_result(ret, msg);
            
        } else if (strcmp(argv[1], "--del-sys") == 0) {
            int val = atoi(argv[2]);
            ret = ioctl(fd, MONITOR_IOC_DEL_SYS, &val);
            snprintf(msg, sizeof(msg), "Syscall %d rimossa.", val);
            print_result(ret, msg);
            
        } else {
            print_usage(argv[0]);
        }
    } else {
        print_usage(argv[0]);
    }

    close(fd);
    return EXIT_SUCCESS;
}
