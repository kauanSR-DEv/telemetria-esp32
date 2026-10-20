"""Conferência entre os bytes observados e as decisões reais do RX.

Permite ressalvas documentais apenas quando as decisões do RX estão completas
e todos os quadros originais do TX ainda estão preservados e corroborados.
Nunca estima classificações, timestamps ou medidas ausentes.
"""
import re
from collections import Counter


def residual(data, technique):
    result = 0
    for byte in data:
        if technique == 'SUM':
            result = (result + byte) & 255
        else:
            result ^= byte
            for _ in range(8):
                result = ((result << 1) ^ 7) & 255 if result & 128 else (result << 1) & 255
    return result


def hex_bytes(value, length):
    if not re.fullmatch('[0-9A-Fa-f]{%d}' % (2 * length), value):
        raise ValueError('campo hexadecimal inválido')
    return bytes.fromhex(value)


def audit(records, n, technique, mode, meta):
    errors, warnings, fragments = [], [], []
    tx, rx, noise = (records[k] for k in ('TX', 'RX', 'NOISE'))
    original_frames = {}
    for row in tx['rows']:
        seq = row['seq']
        if seq in original_frames:
            errors.append(f'Auditoria: sequência TX {seq} duplicada.')
        original_frames[seq] = row['quadro_hex'].upper()

    # O padrão abaixo foi observado no arquivo bruto. Os bytes de quadro e o
    # tempo de cálculo são extraídos como fragmentos; não viram linhas completas.
    for role, record in records.items():
        for bad in record.get('invalid_lines', []):
            if bad['antes_start']:
                warnings.append(f'{role}: bytes inválidos antes do START atual, na linha {bad["linha"]}.')
                continue
            text = bad['texto']
            fragment = re.fullmatch('\ufffd{64},([0-9A-Fa-f]{16}),([0-9]+)', text) if role == 'TX' else None
            auxiliary = re.fullmatch('\ufffd+[0-9]+,reconciliar_depois', text) if role == 'RX' else None
            if fragment:
                frame = bytes.fromhex(fragment[1])
                seq = str(int.from_bytes(frame[3:5], 'big'))
                if seq in original_frames:
                    errors.append(f'Auditoria: fragmento TX repete sequência {seq}.')
                original_frames[seq] = fragment[1].upper()
                fragments.append({'linha_txt': bad['linha'], 'seq_no_quadro': seq,
                                  'quadro_hex': fragment[1].upper(),
                                  'calculo_us_preservado': fragment[2],
                                  't_inicio_us': 'NA', 'intervalo_us': 'NA'})
                warnings.append(f'TX: sequência {seq} com campos iniciais danificados; quadro preservado e conferido no NOISE. Tempos dessa linha excluídos das médias.')
            elif auxiliary:
                warnings.append(f'RX: mensagem auxiliar de lacuna danificada na linha {bad["linha"]}; as classificações são conferidas separadamente.')
            else:
                errors.append(f'{role}: bytes inválidos em linha não reconhecida como ressalva segura (linha {bad["linha"]}).')

    expected_seqs = {str(i) for i in range(n)}
    if set(original_frames) != expected_seqs:
        errors.append('Auditoria: nem todos os quadros originais do TX estão preservados em linhas completas ou fragmentos verificáveis.')
    for role in ('RX', 'NOISE'):
        seqs = [row['seq_original'] for row in records[role]['rows']]
        if len(seqs) != n or set(seqs) != expected_seqs:
            errors.append(f'Auditoria: {role} precisa dos N registros individuais, sem lacunas ou duplicações.')
    noise_map = {r['seq_original']: r for r in noise['rows']}
    rx_map = {r['seq_original']: r for r in rx['rows']}
    pairs = 0
    for seq in sorted(expected_seqs, key=int):
        if seq not in original_frames or seq not in noise_map or seq not in rx_map:
            continue
        z, r = noise_map[seq], rx_map[seq]
        try:
            original = hex_bytes(z['original_hex'], 8)
            received = hex_bytes(z['receptor_hex'], 8)
            target = hex_bytes(z['alvos_hex'], 8)
            changed = hex_bytes(z['xor_hex'], 8)
            if original[:3] != bytes.fromhex('A55A02') or int.from_bytes(original[3:5], 'big') != int(seq):
                raise ValueError('quadro original com marcador, LEN ou sequência incompatível')
            if residual(original[2:], technique):
                raise ValueError('resíduo do quadro original não é zero')
            if original.hex().upper() != original_frames[seq]:
                raise ValueError('quadro preservado do TX diverge da UART limpa do NOISE')
            if any(target[:2]):
                raise ValueError('alvos sobre o marcador')
            bits = sum(b.bit_count() for b in target)
            positions = ';'.join(f'{byte}.{bit}' for byte in range(8) for bit in range(8) if target[byte] & (1 << bit)) or 'NA'
            if z['posicoes_byte_bit'] != positions:
                raise ValueError('posições registradas divergem da máscara de alvos')
            if mode in (0, 1, 2) and bits != mode:
                raise ValueError('quantidade de alvos incompatível com o modo')
            if mode == 3:
                nonzero = [b for b in target if b]
                if len(nonzero) != 1 or not 3 <= bits <= 8:
                    raise ValueError('rajada fora de um único byte ou comprimento inválido')
                burst = nonzero[0]
                while not burst & 1:
                    burst >>= 1
                if burst & (burst + 1):
                    raise ValueError('rajada não contígua')
            xor = bytes(a ^ b for a, b in zip(original, received))
            if changed != xor or bytes(a & ~m for a, m in zip(original, target)) != received:
                raise ValueError('XOR ou efeito físico incompatível com os alvos')
            if int(z['alvos_n']) != bits or int(z['mudancas_n']) != sum(b.bit_count() for b in xor):
                raise ValueError('contagem de bits incompatível com os bytes')
            reference = bytes.fromhex('D391') + original[3:5] + bytes([2]) + original[5:7]
            if hex_bytes(z['debug_original_hex'], 7) != reference or hex_bytes(z['debug_receptor_hex'], 7) != reference:
                raise ValueError('referência ausente, alterada ou incompatível')
            if hex_bytes(r['payload_referencia'], 2) != original[5:7]:
                raise ValueError('referência do RX diverge dos bytes originais')
            if r['classe'] == 'PERDIDO':
                # Uma perda legítima pode ocorrer por enquadramento/timeout.
                # A classificação continua sendo a registrada pelo RX.
                if r['decisao'] != 'SEM_CHECAGEM' or r['residuo'] != 'NA':
                    raise ValueError('PERDIDO com decisão de checagem concluída')
            else:
                if received[2] != 2:
                    raise ValueError('checagem concluída apesar de LEN inválido')
                value = residual(received[2:], technique)
                expected = 'CORROMPIDO' if value else 'OK' if received[5:7] == original[5:7] else 'NAO_DETECTADO'
                if r['classe'] != expected or r['residuo'].upper() != f'{value:02X}':
                    raise ValueError('classe/resíduo do RX incompatível com os bytes observados')
                if r['decisao'] != ('DESCARTADO' if value else 'ACEITO'):
                    raise ValueError('decisão incompatível com o resíduo')
                if hex_bytes(r['payload_recebido'], 2) != received[5:7] or int(r['seq_recebida']) != int.from_bytes(received[3:5], 'big'):
                    raise ValueError('payload/sequência recebida incompatível com o NOISE')
                if r['payload_confere'] != ('SIM' if received[5:7] == original[5:7] else 'NAO'):
                    raise ValueError('payload_confere incompatível com os bytes')
            pairs += 1
        except (ValueError, KeyError, TypeError) as exc:
            errors.append(f'Auditoria, sequência {seq}: {exc}.')
    if warnings and meta.get('estado') != 'CONCLUIDA':
        errors.append('Auditoria: aceitar ressalvas exige sessao.json com encerramento CONCLUIDA.')
    cfg = meta.get('configuracao', {})
    if cfg and any(cfg.get(k) != v for k, v in [('tecnica', technique), ('modo', mode), ('N', n)]):
        errors.append('Auditoria: configuração do coletor diverge dos cabeçalhos.')
    if cfg and str(cfg.get('seed')) != noise['start'].get('seed'):
        errors.append('Auditoria: semente do coletor diverge do NOISE.')
    if rx['summary'].get('sessao') != rx['start'].get('sessao'):
        errors.append('Auditoria: sessão do resumo RX diverge do START.')
    return {'erros': errors, 'ressalvas': warnings, 'fragmentos_tx': fragments,
            'quadros_cruzados': pairs, 'quadros_tx_preservados': len(original_frames)}
