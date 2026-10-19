/*
 * Injetor v1.12-sincronizado para TX/RX protocolo 1.2, ESP32 classico, core 3.3.12.
 * GPIO25 -> 1k -> base; base -> 10k -> GND; emissor -> GND;
 * TX17 -> 1k -> RX16 + coletor. GPIO26 antes desse resistor;
 * GPIO27 depois. Todos os GNDs comuns. Nao liga os 3V3 entre placas.
 * v1.12: reverte MarkerCapture da v1.11 (que deslocava a 15a borda para -21 ticks);
 * le GPIO26 na entrada da ISR antes de capturar TIMERG0+CCOUNT em todas as bordas;
 * sincroniza o SysTick de 1 kHz do Core 0 (CCOMPARE0) para +850 us apos o SOF
 * (dentro da guarda ociosa de 2 ms), evitando que xPortSysTickHandler (RSIL 3)
 * colida com o marcador A5 5A ou com os pulsos; silencia o loop do Core 1 no
 * barramento APB/filas enquanto aguarda e captura o SOF; recupera atraso isolado
 * tambem na borda 0 (ajustando first/firstCycle se confirmado pelas bordas
 * seguintes) e na penultima borda (confirmada pela ultima borda estrita).
 * v1.10: uma ISR por quadro; eventos pelo CCOUNT local, sem rearme entre bordas.
 * CPU fixa em 240 MHz; nao le contador APB durante os pulsos.
 * v1.9: confirma uma borda interna atrasada pelas bordas seguintes; nunca move SOF.
 * v1.8: recupera associacao apos SOF perdido usando a contagem da UART limpa.
 * Falhas continuam invalidando a rodada; recuperacao nao equivale a estabilidade.
 * CAL, SELFTEST, MODE 0..3, SEED n, START [1..1000], END, STOP, STATUS, HELP.
 * Fase inicial -55 ticks, baseada nas fases -60/-55/-50 com 40/40 nos logs.
 * CAL opcional: mede janela util por bytes recebidos; nao gera resultados experimentais.
 * UART1 e UART2 sao apenas observadores de entrada, sem pinos TX.
 * Timer Group 0 / Timer 0, 10 MHz, ISR propria em IRAM no Core 0 (isolado do loop/UARTs).
 * Planos preparados antes de START; a copia D3 91 fica intacta.
 * A coluna xor_hex e MEDIDA pelos dois receptores UART; nao e a mascara
 * planejada. qualidade=FALHA ou RESUMO valido=NAO invalida a rodada.
 * Compilar e testar MODE 0 antes de MODE 1. Validacao fisica indispensavel.
 */
#ifndef NOISE_HOST_TEST
#include <Arduino.h>
#include <atomic>
#include <esp_timer.h>
#include <esp_cpu.h>
#include <driver/timer.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <hal/timer_ll.h>
#include <soc/gpio_struct.h>
#include <soc/timer_group_struct.h>
#include <esp_pm.h>
#if !CONFIG_IDF_TARGET_ESP32
#error "Este sketch e para ESP32-WROOM-32 classico; nao C3/S3."
#endif
#define NOISE_FAST IRAM_ATTR
#define NOISE_DATA DRAM_ATTR
#else
#include <stdint.h>
#include <stddef.h>
#define NOISE_FAST
#define NOISE_DATA
#endif
#define NOISE_INLINE inline __attribute__((always_inline))

namespace NoiseLogic {
constexpr int32_t MARKER_TOLERANCE_TICKS = 25; // 2,5 us; igual ao ajuste da bancada.
constexpr uint32_t MAX_EVENT_LATE_TICKS = 20; // 2 us; nao ampliar para mascarar atraso.
constexpr uint32_t ALARM_ADVANCE_TICKS = 60; // IRQ 6,0 us antes do instante desejado.
constexpr uint32_t MAX_EVENT_EARLY_TICKS = 5; // Ate 0,5 us cedo, dentro da janela do bit.
NOISE_INLINE uint32_t elapsedCycleTicks(uint32_t current, uint32_t first) {
  return uint32_t(current-first)/24u;
}
NOISE_INLINE uint64_t alarmAt(uint64_t desired) {
  return desired - ALARM_ADVANCE_TICKS;
}
NOISE_INLINE bool eventInWindow(uint64_t stamp, uint64_t desired) {
  return stamp < desired ? desired - stamp <= MAX_EVENT_EARLY_TICKS :
                          stamp - desired <= MAX_EVENT_LATE_TICKS;
}
// 10 MHz: um tick = 0,1 us. Um decimo de bit = 625/72 ticks.
NOISE_INLINE uint16_t ticks(uint32_t tenths) {
  return uint16_t((tenths * 625u + 36u) / 72u);
}
NOISE_INLINE uint32_t nextRandom(uint32_t &s) {
  s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s;
}
struct Plan { uint8_t mask[8] = {}; uint16_t event[4] = {}; uint8_t n = 0; };
void compileEvents(Plan &p) {
  p.n = 0;
  // Agrupa bits adjacentes do MESMO byte. Nunca inclui start/stop da UART.
  for (uint8_t byte = 2; byte < 8; ++byte) {
    for (uint8_t bit = 0; bit < 8;) {
      if (!(p.mask[byte] & (1u << bit))) { ++bit; continue; }
      uint8_t first = bit;
      while (bit < 8 && (p.mask[byte] & (1u << bit))) ++bit;
      // ON 0,1 bit depois da borda; OFF 0,3 bit antes da borda seguinte.
      // Pulso isolado ~5,2 us; rajada de 8 bits ~66 us, menor que um byte.
      p.event[p.n++] = ticks((byte * 10u + 1u + first) * 10u + 1u);
      p.event[p.n++] = ticks((byte * 10u + 1u + bit) * 10u - 3u);
    }
  }
}
Plan makePlan(uint8_t mode, uint32_t &seed) {
  Plan p;
  if (!mode) return p;
  if (mode == 1 || mode == 2) {
    uint32_t first = nextRandom(seed) % 48u;
    p.mask[2 + first / 8] |= uint8_t(1u << (first % 8));
    if (mode == 2) {
      // Amostra entre as outras 47 posicoes: sem repeticao e sem retry.
      uint32_t second = nextRandom(seed) % 47u;
      if (second >= first) ++second;
      p.mask[2 + second / 8] |= uint8_t(1u << (second % 8));
    }
  } else if (mode == 3) {
    uint32_t byte = 2 + nextRandom(seed) % 6u;
    uint32_t length = 3 + nextRandom(seed) % 6u;
    uint32_t start = nextRandom(seed) % (9u - length);
    p.mask[byte] = uint8_t(((1u << length) - 1u) << start);
  }
  compileEvents(p);
  return p;
}
// Calibracao separada dos experimentos: 16 fases, 40 quadros por fase.
constexpr uint8_t CAL_PHASES = 16, CAL_REPEATS = 40;
constexpr uint16_t CAL_FRAMES = CAL_PHASES * CAL_REPEATS;
constexpr int16_t CAL_MIN = -60, CAL_STEP = 5; // -6,0 a +1,5 us.
constexpr int16_t BENCH_PHASE_TICKS = -55; // Fase medida na bancada: -5,5 us.
// CAL deve invalidar o pulso/fase, mas continuar no proximo quadro.
// Uma falha invalida o quadro e a rodada; o proximo quadro ainda pode ser auditado.
NOISE_INLINE bool disableSessionAfterTiming(bool) { return false; }
// O indice vem dos quadros limpos completos, nao do numero de marcadores vistos.
// UARTs continuam obrigatorias: erro/perda nelas continua invalidando a rodada.
NOISE_INLINE int frameSlot(uint32_t observed, uint32_t limit, bool alreadyPlanned) {
  return observed < limit && !alreadyPlanned ? int(observed) : -1;
}
struct CalBin {
  uint16_t frames=0, exact=0, eligible=0;
  uint32_t faults=0, maxLate=0, changes=0;
};
constexpr int16_t phaseAt(uint8_t i) { return CAL_MIN + CAL_STEP * i; }
void shiftPlan(Plan &p, int16_t phase) {
  for (uint8_t e=0;e<p.n;++e) p.event[e]=uint16_t(int32_t(p.event[e])+phase);
}
Plan calibrationPlan(uint16_t frame) {
  Plan p;
  // Exercita ambos os bytes de payload e todos os bits. Nao conhece o
  // payload antecipadamente; verifica o efeito depois de receber as UARTs.
  uint8_t sample=frame % CAL_REPEATS;
  p.mask[5 + (sample/8)%2] = uint8_t(1u << (sample%8));
  compileEvents(p);
  shiftPlan(p,phaseAt(frame/CAL_REPEATS));
  return p;
}
// Exige 40/40 exatos por fase e >=12 alvos originalmente 1. Seleciona
// o centro da maior janela com >=3 fases consecutivas (largura >=1 us).
int selectCalibration(const CalBin *bins) {
  int bestStart=-1,bestLength=0,start=0,length=0;
  for (int i=0;i<=CAL_PHASES;++i) {
    bool good=i<CAL_PHASES && bins[i].frames==CAL_REPEATS &&
              bins[i].exact==CAL_REPEATS && bins[i].eligible>=12;
    if(good) { if(!length)start=i; ++length; }
    else { if(length>bestLength){bestStart=start;bestLength=length;} length=0; }
  }
  return bestLength>=3 ? bestStart+(bestLength-1)/2 : -1;
}
// Bordas da UART 8N1, LSB primeiro, para A5 5A, apos a queda inicial.
NOISE_DATA static uint8_t markerEdges[] = {1,2,3,4,6,7,8,10,12,13,14,16,17,18,19};
struct Marker {
  uint64_t first = 0, last = 0;
  uint32_t firstCycle = 0;
  int32_t prevError = 0;
  uint8_t index = 0;
  volatile bool seeking = false;
  bool pendingLate = false, usedLate = false;
  // Um atraso de atendimento de IRQ pode deslocar uma captura isolada.
  // So aceitar UMA borda atrasada ate meio bit (43 ticks), exigindo que
  // as demais bordas e o termino do marcador cumpram o limite estrito de 25 ticks.
  volatile uint32_t recoveredLateEdges = 0;
  volatile uint32_t irqs = 0, bursts = 0, timingRejects = 0, levelRejects = 0;
  volatile uint8_t maxMatched = 0, traceCount = 0;
  volatile int32_t lastError = 0;
  uint64_t traceBase = 0;
  uint32_t trace[16] = {};
  uint16_t traceLevels = 0;
  NOISE_FAST bool edge(uint64_t now, bool high, uint32_t cycle = 0) {
    irqs = irqs + 1u;
    uint64_t gap = now - last; last = now;
    if (gap >= 10000u) { // 1 ms de silencio: separa quadro e depuracao.
      bursts = bursts + 1u;
      seeking = !high; index = 0; first = now; firstCycle = cycle;
      prevError = 0;
      pendingLate = false; usedLate = false;
      if(bursts == 1u) traceBase = now;
    }
    if(bursts && traceCount < 16u) {
      uint8_t t = traceCount;
      trace[t] = uint32_t(now - traceBase);
      if(high) traceLevels |= uint16_t(1u << t);
      traceCount = uint8_t(t + 1u);
    }
    if (gap >= 10000u) return false;
    if (!seeking) return false;
    uint32_t expected = ticks(markerEdges[index] * 10u);
    int32_t error = int32_t(now - first) - int32_t(expected);
    bool level = !(index & 1u);
    // Se a borda 0 (queda inicial do start bit) sofreu atraso de IRQ, os pares de
    // bordas seguintes aparecem deslocados negativamente na mesma proporcao, mas
    // preservam o intervalo de 1 bit entre si. Restaura a origem real de first.
    if (high == level && (index == 1u || index == 3u) &&
        (!usedLate || (index == 1u && prevError < -MARKER_TOLERANCE_TICKS))) {
      const int32_t threshold = index == 1u ? -11 : -12;
      const int32_t nominal = index == 1u ? -4 : -8;
      if (error <= threshold && prevError <= threshold) {
        int32_t stepError = error - prevError;
        int32_t shift = nominal - error;
        if (shift > int32_t(ticks(5))) shift = int32_t(ticks(5));
        if (stepError >= -12 && stepError <= 12 && shift > 0) {
          first -= uint64_t(shift);
          firstCycle -= uint32_t(shift) * 24u;
          error += shift;
          pendingLate = false;
          usedLate = true;
        }
      }
    }
    const bool strict = error >= -MARKER_TOLERANCE_TICKS && error <= MARKER_TOLERANCE_TICKS;
    // Nao ignorar bordas ausentes, niveis errados ou atrasos em sequencia.
    // Nao aplicar pulso antes de confirmar todas as 15 transicoes.
    const bool lateStart = high == level && index == 0u && !strict && !usedLate &&
      now > first && error < -MARKER_TOLERANCE_TICKS &&
      error >= -MARKER_TOLERANCE_TICKS - int32_t(ticks(5));
    const bool provisional = high == level && !strict && !usedLate && !pendingLate &&
      index < sizeof(markerEdges)-1 && error > MARKER_TOLERANCE_TICKS &&
      error <= int32_t(ticks(5));
    if (high != level || (!strict && !provisional && !lateStart)) {
      lastError = error;
      if(high != level) levelRejects = levelRejects + 1u;
      else timingRejects = timingRejects + 1u;
      seeking = false; return false;
    }
    prevError = error;
    if (pendingLate) pendingLate = false; // Esta borda confirmou o intervalo absoluto.
    if (provisional || lateStart) { pendingLate = true; usedLate = true; }
    ++index;
    if(index > maxMatched) maxMatched = index;
    if (index == sizeof(markerEdges)) {
      if(usedLate) recoveredLateEdges = recoveredLateEdges + 1u;
      seeking = false; return true;
    }
    return false;
  }
};
NOISE_INLINE uint8_t countBits(uint8_t x) {
  uint8_t n = 0; while (x) { x &= uint8_t(x - 1u); ++n; } return n;
}
// Cada alvo originalmente 1 precisa ser observado 0. Alvos ja 0 sao validos
// sem mudanca. Manter a mascara planejada separada do XOR medido.
NOISE_INLINE uint8_t missedTargets(uint8_t clean, uint8_t dirty, uint8_t mask) {
  return uint8_t(clean & dirty & mask);
}
enum PulseResult : uint8_t { NEXT_EVENT, PULSE_COMPLETE, PULSE_FAILED };
// Mesma rotina executada na placa e nos testes com relogio/GPIO adaptados.
// IO nao imprime nem usa APIs de driver no caminho critico da placa.
template<class State, class IO>
NOISE_INLINE PulseResult pulseStep(State &r, uint8_t &cursor, IO &io) {
  const uint8_t e = cursor;
  if (e >= r.plan.n) { io.set(false); return PULSE_FAILED; }
  const uint64_t deadline = r.firstTicks + r.plan.event[e];
  uint64_t stamp = io.now();
  // Aguarda o instante planejado; a entrada antecipada da ISR nao antecipa o pulso.
  for (uint32_t spin = 0; stamp < deadline && spin < 200000u; ++spin) {
    stamp = io.now();
  }
  uint32_t late = stamp > deadline ? uint32_t(stamp - deadline) : 0u;
  if (late > r.maxLate) r.maxLate = late;
  if (!eventInWindow(stamp, deadline)) {
    io.set(false); // OFF imediato tambem se o evento de desligamento atrasar.
    if (e & 1u) {
      stamp = io.now(); r.actual[e] = uint32_t(stamp - r.firstTicks);
      late = stamp > deadline ? uint32_t(stamp - deadline) : 0u;
      if (late > r.maxLate) r.maxLate = late;
    }
    return PULSE_FAILED;
  }
  io.set(!(e & 1u));
  stamp = io.now(); // Limite superior de software, logo apos escrever GPIO.
  r.actual[e] = uint32_t(stamp - r.firstTicks);
  late = stamp > deadline ? uint32_t(stamp - deadline) : 0u;
  if (late > r.maxLate) r.maxLate = late;
  if (!eventInWindow(stamp, deadline)) { io.set(false); return PULSE_FAILED; }
  cursor = uint8_t(e + 1u);
  if (cursor == r.plan.n) { io.set(false); return PULSE_COMPLETE; }
  if (!io.arm(alarmAt(r.firstTicks + r.plan.event[cursor]))) {
    io.set(false); return PULSE_FAILED;
  }
  return NEXT_EVENT;
}
} // namespace NoiseLogic

#ifndef NOISE_HOST_TEST
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

constexpr uint16_t LIMIT = 1000;
constexpr uint8_t PLAN = 1, DONE = 2, CLEAN = 4, DIRTY = 8, CREF = 16, DREF = 32;
constexpr uint8_t COMPLETE = PLAN | DONE | CLEAN | DIRTY | CREF | DREF;
enum Fault : uint32_t { NO_MARKER=1, TIMING=2, UART_FAULT=4, ORIGINAL=8,
                       EXTRA_CHANGE=16, REFERENCE_CHANGED=32, MISSING=64,
                       TARGET_NOT_LOW=128, INJECTION_DISABLED=256 };
struct Record {
  NoiseLogic::Plan plan;
  uint64_t firstTicks = 0;
  uint32_t actual[4] = {}, maxLate = 0, receivedUs = 0;
  uint8_t clean[8] = {}, dirty[8] = {}, cref[7] = {}, dref[7] = {};
  std::atomic<uint32_t> present{0}; // RMW nativo de 32 bits nas ISRs do Xtensa.
  std::atomic<uint32_t> faults{0};
};
Record records[LIMIT];
timer_isr_handle_t timerInterrupt = nullptr;
gpio_isr_handle_t gpioInterrupt = nullptr;
portMUX_TYPE clockMux = portMUX_INITIALIZER_UNLOCKED;
#if CONFIG_PM_ENABLE
esp_pm_lock_handle_t apbLock = nullptr;
esp_pm_lock_handle_t cpuLock = nullptr;
#endif
QueueHandle_t uartQueues[2] = {};
// 32 bits: evita std::atomic<bool>::load fora de linha em Flash no core 3.3.12.
std::atomic<uint32_t> running{0}, injecting{0};
std::atomic<uint32_t> seen{0};
std::atomic<uint32_t> cleanFrames{0}; // Publicado pela UART limpa antes do proximo quadro.
std::atomic<uint32_t> uartErrors{0};
uint8_t chosenMode = 0;
uint16_t target = 1000, logged = 0, good = 0, bad = 0;
uint32_t chosenSeed = 1, session = 0;
uint32_t changedTotal = 0, selectedTotal = 0;
uint32_t aggregateFaults = 0;
uint32_t sessionMaxLate = 0;
// Fase inicial recuperada das 3 fases 40/40 dos logs enviados pelo usuario.
// Isso disponibiliza o ajuste; nao declara a nova rodada como validada.
bool calibrating=false, phaseReady=true, phaseFromScan=false;
uint64_t calibrationDeadline=0;
int16_t phaseTicks=NoiseLogic::BENCH_PHASE_TICKS;
NoiseLogic::CalBin calBins[NoiseLogic::CAL_PHASES];
NoiseLogic::Marker marker;
std::atomic<int32_t> activeRecord{-1};
uint32_t detectorInterruptMask = 0; // Mascara configurada pelo driver no setup.
std::atomic<uint32_t> detectorPaused{0};
uint32_t pulseFirstCycle = 0; // Epoca do unico quadro ativo; nao ocupa 1000 registros.
uint8_t eventCursor = 0; // Somente nas duas ISRs, no mesmo core.
uint32_t lastCompleteCycle = 0, lastIdlePollCycle = 0;
uint32_t burstsAtResume = 0, lastSeenBursts = 0, unhandledBurstCycle = 0;
uint64_t endAtUs = 0;
char command[64] = {};
uint8_t commandLength = 0;
bool overflow = false;

// No Core 0, desloca o proximo SysTick de 1 kHz (CCOMPARE0) para +850 us apos
// o inicio do SOF. O tick do FreeRTOS passa a cair sempre na guarda ociosa de
// 2 ms (apos o quadro de 8 bytes e os pulsos terminarem em ~694 us), evitando
// que xPortSysTickHandler (RSIL 3) atrase as bordas do SOF ou os pulsos.
NOISE_INLINE uint32_t getCCompare0() {
  uint32_t value = 0;
#ifdef __XTENSA__
  asm volatile("rsr %0, CCOMPARE0" : "=r"(value));
#endif
  return value;
}
NOISE_INLINE void setCCompare0(uint32_t value) {
#ifdef __XTENSA__
  asm volatile("wsr %0, CCOMPARE0\n"
               "esync\n" :: "r"(value));
#else
  (void)value;
#endif
}
NOISE_INLINE void deferSysTickFromSof(uint32_t anchorCycle) {
  setCCompare0(anchorCycle + 204000u); // 850 us a 240 MHz
}
NOISE_INLINE void markFrameHandled() {
  const uint32_t c = esp_cpu_get_cycle_count();
  lastCompleteCycle = c;
  lastIdlePollCycle = c;
  burstsAtResume = marker.bursts;
  lastSeenBursts = burstsAtResume;
  unhandledBurstCycle = c;
}

// O marcador ja foi identificado: bordas do payload nao sao necessarias
// para o planejamento e a leitura clockNow() dessas ISRs pode atrasar o timer.
// Pausa somente a IRQ GPIO26; as duas UARTs continuam observando os dados.
NOISE_INLINE void pauseDetector() {
  GPIO.pin[26].int_ena = 0;
  GPIO.status_w1tc = 1u << 26;
  detectorPaused.store(1, std::memory_order_release);
}
NOISE_INLINE void resumeDetector() {
  if(!detectorPaused.load(std::memory_order_acquire)) return;
  GPIO.status_w1tc = 1u << 26;
  detectorPaused.store(0, std::memory_order_release);
  GPIO.pin[26].int_ena = detectorInterruptMask;
}
NOISE_INLINE void controlLow() { GPIO.out_w1tc = 1u << 25; }
NOISE_INLINE void controlHigh() { GPIO.out_w1ts = 1u << 25; }
// Leitura APB usada apenas em configuracao/agendamento, fora do trem de pulsos.
NOISE_INLINE uint64_t pulseClockNow() {
  timer_ll_trigger_soft_capture(&TIMERG0, 0);
  return timer_ll_get_counter_value(&TIMERG0, 0);
}
// No GPIO nivel 2, preservar captura/leitura contra preempcao pelo timer.
NOISE_FAST uint64_t clockNow() {
  portENTER_CRITICAL_ISR(&clockMux);
  const uint64_t value = pulseClockNow();
  portEXIT_CRITICAL_ISR(&clockMux);
  return value;
}
NOISE_INLINE void disableAlarm() {
  timer_ll_enable_alarm(&TIMERG0, 0, false);
  timer_ll_clear_intr_status(&TIMERG0, TIMER_LL_EVENT_ALARM(0));
}
NOISE_FAST bool schedule(uint64_t value) {
  const uint64_t now = clockNow();
  const uint64_t targetTick = value > now + 6u ? value : now + 6u;
  timer_ll_set_alarm_value(&TIMERG0, 0, targetTick);
  timer_ll_enable_alarm(&TIMERG0, 0, true);
  return true;
}
NOISE_FAST void failPulse(Record &r) {
  controlLow();
  if(NoiseLogic::disableSessionAfterTiming(calibrating))
    injecting.store(false, std::memory_order_relaxed);
  r.faults.fetch_or(TIMING, std::memory_order_relaxed);
  r.present.fetch_or(DONE, std::memory_order_release);
  activeRecord = -1;
  disableAlarm();
}
// 240 MHz / 10 MHz = 24 ciclos por tick de 0,1 us.
// Subtracao uint32_t trata wrap do CCOUNT; cada captura dura menos de 1 ms.
struct PulseIO {
  Record &r;
  const uint32_t firstCycle;
  NOISE_INLINE uint64_t now() {
    return r.firstTicks + NoiseLogic::elapsedCycleTicks(esp_cpu_get_cycle_count(), firstCycle);
  }
  NOISE_INLINE void set(bool high) {
    if (high) controlHigh(); else controlLow();
    asm volatile("memw" ::: "memory"); // concluir escrita APB antes da captura CCOUNT
  }
  // Os eventos seguintes ficam na mesma ISR. Sem escrita/leitura no timer aqui.
  NOISE_INLINE bool arm(uint64_t) { return true; }
};
NOISE_FAST void alarmCallback(void *) {
  disableAlarm(); // Uma unica entrada por quadro, sem rearme entre bordas.
  const int id = activeRecord.load(std::memory_order_relaxed);
  if (id < 0 || !running.load(std::memory_order_relaxed)) { controlLow(); return; }
  Record &r = records[id];
  PulseIO io{r,pulseFirstCycle};
  NoiseLogic::PulseResult result;
  do {
    result = NoiseLogic::pulseStep(r, eventCursor, io);
  } while(result == NoiseLogic::NEXT_EVENT);
  if (result == NoiseLogic::PULSE_FAILED) failPulse(r);
  else {
    controlLow();
    activeRecord.store(-1, std::memory_order_relaxed);
    r.present.fetch_or(DONE, std::memory_order_release);
  }
}
// Emparelha o contador de hardware com o CCOUNT no core que atende as duas ISRs.
// O CCOUNT e capturado logo apos o comando de captura, antes de ler os registradores.
NOISE_FAST uint64_t markerClockNow(uint32_t &cycle) {
  portENTER_CRITICAL_ISR(&clockMux);
  timer_ll_trigger_soft_capture(&TIMERG0, 0);
  asm volatile("memw" ::: "memory");
  cycle = esp_cpu_get_cycle_count();
  const uint64_t value = timer_ll_get_counter_value(&TIMERG0, 0);
  portEXIT_CRITICAL_ISR(&clockMux);
  return value;
}
NOISE_FAST void edgeCallback(void *) {
  // ISR GPIO dedicada: evita o despacho do servico de handlers por pino.
  if (!(GPIO.status & (1u << 26))) return;
  GPIO.status_w1tc = 1u << 26;
  if (!running.load(std::memory_order_relaxed)) return;
  const bool high = (GPIO.in & (1u << 26)) != 0;
  uint32_t cycle;
  const uint64_t now = markerClockNow(cycle);
  if (!high && now - marker.last >= 10000u) {
    if (uint32_t(getCCompare0() - cycle) < 192000u) deferSysTickFromSof(cycle);
  }
  if (!marker.edge(now, high, cycle)) return;
  deferSysTickFromSof(marker.firstCycle);
  const uint32_t observed = cleanFrames.load(std::memory_order_acquire);
  if (observed >= target) return;
  const int slot = NoiseLogic::frameSlot(observed, target,
      (records[observed].present.load(std::memory_order_acquire) & PLAN) != 0);
  if (slot < 0) return; // Nunca aplicar um segundo plano no mesmo registro.
  const uint16_t id = uint16_t(slot);
  pauseDetector();
  seen.fetch_add(1, std::memory_order_relaxed);
  Record &r = records[id];
  r.firstTicks = marker.first;
  pulseFirstCycle = marker.firstCycle;
  if (!injecting.load(std::memory_order_relaxed) && chosenMode) {
    r.faults.fetch_or(INJECTION_DISABLED, std::memory_order_relaxed);
    r.present.fetch_or(PLAN | DONE, std::memory_order_release); return;
  }
  r.present.fetch_or(PLAN, std::memory_order_release);
  if (!r.plan.n) { r.present.fetch_or(DONE, std::memory_order_release); return; }
  if (activeRecord >= 0) {
    failPulse(records[activeRecord]);
    r.faults.fetch_or(TIMING, std::memory_order_relaxed);
    r.present.fetch_or(PLAN | DONE, std::memory_order_release); return;
  }
  activeRecord = id; eventCursor = 0;
  if (!schedule(NoiseLogic::alarmAt(r.firstTicks + r.plan.event[0]))) failPulse(r);
}

// Observador: tamanho fixo 8 para auditar o fio. Nao substitui o parser do RX.
// Mesmo se LEN sofrer ruido, captura os 8 bytes fisicamente recebidos.
struct ObservedStream {
  uint8_t bytes[8] = {}, n = 0, wanted = 0, previous = 0;
  bool ref = false;
  uint16_t frames = 0;
  uint64_t lastRead = 0;
  void reset() { n=0; wanted=0; previous=0; ref=false; frames=0; lastRead=0; }
  void feed(uint8_t b, uint8_t lane) {
    uint64_t now = uint64_t(esp_timer_get_time());
    if (n && now - lastRead > 10000u) { n=0; previous=0; }
    lastRead=now;
    if (!n) {
      if ((previous==0xA5 && b==0x5A) || (previous==0xD3 && b==0x91)) {
        ref = previous==0xD3; wanted=ref ? 7 : 8;
        bytes[0]=previous; bytes[1]=b; n=2; previous=0;
      } else previous=b;
      return;
    }
    bytes[n++]=b;
    if (n != wanted) return;
    n=0; previous=0;
    if (!ref) {
      uint16_t id=frames++;
      if (id>=target) { uartErrors.fetch_add(1); return; }
      Record &r=records[id];
      memcpy(lane ? r.dirty : r.clean,bytes,8);
      if (!lane) r.receivedUs=uint32_t(now);
      r.present.fetch_or(lane ? DIRTY : CLEAN,std::memory_order_release);
      if (!lane) cleanFrames.store(uint32_t(id) + 1u, std::memory_order_release);
    } else {
      if (!frames || frames>target) { uartErrors.fetch_add(1); return; }
      Record &r=records[frames-1];
      memcpy(lane ? r.dref : r.cref,bytes,7);
      r.present.fetch_or(lane ? DREF : CREF,std::memory_order_release);
    }
  }
} streams[2];

void setupObserver(uart_port_t port, int rx, QueueHandle_t *queue) {
  uart_config_t c={}; c.baud_rate=115200; c.data_bits=UART_DATA_8_BITS;
  c.parity=UART_PARITY_DISABLE; c.stop_bits=UART_STOP_BITS_1;
  c.flow_ctrl=UART_HW_FLOWCTRL_DISABLE; c.source_clk=UART_SCLK_APB;
  ESP_ERROR_CHECK(uart_driver_install(port,2048,0,32,queue,0));
  ESP_ERROR_CHECK(uart_param_config(port,&c));
  ESP_ERROR_CHECK(uart_set_pin(port,UART_PIN_NO_CHANGE,rx,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
  // Limiar 64 bytes e timeout 10 simbolos (~868 us): evita disparar IRQ das
  // UARTs observadoras no meio do quadro de 8 bytes enquanto o pulso ocorre.
  ESP_ERROR_CHECK(uart_set_rx_full_threshold(port,64));
  ESP_ERROR_CHECK(uart_set_rx_timeout(port,10));
}
void drainInputs() {
  for (uint8_t lane=0; lane<2; ++lane) {
    uart_event_t ev;
    while (xQueueReceive(uartQueues[lane],&ev,0)==pdTRUE) {
      if (running.load() && ev.type != UART_DATA) {
        if (ev.type==UART_FIFO_OVF || ev.type==UART_BUFFER_FULL || ev.type==UART_FRAME_ERR || ev.type==UART_PARITY_ERR || ev.type==UART_BREAK) uartErrors.fetch_add(1);
      }
    }
    uint8_t buffer[128]; int n;
    while ((n=uart_read_bytes(uart_port_t(lane+1),buffer,sizeof(buffer),0))>0)
      if (running.load()) for (int i=0;i<n;++i) streams[lane].feed(buffer[i],lane);
  }
}
void hexBytes(const uint8_t *bytes, size_t n, char *out) {
  for(size_t i=0;i<n;++i) snprintf(out+2*i,3,"%02X",unsigned(bytes[i]));
}
void eventText(const Record &r, bool actual, char *out, size_t capacity) {
  size_t used=0; out[0]=0;
  for(uint8_t i=0;i<r.plan.n;++i) {
    if(actual && !r.actual[i]) used+=snprintf(out+used,capacity-used,"%sNA",i ? ";" : "");
    else used+=snprintf(out+used,capacity-used,"%s%llu",i ? ";" : "",(unsigned long long)(r.firstTicks+(actual ? r.actual[i] : r.plan.event[i])));
  }
  if(!r.plan.n || !(r.present.load(std::memory_order_acquire)&PLAN)) strcpy(out,"NA");
}
void printRecord(uint16_t id, bool force=false) {
  Record &r=records[id]; uint8_t have=r.present.load(std::memory_order_acquire);
  if(!force && have!=COMPLETE) return;
  uint32_t faults=r.faults.load();
  if(!(have&PLAN)) faults|=NO_MARKER;
  if(have!=COMPLETE) faults|=MISSING;
  if(uartErrors.load()) faults|=UART_FAULT;
  if((have&CLEAN) && (r.clean[2]!=2 || ((uint16_t(r.clean[3])<<8)|r.clean[4])!=id)) faults|=ORIGINAL;
  uint8_t changed[8]={}; uint16_t changes=0, selected=0;
  for(uint8_t i=0;i<8;++i) {
    selected+=NoiseLogic::countBits(r.plan.mask[i]);
    if((have&(CLEAN|DIRTY))==(CLEAN|DIRTY)) {
      changed[i]=r.clean[i]^r.dirty[i]; changes+=NoiseLogic::countBits(changed[i]);
      if((changed[i]&uint8_t(~r.plan.mask[i])) || (r.dirty[i]&uint8_t(~r.clean[i]))) faults|=EXTRA_CHANGE;
      if(NoiseLogic::missedTargets(r.clean[i],r.dirty[i],r.plan.mask[i])) faults|=TARGET_NOT_LOW;
    }
  }
  if((have&(CREF|DREF))==(CREF|DREF) && memcmp(r.cref,r.dref,7)) faults|=REFERENCE_CHANGED;
  if((have&(CLEAN|CREF))==(CLEAN|CREF)) {
    const uint8_t expected[7]={0xD3,0x91,r.clean[3],r.clean[4],2,r.clean[5],r.clean[6]};
    if(memcmp(expected,r.cref,7)) faults|=REFERENCE_CHANGED;
  }
  if(calibrating) {
    auto &bin=calBins[id/NoiseLogic::CAL_REPEATS];
    ++bin.frames;
    bin.faults |= faults;
    bin.changes += changes;
    if(r.maxLate>bin.maxLate) bin.maxLate=r.maxLate;
    bool eligible=false;
    if(have&CLEAN) for(uint8_t i=0;i<8;++i)
      eligible |= (r.clean[i]&r.plan.mask[i])!=0;
    if(eligible) ++bin.eligible;
    if(!faults) ++bin.exact;
    if(faults) ++bad; else ++good;
    aggregateFaults |= faults;
    if(r.maxLate>sessionMaxLate) sessionMaxLate=r.maxLate;
    selectedTotal+=selected; changedTotal+=changes; ++logged;
    if((id+1)%NoiseLogic::CAL_REPEATS==0) {
      Serial.printf("# CAL_FASE,phase_ticks=%d,quadros=%u,exatos=%u,alvos_1=%u,alteracoes=%lu,falhas_mask=%lu,atraso_max_ticks=%lu\n",
        int(NoiseLogic::phaseAt(id/NoiseLogic::CAL_REPEATS)),unsigned(bin.frames),
        unsigned(bin.exact),unsigned(bin.eligible),(unsigned long)bin.changes,
        (unsigned long)bin.faults,(unsigned long)bin.maxLate);
    }
    return;
  }
  char original[17]="NA",received[17]="NA",mask[17],actual[17]="NA",cref[15]="NA",dref[15]="NA";
  if(have&CLEAN) hexBytes(r.clean,8,original);
  if(have&DIRTY) hexBytes(r.dirty,8,received);
  if((have&(CLEAN|DIRTY))==(CLEAN|DIRTY)) hexBytes(changed,8,actual);
  hexBytes(r.plan.mask,8,mask);
  if(have&CREF) hexBytes(r.cref,7,cref);
  if(have&DREF) hexBytes(r.dref,7,dref);
  char planned[88],applied[88],seq[12]="NA",time[24]="NA",positions[64]="NA";
  eventText(r,false,planned,sizeof(planned)); eventText(r,true,applied,sizeof(applied));
  if(have&CLEAN) snprintf(seq,sizeof(seq),"%u",unsigned((uint16_t(r.clean[3])<<8)|r.clean[4]));
  if(have&PLAN) snprintf(time,sizeof(time),"%llu",(unsigned long long)(r.firstTicks/10u));
  if(selected) {
    size_t used=0;
    for(uint8_t byte=2;byte<8;++byte) for(uint8_t bit=0;bit<8;++bit) if(r.plan.mask[byte]&(1u<<bit))
      used+=snprintf(positions+used,sizeof(positions)-used,"%s%u.%u",used ? ";" : "",unsigned(byte),unsigned(bit));
  }
  Serial.printf("QUADRO,%lu,%u,%u,%s,%s,%s,%s,%s,%s,%u,%u,%s,%s,%lu,%s,%s,%s,%lu,%s\n",
    (unsigned long)session,unsigned(chosenMode),unsigned(id),seq,time,positions,mask,original,received,
    unsigned(selected),unsigned(changes),actual,planned,(unsigned long)r.maxLate,applied,cref,dref,
    (unsigned long)faults,faults ? "FALHA" : "VALIDO");
  if(faults) ++bad; else ++good;
  aggregateFaults |= faults;
  if(r.maxLate > sessionMaxLate) sessionMaxLate = r.maxLate;
  selectedTotal+=selected; changedTotal+=changes; ++logged;
}
void summarize(const char *reason) {
  running.store(false); injecting.store(false); controlLow();
  disableAlarm(); activeRecord=-1;
  bool valid=bad==0 && logged==target && seen.load()==target && streams[0].frames==target && streams[1].frames==target && uartErrors.load()==0 && !strcmp(reason,"DONE");
  if(seen.load()!=target) aggregateFaults |= NO_MARKER;
  if(logged!=target || streams[0].frames!=target || streams[1].frames!=target || strcmp(reason,"DONE")) aggregateFaults |= MISSING;
  if(uartErrors.load()) aggregateFaults |= UART_FAULT;
  if(calibrating) {
    // TIMING reprova a fase afetada (exatos<40), nao fases independentes.
    const uint32_t fatal=NO_MARKER|UART_FAULT|ORIGINAL|
                         REFERENCE_CHANGED|MISSING|INJECTION_DISABLED;
    int index=NoiseLogic::selectCalibration(calBins);
    bool complete=logged==NoiseLogic::CAL_FRAMES && seen.load()==target &&
      streams[0].frames==target && streams[1].frames==target &&
      uartErrors.load()==0 && !strcmp(reason,"DONE");
    phaseReady=index>=0 && complete && !(aggregateFaults&fatal);
    if(phaseReady) {
      phaseTicks=NoiseLogic::phaseAt(uint8_t(index)); phaseFromScan=true;
    }
    Serial.printf("# CAL_RESULT,%s,phase_ticks=%d,alteracoes=%lu,atraso_max_ticks=%lu,falhas_mask=%lu\n",
      phaseReady ? "SUCESSO" : "FALHA",int(phaseTicks),
      (unsigned long)changedTotal,(unsigned long)sessionMaxLate,(unsigned long)aggregateFaults);
    Serial.println(phaseReady ?
      "# Calibracao em RAM pronta. Agora MODE 1, SEED 1, START 10 na ordem RX/NOISE/TX." :
      "# Nao houve janela confiavel. Nao usar estes dados como experimento. Envie SELFTEST, CAL_FASE e CAL_RESULT.");
    calibrating=false; chosenMode=0; endAtUs=0; return;
  }
  // Resumo tambem no CSV: a evidencia de validade viaja junto com o arquivo.
  Serial.printf("RESUMO,%lu,%u,NA,NA,NA,NA,NA,NA,NA,%lu,%lu,NA,NA,%lu,NA,NA,NA,%lu,%s\n",
    (unsigned long)session,unsigned(chosenMode),(unsigned long)selectedTotal,(unsigned long)changedTotal,
    (unsigned long)sessionMaxLate,(unsigned long)aggregateFaults,valid ? "VALIDO" : "FALHA");
  Serial.printf("# %s,NOISE,N=%u,registrados=%u,SOF=%u,limpos=%u,receptor=%u,validos=%u,falhas=%u,erros_uart=%lu,valido=%s\n",
    reason,unsigned(target),unsigned(logged),unsigned(seen.load()),unsigned(streams[0].frames),unsigned(streams[1].frames),unsigned(good),unsigned(bad),(unsigned long)uartErrors.load(),valid ? "SIM" : "NAO");
  Serial.printf("# DIAG,irq=%lu,bursts=%lu,max_bordas=%u,rejeicoes_tempo=%lu,rejeicoes_nivel=%lu,ultimo_erro_ticks=%ld\n",
    (unsigned long)marker.irqs,(unsigned long)marker.bursts,unsigned(marker.maxMatched),
    (unsigned long)marker.timingRejects,(unsigned long)marker.levelRejects,(long)marker.lastError);
  Serial.printf("# MARKER_DIAG,bordas_tardias_confirmadas=%lu,limite_normal_ticks=25,limite_isolado_ticks=43,max_isoladas_por_marcador=1\n",
    (unsigned long)marker.recoveredLateEdges);
  Serial.print("# EDGE_TRACE,tick_us=0.1,delta@nivel=");
  for(uint8_t i=0;i<marker.traceCount;++i) Serial.printf("%s%lu@%u",i ? ";" : "",(unsigned long)marker.trace[i],unsigned((marker.traceLevels>>i)&1u));
  Serial.println();
  endAtUs=0;
}
void startSession(uint16_t n) {
  if(running.load()) { Serial.println("# ERRO: use END depois de TX DONE ou STOP para abortar."); return; }
  if(!n || n>LIMIT) { Serial.println("# ERRO: START 1..1000."); return; }
  if(chosenMode && !calibrating && !phaseReady) {
    Serial.println("# ERRO: execute CAL e conclua a calibracao antes dos modos 1..3."); return;
  }
  controlLow(); disableAlarm(); activeRecord=-1;
  uint32_t planSeed=chosenSeed;
  for(uint16_t i=0;i<n;++i) {
    // Sorteio e conversao de byte/bit para instantes fora das interrupcoes.
    records[i].plan=calibrating ? NoiseLogic::calibrationPlan(i) : NoiseLogic::makePlan(chosenMode,planSeed);
    if(!calibrating && chosenMode) NoiseLogic::shiftPlan(records[i].plan,phaseTicks);
    records[i].firstTicks=0; records[i].receivedUs=0; records[i].maxLate=0;
    memset(records[i].actual,0,sizeof(records[i].actual));
    memset(records[i].clean,0,8); memset(records[i].dirty,0,8); memset(records[i].cref,0,7); memset(records[i].dref,0,7);
    records[i].present.store(0); records[i].faults.store(0);
  }
  for(uint8_t i=0;i<2;++i) { uart_flush_input(uart_port_t(i+1)); xQueueReset(uartQueues[i]); streams[i].reset(); }
  target=n; logged=good=bad=0; selectedTotal=changedTotal=aggregateFaults=0; seen.store(0); cleanFrames.store(0); uartErrors.store(0);
  sessionMaxLate=0; endAtUs=0; ++session;
  marker=NoiseLogic::Marker{}; marker.last=clockNow();
  if(calibrating) Serial.printf("# CAL_START,N=%u,nao_usar_como_experimento=1\n",unsigned(target));
  else {
    Serial.printf("# PHASE,phase_ticks=%d,tick_us=0.1,origem_tempo=ISR_GPIO,pulsos=CCOUNT_240MHz_ISR_UNICA,origem_fase=%s\n",
      int(phaseTicks),phaseFromScan ? "CAL_DESTA_EXECUCAO" : "LOG_BANCADA_V1_4");
  Serial.printf("# START,NOISE,sessao=%lu,N=%u,modo=%u,seed=%lu,tick_us=0.1,byte_zero=SOF_A5,bit_zero=LSB,guard_min_us=1000,firmware=1.12-sincronizado,marker_ticks=25,pulso_max_atraso_ticks=20,adianto_irq_ticks=60,pulso_max_adianto_ticks=5\n",(unsigned long)session,unsigned(target),unsigned(chosenMode),(unsigned long)chosenSeed);
  Serial.println("tipo,sessao,modo,indice,seq_original,t_sof_us,posicoes_byte_bit,alvos_hex,original_hex,receptor_hex,alvos_n,mudancas_n,xor_hex,eventos_planejados_ticks,atraso_max_ticks,eventos_aplicados_ticks,debug_original_hex,debug_receptor_hex,falhas_mask,qualidade");
  }
  markFrameHandled();
  injecting.store(true); running.store(true);
  resumeDetector();
}
// Diagnostico DC dentro do firmware real. delay() apenas para estabilizar
// a medicao DC; todos os pulsos de ruido continuam no timer de hardware.
bool selfTest() {
  if(running.load()) { Serial.println("# ERRO: sessao ativa."); return false; }
  controlLow(); disableAlarm(); activeRecord=-1;
  delay(3);
  int a25=gpio_get_level(GPIO_NUM_25),a26=gpio_get_level(GPIO_NUM_26),a27=gpio_get_level(GPIO_NUM_27);
  // Nao aplicar nivel continuo se o enlace nao esta em repouso alto.
  if(a25 || !a26 || !a27) {
    phaseReady=false;
    Serial.printf("# SELFTEST,FALHA_REPOUSO,D25=%d,D26=%d,D27=%d\n",a25,a26,a27); return false;
  }
  controlHigh(); delay(3);
  int b25=gpio_get_level(GPIO_NUM_25),b26=gpio_get_level(GPIO_NUM_26),b27=gpio_get_level(GPIO_NUM_27);
  controlLow(); delay(3);
  int c25=gpio_get_level(GPIO_NUM_25),c26=gpio_get_level(GPIO_NUM_26),c27=gpio_get_level(GPIO_NUM_27);
  bool ok=b25 && b26 && !b27 && !c25 && c26 && c27;
  if(!ok)phaseReady=false;
  Serial.printf("# SELFTEST,%s,OFF=%d%d%d,ON=%d%d%d,OFF_FINAL=%d%d%d,ordem=D25_D26_D27\n",
    ok ? "OK" : "FALHA",a25,a26,a27,b25,b26,b27,c25,c26,c27);
  return ok;
}
void beginCalibration() {
  if(running.load()) { Serial.println("# ERRO: sessao ativa."); return; }
  phaseReady=false;
  if(!selfTest())return;
  for(auto &bin:calBins) bin=NoiseLogic::CalBin{};
  calibrating=true; chosenMode=1;
  calibrationDeadline=uint64_t(esp_timer_get_time())+90000000u;
  startSession(NoiseLogic::CAL_FRAMES);
  Serial.println("# AGUARDANDO_TX: envie SUM e START 640 somente no TX. RX sem START nesta calibracao.");
}
void help() {
  Serial.println("# Fase inicial -55 ticks, baseada no log da bancada. Validar cada rodada normalmente.");
  Serial.println("# CAL opcional (640 quadros) / SELFTEST (enlace parado); reset restaura fase -55.");
  Serial.println("# MODE 0|1|2|3; SEED 1..4294967295; START [1..1000]; END; STOP; STATUS; HELP");
  Serial.println("# Ordem: MODE/SEED no injetor; START N no RX; START N aqui; START N no TX.");
  Serial.println("# END somente depois de TX DONE. STOP aborta. MODE/SEED somente fora de sessao.");
}
void status() {
  Serial.printf("# STATUS,NOISE,ativo=%u,modo=%u,SOF=%u,limpos=%u,receptor=%u,registrados=%u,erros_uart=%lu\n",unsigned(running.load()),unsigned(chosenMode),unsigned(seen.load()),unsigned(streams[0].frames),unsigned(streams[1].frames),unsigned(logged),(unsigned long)uartErrors.load());
}
void execute(char *s) {
  while(isspace((unsigned char)*s)) ++s;
  for(char *p=s;*p;++p) *p=char(toupper((unsigned char)*p));
  size_t len=strlen(s); while(len && isspace((unsigned char)s[len-1])) s[--len]=0;
  if(!strcmp(s,"CAL")) beginCalibration();
  else if(!strcmp(s,"SELFTEST")) selfTest();
  else if(!strcmp(s,"HELP")) help();
  else if(!strcmp(s,"STATUS")) status();
  else if(!strcmp(s,"STOP")) { if(running.load()) summarize("ABORT"); else controlLow(); }
  else if(!strcmp(s,"END")) {
    if(running.load()) { injecting.store(false); controlLow(); disableAlarm(); activeRecord=-1; endAtUs=uint64_t(esp_timer_get_time())+20000u; }
    else status();
  } else if(!strcmp(s,"START")) startSession(1000);
  else if(!strncmp(s,"START ",6)) {
    char *tail=nullptr; unsigned long n=strtoul(s+6,&tail,10);
    if(tail==s+6 || *tail || !n || n>1000) Serial.println("# ERRO: START 1..1000."); else startSession(uint16_t(n));
  } else if(!strncmp(s,"MODE ",5)) {
    char *tail=nullptr; unsigned long m=strtoul(s+5,&tail,10);
    if(running.load()) Serial.println("# ERRO: modo so muda entre sessoes.");
    else if(tail==s+5 || *tail || m>3) Serial.println("# ERRO: MODE 0..3.");
    else { chosenMode=uint8_t(m); controlLow(); status(); }
  } else if(!strncmp(s,"SEED ",5)) {
    char *tail=nullptr; unsigned long n=strtoul(s+5,&tail,10);
    if(running.load()) Serial.println("# ERRO: seed so muda entre sessoes.");
    else if(tail==s+5 || *tail || !n || s[5]=='-') Serial.println("# ERRO: SEED inteiro positivo de 32 bits.");
    else { chosenSeed=uint32_t(n); Serial.printf("# SEED=%lu\n",(unsigned long)chosenSeed); }
  } else if(*s) Serial.println("# ERRO: use HELP.");
}
void readCommands() {
  while(Serial.available()) {
    char c=char(Serial.read());
    if(c=='\r' || c=='\n') {
      if(overflow) Serial.println("# ERRO: comando muito longo.");
      else if(commandLength) { command[commandLength]=0; execute(command); }
      commandLength=0; overflow=false;
    } else if(commandLength<sizeof(command)-1) command[commandLength++]=c; else overflow=true;
  }
}
// Registra Timer0 (nivel 3) e GPIO26 (nivel 2) no Core 0, isolados do loop()
// e das ISRs de UART0/UART1/UART2 que rodam no Core 1.
void initRealtimeCore0(void *arg) {
  SemaphoreHandle_t done = static_cast<SemaphoreHandle_t>(arg);
  timer_config_t cfg={}; cfg.divider=8; cfg.counter_dir=TIMER_COUNT_UP;
  cfg.counter_en=TIMER_PAUSE; cfg.alarm_en=TIMER_ALARM_DIS;
  cfg.auto_reload=TIMER_AUTORELOAD_DIS; cfg.intr_type=TIMER_INTR_LEVEL;
  cfg.clk_src=TIMER_SRC_CLK_APB;
  ESP_ERROR_CHECK(timer_init(TIMER_GROUP_0,TIMER_0,&cfg));
  ESP_ERROR_CHECK(timer_set_counter_value(TIMER_GROUP_0,TIMER_0,0));
  ESP_ERROR_CHECK(timer_isr_register(TIMER_GROUP_0,TIMER_0,alarmCallback,nullptr,
                                   ESP_INTR_FLAG_IRAM|ESP_INTR_FLAG_LEVEL3,&timerInterrupt));
  ESP_ERROR_CHECK(timer_enable_intr(TIMER_GROUP_0,TIMER_0));
  ESP_ERROR_CHECK(timer_start(TIMER_GROUP_0,TIMER_0));
  ESP_ERROR_CHECK(gpio_isr_register(edgeCallback,nullptr,
                                  ESP_INTR_FLAG_IRAM|ESP_INTR_FLAG_LEVEL2,&gpioInterrupt));
  ESP_ERROR_CHECK(gpio_set_intr_type(GPIO_NUM_26,GPIO_INTR_ANYEDGE));
  ESP_ERROR_CHECK(gpio_intr_enable(GPIO_NUM_26));
  detectorInterruptMask = GPIO.pin[26].int_ena;
  xSemaphoreGive(done);
  vTaskDelete(nullptr);
}
void setup() {
  digitalWrite(25,LOW); pinMode(25,OUTPUT); Serial.begin(115200);
  if(!setCpuFrequencyMhz(240) || getCpuFrequencyMhz()!=240) {
    Serial.println("# ERRO: CPU precisa estar fixa em 240 MHz para os pulsos CCOUNT.");
    while(true) delay(1000);
  }
  gpio_config_t io={}; io.pin_bit_mask=(1ULL<<26)|(1ULL<<27); io.mode=GPIO_MODE_INPUT;
  ESP_ERROR_CHECK(gpio_config(&io));
  setupObserver(UART_NUM_1,26,&uartQueues[0]); setupObserver(UART_NUM_2,27,&uartQueues[1]);
  // Habilita leitura do nivel fisico D25, alem da saida, para SELFTEST.
  ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_25,GPIO_MODE_INPUT_OUTPUT));
  controlLow();
#if CONFIG_PM_ENABLE
  ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_APB_FREQ_MAX,0,"noise-apb",&apbLock));
  ESP_ERROR_CHECK(esp_pm_lock_acquire(apbLock));
  ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX,0,"noise-cpu",&cpuLock));
  ESP_ERROR_CHECK(esp_pm_lock_acquire(cpuLock));
#endif
  if(getApbFrequency()!=80000000u) {
    Serial.println("# ERRO: APB deve estar em 80 MHz para tick=0,1 us.");
    while(true) delay(1000);
  }
  SemaphoreHandle_t initDone = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(initRealtimeCore0, "rt_init", 4096, initDone, configMAX_PRIORITIES - 1, nullptr, 0);
  xSemaphoreTake(initDone, portMAX_DELAY);
  vSemaphoreDelete(initDone);
  Serial.println("# NOISE v1.12-sincronizado pronto; timer=TG0T0(Core0); pulsos=CCOUNT_240MHz_ISR_unica; 10MHz; IRQ_timer=3; IRQ_gpio=2; GPIO25 controle; GPIO26 original; GPIO27 receptor; modo inicial 0."); help();
  Serial.printf("# CLOCK,cpu_mhz=%u,apb_hz=%lu\n",unsigned(getCpuFrequencyMhz()),(unsigned long)getApbFrequency());
}
void loop() {
  // Enquanto o marcador SOF esta sendo capturado (~165 us) ou um pulso esta
  // armado/ativo no quadro atual (~520 us), congela o Core 1 em registrador
  // para nao gerar trafego no barramento APB nem chamadas de fila/spinlock.
  while(running.load(std::memory_order_relaxed) &&
        (activeRecord.load(std::memory_order_relaxed) >= 0 || marker.seeking)) {
    asm volatile("nop");
  }
  if(running.load(std::memory_order_relaxed) &&
     !detectorPaused.load(std::memory_order_acquire) && !endAtUs) {
    const uint32_t cycle = esp_cpu_get_cycle_count();
    const uint32_t b = marker.bursts;
    if(b == burstsAtResume) {
      // Sem transicoes no GPIO26 desde a retomada do detector: nenhuma UART
      // recebeu inicio de novo quadro. Nos primeiros 55 ms apos o quadro
      // anterior (o proximo SOF chega em ~50-51 ms), mantem o Core 1 em
      // silencio absoluto fora do barramento APB e das filas do FreeRTOS.
      if(uint32_t(cycle - lastCompleteCycle) < 13200000u) return;
      if(uint32_t(cycle - lastIdlePollCycle) < 2400000u) return;
      lastIdlePollCycle = cycle;
    } else {
      // Houve transicao no GPIO26 mas o SOF nao foi reconhecido: aguarda
      // 4,5 ms para o quadro e a copia D3 91 terminarem de chegar nas UARTs.
      if(b != lastSeenBursts) { lastSeenBursts = b; unhandledBurstCycle = cycle; }
      if(uint32_t(cycle - unhandledBurstCycle) < 1080000u) return;
      unhandledBurstCycle = cycle;
    }
    if(activeRecord.load(std::memory_order_relaxed) >= 0 || marker.seeking) return;
  }
  readCommands(); drainInputs();
  if(calibrating && running.load() && uint64_t(esp_timer_get_time())>=calibrationDeadline)
    summarize("CAL_TIMEOUT");
  if(running.load()) {
    uint32_t now=uint32_t(esp_timer_get_time());
    if(logged<target) {
      Record &r=records[logged]; uint8_t have=r.present.load(std::memory_order_acquire);
      if(have==COMPLETE) {
        // DONE + quadro e referencia de ambas UARTs: pulsos encerrados,
        // copia de depuracao recebida. Imprime antes de reativar o detector.
        printRecord(logged);
        resumeDetector();
        markFrameHandled();
      }
      else if(!(have&PLAN) && (have&(CLEAN|DIRTY|CREF|DREF))==(CLEAN|DIRTY|CREF|DREF)) {
        // As UARTs receberam o quadro e as referencias, mas a IRQ perdeu SOF.
        // Manter a falha explicita, sem desligar o proximo quadro ou deslocar seu plano.
        r.faults.fetch_or(NO_MARKER); r.present.fetch_or(DONE);
        printRecord(logged,true);
        resumeDetector();
        markFrameHandled();
      }
      else if((have&CLEAN) && uint32_t(now-r.receivedUs)>20000u) {
        // Timeout invalida este registro. Nunca cancelar pulso de outro registro.
        if(activeRecord==int(logged)) failPulse(r);
        r.faults.fetch_or(MISSING); r.present.fetch_or(DONE);
        printRecord(logged,true);
        resumeDetector();
        markFrameHandled();
      }
    }
    if(logged==target) summarize("DONE");
    else if(endAtUs && uint64_t(esp_timer_get_time())>=endAtUs) {
      for(;logged<target;) printRecord(logged,true);
      summarize("DONE");
    }
    return; // Sem delay(1) durante sessao ativa para eliminar jitter de 1 kHz.
  }
  delay(1); // So fora de sessao; nunca usado durante captura/injecao.
}
#endif
