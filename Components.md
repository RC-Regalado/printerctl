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
con firmware Marlin por USB serial. El MVP actual llega hasta el
Milestone 7: puede leer archivos G-code, normalizarlos y transmitirlos
línea por línea esperando `ok` antes de enviar el siguiente comando.

La prioridad del host es seguridad y observabilidad, no throughput:

- no hace homing automático;
- no calienta, mueve ejes ni extruye salvo acción explícita del operador;
- no abre serial en `--dry-run`;
- detiene el streaming ante `Error:`, `Resend:`, timeout o desconexión;
- registra TX/RX, progreso, errores y cambios de estado.

## 2. Vista general

```text
CLI
 |
 v
Config + Logger
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
```

## 3. Componentes del host

| Componente | Ruta | Responsabilidad |
|---|---|---|
| CLI | `src/cli`, `include/printerctl/cli.h` | Parsear argumentos, validar intención del operador, mapear errores a exit codes y renderizar salida humana. |
| Config | `src/config`, `include/printerctl/config.h` | Resolver defaults, archivo `--config` y overrides de CLI. |
| Logging | `src/logging`, `include/printerctl/logging.h` | Emitir logs TSV estructurados con timestamp, niveles y eventos. |
| G-code | `src/gcode`, `include/printerctl/gcode.h` | Leer archivos regulares, normalizar líneas, remover comentarios, calcular progreso y bloquear `G28`. |
| Marlin | `src/marlin`, `include/printerctl/marlin.h` | Clasificar respuestas: `ok`, `Error:`, `busy:`, `echo:`, `Resend:` e info. Parsear `M105`, `M114`, `M115`. |
| Serial | `src/serial`, `include/printerctl/serial.h` | Abrir/configurar USB serial, escribir líneas, leer respuestas por línea, reportar timeout/I/O. |
| Printer | `src/printer`, `include/printerctl/printer.h` | Orquestar sesión: conectar, ejecutar comandos, consultar estado y hacer streaming confiable. |
| Tests | `tests` | Validar CLI, parser, serial con pseudo-terminales y mock Marlin. |
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
| `print FILE` | comandos normalizados del archivo | Implementado con un comando en vuelo. |
| `pause` | por definir en M8 | Aún no disponible. |
| `resume` | por definir en M8 | Aún no disponible. |
| `cancel` | por definir en M8 | Aún no disponible. |
| `home` | `G28` | Requiere confirmación en parser, ejecución aún no disponible. |

## 6. Seguridad relevante para firmware

La impresora objetivo es una EasyThreed K9 con controlador STM32 y
firmware Marlin. El hardware disponible solo posee home en Z-min. Por
esa razón, las pruebas automáticas con hardware real deben limitarse a
comandos inocuos como:

- `M105`
- `M114`
- `M115`
- `G4 P<ms>`

No se debe usar automáticamente:

- `G28`;
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
- Reanudación automática.
- Base de datos persistente de trabajos.
- Daemon host para controlar prints desde otro proceso.
- Alto throughput con ventana de comandos.
