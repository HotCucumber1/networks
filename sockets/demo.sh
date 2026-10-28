#!/usr/bin/env bash
# Прогоняет все сценарии демонстрации и сохраняет вывод в output/*.txt.
# Использование: ./demo.sh
set -u
cd "$(dirname "$0")"
make -s all asan tsan

OUT=output
PORT=5555
VALGRIND=${VALGRIND:-valgrind}
mkdir -p "$OUT"

wait_port() {
	for _ in $(seq 1 300); do
		grep -q 'Ctrl+C' "$OUT/.srv" 2>/dev/null && return 0
		sleep 0.1
	done
	echo "сервер не поднялся" >&2
	return 1
}

section() { printf '\n===== %s =====\n' "$*"; }

# --- 1. Базовый цикл
{
	rm -f "$OUT/.srv"; build/server > "$OUT/.srv" 2>&1 &
	srv=$!
	wait_port
	section "Клиент 1 (число 42)";   echo 42  | build/client
	section "Клиент 2 (число 100)";  echo 100 | build/client -n "Client of Ivan Petrov"
	section "Клиент 3 (неверный ввод, затем 1)"; printf 'abc\n1\n' | build/client
	section "Клиент 4 (число 0 — команда остановки сервера)"; echo 0 | build/client
	wait $srv; echo "код завершения сервера: $?"
	section "Вывод сервера"; cat "$OUT/.srv"
} > "$OUT/01_basic.txt" 2>&1

# --- 2. Таймауты: молчащий клиент и slowloris; обычный клиент обслуживается параллельно
{
	rm -f "$OUT/.srv"; build/server > "$OUT/.srv" 2>&1 &
	srv=$!
	wait_port
	section "Молчащий клиент (подключился и ничего не шлёт) и slowloris (байт в секунду, без перевода строки)"
	(
		start=$(date +%s%N); exec 3<>/dev/tcp/127.0.0.1/$PORT
		cat <&3
		echo "молчащий клиент: сервер закрыл соединение через $(( ($(date +%s%N) - start) / 1000000 )) мс"
	) & silent=$!
	(
		trap '' PIPE
		start=$(date +%s%N); exec 3<>/dev/tcp/127.0.0.1/$PORT
		for c in $(echo "Client of Slow Loris" | grep -o .); do printf '%s' "$c" >&3 2>/dev/null || break; sleep 1; done
		echo "slowloris: отправка прервана (соединение разорвано сервером) через $(( ($(date +%s%N) - start) / 1000000 )) мс"
	) & slow=$!
	sleep 1
	section "Обычный клиент в это же время"; echo 77 | build/client
	wait $silent $slow
	kill -INT $srv; wait $srv; echo "код завершения сервера: $?"
	section "Вывод сервера"; cat "$OUT/.srv"
} > "$OUT/02_timeouts.txt" 2>&1

# --- 3. Graceful shutdown: SIGINT во время незавершённого запроса
{
	rm -f "$OUT/.srv"; build/server > "$OUT/.srv" 2>&1 &
	srv=$!
	wait_port
	section "Клиент начинает запрос, через 1 с серверу приходит SIGINT, ещё через 2 с клиент дописывает число"
	( printf 'Client of Late Student|'; sleep 3; printf '5\n'; sleep 1 ) | nc 127.0.0.1 $PORT & late=$!
	sleep 1; kill -INT $srv
	sleep 0.3
	section "Новое подключение после SIGINT (должно быть отвергнуто)"
	echo 10 | build/client
	wait $late
	echo; wait $srv; echo "код завершения сервера: $?"
	section "Вывод сервера"; cat "$OUT/.srv"
} > "$OUT/03_graceful_shutdown.txt" 2>&1

# --- 4. Нагрузка: 1000 одновременных соединений + 5 молчащих, статистика в реальном времени
{
	rm -f "$OUT/.srv"; build/server -q -i 1 > "$OUT/.srv" 2>&1 &
	srv=$!
	wait_port
	section "loadtest: 1000 одновременных + 5 молчащих"
	build/loadtest -n 1000 -c 1000 -S 5
	section "loadtest: 20000 запросов, окно 500"
	build/loadtest -n 20000 -c 500
	section "SIGINT"
	kill -INT $srv; wait $srv; echo "код завершения сервера: $?"
	section "Вывод сервера"; cat "$OUT/.srv"
} > "$OUT/04_load.txt" 2>&1

# --- 5. Утечки памяти и дескрипторов
{
	for variant in valgrind asan tsan; do
		case $variant in
			valgrind) cmd=("$VALGRIND" --trace-children=yes --leak-check=full --show-leak-kinds=all --track-fds=yes --error-exitcode=1 build/server -q -i 0) ;;
			asan)     cmd=(build-asan/server -q -i 0) ;;
			tsan)     cmd=(setarch "$(uname -m)" -R build-tsan/server -q -i 0) ;;
		esac
		section "$variant: ${cmd[*]/#$VALGRIND/valgrind}"
		rm -f "$OUT/.srv"; "${cmd[@]}" > "$OUT/.srv" 2>&1 &
		srv=$!
		wait_port
		build/loadtest -n 2000 -c 1000 -S 3
		kill -INT $srv; wait $srv; echo "код завершения сервера: $?"
		section "$variant: вывод сервера"; cat "$OUT/.srv"
	done
} > "$OUT/05_memcheck.txt" 2>&1

# --- 6. Живучесть: рабочий процесс убит kill -9 под нагрузкой, мастер его перезапускает
{
	rm -f "$OUT/.srv"; build/server -q -i 1 > "$OUT/.srv" 2>&1 &
	srv=$!
	wait_port
	sleep 1.5
	section "Рабочие процессы до сбоя"; ps -o pid,ppid,nlwp,cmd --ppid $srv
	section "Нагрузка 30000 запросов; во время неё kill -9 одного рабочего процесса"
	build/loadtest -n 30000 -c 200 & load=$!
	sleep 0.3
	victim=$(pgrep -P $srv | head -1)
	echo "kill -9 $victim"; kill -9 "$victim"
	wait $load
	echo "(обрываться могут только запросы, которые в момент kill -9 были в работе у убитого процесса)"
	sleep 1.2
	section "Рабочие процессы после перезапуска"; ps -o pid,ppid,nlwp,cmd --ppid $srv
	section "Повторная нагрузка — сервер полностью работоспособен"
	build/loadtest -n 5000 -c 500
	kill -INT $srv; wait $srv; echo "код завершения сервера: $?"
	section "Вывод сервера"; cat "$OUT/.srv"
} > "$OUT/06_worker_restart.txt" 2>&1

rm -f "$OUT"/.srv "$OUT"/.cli*
echo "готово, вывод в $OUT/"
