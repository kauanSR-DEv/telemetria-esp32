"""Dados inteiramente sintéticos, criados em pastas temporárias.

Estes testes verificam o software de coleta/organização e não são ensaios físicos.
"""
import csv
import hashlib
import json
import queue
import sys
import tempfile
import threading
import time
import types
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'ferramentas'))
import registros as r
import coletar


def fixture(folder, n=2, classes=None, mode=0):
    classes = classes or ['OK'] * n
    counts = {k: classes.count(k) for k in r.CLASSES}
    tx = ['# START,TX,sessao=1,tecnica=SUM,N=%d,periodo_us=50000,overhead=6' % n, ','.join(r.TX)]
    rx = ['# START,RX,sessao=2,tecnica=SUM,N=%d,periodo_us=50000,overhead=6' % n, ','.join(r.RX)]
    no = ['# START,NOISE,sessao=3,N=%d,modo=%d,seed=1,tick_us=0.1,firmware=1.12-sincronizado' % (n, mode), ','.join(r.NOISE)]
    for i, cls in enumerate(classes):
        data = [0xA5, 0x5A, 2, i >> 8, i & 255, 128, 128]
        data.append(-sum(data[2:]) % 256)
        original = bytes(data).hex().upper()
        dirty = data.copy()
        if cls == 'CORROMPIDO':
            dirty[5] = 0
        elif cls == 'NAO_DETECTADO':
            dirty[5] = dirty[6] = 0
        actual = bytes(a ^ b for a, b in zip(data, dirty)).hex().upper()
        tx.append(f'1,SUM,{i},{100000+i*50000},{"NA" if i == 0 else 50000},8080,{original},3')
        accepted = cls in ('OK', 'NAO_DETECTADO')
        decision = 'ACEITO' if accepted else 'DESCARTADO' if cls == 'CORROMPIDO' else 'SEM_CHECAGEM'
        residue = f'{sum(dirty[2:]) % 256:02X}' if cls != 'PERDIDO' else 'NA'
        payload = bytes(dirty[5:7]).hex().upper() if cls != 'PERDIDO' else 'NA'
        matches = 'SIM' if payload == '8080' else 'NA' if cls == 'PERDIDO' else 'NAO'
        rx.append(','.join(map(str, [2, 'SUM', i, i, i+1, 100000+i*50000,
                   'NA' if i == 0 else 50000, residue, decision, payload, '8080', matches,
                   'NA' if cls == 'PERDIDO' else 4, 'NA', cls, 'NA', 'ORDEM' if accepted else 'NA'])))
        no.append(','.join(map(str, ['QUADRO', 3, mode, i, i, 100000+i*50000, 'NA',
                  '0000000000000000', original, bytes(dirty).hex().upper(), 0,
                  sum((a ^ b).bit_count() for a, b in zip(data, dirty)), actual,
                  'NA', 0, 'NA', 'D3910000028080', 'D3910000028080', 0, 'VALIDO'])))
    tx.append(f'# DONE,TX,sessao=1,enviados={n}')
    rx += ['# RESUMO,sessao=2,N=%d,%s,INCONCLUSIVO=0,PENDENTE=0,taxa_percentual=NA,coleta_valida=SIM' %
           (n, ','.join(f'{k}={v}' for k, v in counts.items())), '# VERIFICACAO_MEDIA_US,4.0000,amostras=%d' % n]
    no.append(f'# DONE,NOISE,N={n},registrados={n},SOF={n},limpos={n},receptor={n},validos={n},falhas=0,erros_uart=0,valido=SIM')
    for name, lines in [('tx', tx), ('rx', rx), ('noise', no)]:
        (folder / f'{name}.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')


class RegistrosTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def change(self, file, old, new):
        p = self.path / file
        p.write_text(p.read_text().replace(old, new), encoding='utf-8')

    def test_valid_short_session_and_raw_bytes_preserved(self):
        fixture(self.path)
        raw = {p.name: p.read_bytes() for p in self.path.glob('*.txt')}
        s = r.organize(self.path)
        self.assertTrue(s['coleta_valida'], s['pendencias'])
        self.assertFalse(s['elegivel_tabela_final'])
        self.assertIsNone(s['taxa_nao_deteccao_percentual'])
        self.assertEqual(s['metricas']['calculo_tx_us']['media'], 3)
        self.assertEqual(s['metricas']['intervalo_tx_us']['amostras'], 1)
        self.assertEqual(raw, {p.name: p.read_bytes() for p in self.path.glob('*.txt')})

    def test_true_table_rate_undetected_evidence_and_lost_excluded(self):
        fixture(self.path, 4, ['OK', 'CORROMPIDO', 'PERDIDO', 'NAO_DETECTADO'], mode=2)
        s = r.organize(self.path)
        self.assertTrue(s['coleta_valida'], s['pendencias'])
        self.assertEqual(s['taxa_nao_deteccao_percentual'], 50)
        self.assertEqual(s['metricas']['verificacao_rx_us']['amostras'], 3)
        with (self.path / 'evidencias_nd.csv').open(encoding='utf-8-sig', newline='') as f:
            evidence = list(csv.DictReader(f))
        self.assertEqual(len(evidence), 1)
        self.assertEqual(evidence[0]['seq'], '3')

    def test_missing_file_is_incomplete(self):
        s = r.organize(self.path)
        self.assertFalse(s['coleta_valida'])

    def test_truncated_line(self):
        fixture(self.path)
        self.change('rx.txt', ',4,NA,OK,NA,ORDEM', ',4,NA,OK')
        self.assertFalse(r.organize(self.path)['coleta_valida'])

    def test_duplicate_sequence(self):
        fixture(self.path)
        self.change('tx.txt', '1,SUM,1,', '1,SUM,0,')
        self.assertFalse(r.organize(self.path)['coleta_valida'])

    def test_summary_counts_must_match_rows(self):
        fixture(self.path)
        self.change('rx.txt', 'OK=2', 'OK=1')
        self.assertFalse(r.organize(self.path)['coleta_valida'])

    def test_noise_failures_reject_even_valid_rx(self):
        fixture(self.path)
        self.change('noise.txt', 'falhas=0', 'falhas=1')
        self.assertFalse(r.organize(self.path)['coleta_valida'])

    def test_multiple_sessions_not_merged(self):
        fixture(self.path)
        with (self.path / 'tx.txt').open('a') as f:
            f.write('# START,TX,sessao=2,tecnica=SUM,N=2\n')
        self.assertFalse(r.organize(self.path)['coleta_valida'])

    def test_wrong_technique_or_noise_revision(self):
        fixture(self.path)
        self.change('rx.txt', 'tecnica=SUM', 'tecnica=CRC8')
        self.change('noise.txt', '1.12-sincronizado', '1.11-captura')
        s = r.organize(self.path)
        self.assertFalse(s['coleta_valida'])
        self.assertTrue(any('1.12' in e for e in s['pendencias']))

    def test_clean_observation_must_match_tx(self):
        fixture(self.path)
        self.change('noise.txt', 'A55A0200008080FE', 'A55A0200008080FF')
        self.assertFalse(r.organize(self.path)['coleta_valida'])

    def test_abort_metadata_rejects_even_complete_counts(self):
        fixture(self.path)
        (self.path / 'sessao.json').write_text(json.dumps({'estado': 'INTERROMPIDA', 'erro': 'USB desconectado'}))
        self.assertFalse(r.organize(self.path)['coleta_valida'])

    def test_consolidation_chooses_latest_valid_official_session(self):
        base = self.path / 'resultados/coletas'
        for name, n in [('20261018_100000_SUM_M0_seed1', 1000), ('20261018_110000_SUM_M0_seed1', 10)]:
            folder = base / name
            folder.mkdir(parents=True)
            fixture(folder, n)
            r.organize(folder)
        r.consolidate(self.path)
        md = (self.path / 'resultados/tabelas/README.md').read_text()
        self.assertIn('| 0 | 1000 | 0 | 0 | 0 | NA |', md)
        self.assertIn('100000_SUM', md)
        self.assertNotIn('110000_SUM', md)

    def test_firmware_bytes_match_manifest(self):
        manifest = json.loads((ROOT / 'firmware/SHA256.json').read_text())
        for role, digest in manifest.items():
            p = ROOT / f'firmware/{role}/{role}.ino'
            self.assertEqual(hashlib.sha256(p.read_bytes()).hexdigest(), digest)


class CollectorTests(unittest.TestCase):
    def test_usb_fragments_preserve_exact_raw_bytes_and_complete_lines(self):
        chunks = [b'# STA', b'RT,TX\r\n1,', b'SUM,0\npartial']

        class Connection:
            def __init__(self, **kwargs):
                self.chunks = chunks.copy()

            def open(self):
                pass

            @property
            def in_waiting(self):
                return len(self.chunks[0]) if self.chunks else 0

            def read(self, size):
                if self.chunks:
                    return self.chunks.pop(0)
                time.sleep(0.002)
                return b''

            def close(self):
                pass

        with tempfile.TemporaryDirectory() as tmp, \
             patch.dict(sys.modules, {'serial': types.SimpleNamespace(Serial=Connection)}):
            p = coletar.Port('fake', 'TX', Path(tmp), queue.Queue())
            deadline = time.monotonic() + 1
            while len(p.snapshot()) < 2 and time.monotonic() < deadline:
                time.sleep(0.002)
            p.close()
            self.assertEqual(p.snapshot(), ['# START,TX', '1,SUM,0'])
            self.assertEqual((Path(tmp) / 'tx.txt').read_bytes(), b''.join(chunks))

    def test_commands_automatic_end_only_after_done_and_outputs_valid(self):
        self.run_simulation(fail_tx=False)

    def test_error_aborts_and_preserves_partial_logs_without_end(self):
        self.run_simulation(fail_tx=True)

    def run_simulation(self, fail_tx):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'synthetic'
            source.mkdir()
            fixture(source)
            texts = {role: (source / f'{role.lower()}.txt').read_text().splitlines()
                     for role in ('TX', 'RX', 'NOISE')}
            ports, commands = {}, []

            class FakePort:
                def __init__(self, device, role, folder, events):
                    self.role, self.failure = role, None
                    self.lines = []
                    self.path = folder / f'{role.lower()}.txt'
                    ports[role] = self

                def snapshot(self):
                    return self.lines.copy()

                def send(self, cmd):
                    commands.append((self.role, cmd))
                    if cmd == 'HELP':
                        self.lines.append({'TX': '# HELP SUM CRC START [1..1000] STOP STATUS BENCH',
                                           'RX': '# HELP SUM CRC START [1..1000] END STOP STATUS BENCH',
                                           'NOISE': '# MODE 0|1|2|3; SEED 1..4294967295; START [1..1000]; END; STOP; STATUS; HELP'}[self.role])
                    elif cmd == 'SUM':
                        self.lines.append('# STATUS,TX,tecnica=SUM,ativo=0,enviados=0,alvo=1000' if self.role == 'TX' else '# TECNICA,SUM')
                    elif cmd == 'MODE 0':
                        self.lines.append('# STATUS,NOISE,ativo=0,modo=0,SOF=0,limpos=0,receptor=0,registrados=0,erros_uart=0')
                    elif cmd == 'SEED 1':
                        self.lines.append('# SEED=1')
                    elif cmd == 'START 2':
                        if self.role == 'TX' and fail_tx:
                            self.lines.append('# ERRO: falha sintética de transmissão.')
                        else:
                            self.lines.extend(texts[self.role][:2])
                            if self.role == 'TX':
                                self.lines.extend(texts['TX'][2:])
                                ports['RX'].lines.extend(texts['RX'][2:-2])
                                ports['NOISE'].lines.extend(texts['NOISE'][2:])
                    elif cmd == 'END' and self.role == 'RX':
                        self.lines.extend(texts['RX'][-2:])

                def close(self):
                    self.path.write_text('\n'.join(self.lines) + '\n', encoding='utf-8')

            events = queue.Queue()
            cfg = {'tecnica': 'SUM', 'modo': 0, 'N': 2, 'seed': 1,
                   'portas': {'TX': 'fake1', 'RX': 'fake2', 'NOISE': 'fake3'}}
            with patch.object(coletar, 'ROOT', root), patch.object(coletar, 'Port', FakePort), \
                 patch.object(coletar.Session, 'pause', lambda self, n: self.check()), \
                 patch.object(coletar, 'consolidate', lambda: r.consolidate(root)):
                session = coletar.Session(cfg, events, threading.Event())
                session.run()
            s = json.loads((session.folder / 'resumo.json').read_text())
            self.assertEqual(s['coleta_valida'], not fail_tx, s['pendencias'])
            if fail_tx:
                self.assertFalse(any(c == 'END' for _, c in commands))
                self.assertTrue(any(c == 'STOP' for _, c in commands))
            else:
                self.assertLess(commands.index(('RX', 'START 2')), commands.index(('TX', 'START 2')))
                self.assertLess(commands.index(('NOISE', 'START 2')), commands.index(('TX', 'START 2')))
                self.assertGreater(commands.index(('RX', 'END')), commands.index(('TX', 'START 2')))


if __name__ == '__main__':
    unittest.main()
