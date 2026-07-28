# Roadmap actualizado — printerctl

## Alcance de esta revisión

Este documento sustituye los hitos 10 en adelante del roadmap original.

Los hitos 0 a 9 mantienen su alcance actual.

A partir del hito 10, el proyecto evoluciona hacia una arquitectura basada en:

* daemon persistente;
* socket Unix de sistema;
* cola de trabajos;
* persistencia en archivos de texto;
* cliente CLI;
* interfaz TUI en C;
* gateway WebSocket;
* frontend Vue.js;
* ejecución del gateway web mediante Docker.

---

# Milestone 10 — Cancelación por batería baja

## Goal

Cancelar de forma segura la impresión activa cuando APCUPSD notifique una condición de batería baja.

La pérdida de suministro no debe producir reanudación automática.

El sistema debe detener el envío de G-code y almacenar un registro de la sesión cancelada.

## Architectural Decision

APCUPSD ejecutará un script externo cuando detecte batería baja.

El script no controlará directamente el puerto serial ni modificará archivos internos.

El script enviará una orden al proceso de impresión.

Durante esta fase podrá utilizarse una señal Unix o una interfaz temporal interna. Después de implementar el daemon, deberá utilizar el socket Unix del sistema.

## Cancellation Semantics

Una cancelación por batería baja debe:

1. impedir el envío de nuevas instrucciones G-code;
2. no esperar indefinidamente un nuevo `ok`;
3. conservar la última línea reconocida;
4. marcar la sesión como cancelada por fallo de alimentación;
5. cerrar el archivo G-code;
6. almacenar un registro de cancelación;
7. cerrar correctamente los recursos seriales;
8. no reanudar el trabajo cuando regrese la energía.

No debe enviarse automáticamente:

* `M112`;
* `M410`;
* comandos de homing;
* comandos de reanudación;
* movimientos adicionales.

El objetivo es dejar de transmitir, no intentar ejecutar una secuencia física potencialmente insegura cuando la alimentación está comprometida.

## Final State

Agregar un estado explícito:

```c
PRINT_STATE_CANCELLED_POWER_FAILURE
```

El motivo debe diferenciarse de:

```c
PRINT_STATE_CANCELLED_OPERATOR
PRINT_STATE_FAILED_SERIAL
PRINT_STATE_FAILED_PRINTER
PRINT_STATE_COMPLETED
```

## Persistent Record

El registro debe incluir como mínimo:

```text
job_id
file_path
timestamp
final_state
cancel_reason
last_source_line
last_acknowledged_command
progress_percent
serial_device
printer_state
```

Ejemplo conceptual:

```text
job_id=job-000042
file_path=/var/lib/printerctl/jobs/part.gcode
timestamp=2026-07-20T18:40:00-06:00
final_state=cancelled_power_failure
cancel_reason=apcupsd_low_battery
last_source_line=1842
progress_percent=47.31
serial_device=/dev/ttyACM0
```

## Codex Tasks

* Revisar la máquina de estados.
* Validar la separación entre cancelación manual y cancelación eléctrica.
* Definir el formato del registro persistente.
* Revisar condiciones de carrera entre confirmación serial y cancelación.
* Confirmar que no existe reanudación automática.

## Claude Tasks

* Agregar el estado `PRINT_STATE_CANCELLED_POWER_FAILURE`.
* Implementar la solicitud de cancelación asíncrona.
* Detener el streaming al recibir la solicitud.
* Persistir el registro de sesión.
* Liberar recursos de archivo y serial.
* Implementar pruebas automatizadas.
* Crear el script de APCUPSD.
* Documentar el script `.sh`.

## APCUPSD Script

Ruta propuesta:

```text
scripts/apcupsd/printerctl-low-battery.sh
```

Responsabilidad:

```text
Notificar a printerctl que APCUPSD detectó batería baja.
```

El script debe:

* usar `#!/usr/bin/env bash`;
* usar `set -Eeuo pipefail`;
* validar que el proceso o socket exista;
* registrar fallo de comunicación;
* tener timeout;
* no controlar el puerto serial;
* no matar el proceso con `SIGKILL`;
* devolver códigos de salida documentados.

## Acceptance Criteria

* Una notificación de batería baja cancela la sesión activa.
* No se envían nuevas líneas después de procesar la cancelación.
* El trabajo queda marcado como `cancelled_power_failure`.
* Se persiste la última línea reconocida.
* El proceso libera correctamente archivo y serial.
* El regreso de alimentación no inicia ni reanuda trabajos.
* El script de APCUPSD está documentado.
* Las pruebas demuestran que no existe auto-resume.

---

# Milestone 11 — Documentación del núcleo de impresión

## Goal

Completar la documentación del núcleo antes de iniciar la transformación hacia daemon e interfaces múltiples.

## Required Documentation

Mantener:

```text
README.md
docs/Architecture.md
docs/Roadmap.md
docs/Components.md
docs/Testing.md
docs/Safety.md
docs/Logging.md
docs/APCUPSD.md
docs/Scripts/printerctl-low-battery.md
```

## Required Topics

### Printing lifecycle

Documentar:

```text
queued
starting
printing
paused
completed
cancelled_operator
cancelled_power_failure
failed_serial
failed_printer
```

### Power-failure behavior

Documentar explícitamente:

* qué evento de APCUPSD activa la cancelación;
* cómo se comunica el script con el proceso;
* qué recursos se cierran;
* qué información se conserva;
* por qué no se reanuda automáticamente;
* cómo inspeccionar el trabajo cancelado.

### Script documentation

Cada `.sh` debe incluir:

* propósito;
* uso;
* argumentos;
* variables de entorno;
* dependencias;
* efectos laterales;
* códigos de salida;
* fallos posibles;
* procedimiento de recuperación.

## Codex Tasks

* Revisar coherencia entre implementación y documentación.
* Actualizar diagramas.
* Validar documentación de seguridad.
* Revisar procedimientos de prueba.

## Claude Tasks

* Actualizar archivos Markdown.
* Agregar ejemplos de uso.
* Documentar scripts.
* Corregir referencias obsoletas.
* Verificar comandos desde un checkout limpio.

## Acceptance Criteria

* La arquitectura actual está documentada.
* El fallo de alimentación tiene una política explícita.
* Los scripts tienen documentación individual.
* Las instrucciones de compilación y prueba funcionan.
* El roadmap está preparado para la fase daemon.

---

# Phase II — Daemon and IPC Architecture

## Architectural Goal

Transformar el núcleo de impresión en un servicio persistente.

El proceso `printerctld` será el único componente autorizado para:

* abrir el puerto serial;
* controlar la impresora;
* mantener la cola;
* iniciar trabajos;
* pausar;
* reanudar;
* cancelar;
* persistir estados;
* procesar eventos de APCUPSD.

Las interfaces actuarán como clientes:

```text
printerctl CLI
printerctl-tui
printerctl-web-gateway
```

## Target Architecture

```text
┌─────────────────────────────────────────────┐
│                 Vue.js UI                   │
└────────────────────┬────────────────────────┘
                     │ WebSocket / JSON
                     ▼
┌─────────────────────────────────────────────┐
│      printerctl-web-gateway container       │
│                                             │
│ JSON ↔ internal Redis-style frames          │
└────────────────────┬────────────────────────┘
                     │ Unix socket
                     ▼
┌─────────────────────────────────────────────┐
│                 printerctld                 │
│                                             │
│  Unix Socket Server                         │
│  Command Dispatcher                         │
│  Job Queue                                  │
│  Print Engine                               │
│  State Persistence                          │
│  APCUPSD Integration                        │
│  Serial / Marlin                            │
└────────────────────┬────────────────────────┘
                     │ USB serial
                     ▼
┌─────────────────────────────────────────────┐
│                   Marlin                    │
└─────────────────────────────────────────────┘

printerctl CLI ──────┐
                     ├── Unix socket
printerctl-tui ──────┘
```

---

# Milestone 12 — Extract Print Engine

## Goal

Separar el motor de impresión del CLI actual.

El motor no debe depender de:

* argumentos CLI;
* terminal;
* TUI;
* HTTP;
* WebSocket;
* formato JSON.

## Modules

Crear:

```text
src/core/
├── print_engine.c
├── print_session.c
├── printer_state.c
├── cancellation.c
└── job_state.c

include/printerctl/core/
├── print_engine.h
├── print_session.h
├── printer_state.h
├── cancellation.h
└── job_state.h
```

## Print Engine API

Interfaz conceptual:

```c
int print_engine_start(
    struct print_engine *engine,
    const struct print_job *job
);

int print_engine_pause(struct print_engine *engine);
int print_engine_resume(struct print_engine *engine);

int print_engine_cancel(
    struct print_engine *engine,
    enum cancel_reason reason
);

int print_engine_tick(struct print_engine *engine);
```

## Requirements

* No leer argumentos CLI.
* No imprimir directamente en stdout.
* Reportar cambios mediante callbacks o eventos.
* Mantener una máquina de estados explícita.
* Permitir cancelación asíncrona.
* Poder ser ejecutado por el daemon.

## Codex Tasks

* Definir límites del motor.
* Revisar ownership de memoria.
* Revisar máquina de estados.
* Aprobar API pública.

## Claude Tasks

* Extraer lógica existente.
* Eliminar dependencias del CLI.
* Crear pruebas unitarias.
* Mantener compatibilidad con streaming actual.

## Acceptance Criteria

* El motor puede probarse sin iniciar CLI.
* No depende de sockets ni UI.
* La cancelación manual y eléctrica están diferenciadas.
* Las pruebas existentes siguen funcionando.

---

# Milestone 13 — Job Queue Model

## Goal

Implementar una cola de trabajos administrada por el daemon.

Solo puede existir un trabajo activo.

Puede haber múltiples trabajos pendientes.

## Job States

```c
enum job_state {
    JOB_STATE_QUEUED,
    JOB_STATE_STARTING,
    JOB_STATE_PRINTING,
    JOB_STATE_PAUSED,
    JOB_STATE_COMPLETED,
    JOB_STATE_CANCELLED_OPERATOR,
    JOB_STATE_CANCELLED_POWER_FAILURE,
    JOB_STATE_FAILED_SERIAL,
    JOB_STATE_FAILED_PRINTER,
    JOB_STATE_REMOVED
};
```

## Job Model

```c
struct print_job {
    char id[64];
    char file_path[PATH_MAX];

    enum job_state state;

    size_t current_line;
    size_t total_lines;

    double progress_percent;

    time_t submitted_at;
    time_t started_at;
    time_t finished_at;

    char failure_reason[256];
};
```

## Operations

La cola debe soportar:

```text
enqueue
list
inspect
start
remove
cancel
pause
resume
move
```

El orden puede modificarse únicamente sobre trabajos pendientes.

## Queue Rules

* Un solo trabajo activo.
* Un trabajo completado no vuelve a cola automáticamente.
* Un trabajo cancelado por batería no vuelve a cola automáticamente.
* El daemon no inicia el siguiente trabajo después de una cancelación eléctrica hasta recibir una orden explícita.
* La cola debe conservarse entre reinicios.

## Acceptance Criteria

* Se pueden encolar varios trabajos.
* Solo uno puede estar activo.
* El orden se conserva.
* Los estados son persistentes.
* Una cancelación eléctrica bloquea el inicio automático del siguiente trabajo.

---

# Milestone 14 — Text Persistence Layer

## Goal

Persistir cola, trabajos y eventos usando archivos de texto.

No utilizar SQLite en esta fase.

## Directory Layout

```text
/var/lib/printerctl/
├── jobs/
│   ├── queue.txt
│   ├── active.txt
│   └── history.txt
├── events/
│   └── events.log
├── sessions/
│   └── job-000042.log
└── gcode/
```

## Format Strategy

Usar un formato line-oriented controlado por el proyecto.

Ejemplo:

```text
JOB|job-000042|queued|/var/lib/printerctl/gcode/part.gcode|1721500000
```

Historial:

```text
STATE|job-000042|printing|1721500200
STATE|job-000042|cancelled_power_failure|1721500400
```

No usar delimitadores sin mecanismo de escape.

Debe definirse una función de escape o un campo de longitud.

## Persistence Requirements

* Escritura mediante archivo temporal.
* `fsync` antes de reemplazar estado crítico.
* Reemplazo mediante `rename`.
* Reconstrucción al iniciar.
* Detección de registros truncados.
* Registro append-only para eventos.
* Límites de tamaño documentados.

## Codex Tasks

* Diseñar formato textual.
* Definir estrategia de atomicidad.
* Revisar recuperación ante archivos incompletos.
* Definir ownership y locking.

## Claude Tasks

* Implementar parser y serializer.
* Implementar escritura atómica.
* Implementar carga al iniciar.
* Implementar tests de corrupción parcial.

## Acceptance Criteria

* La cola sobrevive reinicios.
* El historial sobrevive reinicios.
* Un archivo truncado no provoca comportamiento indefinido.
* No se pierde el último estado confirmado en condiciones normales.

---

# Milestone 15 — Unix Socket Protocol Design

## Goal

Definir el protocolo interno entre daemon e interfaces.

El socket será de sistema:

```text
/run/printerctl/control.sock
```

## Transport

Usar:

```c
AF_UNIX
SOCK_STREAM
```

## Protocol Style

Usar tramas inspiradas en Redis RESP.

No usar JSON dentro del socket Unix.

El gateway web será responsable de traducir:

```text
WebSocket JSON ↔ internal RESP-like protocol
```

## Frame Types

Propuesta inicial:

```text
+ simple string
- error
: integer
$ bulk string
* array
```

Ejemplos:

```text
*1\r\n
$4\r\n
PING\r\n
```

Respuesta:

```text
+PONG\r\n
```

Encolar:

```text
*3\r\n
$7\r\n
ENQUEUE\r\n
$25\r\n
/path/to/example.gcode\r\n
$8\r\n
priority\r\n
```

Estado:

```text
*1\r\n
$6\r\n
STATUS\r\n
```

## Protocol Rules

* Todos los mensajes deben tener longitud explícita.
* No depender de terminadores dentro del payload.
* Definir tamaño máximo de trama.
* Definir número máximo de argumentos.
* Definir timeout por cliente.
* Definir respuestas de error.
* Versionar el protocolo.

## Base Commands

```text
PING
VERSION
STATUS
PRINTER.INFO
PRINTER.TEMP
PRINTER.POSITION

JOB.ENQUEUE
JOB.LIST
JOB.GET
JOB.REMOVE
JOB.MOVE
JOB.START

PRINT.PAUSE
PRINT.RESUME
PRINT.CANCEL

EVENT.SUBSCRIBE
EVENT.UNSUBSCRIBE
```

## Acceptance Criteria

* Existe especificación formal en Markdown.
* Parser probado con tramas fragmentadas.
* Parser probado con múltiples tramas en un mismo buffer.
* Payloads inválidos se rechazan.
* Límites de memoria están definidos.
* El protocolo no depende de JSON.

---

# Milestone 16 — Unix Socket Server

## Goal

Implementar el servidor IPC dentro de `printerctld`.

## Runtime Path

```text
/run/printerctl/control.sock
```

## Ownership

Como el sistema será de un solo usuario:

```text
owner: printerctl
group: printerctl
mode: 0600
```

El servicio web deberá acceder mediante una configuración explícita de usuario compartido o montaje con permisos compatibles.

Si el gateway corre con otro UID, deberá utilizarse grupo:

```text
owner: printerctl
group: printerctl-web
mode: 0660
```

## Responsibilities

* crear directorio runtime;
* eliminar socket obsoleto;
* bind;
* listen;
* accept;
* parsear tramas;
* despachar comandos;
* responder;
* gestionar clientes desconectados;
* publicar eventos.

## Event Loop

Preferir:

```text
epoll
├── Unix listening socket
├── connected clients
├── signal handling
├── timers
└── print-engine notifications
```

## Acceptance Criteria

* Varios clientes pueden conectarse.
* El daemon limita el número de clientes.
* Un cliente lento no bloquea la impresión.
* Las tramas parciales se reconstruyen.
* Las conexiones inválidas se cierran de forma segura.
* Los permisos del socket son correctos.

---

# Milestone 17 — Daemon Lifecycle

## Goal

Crear `printerctld` como servicio persistente.

## Process Model

No implementar double-fork por defecto.

El proceso debe ejecutarse en foreground y ser administrado por systemd.

## Binary

```text
/usr/bin/printerctld
```

## Configuration

```text
/etc/printerctl/printerctl.conf
```

## State

```text
/var/lib/printerctl
```

## Runtime

```text
/run/printerctl
```

## Logs

Usar inicialmente:

```text
journald
```

y conservar logs por trabajo en:

```text
/var/lib/printerctl/sessions/
```

## Signals

```text
SIGTERM
    detener aceptación de comandos
    cancelar o cerrar sesión según política
    persistir estado
    cerrar socket
    cerrar serial
    terminar

SIGINT
    mismo comportamiento en desarrollo

SIGHUP
    recargar configuración permitida

SIGPIPE
    ignorar o manejar sin finalizar el daemon
```

## systemd Unit

Crear:

```text
packaging/systemd/printerctld.service
```

## Acceptance Criteria

* systemd puede iniciar y detener el servicio.
* El socket se crea al iniciar.
* El socket se elimina al terminar.
* El estado se reconstruye al reiniciar.
* El daemon no duplica instancias.
* El puerto serial solo es abierto por el daemon.

---

# Milestone 18 — CLI as Unix Socket Client

## Goal

Convertir `printerctl` en cliente exclusivo del daemon.

El CLI no debe abrir directamente el puerto serial.

## Commands

```text
printerctl status
printerctl queue list
printerctl queue add file.gcode
printerctl queue remove JOB_ID
printerctl queue move JOB_ID POSITION
printerctl start JOB_ID
printerctl pause
printerctl resume
printerctl cancel
printerctl temp
printerctl position
printerctl info
```

## Behavior

```text
CLI
  ↓ RESP-like protocol
Unix socket
  ↓
printerctld
```

## Restrictions

* Sin modo directo por defecto.
* No abrir `/dev/tty*`.
* No duplicar lógica de negocio.
* No modificar archivos de persistencia directamente.
* No enviar JSON al daemon.

## Acceptance Criteria

* Todos los comandos operan por socket.
* El daemon ausente produce error claro.
* Los códigos de salida están documentados.
* El CLI no requiere privilegios innecesarios.
* Las pruebas usan un daemon simulado o socket de prueba.

---

# Milestone 19 — APCUPSD Integration Through Unix Socket

## Goal

Migrar el script de batería baja para utilizar el protocolo definitivo.

## Flow

```text
APCUPSD
   ↓ executes
printerctl-low-battery.sh
   ↓
printerctl power low-battery
   ↓ Unix socket
printerctld
   ↓
cancel active print
```

## New Command

```text
POWER.LOW_BATTERY
```

o mediante CLI:

```bash
printerctl power low-battery
```

## Requirements

* El script debe usar el cliente CLI.
* Debe tener timeout.
* Debe registrar fallo si el daemon no responde.
* El comando debe ser idempotente.
* Repetir la señal no debe corromper el estado.
* No iniciar el siguiente trabajo.

## Acceptance Criteria

* La señal llega por socket.
* El trabajo activo se cancela una sola vez.
* Múltiples notificaciones son seguras.
* No existe comunicación directa con serial.
* No existe auto-resume.

---

# Milestone 20 — Event Subscription

## Goal

Permitir que TUI y gateway web reciban cambios en tiempo real.

## Events

```text
printer.state
printer.temperature
printer.position

job.queued
job.started
job.progress
job.paused
job.resumed
job.completed
job.cancelled
job.failed

queue.changed
power.low_battery
daemon.shutdown
```

## Subscription Model

Cliente:

```text
EVENT.SUBSCRIBE job.* printer.* queue.changed
```

El servidor mantiene la conexión abierta y envía eventos RESP-like.

## Requirements

* Backpressure.
* Límite de cola por cliente.
* Desconexión de clientes excesivamente lentos.
* Secuencia o timestamp por evento.
* Eventos no deben bloquear el motor de impresión.

## Acceptance Criteria

* Un cliente puede suscribirse.
* Recibe progreso en tiempo real.
* Un cliente lento no bloquea el daemon.
* El daemon elimina correctamente suscripciones desconectadas.

---

# Milestone 21 — TUI Foundation in C

## Goal

Crear una interfaz terminal en C que controle el daemon.

## Binary

```text
printerctl-tui
```

## Initial Views

```text
Dashboard
Queue
Current Job
Printer Status
Recent Events
Logs
```

## Initial Actions

```text
enqueue
start
pause
resume
cancel
remove
reorder
refresh
quit
```

## Architecture

```text
TUI
├── socket client
├── protocol decoder
├── local view model
├── event subscriber
└── terminal renderer
```

## Library Decision

Preferir `ncurses` o `ncursesw`.

No mezclar lógica del daemon dentro de la TUI.

## Acceptance Criteria

* La TUI se conecta al socket.
* Muestra cola y trabajo activo.
* Actualiza progreso por eventos.
* Puede pausar, reanudar y cancelar.
* La desconexión del daemon se muestra claramente.
* La TUI no abre el puerto serial.

---

# Milestone 22 — WebSocket Gateway

## Goal

Crear un gateway que traduzca entre WebSocket JSON y el protocolo Unix interno.

## Responsibilities

```text
WebSocket JSON
    ↕
request validation
authorization boundary
command translation
event translation
    ↕
RESP-like Unix protocol
```

## JSON Request Example

```json
{
  "id": "request-42",
  "command": "job.enqueue",
  "params": {
    "file": "/jobs/example.gcode"
  }
}
```

## Internal Translation

```text
*2\r\n
$11\r\n
JOB.ENQUEUE\r\n
$19\r\n
/jobs/example.gcode\r\n
```

## JSON Response Example

```json
{
  "id": "request-42",
  "ok": true,
  "result": {
    "job_id": "job-000042",
    "state": "queued"
  }
}
```

## Event Example

```json
{
  "event": "job.progress",
  "data": {
    "job_id": "job-000042",
    "progress": 42.5
  }
}
```

## Docker Requirements

El gateway se ejecutará dentro de Docker.

Debe montar:

```text
/run/printerctl/control.sock
```

Ejemplo conceptual:

```yaml
services:
  printerctl-web:
    volumes:
      - /run/printerctl/control.sock:/run/printerctl/control.sock
```

El contenedor debe ejecutar con un UID/GID autorizado para acceder al socket.

## Security Boundary

El gateway debe:

* validar comandos;
* limitar tamaño de mensajes;
* rechazar comandos desconocidos;
* impedir rutas arbitrarias;
* no exponer comandos Marlin sin filtrar;
* implementar autenticación si sale de localhost;
* limitar frecuencia de solicitudes;
* manejar clientes WebSocket lentos.

## Acceptance Criteria

* JSON se traduce a tramas internas.
* Eventos internos se convierten en JSON.
* El gateway no controla serial.
* El contenedor accede al socket con permisos mínimos.
* Entradas inválidas se rechazan.
* El daemon permanece funcional si el gateway falla.

---

# Milestone 23 — Vue.js Web Interface

## Goal

Crear una interfaz web para controlar el daemon mediante el gateway WebSocket.

## Views

```text
Dashboard
Queue
Current Print
Printer Status
Job History
Power Events
Settings
```

## Features

* conectar y reconectar WebSocket;
* mostrar estado del daemon;
* mostrar trabajo activo;
* mostrar progreso;
* mostrar temperatura;
* listar cola;
* agregar trabajo;
* eliminar trabajo pendiente;
* reordenar cola;
* iniciar;
* pausar;
* reanudar;
* cancelar;
* mostrar fallo de batería;
* mostrar historial.

## Restrictions

* La interfaz no debe asumir que una orden fue aplicada hasta recibir confirmación.
* No debe modificar estado crítico solo localmente.
* Debe reconciliar estado al reconectar.
* No debe enviar G-code arbitrario directamente.

## Acceptance Criteria

* La UI se recupera de desconexiones.
* El estado se obtiene nuevamente al reconectar.
* Los eventos actualizan la interfaz.
* Los comandos muestran éxito o error.
* La cola refleja el estado real del daemon.

---

# Milestone 24 — Job File Ingestion

## Goal

Permitir que TUI y web agreguen archivos G-code de forma controlada.

## Canonical Directory

```text
/var/lib/printerctl/gcode/
```

## Rules

* Solo aceptar archivos dentro del directorio autorizado.
* Resolver `realpath`.
* Rechazar symlinks fuera del directorio.
* Rechazar path traversal.
* Definir tamaño máximo.
* Validar extensión.
* Ejecutar dry-run antes de encolar.
* Calcular identificador o hash.
* Evitar colisiones de nombres.

## Web Upload

El gateway web debe:

1. recibir archivo;
2. almacenarlo temporalmente;
3. validar tamaño;
4. moverlo al directorio autorizado;
5. solicitar al daemon que lo registre;
6. eliminar temporales tras fallo.

## Acceptance Criteria

* No pueden encolarse rutas arbitrarias.
* Archivos inválidos se rechazan.
* La carga web no escribe fuera del directorio autorizado.
* El daemon valida nuevamente la ruta.

---

# Milestone 25 — Queue Recovery and Startup Policy

## Goal

Definir qué ocurre cuando el daemon reinicia.

## Startup Rules

Al iniciar:

* cargar cola persistida;
* validar archivos pendientes;
* marcar trabajos previamente activos como interrumpidos;
* no reanudar automáticamente;
* no iniciar automáticamente el siguiente trabajo;
* esperar una orden explícita.

## Interrupted State

Agregar:

```c
JOB_STATE_INTERRUPTED_DAEMON_RESTART
```

## Recovery Flow

```text
daemon starts
    ↓
loads state
    ↓
finds previous active job
    ↓
marks interrupted
    ↓
publishes recovery event
    ↓
waits for operator
```

## Acceptance Criteria

* Un reinicio no reanuda impresión.
* El trabajo anterior queda registrado.
* La cola pendiente se conserva.
* El operador puede decidir cómo continuar.

---

# Milestone 26 — Packaging and Deployment

## Goal

Preparar instalación reproducible.

## Deliverables

```text
packaging/
├── systemd/
│   └── printerctld.service
├── tmpfiles/
│   └── printerctl.conf
├── sysusers/
│   └── printerctl.conf
├── udev/
│   └── 99-printerctl.rules
└── arch/
    └── PKGBUILD
```

## Docker Deliverables

```text
docker/
├── web-gateway/
│   └── Dockerfile
├── frontend/
│   └── Dockerfile
└── compose.yaml
```

## Documentation

```text
docs/Installation.md
docs/Daemon.md
docs/SocketProtocol.md
docs/WebGateway.md
docs/TUI.md
docs/APCUPSD.md
docs/Rollback.md
```

## Acceptance Criteria

* El daemon puede instalarse como servicio.
* Los directorios se crean con permisos correctos.
* Docker Compose inicia web y gateway.
* El socket se comparte de forma segura.
* Existe procedimiento de rollback.

---

# Milestone 27 — Full System Acceptance

## Goal

Validar la arquitectura completa.

## Test Scenario

```text
1. Iniciar printerctld.
2. Conectar CLI.
3. Conectar TUI.
4. Conectar gateway web.
5. Abrir Vue UI.
6. Cargar G-code.
7. Encolar trabajo.
8. Iniciar impresión.
9. Observar progreso en TUI y web.
10. Pausar desde TUI.
11. Reanudar desde web.
12. Simular batería baja.
13. Verificar cancelación.
14. Verificar registro.
15. Reiniciar daemon.
16. Verificar recuperación sin auto-resume.
```

## Acceptance Criteria

* Solo el daemon abre serial.
* Todas las interfaces ven el mismo estado.
* La cola sobrevive reinicios.
* Los eventos llegan a TUI y web.
* La batería baja cancela el trabajo.
* No se inicia el siguiente trabajo.
* No existe reanudación automática.
* La caída de una interfaz no afecta la impresión.
* La caída del gateway no afecta CLI ni TUI.
* La documentación coincide con el comportamiento.

---

# Backlog After Daemon MVP

1. Múltiples impresoras.
2. Persistencia SQLite.
3. Reanudación manual asistida.
4. Marlin checksums y `Resend:`.
5. TLS para gateway remoto.
6. Roles y usuarios.
7. Métricas Prometheus.
8. Historial gráfico.
9. Cámara de impresión.
10. Integración con OctoPrint-like plugins.
11. Socket activation mediante systemd.
12. Plugin system.
13. Mobile UI.
14. Notificaciones externas.
15. Estimación de tiempo restante.
