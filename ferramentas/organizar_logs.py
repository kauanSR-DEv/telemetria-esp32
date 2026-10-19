"""Converte tx.txt, rx.txt e noise.txt de uma pasta em CSVs e resumo."""
import argparse
from pathlib import Path
from registros import organize, consolidate


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('pasta', type=Path, help='Pasta contendo somente uma rodada das três placas')
    args = p.parse_args()
    if not args.pasta.is_dir():
        p.error('A pasta não existe.')
    s = organize(args.pasta)
    consolidate()
    print('Resumo:', args.pasta / 'resumo.md')
    print('Coleta válida:', 'SIM' if s['coleta_valida'] else 'NÃO')
    for issue in s['pendencias']:
        print('-', issue)
    return 0 if s['coleta_valida'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
