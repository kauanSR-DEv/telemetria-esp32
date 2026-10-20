"""Converte tx.txt, rx.txt e noise.txt de uma pasta em CSVs e resumo."""
import argparse
import json
from pathlib import Path
from registros import ROOT, organize, consolidate


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('pasta', type=Path, nargs='?', help='Pasta contendo somente uma rodada das três placas')
    p.add_argument('--todas', action='store_true', help='Reanalisa as pastas existentes em resultados/coletas, sem executar testes físicos')
    args = p.parse_args()
    if args.todas:
        folders = [f for f in sorted((ROOT / 'resultados/coletas').iterdir())
                   if f.is_dir() and all((f / (role + '.txt')).exists() for role in ('tx', 'rx', 'noise'))]
    elif args.pasta and args.pasta.is_dir():
        folders = [args.pasta]
    else:
        p.error('Informe uma pasta existente ou use --todas.')
    invalid = 0
    processed = 0
    for folder in folders:
        meta = folder / 'sessao.json'
        if meta.exists() and json.loads(meta.read_text(encoding='utf-8')).get('estado') == 'NAO_INICIADA':
            print(folder.name + ': sem START; diagnóstico preservado e ignorado nas tabelas.')
            continue
        s = organize(folder)
        processed += 1
        print(folder.name + ': ' + s['situacao'])
        for issue in s['pendencias']:
            print('  Pendência:', issue)
        for warning in s.get('ressalvas', []):
            print('  Ressalva:', warning)
        invalid += not s['coleta_valida']
    consolidate()
    print('Pastas reanalisadas:', processed, '| Com pendências:', invalid)
    print('Os TXT originais foram preservados. As tabelas foram atualizadas.')
    return 1 if invalid else 0


if __name__ == '__main__':
    raise SystemExit(main())
