# NOISE v1.12-sincronizado

## Resumo das alterações em relação à v1.10 e v1.11

1. **Reversão do `MarkerCapture` da v1.11 e leitura imediata de `GPIO.in` na ISR**:
   - Na `v1.11`, o `MarkerCapture` ancorava `baseTicks` após `markerClockNow` e `baseCycle` antes de `markerClockNow`, além de usar divisão inteira truncada `/ 24u` nas bordas 1..15. Isso deslocava o erro nominal da 15ª transição (`bit 19`) de `-18,8` ticks para `-21,0` ticks (`1628@1` vs `1649` esperados), deixando apenas 4 ticks de margem até `-25`.
   - A `v1.12` lê `(GPIO.in & (1u << 26)) != 0` logo na entrada de `edgeCallback` (antes do timer APB), mas mantém `markerClockNow(cycle)` em todas as 16 bordas como na `v1.10`, preservando a época dos pulsos e a calibração de `-55` ticks que obteve 100% de sucesso nos pulsos (`4.883/4.883`) na bancada.

2. **Sincronização do `SysTick` de 1 kHz do Core 0 (`CCOMPARE0`) para fora do quadro (`deferSysTickFromSof`)**:
   - Na arquitetura Xtensa LX6 (ESP32), a ISR de nível 1 `xPortSysTickHandler` entra em `portENTER_CRITICAL_ISR(&xKernelLock)`, que executa `RSIL 3` (`XCHAL_EXCM_LEVEL = 3`), mascarando tanto o nível 2 (`edgeCallback`) quanto o nível 3 (`alarmCallback`) por ~2,2 a 6,0 µs (22 a 60 ticks).
   - A `v1.12` reposiciona `CCOMPARE0` no Core 0 para `firstCycle + 204000u` (`+850 µs` após o início do `SOF`). Assim, o quadro de 8 bytes e os pulsos terminam em `~694 µs`, e o `SysTick` de 1 kHz do Core 0 passa a ocorrer sempre dentro da guarda ociosa de 2 ms (a `+850 µs` do quadro atual e a `-150 µs` do próximo quadro, seja em período de 50 ms ou 51 ms), sem perder nenhum tick do FreeRTOS e sem colidir com o `SOF` nem com os pulsos.

3. **Silêncio do Core 1 no barramento APB e em filas durante a espera e captura do `SOF`**:
   - O `while` de espera em registrador no topo de `loop()` agora abrange `activeRecord >= 0 || marker.seeking`.
   - Enquanto `detectorPaused == 0` e nenhum novo burst ocorreu em `GPIO26` nos primeiros `55 ms` (`13.200.000` ciclos de `CCOUNT`) após o quadro anterior, o Core 1 não chama `drainInputs()` nem `readCommands()`, eliminando tráfego APB (`UART0/1/2`) e contenção de spinlocks do FreeRTOS no instante em que o próximo `SOF` chega.
   - `printRecord(logged)` é executado antes de `resumeDetector()`.

4. **Recuperação de atraso isolado na borda 0 e na 14ª transição (`Marker::edge`)**:
   - Mantém `MARKER_TOLERANCE_TICKS = 25` e o limite de `43` ticks (`ticks(5)`) para no máximo uma borda atrasada por marcador.
   - Permite confirmar a penúltima transição (`index = 13`, `bit 18`) pela última transição estrita (`index = 14`, `bit 19`).
   - Se a borda 0 (queda inicial do start bit) atrasar no primeiro quadro (antes da sincronização de `CCOMPARE0`), causando deslocamento negativo proporcional nas bordas seguintes mas preservando o intervalo estrito de 1 bit entre `index = 0` e `index = 1` (ou `2` e `3`), restaura `first` e `firstCycle` e exige que todas as bordas restantes cumpram `±25` ticks.
