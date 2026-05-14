MODNAME="syscall_monitor"

if [ "$EUID" -ne 0 ]; then
  echo "[-] Questo script deve essere eseguito come root"
  exit
fi

echo "[*] Preparazione test di Hot-Unloading..."

# Carica il modulo se non è già caricato
lsmod | grep -q "$MODNAME"
if [ $? -ne 0 ]; then
    insmod ../$MODNAME.ko || { echo "[-] Fallito caricamento modulo"; exit 1; }
fi

cat << 'EOF' > spammer.c
#include <unistd.h>
#include <sys/syscall.h>
int main() {
    while(1) { syscall(39); } // Spamma getpid()
}
EOF
gcc spammer.c -o spammer

# tool per configurare il modulo da bash (non apro il driver, altrimenti non potrei rimuovere il modulo)
cat << 'EOF' > configurator.c
#include <fcntl.h>
#include <sys/ioctl.h>
#include "../include/monitor_ioctl.h"
int main() {
    int fd = open("/dev/syscall_monitor", O_RDWR);
    int limit = 1, sys_num = 39, uid = 0;
    ioctl(fd, MONITOR_IOC_SET_MAX, &limit); // 1 syscall al secondo!
    ioctl(fd, MONITOR_IOC_ADD_SYS, &sys_num);
    ioctl(fd, MONITOR_IOC_ADD_UID, &uid);
    return 0;
}
EOF
gcc configurator.c -o configurator
./configurator

echo "[*] Avvio 5 processi spammer in background (cadranno subito nella waitqueue)..."
for i in {1..5}; do
    ./spammer &
    SPAMMER_PIDS+=($!)
done

sleep 2

echo "[*] TENTATIVO DI RIMOZIONE MODULO (rmmod) MENTRE I THREAD SONO BLOCCATI..."
rmmod $MODNAME

if [ $? -eq 0 ]; then
    echo "[+] Modulo rimosso con successo!"
else
    echo "[-] ERRORE: Il modulo non si è smontato."
fi

echo "[*] Verifica stato processi (dovrebbero continuare senza errori visto che i token sono stati rilasciati)..."
sleep 1
ps -p ${SPAMMER_PIDS[0]} > /dev/null
if [ $? -eq 0 ]; then
    echo "[+] I processi utente non sono andati in Segmentation Fault o Kernel Panic. Test superato."
fi

for pid in "${SPAMMER_PIDS[@]}"; do
    kill -9 $pid 2>/dev/null
done
rm spammer configurator spammer.c configurator.c

echo "[+] TEST HOT-UNLOADING COMPLETATO."
