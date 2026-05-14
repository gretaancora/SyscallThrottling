# Numero di iterazioni da eseguire
ITERATIONS=50

# Carica il modulo se non è già caricato
lsmod | grep -q "$MODNAME"
if [ $? -ne 0 ]; then
    insmod ../$MODNAME.ko || { echo "[-] Fallito caricamento modulo"; exit 1; }
fi

echo "========================================="
echo " AVVIO BENCHMARK AUTOMATIZZATO ($ITERATIONS RUN)"
echo "========================================="
echo "Assicurati di non usare pesantemente il PC durante il test..."
sleep 2

# Variabile di appoggio per immagazzinare tutti i risultati
ALL_VALUES=""

for i in $(seq 1 $ITERATIONS); do
    echo -n " -> Esecuzione $i/$ITERATIONS in corso... "
    
    # Eseguiamo la test_suite e salviamo l'output in un file temporaneo
    sudo ./test_suite > /tmp/sysmon_test.log
    
    # Estraiamo l'overhead
    VAL=$(grep "OVERHEAD ASSOLUTO AGGIUNTO" /tmp/sysmon_test.log | awk '{print $6}')
    
    # Stampa a video il risultato del singolo run
    echo "Risultato: $VAL ns"
    
    # Accodiamo il valore alla nostra variabile (separato da spazio)
    ALL_VALUES="$ALL_VALUES $VAL"
done

echo "========================================="
echo "[*] Benchmark completato!"
echo "-----------------------------------------"

# Passiamo tutti i valori accumulati direttamente ad awk per la statistica
echo "$ALL_VALUES" | awk '{
    for(i=1; i<=NF; i++) {
        val = $i;
        sum += val;
        sumsq += (val * val);
        count++;
    }
} 
END {
    if (count > 0) {
        media = sum / count;
        varianza = (sumsq / count) - (media * media);
        dev_std = sqrt(varianza);
        printf "[+] OVERHEAD MEDIO        : %.2f ns\n", media;
        printf "[+] DEVIAZIONE STANDARD   : ±%.2f ns\n", dev_std;
    }
}'
echo "========================================="
