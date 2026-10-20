"""Dados inteiramente sintéticos, criados em pastas temporárias.

Estes testes verificam o software de coleta/organização e não são ensaios físicos.
"""
import csv
import hashlib
import io
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
        elif cls == 'PERDIDO':
            dirty[2] = 0
        mask = [a ^ b for a, b in zip(data, dirty)]
        if mode in (1, 2):
            for bit in (2, 3):
                if sum(v.bit_count() for v in mask) < mode:
                    mask[2] |= 1 << bit
        elif mode == 3:
            mask[5] = 7
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
        positions = ';'.join(f'{j}.{k}' for j in range(8) for k in range(8) if mask[j] & (1 << k)) or 'NA'
        ref = 'D391' + bytes(data[3:5]).hex().upper() + '028080'
        no.append(','.join(map(str, ['QUADRO', 3, mode, i, i, 100000+i*50000, positions,
                  bytes(mask).hex().upper(), original, bytes(dirty).hex().upper(), sum(v.bit_count() for v in mask),
                  sum((a ^ b).bit_count() for a, b in zip(data, dirty)), actual,
                  'NA', 0, 'NA', ref, ref, 0, 'VALIDO'])))
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

    def damage_tx(self, seq=1, mismatch=False):
        p = self.path / 'tx.txt'
        lines = p.read_bytes().splitlines(keepends=True)
        for i, line in enumerate(lines):
            fields = line.rstrip().split(b',')
            if len(fields) == 8 and fields[0].isdigit() and fields[2] == str(seq).encode():
                frame = fields[6]
                if mismatch:
                    frame = frame[:-2] + b'00'
                lines[i] = b'\xff' * 64 + b',' + frame + b',' + fields[7] + b'\n'
                break
        p.write_bytes(b''.join(lines))

    def completed_meta(self, mode=0):
        (self.path / 'sessao.json').write_text(json.dumps({
            'estado': 'CONCLUIDA', 'configuracao': {'tecnica': 'SUM', 'modo': mode, 'N': 2, 'seed': 1}}))

    def test_partial_tx_accepted_only_with_corroborated_frame_and_explicit_samples(self):
        fixture(self.path)
        self.completed_meta()
        self.damage_tx()
        originals = {p.name: p.read_bytes() for p in self.path.glob('*.txt')}
        s = r.organize(self.path)
        self.assertTrue(s['coleta_valida'], s['pendencias'])
        self.assertEqual(s['situacao'], 'VALIDA_COM_RESSALVAS')
        self.assertFalse(s['validacao_estrita_aprovada'])
        self.assertEqual(s['auditoria_cruzada']['quadros_conferidos'], 2)
        self.assertEqual(s['metricas']['calculo_tx_us']['amostras'], 1)
        self.assertEqual(s['metricas']['intervalo_tx_us']['amostras'], 0)
        self.assertEqual(originals, {p.name: p.read_bytes() for p in self.path.glob('*.txt')})
        self.assertTrue(s['ressalvas'])

    def test_missing_tx_frame_without_fragment_cannot_be_waived(self):
        fixture(self.path)
        self.completed_meta()
        p = self.path / 'tx.txt'
        p.write_bytes(b'\n'.join(l for l in p.read_bytes().splitlines() if not l.startswith(b'1,SUM,1,')) + b'\n')
        self.assertFalse(r.organize(self.path)['coleta_valida'])

    def test_corrupted_tx_fragment_with_different_frame_cannot_be_waived(self):
        fixture(self.path)
        self.completed_meta()
        self.damage_tx(mismatch=True)
        self.assertFalse(r.organize(self.path)['coleta_valida'])

    def test_missing_receiver_row_cannot_be_waived(self):
        fixture(self.path)
        self.completed_meta()
        self.damage_tx()
        p = self.path / 'rx.txt'
        p.write_bytes(b'\n'.join(l for l in p.read_bytes().splitlines() if not l.startswith(b'2,SUM,1,')) + b'\n')
        self.assertFalse(r.organize(self.path)['coleta_valida'])

    def test_bad_rx_auxiliary_can_be_waived_but_classifications_remain_complete(self):
        fixture(self.path)
        self.completed_meta()
        p = self.path / 'rx.txt'
        p.write_bytes(b'\xff'*64 + b'old summary\n' + p.read_bytes().replace(b'2,SUM,1,', b'\xff'*64 + b'3,reconciliar_depois\n2,SUM,1,'))
        s = r.organize(self.path)
        self.assertTrue(s['coleta_valida'], s['pendencias'])
        self.assertEqual(s['situacao'], 'VALIDA_COM_RESSALVAS')
        self.assertEqual(s['contagens']['OK'], 2)

    def test_unknown_corrupted_line_remains_blocking(self):
        fixture(self.path)
        self.completed_meta()
        p = self.path / 'rx.txt'
        p.write_bytes(p.read_bytes() + b'\xff'*64 + b'unknown event\n')
        self.assertFalse(r.organize(self.path)['coleta_valida'])

    def test_waiver_requires_completed_session_metadata(self):
        fixture(self.path)
        self.damage_tx()
        self.assertFalse(r.organize(self.path)['coleta_valida'])

    def test_noise_quality_label_does_not_hide_inconsistent_bytes(self):
        fixture(self.path)
        self.change('noise.txt', '0000000000000000,A55A', '0000000000800000,A55A')
        self.assertFalse(r.organize(self.path)['coleta_valida'])

    def test_correct_counts_with_wrong_residue_cannot_be_waived(self):
        fixture(self.path)
        self.completed_meta()
        self.damage_tx()
        self.change('rx.txt', ',00,ACEITO,', ',01,ACEITO,')
        self.assertFalse(r.organize(self.path)['coleta_valida'])


class CollectorTests(unittest.TestCase):
    def identify_session(self, port):
        s = coletar.Session({}, queue.Queue(), threading.Event())
        s.ports['TX'] = port
        s.command_file = io.StringIO()
        s.commands = csv.writer(s.command_file)
        return s

    def test_identity_does_not_require_exact_help_text(self):
        self.assertEqual(coletar.reported_role('# Comandos: HELP, SUM, CRC, START N, STOP, STATUS; BENCH'), 'TX')
        self.assertEqual(coletar.reported_role('# HELP: SUM; CRC; START; END; STOP; STATUS'), 'RX')
        self.assertEqual(coletar.reported_role('# STATUS,TX,tecnica=SUM,ativo=0,enviados=1000'), 'TX')
        self.assertEqual(coletar.reported_role('# MODE 0 1 2 3 | SEED | START | STATUS'), 'NOISE')
        self.assertIsNone(coletar.reported_role('rst:0x1 (POWERON_RESET)'))

    def test_tx_can_be_identified_by_status_without_help(self):
        class Port:
            failure = None
            def __init__(self): self.lines = []
            def snapshot(self): return self.lines.copy()
            def send(self, cmd):
                if cmd == 'STATUS': self.lines.append('# STATUS,TX,tecnica=SUM,ativo=0,enviados=1000')
        self.identify_session(Port()).identify('TX', attempts=1, timeout=0.1)

    def test_identity_retries_a_lost_initial_command(self):
        class Port:
            failure = None
            def __init__(self): self.lines, self.count = [], 0
            def snapshot(self): return self.lines.copy()
            def send(self, cmd):
                if cmd == 'STATUS':
                    self.count += 1
                    if self.count == 2: self.lines.append('# STATUS,TX,tecnica=SUM,ativo=0')
        p = Port()
        self.identify_session(p).identify('TX', attempts=2, timeout=0.05)
        self.assertEqual(p.count, 2)

    def test_identity_points_out_swapped_ports(self):
        class Port:
            failure = None
            def snapshot(self): return ['# STATUS,NOISE,ativo=0'] if hasattr(self, 'sent') else []
            def send(self, cmd): self.sent = True
        with self.assertRaisesRegex(RuntimeError, 'respondeu como NOISE'):
            self.identify_session(Port()).identify('TX', attempts=1, timeout=0.1)

    def test_identity_failure_exposes_actual_response(self):
        class Port:
            failure = None
            def __init__(self): self.lines = []
            def snapshot(self): return self.lines.copy()
            def send(self, cmd): self.lines.append('Firmware de teste: resposta diferente')
        with self.assertRaisesRegex(RuntimeError, 'Firmware de teste: resposta diferente'):
            self.identify_session(Port()).identify('TX', attempts=1, timeout=0.05)

    def test_failure_before_start_has_one_diagnostic_and_no_stop(self):
        commands = []
        class Port:
            failure = None
            def __init__(self, device, role, folder, events):
                self.path = folder / (role.lower() + '.txt')
                self.path.write_text('Diagnóstico pré-ensaio\n')
            def snapshot(self): return []
            def send(self, cmd): commands.append(cmd)
            def close(self): pass
        with tempfile.TemporaryDirectory() as tmp, \
             patch.object(coletar, 'ROOT', Path(tmp)), patch.object(coletar, 'Port', Port), \
             patch.object(coletar.Session, 'pause', lambda self, n: None), \
             patch.object(coletar.Session, 'identify', side_effect=RuntimeError('TX: HELP incompatível')):
            cfg = {'tecnica': 'SUM', 'modo': 0, 'N': 10, 'seed': 1,
                   'portas': {'TX': 'fake1', 'RX': 'fake2', 'NOISE': 'fake3'}}
            s = coletar.Session(cfg, queue.Queue(), threading.Event())
            s.run()
            result = json.loads((s.folder / 'resumo.json').read_text())
            self.assertEqual(result['estado'], 'NAO_INICIADA')
            self.assertEqual(result['pendencias'], ['TX: HELP incompatível'])
            self.assertFalse(result['coleta_valida'])
            self.assertFalse(any(c.startswith('START') or c == 'STOP' for c in commands))
            self.assertFalse((Path(tmp) / 'resultados/tabelas').exists())

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
