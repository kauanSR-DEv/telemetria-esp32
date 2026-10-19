/*
 * RX - Telemetria ESP32, protocolo 1.2, etapa SEM injetor de ruido.
 * Alvo: ESP32-WROOM-32 / ESP32 Dev Module / Arduino-ESP32 3.3.12.
 * UART2: RX=16, TX=17, 115200, 8N1. USB: Monitor Serial a 115200.
 * Comandos: HELP, SUM, CRC, START [1..1000], END, STOP, STATUS, BENCH.
 * START prepara RX; depois inicie TX. END confirma que TX terminou N envios.
 * STOP aborta: nao inventa perdas dos envios que talvez nem aconteceram.
 * Nao gera payloads nem usa a referencia para aceitar/descartar.
 */
#include <Arduino.h>
#include <esp_timer.h>
#include <atomic>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

class RxProtocol {
 public:
  enum Technique : uint8_t { SUM, CRC };
  static constexpr uint32_t BAUD = 115200, PERIOD_US = 50000;
  static constexpr uint32_t FRAME_TIMEOUT_US = 2000, GAP_TIMEOUT_US = 75000;
  static constexpr uint16_t MAX_FRAMES = 1000;
  static uint64_t now() { return uint64_t(esp_timer_get_time()); }
  static const char *name(Technique kind) { return kind == SUM ? "SUM" : "CRC8"; }
  static uint8_t check(const volatile uint8_t *data, size_t n, Technique kind) {
    uint8_t result = 0;
    for (size_t i = 0; i < n; ++i) {
      if (kind == SUM) result = uint8_t(result + data[i]);
      else {
        result ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
          result = uint8_t((result & 0x80) ? (result << 1) ^ 0x07 : result << 1);
      }
    }
    return result;
  }
};

// Conta lacunas modulares sem usar sequencia de um quadro reprovado.
class SequenceTracker {
 public:
  enum Event : uint8_t { ORDER, GAP, OLD };
  uint16_t expected = 0;
  Event accept(uint16_t received, uint16_t &gap) {
    gap = uint16_t(received - expected);
    if (gap >= 32768) return OLD;
    expected = uint16_t(received + 1);
    return gap ? GAP : ORDER;
  }
};

class Receiver {
 public:
  enum Parser : uint8_t { SEEK, FRAME, REFERENCE };
  enum Decision : uint8_t { ACCEPTED, REJECTED, INCOMPLETE };
  enum Cause : uint8_t { NONE, LENGTH, FRAME_TIMEOUT, UART_ERROR, GAP_OR_TIMEOUT };
  enum Classification : uint8_t { UNSEEN, OK, CORRUPTED, LOST, UNDETECTED, UNKNOWN };
  struct Candidate {
    uint8_t bytes[8] = {}, size = 0, residue = 0;
    Decision decision = INCOMPLETE;
    Cause cause = NONE;
    SequenceTracker::Event sequenceEvent = SequenceTracker::ORDER;
    uint32_t id = 0, verifyUs = 0;
    uint64_t startUs = 0, decisionUs = 0, intervalUs = 0, recoveryUs = 0;
    bool hasInterval = false, hasRecovery = false;
  };
  struct Reference { uint8_t bytes[7] = {}; uint64_t arrivalUs = 0; };
  struct Record {
    Candidate candidate;
    uint16_t originalPayload = 0;
    Classification classification = UNSEEN;
    bool hasReference = false, hasCandidate = false;
  };

  static constexpr uint8_t QUEUE_SIZE = 16;
  RxProtocol::Technique technique = RxProtocol::SUM;
  Parser parser = SEEK;
  Candidate current, candidates[QUEUE_SIZE];
  Reference currentReference, references[QUEUE_SIZE];
  uint8_t candidateCount = 0, referenceCount = 0, referenceSize = 0, prefix = 0;
  Record records[RxProtocol::MAX_FRAMES];
  SequenceTracker sequence;
  std::atomic<uint32_t> uartErrors{0};
  bool active = false, started = false, startupWarning = false, endRequested = false;
  bool auditError = false, recovering = false;
  uint16_t target = 1000, associated = 0;
  uint32_t session = 0, nextId = 1, duplicatedReferences = 0, timeoutEvents = 0;
  uint64_t armedUs = 0, prefixUs = 0, lastSofUs = 0, sofCompleteUs = 0;
  uint64_t nextGapTimeoutUs = 0, lossEvidenceThroughUs = 0, recoveryStartUs = 0;
  uint64_t finalizeAfterUs = 0;
  char command[64] = {};
  size_t commandLength = 0;
  bool commandOverflow = false;
  volatile uint8_t benchSink = 0;

  void begin() {
    Serial.begin(RxProtocol::BAUD);
    Serial2.setRxBufferSize(2048);
    Serial2.begin(RxProtocol::BAUD, SERIAL_8N1, 16, 17);
    Serial2.setRxFIFOFull(1); // Copia bytes cedo; evita esperar o FIFO encher.
    Serial2.setRxTimeout(1);
    Serial2.onReceiveError([this](hardwareSerial_error_t error) {
      if (error != UART_NO_ERROR) uartErrors.fetch_or(1u << unsigned(error), std::memory_order_relaxed);
    });
    Serial.println("# RX pronto: TX GPIO17 -> GPIO16; GND comum."); help();
  }
  void help() {
    Serial.println("# HELP SUM CRC START [1..1000] END STOP STATUS BENCH");
    Serial.println("# START prepara RX. Depois inicie TX com a mesma tecnica e quantidade.");
    Serial.println("# END somente apos DONE do TX. STOP cancela uma coleta incompleta.");
  }
  void start(uint16_t count) {
    if (active) { Serial.println("# ERRO: sessao ativa; use END ou STOP."); return; }
    if (!count || count > RxProtocol::MAX_FRAMES) { Serial.println("# ERRO: use START [1..1000]."); return; }
    while (Serial2.available()) Serial2.read();
    Serial2.eventQueueReset(); uartErrors.store(0, std::memory_order_relaxed);
    for (Record &r : records) r = Record{};
    target = count; associated = 0; ++session;
    candidateCount = referenceCount = referenceSize = prefix = 0;
    parser = SEEK; current = Candidate{}; currentReference = Reference{};
    sequence.expected = 0; nextId = 1; timeoutEvents = duplicatedReferences = 0;
    started = startupWarning = endRequested = auditError = recovering = false;
    lastSofUs = lossEvidenceThroughUs = finalizeAfterUs = 0;
    armedUs = RxProtocol::now(); active = true;
    Serial.printf("# START,RX,sessao=%lu,tecnica=%s,N=%u,periodo_us=50000,overhead=6\n",
                  (unsigned long)session, RxProtocol::name(technique), unsigned(target));
    Serial.println("sessao,tecnica,seq_original,seq_recebida,id_rx,t_sof_us,intervalo_us,residuo,decisao,payload_recebido,payload_referencia,payload_confere,verificacao_us,recuperacao_ms,classe,motivo,evento_seq");
  }
  static const char *decisionName(Decision d) {
    return d == ACCEPTED ? "ACEITO" : d == REJECTED ? "DESCARTADO" : "SEM_CHECAGEM";
  }
  static const char *className(Classification c) {
    switch (c) { case OK: return "OK"; case CORRUPTED: return "CORROMPIDO";
      case LOST: return "PERDIDO"; case UNDETECTED: return "NAO_DETECTADO";
      default: return "INCONCLUSIVO"; }
  }
  static const char *causeName(Cause c) {
    switch(c) { case LENGTH: return "COMPRIMENTO"; case FRAME_TIMEOUT: return "TIMEOUT_INTERNO";
      case UART_ERROR: return "ERRO_UART"; case GAP_OR_TIMEOUT: return "LACUNA_OU_TIMEOUT";
      default: return "NENHUM"; }
  }
  static uint16_t number(const uint8_t *b) { return uint16_t((uint16_t(b[0]) << 8) | b[1]); }
  void beginRecovery(uint64_t time) {
    if (!recovering) { recovering = true; recoveryStartUs = time; }
  }
  void pushCandidate() {
    if (candidateCount == QUEUE_SIZE) {
      auditError = true; Serial.println("# ERRO_AUDITORIA: fila de candidatos cheia; repetir coleta.");
    } else candidates[candidateCount++] = current;
    parser = SEEK; prefix = 0;
  }
  void abandon(Cause cause, uint64_t time) {
    // Retoma depois de LEN: bytes posteriores ja recebidos continuam na busca.
    uint8_t tail[5] = {}, tailSize = current.size > 3 ? current.size - 3 : 0;
    for (uint8_t i = 0; i < tailSize; ++i) tail[i] = current.bytes[i+3];
    current.decision = INCOMPLETE; current.cause = cause; current.decisionUs = time;
    beginRecovery(time); pushCandidate();
    for (uint8_t i = 0; i < tailSize; ++i) feed(tail[i], time);
  }
  void decide(uint64_t time) {
    const uint64_t t = RxProtocol::now();
    current.residue = RxProtocol::check(current.bytes + 2, 6, technique);
    current.verifyUs = uint32_t(RxProtocol::now() - t);
    current.decision = current.residue ? REJECTED : ACCEPTED;
    current.decisionUs = RxProtocol::now(); // Fixada ANTES de qualquer leitura da referencia.
    if (current.decision == ACCEPTED) {
      const uint16_t received = number(current.bytes + 3);
      uint16_t gap = 0; current.sequenceEvent = sequence.accept(received, gap);
      if (current.sequenceEvent == SequenceTracker::GAP) {
        // A lacuna declara faltas ANTERIORES, nunca a validade deste payload.
        if (current.startUs) lossEvidenceThroughUs = current.startUs - 1;
        Serial.printf("# LACUNA,esperados_nao_checados=%u,reconciliar_depois\n", unsigned(gap));
      }
      if (recovering) {
        current.hasRecovery = true; current.recoveryUs = time - recoveryStartUs; recovering = false;
      }
      if (current.sequenceEvent != SequenceTracker::OLD)
        Serial.printf("# APP,seq=%u,valor=%u\n", unsigned(received), unsigned(number(current.bytes + 5)));
      else Serial.printf("# REPETIDO_OU_FORA_DE_ORDEM,seq=%u\n", unsigned(received));
    }
    pushCandidate();
  }
  void feed(uint8_t byte, uint64_t time) {
    if (!active) return;
    // Verifica o prazo antes de incorporar um byte que chegou tarde.
    if (parser == FRAME && time - sofCompleteUs >= RxProtocol::FRAME_TIMEOUT_US)
      abandon(FRAME_TIMEOUT, time);
    if (parser == REFERENCE && time - currentReference.arrivalUs >= 5000) {
      auditError = true; parser = SEEK; prefix = 0;
      Serial.println("# ERRO_AUDITORIA: referencia incompleta.");
    }
    if (parser == FRAME) {
      current.bytes[current.size++] = byte;
      if (current.size == 3 && byte != 2) abandon(LENGTH, time);
      else if (current.size == 8) decide(time);
      return;
    }
    if (parser == REFERENCE) {
      currentReference.bytes[referenceSize++] = byte;
      if (referenceSize == 7) {
        if (referenceCount == QUEUE_SIZE) {
          auditError = true; Serial.println("# ERRO_AUDITORIA: fila de referencias cheia.");
        } else references[referenceCount++] = currentReference;
        parser = SEEK; prefix = 0; audit();
      }
      return;
    }
    if (prefix == 0xA5 && byte == 0x5A) {
      // Um candidato de um periodo anterior nao pode receber a referencia deste.
      // Nao usar SEQ recebida para adivinhar a associacao de um quadro corrompido.
      while (candidateCount && time - candidates[0].decisionUs > 5000) {
        auditError = true; shiftCandidate();
        Serial.println("# ERRO_AUDITORIA: quadro anterior sem referencia; repetir coleta.");
      }
      current = Candidate{}; current.bytes[0] = 0xA5; current.bytes[1] = 0x5A; current.size = 2;
      current.startUs = prefixUs; current.id = nextId++;
      current.hasInterval = started;
      if (started) current.intervalUs = current.startUs - lastSofUs;
      lastSofUs = current.startUs; sofCompleteUs = time;
      nextGapTimeoutUs = lastSofUs + RxProtocol::GAP_TIMEOUT_US;
      started = true; parser = FRAME; prefix = 0;
    } else if (prefix == 0xD3 && byte == 0x91) {
      currentReference = Reference{};
      currentReference.bytes[0] = 0xD3; currentReference.bytes[1] = 0x91;
      currentReference.arrivalUs = prefixUs; referenceSize = 2;
      parser = REFERENCE; prefix = 0;
    } else if (byte == 0xA5 || byte == 0xD3) { prefix = byte; prefixUs = time; }
    else prefix = 0;
  }
  void shiftCandidate() {
    for (uint8_t i = 1; i < candidateCount; ++i) candidates[i-1] = candidates[i];
    if (candidateCount) --candidateCount;
  }
  void shiftReference() {
    for (uint8_t i = 1; i < referenceCount; ++i) references[i-1] = references[i];
    if (referenceCount) --referenceCount;
  }
  void emit(uint16_t original, const Record &r) {
    char seq[8] = "NA", residue[4] = "NA", payload[5] = "NA", ref[5] = "NA";
    char interval[24] = "NA", startTime[24] = "NA", verify[16] = "NA", recovery[24] = "NA";
    const Candidate &c = r.candidate;
    if (r.hasCandidate) {
      snprintf(startTime, sizeof(startTime), "%llu", (unsigned long long)c.startUs);
      if (c.size >= 5) snprintf(seq, sizeof(seq), "%u", unsigned(number(c.bytes + 3)));
      if (c.size >= 7) snprintf(payload, sizeof(payload), "%02X%02X", unsigned(c.bytes[5]), unsigned(c.bytes[6]));
      if (c.hasInterval) snprintf(interval, sizeof(interval), "%llu", (unsigned long long)c.intervalUs);
      if (c.decision != INCOMPLETE) {
        snprintf(residue, sizeof(residue), "%02X", unsigned(c.residue));
        snprintf(verify, sizeof(verify), "%lu", (unsigned long)c.verifyUs);
      }
      if (c.hasRecovery) snprintf(recovery, sizeof(recovery), "%.3f", double(c.recoveryUs)/1000.0);
    }
    if (r.hasReference) snprintf(ref, sizeof(ref), "%04X", unsigned(r.originalPayload));
    const char *matches = "NA";
    if (r.hasReference && r.hasCandidate && c.size >= 7)
      matches = number(c.bytes + 5) == r.originalPayload ? "SIM" : "NAO";
    const char *event = !r.hasCandidate || c.decision != ACCEPTED ? "NA" :
      c.sequenceEvent == SequenceTracker::ORDER ? "ORDEM" : c.sequenceEvent == SequenceTracker::GAP ? "LACUNA" : "REPETIDO_OU_FORA_DE_ORDEM";
    Serial.printf("%lu,%s,%u,%s,%lu,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                  (unsigned long)session, RxProtocol::name(technique), unsigned(original), seq,
                  (unsigned long)(r.hasCandidate ? c.id : 0), startTime, interval, residue,
                  r.hasCandidate ? decisionName(c.decision) : "SEM_CHECAGEM", payload, ref,
                  matches, verify, recovery, className(r.classification), causeName(c.cause), event);
  }
  void audit() {
    while (referenceCount) {
      const Reference &ref = references[0];
      const bool matched = candidateCount && candidates[0].startUs <= ref.arrivalUs;
      if (!matched && ref.arrivalUs > lossEvidenceThroughUs) break;
      // A referencia permaneceu opaca ate haver decisao ou prova de falta.
      const uint16_t original = number(ref.bytes + 2);
      if (ref.bytes[4] != 2 || original >= target) {
        auditError = true; Serial.println("# ERRO_AUDITORIA: referencia invalida ou N divergente.");
        if (matched) shiftCandidate();
        shiftReference(); continue;
      }
      if (records[original].classification != UNSEEN) {
        ++duplicatedReferences; auditError = true;
        Serial.printf("# REFERENCIA_DUPLICADA,seq=%u; nao contar novamente.\n", unsigned(original));
        if (matched) shiftCandidate();
        shiftReference(); continue;
      }
      Record r;
      r.hasReference = true; r.originalPayload = number(ref.bytes + 5);
      if (matched) {
        r.hasCandidate = true; r.candidate = candidates[0];
        if (r.candidate.decision == REJECTED) r.classification = CORRUPTED;
        else if (r.candidate.decision == INCOMPLETE) r.classification = LOST;
        else r.classification = number(r.candidate.bytes + 5) == r.originalPayload ? OK : UNDETECTED;
        shiftCandidate();
      } else {
        r.classification = LOST; r.candidate.cause = GAP_OR_TIMEOUT;
      }
      records[original] = r; ++associated; emit(original, r); shiftReference();
    }
  }
  void handleUartError(uint64_t time, uint32_t flags) {
    if (!active || !flags) return;
    // Os avisos do driver sao assincronos, sem identificacao exata do byte.
    // Invalida a coleta para nao atribuir uma falha ao envio errado.
    auditError = true; Serial.printf("# ERRO_UART,flags=%lu; repetir coleta.\n", (unsigned long)flags);
    if (parser == FRAME) abandon(UART_ERROR, time);
    else if (parser == REFERENCE) { parser = SEEK; prefix = 0; }
    beginRecovery(time);
  }
  void poll(uint64_t time) {
    if (!active) return;
    if (parser == FRAME && time - sofCompleteUs >= RxProtocol::FRAME_TIMEOUT_US) abandon(FRAME_TIMEOUT, time);
    if (parser == REFERENCE && time - currentReference.arrivalUs >= 5000) {
      auditError = true; parser = SEEK; prefix = 0; Serial.println("# ERRO_AUDITORIA: timeout da referencia.");
    }
    if (!started && !startupWarning && time - armedUs >= 1000000) {
      startupWarning = true;
      Serial.println("# AGUARDANDO_TX: sessao nao iniciada; RX segue armado, sem contar perdas.");
    }
    // Depois de N referencias, silencio faz parte do encerramento, nao de novos envios.
    if (started && associated < target) {
      while (time >= nextGapTimeoutUs && timeoutEvents < target) {
        ++timeoutEvents; lossEvidenceThroughUs = time; beginRecovery(nextGapTimeoutUs);
        Serial.printf("# TIMEOUT_ENTRE_QUADROS,t_us=%llu; reconciliar com lacunas.\n",
                      (unsigned long long)nextGapTimeoutUs);
        nextGapTimeoutUs += RxProtocol::PERIOD_US;
      }
    }
    audit();
    if (started && associated == target && !endRequested) {
      endRequested = true; finalizeAfterUs = lastSofUs + RxProtocol::PERIOD_US + RxProtocol::GAP_TIMEOUT_US;
    }
    if (endRequested && time >= finalizeAfterUs) finish(false);
  }
  void finish(bool aborted) {
    if (!active) return;
    if (!aborted) { lossEvidenceThroughUs = RxProtocol::now(); audit(); }
    const bool uncertain = auditError || candidateCount || referenceCount || !started || parser != SEEK;
    for (uint16_t i = 0; i < target; ++i) {
      if (records[i].classification != UNSEEN) continue;
      Record r; r.classification = !aborted && !uncertain ? LOST : UNKNOWN;
      r.candidate.cause = GAP_OR_TIMEOUT; records[i] = r; emit(i, r);
    }
    active = false; summary(aborted);
  }
  void summary(bool aborted = false) {
    uint16_t counts[6] = {}; uint64_t verifyTotal = 0; uint16_t checked = 0;
    for (uint16_t i = 0; i < target; ++i) {
      ++counts[records[i].classification];
      if (records[i].hasCandidate && records[i].candidate.decision != INCOMPLETE) {
        verifyTotal += records[i].candidate.verifyUs; ++checked;
      }
    }
    const unsigned denominator = counts[CORRUPTED] + counts[UNDETECTED];
    char rate[24] = "NA";
    if (denominator) snprintf(rate, sizeof(rate), "%.6f", 100.0*counts[UNDETECTED]/denominator);
    const bool valid = !active && !aborted && !auditError && !counts[UNKNOWN] && !counts[UNSEEN];
    Serial.printf("# RESUMO,sessao=%lu,N=%u,OK=%u,CORROMPIDO=%u,PERDIDO=%u,NAO_DETECTADO=%u,INCONCLUSIVO=%u,PENDENTE=%u,taxa_percentual=%s,coleta_valida=%s\n",
                  (unsigned long)session, unsigned(target), unsigned(counts[OK]), unsigned(counts[CORRUPTED]),
                  unsigned(counts[LOST]), unsigned(counts[UNDETECTED]), unsigned(counts[UNKNOWN]),
                  unsigned(counts[UNSEEN]), rate, valid ? "SIM" : "NAO");
    if (checked) Serial.printf("# VERIFICACAO_MEDIA_US,%.4f,amostras=%u\n", double(verifyTotal)/checked, unsigned(checked));
    if (aborted) Serial.println("# ABORTADA: coleta parcial; nao usar como rodada oficial.");
  }
  void bench() {
    if (active) { Serial.println("# ERRO: BENCH somente fora da sessao."); return; }
    volatile uint8_t bytes[6] = {2, 0, 1, 0, 200, 53};
    constexpr uint32_t repetitions = 10000;
    const uint64_t t = RxProtocol::now();
    for (uint32_t i = 0; i < repetitions; ++i)
      benchSink = uint8_t(benchSink ^ RxProtocol::check(bytes, 6, technique));
    const uint64_t elapsed = RxProtocol::now() - t;
    Serial.printf("# BENCH,RX,%s,repeticoes=%lu,tempo_total_us=%llu,us_por_verificacao=%.4f\n",
                  RxProtocol::name(technique), (unsigned long)repetitions,
                  (unsigned long long)elapsed, double(elapsed)/repetitions);
  }
  void execute(char *line) {
    while (*line == ' ' || *line == '\t') ++line;
    for (char *p = line; *p; ++p) *p = char(toupper((unsigned char)*p));
    size_t len = strlen(line); while (len && isspace((unsigned char)line[len-1])) line[--len] = 0;
    if (!strcmp(line, "HELP")) help();
    else if (!strcmp(line, "STATUS")) summary();
    else if (!strcmp(line, "SUM") || !strcmp(line, "CRC")) {
      if (active) Serial.println("# ERRO: encerre a sessao antes de trocar a tecnica.");
      else { technique = !strcmp(line, "SUM") ? RxProtocol::SUM : RxProtocol::CRC;
        Serial.printf("# TECNICA,%s\n", RxProtocol::name(technique)); }
    } else if (!strcmp(line, "START")) start(1000);
    else if (!strncmp(line, "START ", 6)) {
      char *end = nullptr; unsigned long n = strtoul(line + 6, &end, 10);
      if (end == line + 6 || *end || n < 1 || n > 1000) Serial.println("# ERRO: use START [1..1000].");
      else start(uint16_t(n));
    } else if (!strcmp(line, "END")) {
      if (!active) Serial.println("# ERRO: nenhuma sessao ativa.");
      else {
        endRequested = true;
        finalizeAfterUs = started ? lastSofUs + RxProtocol::PERIOD_US + RxProtocol::GAP_TIMEOUT_US : RxProtocol::now();
        Serial.println("# END: confirmacao de que TX terminou N envios; aguardando prazo final.");
      }
    } else if (!strcmp(line, "STOP")) finish(true);
    else if (!strcmp(line, "BENCH")) bench();
    else if (*line) Serial.println("# ERRO: comando desconhecido; use HELP.");
  }
  void readCommands() {
    while (Serial.available()) {
      int value = Serial.read(); if (value < 0) break;
      char c = char(value);
      if (c == '\r' || c == '\n') {
        if (commandOverflow) Serial.println("# ERRO: comando longo demais.");
        else if (commandLength) { command[commandLength] = 0; execute(command); }
        commandLength = 0; commandOverflow = false;
      } else if (commandLength + 1 < sizeof(command)) command[commandLength++] = c;
      else commandOverflow = true;
    }
  }
  void tick() {
    readCommands();
    handleUartError(RxProtocol::now(), uartErrors.exchange(0, std::memory_order_relaxed));
    while (Serial2.available()) {
      const int value = Serial2.read(); if (value < 0) break;
      feed(uint8_t(value), RxProtocol::now());
    }
    handleUartError(RxProtocol::now(), uartErrors.exchange(0, std::memory_order_relaxed));
    poll(RxProtocol::now());
  }
};

Receiver receiver;
void setup() { receiver.begin(); }
void loop() { receiver.tick(); delay(1); }
