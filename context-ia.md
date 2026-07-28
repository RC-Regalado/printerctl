---
title: Context IA - Firmware Compatibility Layer
tags: [printerctl, firmware, marlin, compatibility, agent-context]
---

# Context IA — Capa de compatibilidad en firmware

Este documento entrega contexto a un segundo agente encargado de
modificar el firmware de la impresora para crear una "capa de
compatibilidad" fuera del estándar Marlin, sin romper el cliente host
`printerctl`.

## 1. Resumen ejecutivo

`printerctl` es el cliente host. Habla por USB serial con una impresora
Marlin y, en el estado actual, asume un protocolo simple:

```text
host envía 1 línea G-code
firmware responde 0..N líneas informativas
firmware termina con ok, Error: o Resend:
host envía la siguiente línea solo si recibió ok
```

La capa de compatibilidad del firmware puede agregar comandos o
respuestas propietarias, pero debe mantener este contrato para no romper
el streaming seguro del host.

## 2. Estado actual del host

Implementado hasta Milestone 10 más la actualización de end-stops
(2026-07-21, ver `Agents.md`):

- CLI y configuración.
- Logs estructurados en `stderr`.
- Parser G-code con `--dry-run`.
- Parser de respuestas Marlin.
- Transporte serial Linux.
- Comandos únicos: `M105`, `M114`, `M115`, `send`, `home` (todos los
  ejes o uno solo).
- Streaming `print FILE` con un solo comando en vuelo, con
  pausa/reanudación/cancelación (operador vía señales, o batería baja
  vía `SIGPWR`).
- Registro de sesiones interrumpidas (write-only, nunca leído de vuelta).

No implementado aún:

- checksums;
- números de línea;
- recuperación automática de `Resend:`;
- daemon/socket Unix (planificado, Fase II del roadmap revisado).

## 3. Reglas que el firmware no debe romper

### 3.1 Ack obligatorio

Todo comando que el firmware acepte debe terminar con:

```text
ok
```

o:

```text
ok <payload>
```

Sin `ok`, `printerctl` esperará hasta timeout y detendrá el print.

### 3.2 Errores terminales

Errores deben usar:

```text
Error:<mensaje>
```

El host detiene el streaming, muestra el mensaje al operador y marca la
sesión como fallida.

### 3.3 No depender de Resend

El host detecta:

```text
Resend:<n>
```

pero todavía no puede reenviar líneas. Para el MVP, `Resend:` es un
fallo terminal. La capa de compatibilidad no debe requerir line numbers,
checksums ni resend para funcionar.

### 3.4 Información extra permitida

El firmware puede emitir líneas informativas antes del `ok`:

```text
echo:...
busy:...
Cap:...
compat:...
```

`printerctl` las loguea y sigue esperando. Esto es la vía más segura
para exponer metadata propietaria sin modificar el host.

## 4. Formatos que el host parsea explícitamente

### 4.1 Temperatura — `M105`

Debe conservar un formato compatible con Marlin:

```text
ok T:27.50 /0.00 B:25.00 /0.00 @:0 B@:0
```

El payload puede venir en el mismo `ok` o en una línea informativa antes
del `ok`, pero mantener el formato Marlin reduce cambios en el host.

### 4.2 Posición — `M114`

Formato esperado:

```text
X:0.00 Y:0.00 Z:0.00 E:0.00 Count X:0 Y:0 Z:0
ok
```

### 4.3 Firmware info — `M115`

Debe incluir `FIRMWARE_NAME:` y terminar con `ok`:

```text
FIRMWARE_NAME:Marlin 2.1.2.1 PROTOCOL_VERSION:1.0 MACHINE_TYPE:3D Printer EXTRUDER_COUNT:1
Cap:...
ok
```

La compatibilidad propietaria puede anunciarse con líneas `Cap:` o
líneas informativas nuevas antes del `ok`.

## 5. Propuesta mínima para la capa propietaria

La forma menos invasiva es agregar comandos `M` propietarios que sigan
siendo line-oriented y ack-driven.

Ejemplo:

```text
M9000
compat:layer=printerctl version=1
compat:feature=safe_streaming
ok
```

Reglas:

- prefijar metadata propietaria con `compat:` o `echo:compat:`;
- no cambiar la semántica de `ok`;
- no cambiar `M105`, `M114`, `M115` de forma incompatible;
- no exigir al host que mande múltiples comandos para completar un ack;
- no emitir prompts interactivos que bloqueen el flujo;
- no hacer acciones físicas por efecto colateral de comandos de consulta.

## 6. Seguridad de hardware

Desde 2026-07-21 la máquina objetivo tiene end-stops min habilitados en
los tres ejes (antes solo Z-min). Para pruebas automáticas o smoke
tests desde `printerctl`, usar solo comandos inocuos:

```text
M105
M114
M115
G4 P250
G28          (solo a través del comando 'home' dedicado)
G28 X / Y / Z
```

Evitar sin autorización explícita:

```text
G28          (fuera del comando 'home': send/print lo rechazan igual)
G0
G1
M104
M109
M140
M190
M112
```

`M112` solo debe pasar si el operador lo envía explícitamente con
`send`; el software no lo genera automáticamente.

## 7. Qué debe entregar el agente de firmware

Para cada cambio de firmware, entregar:

- comandos nuevos o modificados;
- ejemplo exacto de request/response serial;
- si el comando es terminal con `ok`, `Error:` o `Resend:`;
- si emite `busy:` durante operaciones largas;
- riesgos de seguridad física;
- cómo probarlo sin movimiento, calor ni extrusión;
- impacto esperado en `printerctl`.

## 8. Criterios de aceptación de compatibilidad

Un cambio de firmware es compatible con el host actual si:

1. `printerctl --port <dev> temp` sigue funcionando.
2. `printerctl --port <dev> position` sigue funcionando.
3. `printerctl --port <dev> info` sigue funcionando.
4. `printerctl --port <dev> print safe.gcode` recibe un `ok` por cada comando.
5. líneas propietarias nuevas no impiden llegar al `ok`.
6. errores físicos o lógicos usan `Error:<mensaje>`.
7. no aparece `Resend:` salvo que se acepte detener el print.

## 9. Recomendación operativa

Primero agregar solo una consulta de capacidades propietaria, sin acción
física:

```text
M9000
```

Respuesta sugerida:

```text
compat:printerctl=1
compat:transport=line_ack
compat:safety=no_auto_motion
ok
```

Esto permite validar parsing, logging y tolerancia a líneas
informativas sin tocar motores, heaters ni endstops.
