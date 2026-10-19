"""Organiza uma rodada dos firmwares finais. Não depende de pyserial.

Os TXT são evidência primária e nunca são alterados por este módulo.
Uma coleta só entra na tabela final se houver evidência completa das 3 placas.
"""
from __future__ import annotations

import csv
import hashlib
import json
import statistics
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TX = 'sessao,tecnica,seq,t_inicio_us,intervalo_us,payload_hex,quadro_hex,calculo_us'.split(',')
RX = ('sessao,tecnica,seq_original,seq_recebida,id_rx,t_sof_us,intervalo_us,residuo,'
      'decisao,payload_recebido,payload_referencia,payload_confere,verificacao_us,'
      'recuperacao_ms,classe,motivo,evento_seq').split(',')
NOISE = ('tipo,sessao,modo,indice,seq_original,t_sof_us,posicoes_byte_bit,alvos_hex,'
         'original_hex,receptor_hex,alvos_n,mudancas_n,xor_hex,eventos_planejados_ticks,'
         'atraso_max_ticks,eventos_aplicados_ticks,debug_original_hex,debug_receptor_hex,'
         'falhas_mask,qualidade').split(',')
CLASSES = ['OK', 'CORROMPIDO', 'PERDIDO', 'NAO_DETECTADO']
TABLE = ['Modo', 'OK', 'CORROMPIDO', 'PERDIDO', 'Não detectado', 'Taxa de não detecção (%)']


def kv(line: str) -> dict:
    return dict(part.split('=', 1) for part in line.split(',') if '=' in part)


def save_csv(path: Path, fields: list, rows) -> None:
    with path.open('w', encoding='utf-8-sig', newline='') as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction='ignore')
        w.writeheader()
        w.writerows(rows)


def number(value):
    if value in ('NA', '', None):
        return None
    try:
        return float(value)
    except (ValueError, TypeError):
        return None


def metric(rows: list, field: str) -> dict:
    values = [number(r.get(field)) for r in rows]
    values = [v for v in values if v is not None]
    return {'amostras': len(values), 'media': statistics.mean(values) if values else None,
            'minimo': min(values) if values else None, 'maximo': max(values) if values else None}


def parse(path: Path, role: str, fields: list, issues: list) -> dict:
    if not path.exists():
        issues.append(f'{role}: falta o arquivo {path.name}.')
        return {'start': {}, 'rows': [], 'lines': [], 'summary': {}, 'done': {}}
    raw = path.read_bytes()
    try:
        text = raw.decode('utf-8-sig')
    except UnicodeDecodeError:
        text = raw.decode('utf-8-sig', errors='replace')
        issues.append(f'{role}: bytes inválidos para UTF-8 no TXT.')
    lines = text.splitlines()
    starts = [i for i, s in enumerate(lines) if s.startswith(f'# START,{role},')]
    if len(starts) != 1:
        issues.append(f'{role}: esperado um START; encontrados {len(starts)}. Separe as rodadas.')
    start = kv(lines[starts[0]]) if starts else {}
    active = lines[starts[0]:] if starts else lines
    rows, header_seen = [], 0
    for line in active:
        if line.startswith('#') or not line.strip():
            continue
        parts = next(csv.reader([line]))
        if parts == fields:
            header_seen += 1
            continue
        candidate = (role == 'NOISE' and parts[0] in ('QUADRO', 'RESUMO')) or parts[0].isdigit()
        if candidate:
            if len(parts) != len(fields) or not header_seen:
                issues.append(f'{role}: linha tabular incompleta ou sem cabeçalho: {line[:90]}')
            elif role != 'NOISE' or parts[0] == 'QUADRO':
                rows.append(dict(zip(fields, parts)))
    if header_seen != 1:
        issues.append(f'{role}: esperado um cabeçalho CSV; encontrados {header_seen}.')
    if raw and not raw.endswith(b'\n'):
        issues.append(f'{role}: o TXT termina com uma linha sem quebra final; verificar captura.')
    for line in active:
        if line.startswith(('# ERRO', '# ABORT', '# ABORTADA', '# CAL_START')):
            issues.append(f'{role}: {line}')
    summary = next((kv(s) for s in reversed(active) if s.startswith('# RESUMO,')), {})
    done = next((kv(s) for s in reversed(active) if s.startswith(f'# DONE,{role},')), {})
    return {'start': start, 'rows': rows, 'lines': active, 'summary': summary, 'done': done}


def organize(folder: Path) -> dict:
    folder = Path(folder)
    issues = []
    records = {role: parse(folder / f'{role.lower()}.txt', role, fields, issues)
               for role, fields in [('TX', TX), ('RX', RX), ('NOISE', NOISE)]}
    tx, rx, noise = (records[k] for k in ('TX', 'RX', 'NOISE'))
    technique = tx['start'].get('tecnica', 'DESCONHECIDA')
    n = int(tx['start'].get('N', '0')) if tx['start'].get('N', '').isdigit() else 0
    mode = int(noise['start'].get('modo', '-1')) if noise['start'].get('modo', '').isdigit() else -1
    if technique not in ('SUM', 'CRC8') or rx['start'].get('tecnica') != technique:
        issues.append('TX e RX devem declarar a mesma técnica: SUM ou CRC8.')
    if not 1 <= n <= 1000 or mode not in range(4):
        issues.append('Quantidade ou modo ausente/inválido nos cabeçalhos.')
    if noise['start'].get('firmware') != '1.12-sincronizado':
        issues.append('O cabeçalho do NOISE não confirma a versão 1.12-sincronizado.')
    if noise['start'].get('seed', '').isdigit() is False:
        issues.append('Semente do NOISE não identificada.')
    for role, field in [('TX', 'seq'), ('RX', 'seq_original'), ('NOISE', 'seq_original')]:
        r = records[role]
        if r['start'].get('N') != str(n):
            issues.append(f'{role}: N diverge do TX.')
        if len(r['rows']) != n:
            issues.append(f'{role}: {len(r["rows"])} registros; esperado {n}.')
        seqs = [int(v[field]) for v in r['rows'] if v[field].isdigit()]
        if sorted(seqs) != list(range(n)):
            issues.append(f'{role}: sequências ausentes, duplicadas ou fora da faixa.')
        if any(v.get('sessao') != r['start'].get('sessao') for v in r['rows']):
            issues.append(f'{role}: mistura de sessões no CSV.')
        if role != 'NOISE' and any(v.get('tecnica') != technique for v in r['rows']):
            issues.append(f'{role}: técnica divergente em linhas do CSV.')
    if tx['done'].get('enviados') != str(n) or tx['done'].get('sessao') != tx['start'].get('sessao'):
        issues.append('TX: DONE não confirma o total e a sessão esperados.')
    if rx['summary'].get('coleta_valida') != 'SIM' or rx['summary'].get('N') != str(n):
        issues.append('RX: resumo final não confirma coleta válida e N esperado.')
    counts = Counter(r['classe'] for r in rx['rows'])
    for k in CLASSES:
        if rx['summary'].get(k) != str(counts[k]):
            issues.append(f'RX: contagem de {k} diverge do resumo.')
    if any(r['classe'] not in CLASSES for r in rx['rows']):
        issues.append('RX: há classificações inconclusivas ou desconhecidas.')
    for k in ('INCONCLUSIVO', 'PENDENTE'):
        if rx['summary'].get(k) != '0':
            issues.append(f'RX: {k} não é zero.')
    ndone = noise['done']
    if ndone.get('valido') != 'SIM' or any(ndone.get(k) != str(n) for k in
            ('N', 'registrados', 'SOF', 'limpos', 'receptor', 'validos')):
        issues.append('NOISE: DONE não confirma rodada válida com todos os registros.')
    if ndone.get('falhas') != '0' or ndone.get('erros_uart') != '0':
        issues.append('NOISE: há falhas ou erros de UART, ou falta o resumo.')
    indexes = [int(v['indice']) for v in noise['rows'] if v['indice'].isdigit()]
    if sorted(indexes) != list(range(n)):
        issues.append('NOISE: índices ausentes, duplicados ou fora da faixa.')
    if any(v['modo'] != str(mode) or v['qualidade'] != 'VALIDO' or v['falhas_mask'] != '0'
           for v in noise['rows']):
        issues.append('NOISE: linha com modo divergente, falha ou qualidade inválida.')
    txmap = {v['seq']: v for v in tx['rows']}
    noisemap = {v['seq_original']: v for v in noise['rows']}
    evidence = []
    for v in rx['rows']:
        seq = v['seq_original']
        t, z = txmap.get(seq, {}), noisemap.get(seq, {})
        if t and z and t['quadro_hex'].upper() != z['original_hex'].upper():
            issues.append(f'Quadro {seq}: bytes do TX divergem da UART limpa do NOISE.')
        if t and v['payload_referencia'] != 'NA' and t['payload_hex'].upper() != v['payload_referencia'].upper():
            issues.append(f'Quadro {seq}: referência do RX diverge do TX.')
        if v['classe'] in ('OK', 'NAO_DETECTADO'):
            if v['decisao'] != 'ACEITO' or v['residuo'] != '00':
                issues.append(f'Quadro {seq}: classe aceita incompatível com decisão/resíduo.')
            expected = 'SIM' if v['classe'] == 'OK' else 'NAO'
            if v['payload_confere'] != expected:
                issues.append(f'Quadro {seq}: classe incompatível com payload_confere.')
        if v['classe'] == 'CORROMPIDO' and (v['decisao'] != 'DESCARTADO' or v['residuo'] in ('00', 'NA')):
            issues.append(f'Quadro {seq}: CORROMPIDO incompatível com decisão/resíduo.')
        if v['classe'] == 'NAO_DETECTADO':
            evidence.append({'seq': seq, 'tx_hex': t.get('quadro_hex', 'NA'),
                             'rx_hex_observado_noise': z.get('receptor_hex', 'NA'),
                             'posicoes_alvo': z.get('posicoes_byte_bit', 'NA'),
                             'xor_medido': z.get('xor_hex', 'NA'), 'residuo_rx': v['residuo'],
                             'payload_recebido': v['payload_recebido'], 'payload_original': v['payload_referencia']})
    # O software do computador também pode ter detectado perda de conexão/aborto.
    meta_path = folder / 'sessao.json'
    meta = json.loads(meta_path.read_text(encoding='utf-8')) if meta_path.exists() else {}
    if meta.get('erro'):
        issues.append('Coletor: ' + str(meta['erro']))
    if meta.get('estado') and meta['estado'] != 'CONCLUIDA':
        issues.append('Coletor: sessão não concluída normalmente.')
    denom = counts['CORROMPIDO'] + counts['NAO_DETECTADO']
    rate = 100 * counts['NAO_DETECTADO'] / denom if denom else None
    checks = {
        'calculo_tx_us': metric(tx['rows'], 'calculo_us'),
        'verificacao_rx_us': metric(rx['rows'], 'verificacao_us'),
        'intervalo_tx_us': metric(tx['rows'], 'intervalo_us'),
        'intervalo_rx_us': metric(rx['rows'], 'intervalo_us'),
        'recuperacao_rx_ms': metric(rx['rows'], 'recuperacao_ms'),
    }
    summary = {
        'tecnica': technique, 'modo': mode, 'N': n, 'seed': noise['start'].get('seed'),
        'contagens': {k: counts[k] for k in CLASSES}, 'taxa_nao_deteccao_percentual': rate,
        'coleta_valida': not issues, 'elegivel_tabela_final': not issues and n == 1000,
        'pendencias': list(dict.fromkeys(issues)), 'metricas': checks, 'overhead_bytes': 6,
        'iniciada_em': meta.get('iniciada_em', ''),
        'sha256_logs': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in (folder / f'{r}.txt' for r in ('tx', 'rx', 'noise')) if p.exists()},
    }
    for role, fields in [('TX', TX), ('RX', RX), ('NOISE', NOISE)]:
        save_csv(folder / f'{role.lower()}.csv', fields, records[role]['rows'])
    save_csv(folder / 'evidencias_nd.csv', ['seq', 'tx_hex', 'rx_hex_observado_noise', 'posicoes_alvo',
             'xor_medido', 'residuo_rx', 'payload_recebido', 'payload_original'], evidence)
    save_csv(folder / 'tabela_professor.csv', TABLE, [table_row(summary)])
    (folder / 'resumo.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    lines = [f'# {technique} · modo {mode}', '',
             f'Coleta: **{"VÁLIDA" if not issues else "REVISAR"}** · N = {n} · semente = {summary["seed"]}', '',
             '| Modo | OK | CORROMPIDO | PERDIDO | Não detectado | Taxa (%) |',
             '|---|---:|---:|---:|---:|---:|', markdown_row(summary), '',
             'A tabela acima descreve esta coleta. Apenas coletas válidas com 1000 quadros entram nas tabelas finais.', '',
             '| Medida | Média | Mínimo | Máximo | Amostras |', '|---|---:|---:|---:|---:|']
    for key, val in checks.items():
        lines.append(f'| {key} | {fmt(val["media"])} | {fmt(val["minimo"])} | {fmt(val["maximo"])} | {val["amostras"]} |')
    lines += ['', 'Overhead: 6 bytes por quadro, excluindo referência e bits de início/parada.',
              'NA indica ausência de amostra ou denominador zero; não significa zero medido.', '',
              '## Evidências', '', '[TX completo](tx.txt) · [RX completo](rx.txt) · [NOISE completo](noise.txt)', '',
              '[Erros não detectados desta coleta](evidencias_nd.csv)', '']
    if issues:
        lines += ['## Pendências', ''] + ['- ' + i for i in summary['pendencias']] + ['']
    (folder / 'resumo.md').write_text('\n'.join(lines), encoding='utf-8')
    return summary


def fmt(value):
    return 'NA' if value is None else f'{value:.6f}'.rstrip('0').rstrip('.')


def table_row(s):
    c = s['contagens']
    return dict(zip(TABLE, [s['modo'], c['OK'], c['CORROMPIDO'], c['PERDIDO'],
                           c['NAO_DETECTADO'], fmt(s['taxa_nao_deteccao_percentual'])]))


def markdown_row(s):
    return '| ' + ' | '.join(str(v) for v in table_row(s).values()) + ' |'


def consolidate(root: Path = ROOT) -> None:
    """Usa a pasta mais recente, pelo nome datado, dentre as coletas elegíveis."""
    selected = {}
    for file in sorted((root / 'resultados/coletas').glob('*/resumo.json')):
        s = json.loads(file.read_text(encoding='utf-8'))
        if s.get('elegivel_tabela_final'):
            selected[(s['tecnica'], s['modo'])] = (s, file.parent)
    dest = root / 'resultados/tabelas'
    dest.mkdir(parents=True, exist_ok=True)
    lines = ['# Resultados consolidados', '',
             'Somente rodadas válidas com 1000 quadros. Se houver repetição, usa-se a pasta datada mais recente.', '',
             'Taxa = ND / (CORROMPIDO + ND) × 100. NA: denominador zero. A coletar: sem rodada elegível.', '']
    for technique, modes in [('SUM', range(4)), ('CRC8', range(1, 4))]:
        rows = []
        lines += [f'## {technique}', '', '| Modo | OK | CORROMPIDO | PERDIDO | Não detectado | Taxa (%) |',
                  '|---|---:|---:|---:|---:|---:|']
        links = []
        for mode in modes:
            if (technique, mode) in selected:
                s, folder = selected[(technique, mode)]
                rows.append(table_row(s))
                lines.append(markdown_row(s))
                links.append(f'- Modo {mode}: [resumo e logs](../coletas/{folder.name}/resumo.md) · semente {s["seed"]}')
            else:
                rows.append(dict(zip(TABLE, [mode] + ['A coletar'] * 5)))
                lines.append(f'| {mode} | A coletar | A coletar | A coletar | A coletar | A coletar |')
        save_csv(dest / f'{technique}.csv', TABLE, rows)
        lines += ['', *links, '']
    (dest / 'README.md').write_text('\n'.join(lines), encoding='utf-8')
