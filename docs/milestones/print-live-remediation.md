# PRINT-LIVE-001 — Event Pipe Autonomous Drain Remediation

## Estado

Implementado el 2026-09-02.

## Causa raíz

Durante `PRINT`, el worker emitía eventos `PRINT_UPDATE` hacia `event_pipe`.
El control context solo drenaba esos eventos como efecto secundario de atender
requests PLP como `STATUS` o `TEMP`.

Cuando el pipe acumulaba suficientes eventos, el worker quedaba bloqueado en la
escritura de `event_pipe` y la ejecución no avanzaba al siguiente G-code hasta
que un request PLP externo provocaba el drenado.

## Cambio aplicado

El control context ahora observa autónomamente el fd de eventos del runtime:

- el loop principal de `printerctld` hace `poll()` sobre:
  - socket listener;
  - runtime event fd.
- el loop de cliente conectado hace `poll()` sobre:
  - client fd;
  - runtime event fd.

Cuando el runtime event fd está legible, el control context llama a
`printerctl_daemon_runtime_drain_events()` y actualiza el snapshot publicado sin
depender de tráfico PLP.

No se añadió un segundo lector del `event_pipe`. El fd expuesto por
`printerctl_daemon_runtime_event_fd()` es prestado solo para observación; las
lecturas siguen pasando por `printerctl_daemon_runtime_drain_events()` o
`printerctl_daemon_runtime_wait_event()`.

## Lifecycle resultante

```text
worker
  → event_pipe
  → control poll(runtime event fd)
  → drain runtime events
  → snapshot update
```

Este flujo opera aunque:

- no haya cliente conectado;
- el daemon esté esperando conexiones;
- exista un cliente conectado pero idle;
- `PRINT` esté activo;
- un observer client permanezca conectado.

## Blocking analysis

- `accept()` solo se llama después de que el listener indica `POLLIN`.
- La espera por input de un cliente conectado ya no bloquea el drenado de
  eventos porque el mismo `poll()` incluye el runtime event fd.
- `handle_frame()` conserva waits síncronos para resultados de comandos; esos
  waits ya consumen eventos mediante el runtime y no dependen del socket.
- No se hicieron pipes nonblocking ni se descartan eventos.
- No se introdujo polling serial artificial, sleeps de progreso, keepalives ni
  comandos dummy.

## Evidencia PTY/mock

Se agregaron pruebas deterministas en `tests/test_printerctld.c`:

- `test_print_advances_after_m109_without_secondary_request`
  - G-code:
    - `G1 X0` … `G1 X8`
    - `M109 S200`
    - `G1 X10`
  - Marlin mock:
    - cada `G1` responde `ok`;
    - `M109` responde líneas intermedias `T:`/`echo:busy:` y luego `ok`;
  - verifica que después del `ok` terminal de `M109`, `G1 X10` se transmite sin
    emitir `STATUS`, `TEMP` ni otro request PLP secundario.

- `test_print_progress_event_backlog_does_not_stall_execution`
  - ejecuta 48 comandos `G1` secuenciales, por encima del umbral previo de
    saturación observado;
  - verifica que todos se transmiten exactamente una vez y el daemon sigue vivo.

- `test_status_during_print_has_no_serial_side_effects`
  - `STATUS` durante `PRINT` devuelve snapshot;
  - no transmite comandos serial adicionales.

- `test_temp_during_print_is_busy_and_has_no_serial_side_effects`
  - `TEMP` durante `PRINT` devuelve `PRINTERCTL_PLP1_BUSY` (`0x0203`);
  - no transmite `M105`;
  - `PRINT` conserva ownership de serial.

## Safety

No cambia la autorización física.

Garantías preservadas:

- `STATUS` no genera serial I/O.
- `TEMP` durante `PRINT` no genera `M105`.
- `PRINT` transmite únicamente G-code proveniente del source autorizado.
- No se añadió homing automático.
- No se añadió movimiento fuera del source.
- No se añadió heating/extrusion fuera del source.
- No se añadió `M112`, `M410`, cancelación física automática, recovery ni
  resume automático.
- La barrera R6 de cancelación no fue modificada.

## Archivos modificados

- `include/printerctl/daemon/runtime.h`
  - expone el fd prestado de eventos para observación por el control context.
- `src/daemon/runtime.c`
  - implementa `printerctl_daemon_runtime_event_fd()`.
- `src/printerctld_main.c`
  - drena eventos del runtime aun cuando no hay tráfico de cliente.
- `src/daemon/socket.c`
  - drena eventos del runtime mientras un cliente conectado está idle.
- `tests/test_printerctld.c`
  - agrega cobertura PTY/mock para progreso autónomo, backlog, `STATUS` y
    `TEMP`.

## Deuda

No se introdujo una cola/event coalescing. Si en el futuro los eventos de
progreso son demasiado voluminosos para clientes de observabilidad, eso debe
resolverse como optimización explícita sin romper el ownership del control
context.
