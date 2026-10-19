/*
 * TX - Telemetria ESP32, protocolo 1.2, etapa SEM injetor de ruido.
 * Alvo: ESP32-WROOM-32 / ESP32 Dev Module / Arduino-ESP32 3.3.12.
 * UART2: RX=16, TX=17, 115200, 8N1. USB: Monitor Serial a 115200.
 * Comandos com final de linha: HELP, SUM, CRC, START [1..1000], STOP,
 * STATUS, BENCH. START sozinho transmite os 1000 quadros oficiais.
 * Configure a MESMA tecnica no TX e no RX antes de iniciar.
 */
#include <Arduino.h>
#include <esp_timer.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

class TxProtocol {
 public:
  enum Technique : uint8_t { SUM, CRC };
  static constexpr uint32_t BAUD = 115200;
  static constexpr uint32_t PERIOD_US = 50000;
  static constexpr uint32_t GUARD_US = 2000;
  static constexpr uint16_t MAX_FRAMES = 1000;

  // Volatile impede que o compilador elimine os calculos do BENCH.
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
  static uint8_t makeCheck(const uint8_t *data, Technique kind) {
    const uint8_t result = check(data, 5, kind);
    return kind == SUM ? uint8_t(0u - result) : result;
  }
  static const char *name(Technique kind) { return kind == SUM ? "SUM" : "CRC8"; }
  static uint64_t now() { return uint64_t(esp_timer_get_time()); }
};

class Transmitter {
 public:
  TxProtocol::Technique technique = TxProtocol::SUM;
  bool running = false;
  bool waitingReference = false;
  bool stopRequested = false;
  uint16_t target = 1000, sent = 0;
  uint32_t session = 0;
  uint32_t randomState = 0xC0FFEE01u;
  uint64_t nextFrameAt = 0, referenceAt = 0, frameAt = 0, previousFrameAt = 0;
  uint32_t calculationUs = 0;
  uint8_t frame[8] = {}, reference[7] = {};
  char command[64] = {};
  size_t commandLength = 0;
  bool commandOverflow = false;
  volatile uint8_t benchSink = 0;

  void begin() {
    Serial.begin(TxProtocol::BAUD);
    Serial2.begin(TxProtocol::BAUD, SERIAL_8N1, 16, 17);
    Serial.println("# TX pronto: GPIO17 -> RX GPIO16; GND comum.");
    help();
  }
  void help() {
    Serial.println("# HELP SUM CRC START [1..1000] STOP STATUS BENCH");
    Serial.println("# Prepare RX com START; depois use START no TX. Padrao: SUM e 1000.");
  }
  void status() {
    Serial.printf("# STATUS,TX,tecnica=%s,ativo=%u,enviados=%u,alvo=%u\n",
                  TxProtocol::name(technique), unsigned(running), unsigned(sent), unsigned(target));
  }
  uint8_t nextPayloadByte() {
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;
    return uint8_t(randomState);
  }
  void start(uint16_t count) {
    if (running) { Serial.println("# ERRO: sessao em andamento."); return; }
    if (count == 0 || count > TxProtocol::MAX_FRAMES) {
      Serial.println("# ERRO: quantidade deve estar entre 1 e 1000."); return;
    }
    target = count; sent = 0; ++session;
    randomState = 0xC0FFEE01u; previousFrameAt = 0;
    waitingReference = false; stopRequested = false; running = true;
    nextFrameAt = TxProtocol::now() + 100000; // Tempo para drenar o cabecalho USB.
    Serial.printf("# START,TX,sessao=%lu,tecnica=%s,N=%u,periodo_us=50000,overhead=6\n",
                  (unsigned long)session, TxProtocol::name(technique), unsigned(target));
    Serial.println("sessao,tecnica,seq,t_inicio_us,intervalo_us,payload_hex,quadro_hex,calculo_us");
  }
  void transmitFrame() {
    frame[0] = 0xA5; frame[1] = 0x5A; frame[2] = 2;
    frame[3] = uint8_t(sent >> 8); frame[4] = uint8_t(sent);
    frame[5] = nextPayloadByte(); frame[6] = nextPayloadByte();
    const uint64_t t = TxProtocol::now();
    frame[7] = TxProtocol::makeCheck(frame + 2, technique);
    calculationUs = uint32_t(TxProtocol::now() - t);
    reference[0] = 0xD3; reference[1] = 0x91;
    reference[2] = frame[3]; reference[3] = frame[4]; reference[4] = 2;
    reference[5] = frame[5]; reference[6] = frame[6];
    frameAt = TxProtocol::now();
    if (Serial2.write(frame, sizeof(frame)) != sizeof(frame)) {
      Serial.println("# ERRO: quadro nao foi enfileirado integralmente.");
      running = false; return;
    }
    Serial2.flush(true); // Aguarda o fim fisico; nao limpa RX.
    referenceAt = TxProtocol::now() + TxProtocol::GUARD_US;
    nextFrameAt = frameAt + TxProtocol::PERIOD_US;
    waitingReference = true;
  }
  void transmitReference() {
    if (Serial2.write(reference, sizeof(reference)) != sizeof(reference)) {
      Serial.println("# ERRO: referencia nao foi enfileirada integralmente.");
      running = false; waitingReference = false; return;
    }
    Serial2.flush(true);
    char frameHex[17], interval[24];
    for (uint8_t i = 0; i < 8; ++i) snprintf(frameHex + 2*i, 3, "%02X", unsigned(frame[i]));
    if (sent == 0) strcpy(interval, "NA");
    else snprintf(interval, sizeof(interval), "%llu", (unsigned long long)(frameAt - previousFrameAt));
    Serial.printf("%lu,%s,%u,%llu,%s,%02X%02X,%s,%lu\n", (unsigned long)session,
                  TxProtocol::name(technique), unsigned(sent), (unsigned long long)frameAt,
                  interval, unsigned(frame[5]), unsigned(frame[6]), frameHex,
                  (unsigned long)calculationUs);
    previousFrameAt = frameAt; ++sent; waitingReference = false;
    if (sent == target || stopRequested) {
      running = false;
      Serial.printf("# %s,TX,sessao=%lu,enviados=%u\n", sent == target ? "DONE" : "ABORT",
                    (unsigned long)session, unsigned(sent));
    }
  }
  void bench() {
    if (running) { Serial.println("# ERRO: BENCH somente fora da sessao."); return; }
    volatile uint8_t bytes[6] = {2, 0, 1, 0, 200, 53};
    constexpr uint32_t repetitions = 10000;
    const uint64_t t = TxProtocol::now();
    for (uint32_t i = 0; i < repetitions; ++i)
      benchSink = uint8_t(benchSink ^ TxProtocol::check(bytes, 5, technique));
    const uint64_t elapsed = TxProtocol::now() - t;
    Serial.printf("# BENCH,TX,%s,repeticoes=%lu,tempo_total_us=%llu,us_por_calculo=%.4f\n",
                  TxProtocol::name(technique), (unsigned long)repetitions,
                  (unsigned long long)elapsed, double(elapsed)/repetitions);
  }
  void execute(char *line) {
    while (*line == ' ' || *line == '\t') ++line;
    for (char *p = line; *p; ++p) *p = char(toupper((unsigned char)*p));
    size_t length = strlen(line);
    while (length && isspace((unsigned char)line[length-1])) line[--length] = 0;
    if (!strcmp(line, "HELP")) help();
    else if (!strcmp(line, "STATUS")) status();
    else if (!strcmp(line, "SUM") || !strcmp(line, "CRC")) {
      if (running) Serial.println("# ERRO: pare a sessao antes de mudar a tecnica.");
      else { technique = !strcmp(line, "SUM") ? TxProtocol::SUM : TxProtocol::CRC; status(); }
    } else if (!strcmp(line, "START")) start(1000);
    else if (!strncmp(line, "START ", 6)) {
      char *end = nullptr; unsigned long n = strtoul(line + 6, &end, 10);
      if (end == line + 6 || *end || n < 1 || n > 1000) Serial.println("# ERRO: use START [1..1000].");
      else start(uint16_t(n));
    } else if (!strcmp(line, "STOP")) {
      if (running && waitingReference) stopRequested = true; // Completa a referencia do envio atual.
      else { running = false; Serial.printf("# ABORT,TX,enviados=%u\n", unsigned(sent)); }
    } else if (!strcmp(line, "BENCH")) bench();
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
    if (!running) return;
    const uint64_t t = TxProtocol::now();
    if (waitingReference) { if (t >= referenceAt) transmitReference(); }
    else if (t >= nextFrameAt) transmitFrame();
  }
};

Transmitter transmitter;
void setup() { transmitter.begin(); }
void loop() { transmitter.tick(); delay(1); }
