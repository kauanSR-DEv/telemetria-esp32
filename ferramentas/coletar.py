"""Coletor gráfico para TX/RX/NOISE. Python 3.10+, Tk e pyserial."""
from __future__ import annotations

import csv
import hashlib
import json
import os
import queue
import subprocess
import sys
import threading
import time
import webbrowser
from datetime import datetime
from pathlib import Path
try:
    import tkinter as tk
    from tkinter import ttk, messagebox
except ImportError:
    tk = ttk = messagebox = None

from registros import ROOT, organize, consolidate


class Port:
    def __init__(self, device, role, folder, event_queue):
        import serial
        self.role, self.events = role, event_queue
        self.lines, self.lock = [], threading.Lock()
        self.failure = None
        self.stop = threading.Event()
        self.raw = (folder / f'{role.lower()}.txt').open('xb')
        self.conn = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=2)
        self.conn.port = device
        self.conn.dtr = False
        self.conn.rts = False
        try:
            self.conn.open()
        except Exception:
            self.raw.close()
            raise
        self.worker = threading.Thread(target=self.read, daemon=True)
        self.worker.start()

    def read(self):
        pending = b''
        try:
            while not self.stop.is_set():
                data = self.conn.read(min(self.conn.in_waiting or 1, 8192))
                if not data:
                    continue
                self.raw.write(data)
                self.raw.flush()
                pending += data
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    text = line.decode('utf-8', errors='replace').rstrip('\r')
                    with self.lock:
                        self.lines.append(text)
                    if text.startswith(('# START,', '# DONE,', '# RESUMO,', '# ERRO', '# ABORT')):
                        self.events.put(('log', f'{self.role}: {text}'))
                if len(pending) > 65536:
                    raise RuntimeError('Mais de 64 KiB sem fim de linha; confira baudrate/firmware.')
        except Exception as exc:
            self.failure = f'{self.role}: falha na leitura: {exc}'

    def snapshot(self):
        with self.lock:
            return self.lines.copy()

    def send(self, command):
        self.conn.write((command + '\n').encode('ascii'))
        self.conn.flush()

    def close(self):
        self.stop.set()
        self.worker.join(timeout=2)
        self.conn.close()
        if self.worker.is_alive():
            self.worker.join(timeout=2)
        self.raw.close()


class Session:
    def __init__(self, config, events, cancelled):
        self.config, self.events, self.cancelled = config, events, cancelled
        self.ports, self.folder = {}, None

    def check(self):
        if self.cancelled.is_set():
            raise RuntimeError('Coleta interrompida pelo usuário; logs parciais preservados.')
        for p in self.ports.values():
            if p.failure:
                raise RuntimeError(p.failure)

    def pause(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.check()
            time.sleep(0.05)

    def wait_line(self, role, mark, predicate, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.check()
            for line in self.ports[role].snapshot()[mark:]:
                if line.startswith(('# ERRO', '# ABORT')):
                    raise RuntimeError(f'{role}: {line}')
                if predicate(line):
                    return line
            time.sleep(0.03)
        raise RuntimeError(f'{role}: sem resposta esperada. Confira porta, firmware e Monitor Serial fechado.')

    def command(self, role, command):
        self.check()
        mark = len(self.ports[role].snapshot())
        self.commands.writerow([datetime.now().astimezone().isoformat(), role, command])
        self.command_file.flush()
        self.ports[role].send(command)
        return mark

    def request(self, role, command, predicate, timeout=5):
        mark = self.command(role, command)
        return self.wait_line(role, mark, predicate, timeout)

    def run(self):
        cfg = self.config
        now = datetime.now().astimezone()
        name = now.strftime('%Y%m%d_%H%M%S_%f') + f'_{cfg["tecnica"]}_M{cfg["modo"]}_seed{cfg["seed"]}'
        self.folder = ROOT / 'resultados/coletas' / name
        self.folder.mkdir(parents=True, exist_ok=False)
        meta = {'iniciada_em': now.isoformat(), 'estado': 'EM_ANDAMENTO', 'configuracao': cfg,
                'python': sys.version.split()[0],
                'firmware_sha256': {p.parent.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                    for p in (ROOT / 'firmware').rglob('*.ino')}}
        meta_path = self.folder / 'sessao.json'
        meta_path.write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding='utf-8')
        self.command_file = (self.folder / 'comandos.csv').open('x', encoding='utf-8-sig', newline='')
        self.commands = csv.writer(self.command_file)
        self.commands.writerow(['horario_computador', 'placa', 'comando'])
        try:
            self.events.put(('log', 'Abrindo as três portas. Aguarde a identificação das placas…'))
            for role in ('RX', 'NOISE', 'TX'):
                self.ports[role] = Port(cfg['portas'][role], role, self.folder, self.events)
            # Alguns adaptadores reiniciam a placa ao abrir a porta, mesmo com DTR/RTS baixos.
            self.pause(3)
            identities = {
                'TX': lambda s: s == '# HELP SUM CRC START [1..1000] STOP STATUS BENCH',
                'RX': lambda s: s == '# HELP SUM CRC START [1..1000] END STOP STATUS BENCH',
                'NOISE': lambda s: s.startswith('# MODE 0|1|2|3; SEED '),
            }
            for role, predicate in identities.items():
                self.request(role, 'HELP', predicate)
            tech = 'SUM' if cfg['tecnica'] == 'SUM' else 'CRC'
            printed = 'SUM' if tech == 'SUM' else 'CRC8'
            self.request('TX', tech, lambda s: s.startswith('# STATUS,TX,') and f'tecnica={printed},' in s and 'ativo=0,' in s)
            self.request('RX', tech, lambda s: s == '# TECNICA,' + printed)
            self.request('NOISE', f'MODE {cfg["modo"]}', lambda s: s.startswith('# STATUS,NOISE,') and f'modo={cfg["modo"]},' in s and 'ativo=0,' in s)
            self.request('NOISE', f'SEED {cfg["seed"]}', lambda s: s == f'# SEED={cfg["seed"]}')
            starts = {}
            for role in ('RX', 'NOISE', 'TX'):
                starts[role] = self.command(role, f'START {cfg["N"]}')
                self.wait_line(role, starts[role], lambda s, r=role: s.startswith(f'# START,{r},') and f'N={cfg["N"]},' in s)
            self.events.put(('log', 'Sessão em execução. Os registros estão sendo salvos automaticamente.'))
            self.wait_line('TX', starts['TX'], lambda s: s.startswith('# DONE,TX,') and s.endswith(f'enviados={cfg["N"]}'),
                           timeout=max(20, cfg['N'] * 0.07 + 20))
            # Só confirma END depois do DONE do TX. O NOISE pode concluir sozinho.
            self.command('RX', 'END')
            if not any(s.startswith('# DONE,NOISE,') for s in self.ports['NOISE'].snapshot()[starts['NOISE']:]):
                self.command('NOISE', 'END')
            self.wait_line('RX', starts['RX'], lambda s: s.startswith('# RESUMO,'), timeout=10)
            self.wait_line('NOISE', starts['NOISE'], lambda s: s.startswith('# DONE,NOISE,'), timeout=10)
            self.pause(0.5)  # Drena diagnósticos e a média impressa após o resumo.
            meta['estado'] = 'CONCLUIDA'
        except Exception as exc:
            meta['estado'], meta['erro'] = 'INTERROMPIDA', str(exc)
            self.events.put(('log', str(exc)))
            # STOP aborta; nunca se envia END para fingir uma rodada completa.
            for role in ('TX', 'NOISE', 'RX'):
                if role in self.ports:
                    try:
                        self.commands.writerow([datetime.now().astimezone().isoformat(), role, 'STOP'])
                        self.ports[role].send('STOP')
                    except Exception:
                        pass
            time.sleep(0.3)
        finally:
            for p in self.ports.values():
                try:
                    p.close()
                except Exception as exc:
                    meta['erro'] = str(exc)
                    meta['estado'] = 'INTERROMPIDA'
            self.command_file.close()
            meta['finalizada_em'] = datetime.now().astimezone().isoformat()
            meta_path.write_text(json.dumps(meta, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        summary = organize(self.folder)
        consolidate()
        self.events.put(('finished', (self.folder, summary)))


class App:
    def __init__(self, root):
        self.root = root
        self.events, self.cancelled = queue.Queue(), threading.Event()
        self.busy, self.last_folder = False, None
        root.title('Telemetria ESP32 | Coletor de ensaios')
        root.geometry('920x700')
        root.minsize(760, 620)
        style = ttk.Style()
        style.theme_use('clam')
        style.configure('Title.TLabel', font=('Segoe UI', 22, 'bold'))
        style.configure('Sub.TLabel', foreground='#526477', font=('Segoe UI', 10))
        style.configure('TButton', padding=7)
        frame = ttk.Frame(root, padding=22)
        frame.pack(fill='both', expand=True)
        ttk.Label(frame, text='Telemetria sob ruído real', style='Title.TLabel').pack(anchor='w')
        ttk.Label(frame, text='TX + RX + NOISE 1.12 · TXT completos, CSVs e tabela do professor', style='Sub.TLabel').pack(anchor='w', pady=(3, 14))
        ttk.Label(frame, text='Grave os firmwares pela Arduino IDE e feche TODOS os Monitores Seriais antes da coleta.').pack(anchor='w')
        group = ttk.LabelFrame(frame, text='1 · Portas das placas', padding=12)
        group.pack(fill='x', pady=12)
        self.port_vars, self.combos = {}, []
        for col, role in enumerate(('TX', 'RX', 'NOISE')):
            ttk.Label(group, text=role).grid(row=0, column=col, sticky='w')
            var = tk.StringVar()
            combo = ttk.Combobox(group, textvariable=var, width=20, state='readonly')
            combo.grid(row=1, column=col, sticky='ew', padx=(0, 12))
            self.port_vars[role] = var
            self.combos.append(combo)
            group.columnconfigure(col, weight=1)
        self.refresh_btn = ttk.Button(group, text='Atualizar portas', command=self.refresh)
        self.refresh_btn.grid(row=1, column=3)
        self.port_hint = ttk.Label(group, text='', style='Sub.TLabel', wraplength=820)
        self.port_hint.grid(row=2, column=0, columnspan=4, sticky='w', pady=(8, 0))
        opts = ttk.LabelFrame(frame, text='2 · Ensaio', padding=12)
        opts.pack(fill='x')
        self.tech, self.mode, self.seed, self.count = (tk.StringVar(value=v) for v in ('SUM', '0', '1', '1000'))
        self.option_widgets = []
        for col, (label, var, values) in enumerate([
                ('Técnica', self.tech, ('SUM', 'CRC8')), ('Modo', self.mode, ('0', '1', '2', '3')),
                ('Semente', self.seed, None), ('Quadros', self.count, ('10', '1000'))]):
            ttk.Label(opts, text=label).grid(row=0, column=col, sticky='w')
            w = ttk.Combobox(opts, textvariable=var, values=values, state='readonly', width=12) if values else ttk.Entry(opts, textvariable=var, width=14)
            w.grid(row=1, column=col, sticky='ew', padx=(0, 12))
            opts.columnconfigure(col, weight=1)
            self.option_widgets.append(w)
        ttk.Label(opts, text='Primeiro valide SUM / modo 0 / 10 quadros. As rodadas oficiais usam 1000.', style='Sub.TLabel').grid(row=2, column=0, columnspan=4, sticky='w', pady=(8, 0))
        actions = ttk.Frame(frame)
        actions.pack(fill='x', pady=14)
        self.start_btn = ttk.Button(actions, text='Iniciar coleta', command=self.start)
        self.start_btn.pack(side='left')
        self.stop_btn = ttk.Button(actions, text='Interromper', command=self.cancelled.set, state='disabled')
        self.stop_btn.pack(side='left', padx=8)
        ttk.Button(actions, text='Abrir pasta dos resultados', command=self.open_folder).pack(side='right')
        self.status = tk.StringVar(value='Pronto para configurar o ensaio.')
        ttk.Label(frame, textvariable=self.status, wraplength=850).pack(anchor='w', pady=(0, 8))
        self.log = tk.Text(frame, height=12, bg='#102033', fg='#d5e7f6', font=('Consolas', 10), wrap='word', state='disabled')
        self.log.pack(fill='both', expand=True)
        self.refresh()
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.poll)

    def refresh(self):
        from serial.tools import list_ports
        ports = list(list_ports.comports())
        for combo in self.combos:
            combo['values'] = [p.device for p in ports]
        self.port_hint['text'] = ' | '.join(f'{p.device}: {p.description}' for p in ports) or 'Nenhuma porta encontrada. Conecte as três placas e atualize.'

    def append(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text + '\n')
        self.log.see('end')
        self.log.configure(state='disabled')

    def start(self):
        ports = {k: v.get() for k, v in self.port_vars.items()}
        if len(set(ports.values())) != 3 or not all(ports.values()):
            messagebox.showerror('Portas', 'Selecione três portas diferentes, uma para cada placa.')
            return
        try:
            seed = int(self.seed.get())
            if not 1 <= seed <= 4294967295:
                raise ValueError
        except ValueError:
            messagebox.showerror('Semente', 'Use um inteiro de 1 a 4294967295.')
            return
        cfg = {'portas': ports, 'tecnica': self.tech.get(), 'modo': int(self.mode.get()), 'seed': seed, 'N': int(self.count.get())}
        self.cancelled.clear()
        self.busy = True
        self.start_btn['state'], self.stop_btn['state'] = 'disabled', 'normal'
        self.refresh_btn['state'] = 'disabled'
        for w in self.combos + self.option_widgets:
            w['state'] = 'disabled'
        self.status.set('Coletando… Aguarde a conclusão antes de desconectar as placas.')
        self.append(f'Novo ensaio: {cfg["tecnica"]}, modo {cfg["modo"]}, N={cfg["N"]}, semente={seed}')

        def worker():
            try:
                Session(cfg, self.events, self.cancelled).run()
            except Exception as exc:
                self.events.put(('fatal', str(exc)))
        threading.Thread(target=worker, daemon=True).start()

    def unlock(self):
        self.busy = False
        self.start_btn['state'], self.stop_btn['state'] = 'normal', 'disabled'
        self.refresh_btn['state'] = 'normal'
        for w in self.combos + self.option_widgets:
            w['state'] = 'normal' if isinstance(w, ttk.Entry) and not isinstance(w, ttk.Combobox) else 'readonly'

    def poll(self):
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == 'log':
                    self.append(value)
                elif kind == 'finished':
                    self.last_folder, s = value
                    self.unlock()
                    state = 'VÁLIDA' if s['coleta_valida'] else 'REVISAR'
                    self.status.set(f'Coleta {state}. Arquivos em: {self.last_folder.name}')
                    self.append(f'Finalizado: {state}. Consulte resumo.md para resultados e pendências.')
                    for issue in s['pendencias'][:6]:
                        self.append('• ' + issue)
                    if s['coleta_valida'] and not s['elegivel_tabela_final']:
                        self.append('Ensaio curto: não entra nas tabelas oficiais de 1000 quadros.')
                elif kind == 'fatal':
                    self.unlock()
                    self.status.set('Falha na ferramenta; confira a mensagem abaixo.')
                    self.append(value)
                    messagebox.showerror('Coleta', value)
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def open_folder(self):
        folder = self.last_folder or ROOT / 'resultados'
        if sys.platform == 'win32':
            os.startfile(folder)
        elif sys.platform == 'darwin':
            subprocess.Popen(['open', str(folder)])
        else:
            webbrowser.open(folder.as_uri())

    def close(self):
        if self.busy:
            messagebox.showinfo('Coleta em andamento', 'Clique em Interromper e aguarde o salvamento antes de fechar.')
        else:
            self.root.destroy()


def main():
    if tk is None:
        raise SystemExit('Tk não está instalado. No Windows, use o instalador completo do Python com Tcl/Tk.')
    try:
        import serial
    except ImportError:
        raise SystemExit('Instale: python -m pip install -r requirements.txt')
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == '__main__':
    main()
