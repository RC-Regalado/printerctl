# Handoff al segundo agente - TUI

Fecha: 2026-10-18. Task: implementar por etapas
[roadmap-tui.md](roadmap-tui.md), empezando por TUI-0 y TUI-1.

## Peticion y contexto operativo

El operador quiere avanzar en una TUI mientras termina una impresion.
Tratar el hardware y el daemon actual como ocupados: no consultar siquiera
el socket de produccion durante este trabajo sin autorizacion adicional.
No hay autorizacion para matar procesos, reiniciar servicios, instalar el
binario en uso ni modificar configuracion, markers o records reales.

M28 esta aceptado por el operador y UX-001 tiene regresiones documentadas.
M29 tiene aprobacion del operador, pero la revision tecnica conserva cinco
correcciones requeridas del importador: comentarios INI, bloque embebido,
rechazo binario/NUL, geometria rectangular e identidad del perfil.
No declararlas corregidas ni incorporar su remediacion a este task.

## Lectura obligatoria

- `Agents.md`; skills `printerctl-architecture`,
  `printerctl-safety-contract`, `printerctl-testing` en `.codex/skills/`.
- `docs/phase-2-architecture.md`, `docs/phase-2-roadmap.md`,
  `docs/phase-3-roadmap.md`, `docs/architecture.md`, `docs/roadmap.md`.
- `docs/safety.md`, `docs/testing.md`, `docs/design/protocol-v1.md`.
- `docs/audit/m28-remediation.md`, `docs/audit/m29-review.md`,
  `docs/milestones/milestone-29.md` y el roadmap TUI.
- `include/printerctl/client/plp_client.h`, `src/client/plp_client.c`,
  `include/printerctl/protocol/plp1.h`, `src/cli/cli.c` (observador),
  `src/daemon/socket.c` (limitacion secuencial, no modificar).
- `CMakeLists.txt`, `tests/CMakeLists.txt`, `.codex/hooks/*.sh`;
  `tests/test_plp_client.c`, `tests/test_daemon_socket.c`,
  `tests/test_print_observer.c` y `tests/test_printerctld.c`.

Las fuentes de Phase 1 son historicas para la migracion CLI. Las invariantes
siguen vigentes, pero no restaurar un legacy serial path por leerlas literalmente.

## Arranque seguro

1. Leer `git status --short` y diffs relevantes. El worktree contiene cambios
   previos y archivos nuevos M28/M29: no resetear, limpiar, revertir ni asumir
   que todo cambio pertenece a tu sesion.
2. Coordinar con el otro agente antes de editar CMake, tests/CMakeLists,
   headers compartidos o documentacion global. Preferir `src/tui/`,
   `include/printerctl/tui/`, tests propios y `build-tui*`.
   Un worktree separado solo sirve si incluye el baseline no commiteado
   necesario; crear uno desde HEAD no lo incorpora automaticamente.
3. Registrar nota de diseno TUI-0: dependencia terminal, ownership de I/O,
   estado de presentacion, conexiones cortas y campos no disponibles.
4. Implementar fixtures y pantalla offline; testear sin socket ni hardware.
5. Continuar con PLP unicamente contra sockets de prueba aislados. No usar
   el socket por defecto en un smoke test ni ejecutar CTest global sin aislar.

## Reusar, no duplicar

Target nuevo propuesto: `printerctl-tui`, opcion CMake desactivada por defecto.
Usar client_connect/version/status/close y tipos PLP existentes. No copiar
funciones static de CLI ni parsear su stdout. No llevar curses al core o cliente.

El payload generico de client_request toma memoria prestada hasta el siguiente
request/close; copiar datos necesarios a un modelo de presentacion propio.
No compartir client_t entre threads. No prometer deadline global: revisar
connect/write ademas del poll de respuesta. Ninguna espera de red debe volver
imposible cerrar la TUI ni restaurar la terminal.

El daemon atiende una conexion a la vez y el observador CLI conserva la suya.
Polling TUI: conexion corta por ciclo, frecuencia acotada, timeout visible,
sin busy loop. No resolver contencion modificando el servidor en este task.

STATUS no contiene execution ID/tipo/nombre ni temperatura/XYZ. No confundir
snapshot con QUERY_*, que solicita I/O Marlin. Mostrar desconocido/obsoleto
cuando corresponda. Nunca tratar desconexion como cancelacion o 100% como
COMPLETED. No inferir que toda ejecucion es un print: puede ser una macro.

## Entrega esperada por etapa

- Archivos cambiados y decision minima de dependencia/ownership.
- Comandos exactos y resultados PASS/FAIL/NOT TESTED, separados de resultados
  heredados. Fixtures/modelo, PTY terminal, mock socket y mock Marlin segun etapa.
- Gate de simbolos para que TUI no enlace serial/printer/core/Marlin.
- Regresiones aisladas y sanitizers cuando aplique. Informar incompatibilidades;
  no afirmar PASS por inspeccion ni ejecutar hooks contra produccion.
- Nota de limites: socket secuencial, snapshot sin identidad y deuda M29.
- No hardware, despliegue, restart ni commit global de trabajo de otros agentes.

## Prompt de inicio para el agente

> Implementa TUI-0 y TUI-1 de docs/roadmap-tui.md siguiendo este handoff.
> Lee primero las skills y fuentes indicadas. Mantiene aislada la impresion
> real: no contactes su socket ni serial ni reinicies procesos. Usa el cliente
> PLP existente cuando llegues a TUI-2, no una segunda implementacion.
> Ahora entrega monitor offline con fixtures, tests y nota de diseno; no
> agregues controles fisicos, queue ni cambios al daemon. Reporta evidencia
> ejecutada y pendientes; no confundas aprobacion del operador con cierre
> de findings tecnicos. Conserva los cambios del otro agente.
