import argparse
import csv
from pathlib import Path
import sys


def read_csv(path):
    if not path.exists() or path.stat().st_size == 0:
        return []
    with path.open(newline='', encoding='utf-8') as file:
        return list(csv.DictReader(file))


def number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def main():
    parser = argparse.ArgumentParser(description='Построение графиков по уже сохранённым результатам RDTP без повторной передачи файлов.')
    parser.add_argument('result_dir', help='Папка comparison_... с summary.csv, data/ и logs/')
    args = parser.parse_args()

    out = Path(args.result_dir).expanduser().resolve()
    summary_path = out / 'summary.csv'
    if not summary_path.exists():
        print(f'Не найден файл: {summary_path}', file=sys.stderr)
        return 2

    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except ImportError:
        print('Matplotlib не установлен. Установи: sudo apt install python3-matplotlib', file=sys.stderr)
        return 3

    rows = read_csv(summary_path)
    if not rows:
        print('summary.csv пуст. Графики сравнения построить не из чего.', file=sys.stderr)
        return 4

    graph_dir = out / 'graphs'
    graph_dir.mkdir(parents=True, exist_ok=True)
    colors = {'GBN': 'tab:blue', 'SR': 'tab:orange'}

    valid = [r for r in rows if r.get('status') == 'PASS']
    if not valid:
        print('В summary.csv нет успешных экспериментов со статусом PASS.', file=sys.stderr)
        return 5

    metrics = [
        ('throughput', 'Пропускная способность', 'Скорость, единицы из summary.csv', 'compare_throughput.png'),
        ('retransmissions', 'Количество повторных передач', 'Повторные передачи, шт.', 'compare_retransmissions.png'),
        ('duration_seconds', 'Время передачи', 'Время, с', 'compare_duration.png'),
    ]
    for key, title, ylabel, filename in metrics:
        plt.figure(figsize=(8, 5))
        plotted = False
        for algorithm in ('GBN', 'SR'):
            selected = [r for r in valid if r.get('algorithm', '').upper() == algorithm and number(r.get(key)) is not None]
            selected.sort(key=lambda r: number(r.get('loss_percent')) or 0)
            if selected:
                x = [number(r.get('loss_percent')) for r in selected]
                y = [number(r.get(key)) for r in selected]
                plt.plot(x, y, marker='o', linewidth=2, label=algorithm, color=colors[algorithm])
                plotted = True
        if plotted:
            plt.title(title)
            plt.xlabel('Потери пакетов, %')
            plt.ylabel(ylabel)
            plt.grid(True, alpha=0.3)
            plt.legend()
            plt.tight_layout()
            plt.savefig(graph_dir / filename, dpi=160)
        plt.close()

    for loss in sorted({int(float(r['loss_percent'])) for r in valid if r.get('loss_percent', '').strip()}):
        plt.figure(figsize=(9, 5))
        plotted = False
        for algorithm in ('GBN', 'SR'):
            path = out / 'data' / f'{algorithm.lower()}_{loss}.csv'
            points = read_csv(path)
            if not points:
                continue
            x = [number(p.get('elapsed_ms')) for p in points]
            window = [number(p.get('cwnd')) for p in points]
            throughput = [number(p.get('throughput_kbps')) for p in points]
            pairs = [(a, b) for a, b in zip(x, window) if a is not None and b is not None]
            if pairs:
                plt.plot([p[0] / 1000 for p in pairs], [p[1] for p in pairs], marker='.', label=algorithm, color=colors[algorithm])
                plotted = True
        if plotted:
            plt.title(f'Изменение размера окна, потери {loss}%')
            plt.xlabel('Время, с')
            plt.ylabel('Размер окна, пакеты')
            plt.grid(True, alpha=0.3)
            plt.legend()
            plt.tight_layout()
            plt.savefig(graph_dir / f'window_{loss}.png', dpi=160)
        plt.close()

        plt.figure(figsize=(9, 5))
        plotted = False
        for algorithm in ('GBN', 'SR'):
            path = out / 'data' / f'{algorithm.lower()}_{loss}.csv'
            points = read_csv(path)
            if not points:
                continue
            pairs = [(number(p.get('elapsed_ms')), number(p.get('throughput_kbps'))) for p in points]
            pairs = [(a, b) for a, b in pairs if a is not None and b is not None]
            if pairs:
                plt.plot([p[0] / 1000 for p in pairs], [p[1] for p in pairs], marker='.', label=algorithm, color=colors[algorithm])
                plotted = True
        if plotted:
            plt.title(f'Пропускная способность во времени, потери {loss}%')
            plt.xlabel('Время, с')
            plt.ylabel('Скорость, кбит/с')
            plt.grid(True, alpha=0.3)
            plt.legend()
            plt.tight_layout()
            plt.savefig(graph_dir / f'throughput_{loss}.png', dpi=160)
        plt.close()

    created = sorted(p.name for p in graph_dir.glob('*.png'))
    print('Графики построены по существующим CSV. Повторная передача файлов не запускалась.')
    print(f'Папка: {graph_dir}')
    for name in created:
        print(f'  {name}')
    if not created:
        print('Не удалось построить графики: проверь содержимое summary.csv и data/.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())