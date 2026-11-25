#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

HOST="${HOST:-127.0.0.1}"
BASE_PORT="${BASE_PORT:-19080}"
WINDOW="${WINDOW:-32}"
TIMEOUT_MS="${TIMEOUT_MS:-250}"
FILE_KIB="${FILE_KIB:-64}"
LOSS_LEVELS=( ${LOSS_LEVELS:-0 10 20 30} )
RUN_TIMEOUT="${RUN_TIMEOUT:-120}"
DEBUG="${DEBUG:-1}"


if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then usage; exit 0; fi

for tool in make python3 timeout sha256sum cmp; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Не найдена команда: $tool" >&2; exit 1; }
done

OUT="$ROOT_DIR/submission/comparison_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT"/{logs,data,received,graphs}

if [[ $# -ge 1 ]]; then
    INPUT="$(realpath "$1")"
    [[ -f "$INPUT" ]] || { echo "Файл не найден: $INPUT" >&2; exit 1; }
    INPUT_LABEL="$(basename "$INPUT")"
else
    INPUT="$OUT/comparison_input.bin"
    INPUT_LABEL="comparison_input.bin"
    echo "Создаю тестовый файл ${FILE_KIB} KiB..."
    dd if=/dev/urandom of="$INPUT" bs=1K count="$FILE_KIB" status=progress
fi

FILE_BYTES=$(stat -c '%s' "$INPUT")
FILE_SHA=$(sha256sum "$INPUT" | awk '{print $1}')

printf 'algorithm,loss_percent,file_bytes,duration_seconds,throughput,retransmissions,sha256_ok,status\n' > "$OUT/summary.csv"

cleanup() {
    if [[ -n "${RECEIVER_PID:-}" ]] && kill -0 "$RECEIVER_PID" 2>/dev/null; then
        kill "$RECEIVER_PID" 2>/dev/null || true
        wait "$RECEIVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

run_one() {
    local algorithm="$1" loss="$2" port="$3"
    local tag="${algorithm,,}_${loss}"
    local received="$OUT/received/${tag}.bin"
    local sender_log="$OUT/logs/${tag}_sender.log"
    local receiver_log="$OUT/logs/${tag}_receiver.log"
    local metrics="$OUT/data/${tag}.csv"
    local sender_rc=0 receiver_rc=0 hash_ok=NO status=FAIL
    local debug_args=()
    [[ "$DEBUG" == "1" ]] && debug_args=(-d)

    rm -f "$received" "$ROOT_DIR/rdtp_metrics.csv"
    echo
    echo "============================================================"
    echo "Запуск: $algorithm | потери DATA: ${loss}% | файл: $INPUT_LABEL"
    echo "Окно: $WINDOW | RTO: ${TIMEOUT_MS} ms | UDP port: $port"
    echo "============================================================"

    ./rdt_receiver "$port" "$received" -alg "$algorithm" >"$receiver_log" 2>&1 &
    RECEIVER_PID=$!
    sleep 0.25
    if ! kill -0 "$RECEIVER_PID" 2>/dev/null; then
        echo "Получатель завершился до начала передачи. См. $receiver_log" >&2
        return 1
    fi

    set +e
    timeout --signal=INT --kill-after=5 "$RUN_TIMEOUT" \
        ./rdt_sender "$HOST" "$port" "$INPUT" -alg "$algorithm" \
        -loss "$(awk -v n="$loss" 'BEGIN { printf "%.2f", n/100 }')" \
        -window "$WINDOW" -timeout "$TIMEOUT_MS" "${debug_args[@]}" \
        2>&1 | tee "$sender_log"
    sender_rc=${PIPESTATUS[0]}
    set -e

    if [[ -f "$ROOT_DIR/rdtp_metrics.csv" ]]; then
        cp "$ROOT_DIR/rdtp_metrics.csv" "$metrics"
    fi

    if [[ $sender_rc -eq 0 ]]; then
        for _ in {1..30}; do
            kill -0 "$RECEIVER_PID" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$RECEIVER_PID" 2>/dev/null; then
            kill "$RECEIVER_PID" 2>/dev/null || true
        fi
        wait "$RECEIVER_PID" || receiver_rc=$?
        RECEIVER_PID=""
        if [[ -f "$received" ]] && cmp -s "$INPUT" "$received"; then
            hash_ok=YES
            status=PASS
        fi
    else
        echo "Отправитель завершился с кодом $sender_rc; опыт не засчитывается." | tee -a "$sender_log"
        kill "$RECEIVER_PID" 2>/dev/null || true
        wait "$RECEIVER_PID" 2>/dev/null || true
        RECEIVER_PID=""
    fi

    if [[ -f "$received" ]]; then
        sha256sum "$INPUT" "$received" > "$OUT/logs/${tag}_sha256.txt"
    fi

    python3 - "$algorithm" "$loss" "$FILE_BYTES" "$sender_log" "$hash_ok" "$status" >> "$OUT/summary.csv" <<'PY'
import re,sys,csv
alg,loss,size,log,hash_ok,status=sys.argv[1:]
s=open(log,encoding='utf-8',errors='replace').read()
duration=''; throughput=''; retrans=''
m=re.search(r'Время:\s*([0-9.]+)\s*сек\.,\s*скорость:\s*([0-9.]+\s*(?:B|KiB|MiB|GiB)/s)',s)
if m: duration,throughput=m.groups()
m=re.search(r'повторных передач:\s*(\d+)',s)
if m: retrans=m.group(1)
print(','.join([alg,loss,size,duration,throughput,retrans,hash_ok,status]))
PY

    echo "Результат $tag: $status; проверка файла: $hash_ok"
    if [[ $status != PASS ]]; then
        echo "Опыт $tag не прошёл. Подробности: $sender_log и $receiver_log" >&2
    fi
}

 echo "Компиляция проекта..."
make -s clean
make -s -j2

port_offset=0
for loss in "${LOSS_LEVELS[@]}"; do
    for algorithm in GBN SR; do
        port=$((BASE_PORT + port_offset))
        run_one "$algorithm" "$loss" "$port"
        port_offset=$((port_offset + 1))
    done
done

python3 - "$OUT" <<'PY'
import csv,sys,os,math
from pathlib import Path
out=Path(sys.argv[1])
rows=list(csv.DictReader(open(out/'summary.csv',newline='',encoding='utf-8')))
md=['# Сравнение Go-Back-N и Selective Repeat','',
    f'Размер исходного файла: `{rows[0]["file_bytes"] if rows else "?"}` байт','',
    '| Потери | Алгоритм | Время, с | Скорость | Повторные передачи | Целостность | Статус |',
    '|---:|:---:|---:|---:|---:|:---:|:---:|']
for r in rows:
    md.append(f"| {r['loss_percent']}% | {r['algorithm']} | {r['duration_seconds'] or '—'} | {r['throughput'] or '—'} | {r['retransmissions'] or '—'} | {r['sha256_ok']} | {r['status']} |")
md += ['', '## Интерпретация', '',
       'Сравнивайте только строки со статусом PASS. При росте потерь GBN может повторно отправлять несколько неподтверждённых пакетов окна, тогда как SR повторяет отдельные пакеты. Фактический результат зависит от случайной последовательности потерь, RTT и параметров окна.',
       '', 'Все численные значения взяты из логов отправителя; целостность проверена командой `cmp`.']
(out/'GBN_SR_comparison.md').write_text('\n'.join(md)+'\n',encoding='utf-8')

try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
except Exception as e:
    print('Matplotlib не установлен — CSV и таблица готовы, графики пропущены:',e)
    raise SystemExit(0)

losses=sorted({int(r['loss_percent']) for r in rows})
for field,title,ylabel,filename,parser in [
    ('throughput','Итоговая пропускная способность GBN и SR','Скорость, MiB/s','compare_throughput.png',lambda x: (float(__import__('re').match(r'([0-9.]+)',x).group(1)) / (1024 if 'KiB/s' in x else 1048576 if 'B/s' in x else 1)) if x else float('nan')),
    ('retransmissions','Количество повторных передач GBN и SR','Повторные передачи, шт.','compare_retransmissions.png',lambda x: float(x) if x else float('nan'))]:
    plt.figure(figsize=(8,5))
    for alg in ('GBN','SR'):
        vals=[]
        for loss in losses:
            r=next((x for x in rows if x['algorithm']==alg and int(x['loss_percent'])==loss),None)
            value=(r or {}).get(field,'')
            vals.append(parser(value) if r and r['status']=='PASS' else float('nan'))
        plt.plot(losses,vals,marker='o',label=alg)
    plt.xlabel('Вероятность потери DATA, %'); plt.ylabel(ylabel); plt.title(title)
    plt.grid(True); plt.legend(); plt.tight_layout(); plt.savefig(out/'graphs'/filename,dpi=160); plt.close()
ъ
for metric,title,ylabel,filename in [
    ('cwnd','Размер скользящего окна при потерях 20%','Окно, пакеты','window_20.png'),
    ('throughput_kbps','Пропускная способность во времени при потерях 20%','кбит/с','throughput_20.png')]:
    plt.figure(figsize=(8,5)); plotted=False
    for alg in ('gbn','sr'):
        p=out/'data'/f'{alg}_20.csv'
        if not p.exists(): continue
        data=list(csv.DictReader(open(p,newline='',encoding='utf-8')))
        if not data: continue
        x=[float(r['elapsed_ms'])/1000 for r in data]
        y=[float(r[metric]) for r in data]
        plt.plot(x,y,label=alg.upper()); plotted=True
    plt.xlabel('Время, с'); plt.ylabel(ylabel); plt.title(title); plt.grid(True)
    if plotted:
        plt.legend(); plt.tight_layout(); plt.savefig(out/'graphs'/filename,dpi=160)
    plt.close()

print('Построение графиков завершено.')
PY

echo
echo "Готово. Результаты: $OUT"
echo "Таблица: $OUT/GBN_SR_comparison.md"
echo "CSV:     $OUT/summary.csv"
echo "Логи:    $OUT/logs/"
echo "Метрики: $OUT/data/"
echo "Графики: $OUT/graphs/"
echo
