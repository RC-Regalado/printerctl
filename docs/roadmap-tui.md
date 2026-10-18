# Roadmap especial - TUI

Fecha: 2026-10-18
Estado: PROPOSED. Preparacion e implementacion aislada autorizadas por el
operador; sin autorizacion de despliegue ni de pruebas sobre la impresion real.
Contexto para el segundo agente: [tui-handoff.md](tui-handoff.md).

## Objetivo y limites

Crear `printerctl-tui`, un cliente de terminal del daemon, mientras termina
la impresion indicada por el operador. No implementar una cola: "impresion
en cola" describe la situacion del operador, no una capacidad comprobada.

El primer entregable es un monitor utilizable y de solo lectura. Los controles
fisicos son una etapa posterior condicionada a revision, no parte del MVP.
Este roadmap es un carril paralelo; no renumera milestones ni cierra M29.

## Baseline observado

- C17, CMake >= 3.16, Linux; no hay una biblioteca TUI en el build actual.
- Phase 2 migrada a cliente PLP. `docs/architecture.md` y `Agents.md`
  conservan contexto historico de Phase 1; para el cliente actual consultar
  `docs/phase-2-architecture.md`, headers e implementacion.
- M28 aceptado por el operador. M29 aceptado por el operador, con cierre
  tecnico pendiente de M29-R1 a M29-R5 en `docs/audit/m29-review.md`.
- Reutilizar `printerctl_client` y `printerctl_protocol_plp1`; no invocar
  subprocesos de la CLI ni escribir un segundo codec.
- `STATUS` es snapshot-only: estados, progreso, elapsed, contadores,
  posicion de fuente y fallo. Cada valor exige su bit en `present_fields`.
- `commands_total == 0` significa desconocido. Un 100% no prueba finalizacion;
  el estado terminal del daemon es la autoridad. Preservar UX-001.
- `STATUS` no expone nombre de archivo, execution ID, tipo de ejecucion,
  temperatura ni posicion XYZ. No inventarlos ni inferir tipo por contadores.
- `QUERY_TEMP`/`QUERY_POSITION` hacen I/O real: no son polling inocuo.
- El servidor actual atiende clientes secuencialmente. El observador de print
  conserva la conexion; una segunda conexion puede esperar sin ser atendida.
  Un `connect()` exitoso no demuestra capacidad de respuesta del daemon.

## Contrato arquitectonico

```text
printerctl-tui -> printerctl_client -> PLP/1 -> AF_UNIX -> printerctld
                                                        -> worker
                                                        -> print_engine
                                                        -> printer -> serial
```

La TUI posee terminal, estado de presentacion y una copia del ultimo snapshot,
no la ejecucion. No incluye daemon internals/core/printer/serial. Nunca abre
el puerto, envia senales al daemon ni implementa fallback directo.
Salir, Ctrl+C, fallo de terminal o desconexion solo cierran el cliente.

Propuesta: target opcional `PRINTERCTL_BUILD_TUI`, desactivado por defecto,
para conservar builds headless sin dependencias nuevas obligatorias.
Evaluar ncurses mediante FindCurses local antes de elegir; no instalar una
biblioteca ni escribir un framework terminal propio sin justificarlo.

## TUI-0 - Contrato y aislamiento

Entregables: nota de diseno, esquema de pantalla y matriz de campos/RPC.
Determinar dependencia disponible y separar modelo/render de transporte.
Usar build propio y fixtures; no ejecutar comandos de consulta al daemon real.

Gate: confirmar restricciones de socket, ownership, ausencia de execution ID
y operaciones disponibles leyendo el codigo actual. Registrar contradicciones,
no modificar silenciosamente la arquitectura del daemon para acomodar la TUI.

## TUI-1 - Monitor offline

Crear shell terminal con fixtures seleccionadas explicitamente: IDLE, RUNNING,
PAUSED, CANCELLING, COMPLETED, CANCELLED, FAILED, desconectado y snapshot parcial.
La demo debe identificarse como demo y no abrir ningun socket.

Pantalla compacta: conexion/frescura, daemon/worker, estado de ejecucion,
progreso, ACK/total si existen, tiempo reportado y fallo. Sin dashboard
decorativo, historia inventada, ETA heuristica ni temperaturas ficticias.
Manejar resize, terminal pequena y restauracion del terminal al salir.
No mostrar valores ausentes como cero ni conservar bits de snapshots antiguos.

Gate: modelo/render con tests deterministas; PTY para teclado, resize y salida;
ningun acceso a daemon, serial o configuracion de produccion.

## TUI-2 - Monitor PLP en entorno aislado

Agregar endpoint explicito `--socket PATH`, deadline positivo y polling
limitado (propuesta inicial: 1 s, sin requests simultaneas ni catch-up loops).
Cada ciclo conecta, consulta y cierra; nunca retener una conexion entre polls.
Negociar VERSION/capabilities sin requerir que el daemon anuncie macros.
No reutilizar una conexion despues de timeout/error de protocolo.

Mantener teclado y salida responsivos durante I/O. El cliente actual es
sincrono; su timeout de respuesta no garantiza un limite global de connect
o write. Probar esos caminos antes de prometer deadlines de extremo a extremo.
Si se necesita un hilo de I/O, sera unico owner del cliente/FD, con intercambio
acotado de copias y shutdown/join probado; nunca compartir client_t libremente.
Cambios al cliente compartido requieren pruebas de regresion de la CLI.

Errores: mostrar desconectado/ocupado o ultima lectura obsoleta con edad local;
no convertir timeout en fallo de impresion. Reconexion limitada solo para
lecturas: nunca reenviar una accion cuyo resultado se desconoce.

Gate: daemon/mock con socket temporal; fragmentacion, EOF, timeout, recuperacion,
campo ausente, version incompatible y servidor ocupado por otro observador.
La TUI debe poder salir aun cuando el servidor no la atienda.

## TUI-3 - Integracion y validacion del MVP

Arrancar un daemon de prueba y mock Marlin PTY; otro cliente inicia una
impresion sintetica. La TUI observa sin poseerla y sale sin detenerla.
Probar contencion con el observador CLI existente; documentar indisponibilidad
sin exigir como parte de este carril un servidor multicliente.

Gate: regresion completa aislada, ASan/UBSan y TSan si hay nuevos threads,
boundary scan y revision de acciones fisicas. La bateria de tests de TUI
prueba cero comandos serial adicionales por polling STATUS y salida.
Extender el gate de simbolos al binario TUI sin debilitar el de la CLI.

El MVP puede aceptarse sin controles: los limites reales de STATUS deben
estar visibles como datos no disponibles, no suplirse con probes Marlin.

## TUI-4 - Controles explicitos (DEFERRED, requiere revision)

Solo despues del MVP y con autorizacion separada: evaluar START y controles
host pause/resume/cancel. No incluir raw send, homing, macros, importador Prusa,
calefaccion ni controles SD como parte del primer incremento.

Prerequisito: resolver como identificar con seguridad el tipo/ejecucion
objetivo y su cambio entre confirmacion y RPC. STATUS actual no lo garantiza;
no inferirlo de RUNNING/PAUSED. Si requiere extender PLP, proponer un task
separado en lugar de incluir cambios wire en el trabajo de TUI.

Confirmacion explicita para acciones destructivas, un RPC por intento,
resultado ambiguo tras timeout sin reintento automatico y sin doble envio
por key-repeat. Etiquetar "aceptado" separado de "completado".
No confundir PRINT_CANCEL con SD_CANCEL ni cancellation con emergency stop.

## TUI-5 - Review y entrega

Revision independiente de seguridad, ownership de terminal/FD/threads,
compatibilidad y UX. Entregar comandos reproducibles y limites documentados.
Prueba sobre el sistema real solo al terminar la impresion, confirmar daemon
inactivo y recibir permiso del operador; no reiniciar ni desplegar por defecto.

## Verificacion reproducible

Para la futura implementacion, no ejecutado al escribir este documento:

```bash
cmake -S . -B build-tui -DPRINTERCTL_BUILD_TUI=ON
cmake --build build-tui
bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp \
  --bind "$PWD" "$PWD" --unshare-pid --die-with-parent \
  env TMPDIR=/tmp XDG_STATE_HOME=/tmp/printerctl-tui-state \
  ctest --test-dir build-tui --output-on-failure --timeout 180
bash .codex/hooks/check-boundaries.sh
bash .codex/hooks/check-physical-actions.sh
```

`--dev /dev` debe proporcionar dispositivos minimos y PTY, no serial real;
validar disponibilidad del harness local. Si bwrap no funciona, ejecutar solo
tests demostrablemente aislados y registrar el resto NOT TESTED: nunca parar
el daemon real para liberar `/tmp/printerctl.sock`. Revisar hooks antes de
ejecutarlos: test.sh no agrega aislamiento por si mismo. Revisar manualmente
cualquier alerta fisica; no suprimirla para lograr un PASS.

## Fuera de alcance

Queue/jobs/history, scheduler, multiprinter, Web, event subscriptions,
auto-resume, auto-retry de acciones, recuperacion Resend, redisenar worker o
servidor socket, corregir M29 como trabajo incidental, o cambiar APCUPSD.

## Criterio de cierre

TUI-0 a TUI-3 revisados y reproducibles; cliente util de solo lectura, terminal
restaurado, ningun ownership fisico y ninguna interferencia con ejecuciones.
TUI-4 no bloquea ese cierre. Reportar PASS/FAIL/NOT TESTED con evidencia real;
este documento no certifica que exista implementacion ni pruebas ejecutadas.
