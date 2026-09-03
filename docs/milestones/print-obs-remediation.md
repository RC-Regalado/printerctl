# PRINT-OBS-001 — Print Observable Progress / Daemon Status

## Estado

Implementado el 2026-09-02.

## Nota de numeración

El pedido original etiquetó este trabajo como "M22", pero
`docs/phase-2-roadmap.md` ya usa M22 para "Host Print CLI Migration"
(implementado, ver `docs/design/m22-host-print-migration.md`) y M23
para "Host Print Control UX" (pause/resume/cancel del stream host,
propuesto). PRINT-OBS-001 es ortogonal a ambos: expone progreso
observable sobre el snapshot/STATUS ya existentes, sin tocar jobs,
historial ni la migración de `pause`/`resume`/`cancel` a nivel CLI.
Se documenta aquí como un remediation independiente, siguiendo el
mismo patrón que `print-live-remediation.md` (PRINT-LIVE-001).

## Investigación previa

Antes de implementar se determinó, con referencias exactas:

1. `print_engine.c:183-191` (`on_progress`) ya emite un evento
   `PROGRESS` por cada comando reconocido, con
   `printerctl_gcode_result_t` (`command_number`, `bytes_consumed`,
   `progress_percent` -- basado en bytes, no en conteo de comandos).
2. `worker.c:257-329` (`emit_print_update`/`on_print_event`) ya
   mapeaba cada evento de impresión a un evento IPC `PRINT_UPDATE` con
   `execution_state`, `progress_percent`, `commands_sent`,
   `commands_acknowledged`, `source_position`.
3. `runtime.c:449-459` ya aplicaba esos campos directamente al
   snapshot publicado (`printerctl_daemon_snapshot_t`).
4. `socket.c:453-520` ya codificaba `execution_state`/`progress`/
   `commands_sent`/`commands_acknowledged`/`source_position`/
   `failure_reason` en la respuesta PLP STATUS, condicionado por
   `requested_fields`.
5. `plp_client.c:328-362` ya exponía `printerctl_client_status()`
   genérico.

Es decir: la mayor parte del pipeline (M15/M16) ya existía. Las
brechas reales identificadas fueron:

- **`commands_total` no existía en ninguna capa** (no emitido, no
  almacenado, no codificado). `print_engine`/`gcode.c` son lectores de
  una sola pasada: el conteo total de comandos nunca se conocía antes
  de ejecutar un print real (solo el `--dry-run` de la CLI hacía una
  pasada completa por separado, en `cli.c:615`).
- **Bug real de reset de progreso en pausa/resume**: `worker.c` (antes
  del fix) llamaba `emit_print_update(ctx, PAUSED, NULL, NULL, 0)` y
  `emit_print_update(ctx, RUNNING, NULL, NULL, 0)` en `PAUSED`/
  `RESUMED`, y `emit_print_update` trata `summary == NULL` como "todo
  en cero". `runtime.c` sobrescribe el snapshot sin condicionar, por lo
  que pausar un print ponía `progress`/`commands_sent`/
  `commands_acknowledged`/`source_position` a 0 en el snapshot
  publicado -- violando directamente "no reset counters on
  pause/resume".
- **La CLI nunca llamaba a `printerctl_client_status()` tras
  `PRINT_START`** -- `run_print_start_command()` retornaba
  inmediatamente tras la aceptación (ver
  `docs/design/m22-host-print-migration.md`, que ya marcaba esto como
  trabajo futuro explícito).

## Cambio aplicado

### commands_total (nuevo, end-to-end)

Contador *best-effort* (0 = desconocido), calculado una vez mediante
un pre-scan local del archivo fuente en `print_engine.c` (misma
técnica que ya usaba el `--dry-run` de la CLI: abrir un
`printerctl_gcode_reader_t` separado con `allow_g28 = false` -- igual
política que la pasada real de streaming -- y contar hasta EOF). Un
fallo del pre-scan (archivo corrupto, G28 no autorizado, etc.) nunca
bloquea el print real: solo deja `commands_total = 0`; la pasada real
de streaming hace su propia validación independiente y es la única
autorizada a fallar el print.

Recorrido: `printerctl_print_engine_t` (campo nuevo +
`printerctl_print_engine_commands_total()`) → `worker_context_t`
(consultado una vez en `STARTED`) → `printerctl_daemon_event_t` (campo
`commands_total`, IPC interno de proceso, sin problema de formato de
wire) → `printerctl_daemon_snapshot_t` → PLP/1 STATUS
(`PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_TOTAL = 0x0200`,
`KNOWN_MASK` extendido a `0x03FF`, campo agregado *al final* del orden
canónico -- compatible con clientes existentes, ningún bit
renumerado).

### Fix: pausa/resume ya no resetea el progreso

`worker_context_t` ahora retiene `last_progress`
(`printerctl_print_summary_t`), actualizado en cada evento `PROGRESS`
y reseteado a cero en `STARTED`. Los eventos `PAUSED`/`RESUMED` ahora
reutilizan `&ctx->last_progress` en vez de `NULL`, preservando
`progress`/`commands_sent`/`commands_acknowledged`/`source_position`
a través de la pausa.

### CLI: observador de progreso adjunto

`printerctl print FILE` permanece conectado tras la aceptación de
`PRINT_START` y sondea `STATUS` (misma conexión, cada 200 ms) hasta un
`execution_state` terminal (`COMPLETED`/`CANCELLED`/`FAILED`), nunca
solo por alcanzar 100%. Renderiza líneas del tipo:

```text
printerctl: Printing: 47% (583/1240)
printerctl: Completed: 100% (10/10)
```

Sin modelo de ETA, sin suscripción a eventos -- puro polling
snapshot-only, exactamente como exige STATUS.

## Evidencia PTY (timeline real, ack lento simulado)

```text
printerctl: print accepted by printerctld: '/tmp/.../job.gcode'
printerctl: Printing: 0% (0/10)
printerctl: Printing: 10% (1/10)
printerctl: Printing: 20% (2/10)
...
printerctl: Printing: 90% (9/10)
printerctl: Completed: 100% (10/10)
```

## Client independence

Verificado por test automatizado (`test_print_progress_client_independence`,
ver más abajo): el cliente A hace `PRINT_START` y se desconecta
inmediatamente; el print continúa; un cliente B, sin conexión previa,
observa `execution_state`/`commands_acknowledged`/`commands_total`
correctos leyendo únicamente el snapshot publicado por el daemon.

## Safety

Sin cambios de autorización física. Garantías preservadas:

- `STATUS` sigue siendo snapshot-only, cero I/O serial (verificado por
  `test_repeated_status_polling_has_no_serial_side_effects`, que
  sondea STATUS 10 veces seguidas antes del primer ACK y confirma cero
  comandos serial adicionales).
- El polling de progreso de la CLI nunca escribe al socket más que
  requests STATUS -- no introduce homing, heating, extrusion, M112,
  M410 ni resume/recovery automático.
- El pre-scan de `commands_total` es lectura local de archivo, cero
  I/O serial.
- No se modificó la barrera R6 de cancelación.

## Archivos modificados

- `include/printerctl/core/print_engine.h`, `src/core/print_engine.c`
  -- pre-scan `commands_total` + getter.
- `include/printerctl/daemon/ipc.h` -- campo `commands_total` en
  `printerctl_daemon_event_t`.
- `src/daemon/worker.c` -- propaga `commands_total`; fix de
  pausa/resume vía `last_progress`.
- `include/printerctl/daemon/runtime.h`, `src/daemon/runtime.c` --
  campo `commands_total` en el snapshot.
- `include/printerctl/protocol/plp1.h`, `src/protocol/plp1.c` --
  `PRINTERCTL_PLP1_STATUS_FIELD_COMMANDS_TOTAL`, `KNOWN_MASK`
  extendido, encode/decode.
- `src/daemon/socket.c` -- STATUS codifica `commands_total` cuando se
  solicita y es conocido.
- `src/cli/cli.c` -- observador de progreso adjunto tras `PRINT_START`.
- `docs/design/protocol-v1.md` -- bitmap/ancho de `commands_total`.
- `tests/test_plp1_payloads.c` -- round-trip de `commands_total`;
  movido el bit "reservado" de prueba de `0x0200` a `0x0400` (ya que
  `0x0200` pasa a ser un bit válido).
- `tests/test_daemon_runtime.c` -- propagación de `commands_total`
  evento→snapshot (inyección sintética, sin worker real).
- `tests/test_printerctld.c` -- cobertura PTY/mock: avance/monotonía
  de progreso, pausa/resume preserva progreso, independencia de
  cliente, cero efectos secundarios de STATUS repetido; se reordenó
  `test_cli_print_uses_daemon_print_start()` (el intercambio con el
  mock ahora ocurre antes de `expect_child_exit()`, ya que la CLI deja
  de salir inmediatamente tras la aceptación).

## Verificación

- GCC + Clang, `-Wall -Wextra -Wpedantic`: limpio.
- `ctest` completo: 18/18 en ambos compiladores.
- ASan+UBSan: 18/18, limpio.
- Regresión PRINT-LIVE-001 (`test_print_advances_after_m109_without_secondary_request`,
  `test_print_progress_event_backlog_does_not_stall_execution`): pasan
  sin cambios, dentro del mismo binario `printerctld_tests`.
- Prueba manual PTY end-to-end (ver timeline arriba).

## Deuda / fuera de alcance

- `commands_total` no cubre `PAUSE`/`RESUME`/`CANCEL` de comandos
  individuales fuera de un print host (no aplica: esos mensajes no
  llevan payload).
- Un archivo cuyo pre-scan falle reporta `commands_total = 0`
  (desconocido) para todo el print, incluso si el streaming real sí
  logra empezar; no se reintenta el pre-scan.
- ETA, jobs, historial, cola, suscripción a eventos y Web/TUI quedan
  explícitamente fuera de alcance, tal como especifica PRINT-OBS-001.
