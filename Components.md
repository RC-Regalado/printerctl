---
title: Components
tags: [printerctl, architecture, components, firmware-context]
---

# Components

Fuente de verdad: [[Agents]], [[architecture]], [[roadmap]], [[README]].

Este documento resume los componentes actuales de `printerctl` para un
segundo agente que modificará firmware y necesita entender qué espera el
cliente host.

## 1. Propósito del proyecto

`printerctl` es una CLI Linux escrita en C17 para controlar impresoras 3D
con firmware Marlin por USB serial. El estado actual llega hasta el
Milestone 10 (cancelación por batería baja) más una actualización de
hardware (2026-07-21: end-stops min habilitados en los tres ejes, ver
la enmienda "Home" en `Agents.md`). Puede leer archivos G-code,
normalizarlos, transmitirlos línea por línea esperando `ok` antes de
enviar el siguiente comando, pausar/reanudar/cancelar una sesión
(operador o SD), hacer home (todos los ejes o uno solo) y reaccionar a
una notificación externa de batería baja.

La prioridad del host es seguridad y observabilidad, no throughput:

- no hace homing automático (`home` es siempre una acción explícita del
  operador, nunca un efecto colateral de `send` o de un archivo);
- no calienta, mueve ejes ni extruye salvo acción explícita del operador;
- no abre serial en `--dry-run`;
- detiene el streaming ante `Error:`, `Resend:`, timeout, desconexión o
  una señal de batería baja — siempre de forma elegante, nunca enviando
  nada adicional al detenerse;
- nunca reanuda un trabajo automáticamente, ni tras una desconexión ni
  tras el regreso de la energía;
- registra TX/RX, progreso, errores y cambios de estado.

Ver `docs/safety.md` para el detalle completo de esta política y
`architecture.md` §7.5 para el mapeo de estados del ciclo de vida.

## 2. Vista general

```text
CLI
 |
 v
Config + Logger + Record
 |
 v
Printer session
 |        |        |
 v        v        v
G-code  Marlin   Serial
reader  parser   transport
                  |
                  v
             USB serial / firmware

Fuera de la sesión de impresión:
  scripts/apcupsd/printerctl-low-battery.sh
    -> printerctl power low-battery
    -> lee record.h's active-print marker
    -> SIGPWR al proceso que transmite (si existe)
```

## 3. Componentes del host

| Componente | Ruta | Responsabilidad |
|---|---|---|
| CLI | `src/cli`, `include/printerctl/cli.h` | Parsear argumentos, validar intención del operador, mapear errores a exit codes y renderizar salida humana. |
| Config | `src/config`, `include/printerctl/config.h` | Resolver defaults, archivo `--config` y overrides de CLI. |
| Logging | `src/logging`, `include/printerctl/logging.h` | Emitir logs TSV estructurados con timestamp, niveles y eventos. |
| G-code | `src/gcode`, `include/printerctl/gcode.h` | Leer archivos regulares, normalizar líneas, remover comentarios, calcular progreso y bloquear `G28`. |
| Marlin | `src/marlin`, `include/printerctl/marlin.h` | Clasificar respuestas: `ok`, `Error:`, `busy:`, `echo:`, `Resend:` e info. Parsear `M105`, `M114`, `M115`. |
| Serial | `src/serial`, `include/printerctl/serial.h` | Abrir/configurar USB serial, escribir líneas, leer respuestas por línea, reportar timeout/I/O, acceso exclusivo (`TIOCEXCL`). |
| Printer | `src/printer`, `include/printerctl/printer.h` | Orquestar sesión: conectar, ejecutar comandos, consultar estado, streaming confiable y control (pausa/reanudación/cancelación, operador o batería baja). |
| Record | `src/record`, `include/printerctl/record.h` | Persistir registros de sesiones interrumpidas (write-only, nunca leídos) y el marcador de PID del print activo. |
| Tests | `tests` | Validar CLI, parser, serial con pseudo-terminales y mock Marlin (ver `docs/testing.md`). |
| Scripts | `scripts/apcupsd` | Notificar a `printerctl` eventos de batería baja de APCUPSD (ver `docs/apcupsd.md`). |
| Docs | `docs`, raíz | Documentar decisiones por milestone y contratos operativos. |

## 4. Contrato serial esperado por `printerctl`

El host opera en modo line-oriented:

1. abre el puerto serial configurado, normalmente `115200`;
2. escribe un comando G-code como una línea completa;
3. espera líneas de respuesta;
4. considera el comando finalizado solo con respuesta terminal;
5. envía el siguiente comando únicamente después de `ok`.

Respuestas reconocidas:

| Respuesta firmware | Tratamiento host |
|---|---|
| `ok` | Ack terminal, continúa el streaming. |
| `ok <payload>` | Ack terminal con payload, usado por reportes como temperatura. |
| `Error:<mensaje>` | Fallo terminal, detiene la sesión. |
| `Resend:<n>` | Fallo terminal no soportado en MVP, detiene la sesión. |
| `busy:<mensaje>` | No terminal, reinicia la espera. |
| `echo:<mensaje>` | No terminal informativo. |
| otras líneas | Info no terminal, se loguean y se sigue esperando. |

El timeout mide silencio, no duración total del comando. Si el firmware
emite `busy:` o información periódica, el host seguirá esperando.

## 5. Comandos Marlin usados actualmente

| Comando host | G-code enviado | Estado actual |
|---|---|---|
| `temp` | `M105` | Implementado. |
| `position` | `M114` | Implementado. |
| `info` | `M115` | Implementado. |
| `send "CMD"` | texto explícito del operador | Implementado, bloquea `G28`. |
| `print FILE` | comandos normalizados del archivo | Implementado con un comando en vuelo; bloquea `G28`. |
| `pause`/`resume`/`cancel` | `M25`/`M24`/`M524` (impresión SD) | Implementado (Milestone 8). Un stream por host se controla con señales (SIGUSR1/SIGUSR2/SIGINT), no por estos comandos. |
| `home` | `G28` (o `home X`/`Y`/`Z` -> `G28 X`/`Y`/`Z`) | Implementado sin confirmación (end-stops min habilitados en los tres ejes, 2026-07-21). Requiere `--port`. |
| `power low-battery` | ninguno (nunca abre serial) | Implementado (Milestone 10). Envía `SIGPWR` al proceso que transmite, si existe. |

## 6. Seguridad relevante para firmware

La impresora objetivo es una EasyThreed K9 con controlador STM32 y
firmware Marlin. Desde 2026-07-21 el hardware tiene end-stops min
habilitados en los tres ejes (antes solo Z-min), lo que permitió
habilitar `home` sin confirmación — ver la enmienda "Home" en
`Agents.md`. Aun así, las pruebas automáticas con hardware real siguen
limitándose a comandos inocuos:

- `M105`
- `M114`
- `M115`
- `G4 P<ms>`
- `G28` (y sus variantes por eje) — solo a través del comando `home`
  dedicado, nunca automáticamente ni como parte de otra prueba

No se debe usar automáticamente (es decir, sin que el operador lo pida
explícitamente vía `home`/`send`/`print`):

- `G28` fuera de `home`;
- movimientos `G0`/`G1`;
- calentamiento;
- extrusión;
- `M112` salvo comando explícito del operador.

## 7. Implicaciones para una capa de compatibilidad en firmware

La capa de compatibilidad debe preservar, como mínimo, el contrato que el
host ya entiende:

- cada comando aceptado termina con un `ok`;
- errores terminan con `Error:<mensaje>`;
- esperas largas pueden emitir `busy:<mensaje>`;
- información extra puede emitirse antes del `ok`;
- `M105`, `M114` y `M115` deben seguir siendo parseables por el host;
- no debe requerir checksums, números de línea ni `Resend:` para el MVP.

Si se agrega comportamiento fuera del estándar Marlin, debe ser
compatible hacia atrás: el host puede ignorar líneas informativas nuevas,
pero no puede continuar si el firmware deja de emitir `ok`.

## 8. Fuera de alcance actual

- Recuperación real de `Resend:`.
- Checksums y números de línea.
- Reanudación automática (prohibida por política de seguridad, no solo
  "no implementada" — ver `docs/safety.md` §7).
- Alto throughput con ventana de comandos.

Planificado, pero no implementado todavía (Fase II del roadmap
revisado, Milestones 12+): un daemon host (`printerctld`) que posea el
puerto serial y una cola de trabajos persistente, con `printerctl`
convertido en cliente de un socket Unix. Esto sí está en el roadmap
ahora, a diferencia de cuando este documento se escribió originalmente;
ver `roadmap.md` y `docs/design/unix-socket-service-analysis.md`.
