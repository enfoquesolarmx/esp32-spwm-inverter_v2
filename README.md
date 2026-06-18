# Bitácora — Calibración de bases de tiempo y blindaje de la conmutación

> Jornada de depuración del inversor SPWM de puente H (ESP32 / MCPWM).
> El día que descubrimos que el periférico tiene **dos bases de tiempo distintas**,
> que el "error" del dead-time nunca existió, y que medir nos salvó de romper lo que
> ya funcionaba.

**Fecha:** 2026-06-18
**Rol del equipo:** validador (mide en banco con Saleae) + auditor (revisa el código).
**Método, una vez más:** medir antes de creer. Ninguna base de tiempo se asume; se mide en el pin.

---

## Resumen ejecutivo

Esta sesión empezó como migración de entorno y terminó destapando el cabo más profundo del
proyecto: el módulo de dead-time del MCPWM **no cuenta en la misma base de tiempo que el timer
principal**. Esa única confusión, oculta desde el origen, explicaba por qué "nada cuadraba del
todo". El giro: el valor original (80 ticks) siempre fue correcto; el error lo introdujimos al
intentar "corregirlo" sobre una base equivocada. La medición de un pulso de prueba lo reveló y
nos salvó de dejar el inversor con un dead-time inservible.

Además se cerró la migración a PlatformIO, se exploró la API moderna (`mcpwm_prelude.h`) sin
adoptarla aún, se construyó una **arquitectura de base de tiempo paramétrica** (single source of
truth) que fue la herramienta que expuso el error, y se blindó la conmutación cerca del cruce con
**supresión de pulso mínimo**.

---

## 1. Migración a PlatformIO

Se portó el inversor validado de Arduino IDE a PlatformIO (Debian/Linux, framework Arduino sobre
ESP-IDF). Lo que la transición exigió, y que rompe un sketch si se ignora:

- **Anclar el core a 3.x / IDF v5.x** en `platformio.ini`. El proyecto usa `driver/gptimer.h` y el
  fault handler, que no existen en core 2.x. Si PlatformIO instala el core por defecto, la
  compilación falla por includes ausentes.
- **`#include <Arduino.h>` explícito** y **prototipos de funciones** al inicio. Arduino IDE los
  autogenera; PlatformIO (C++ estricto) no.
- **`IRAM_ATTR` solo en la definición, no en el prototipo.** Ponerlo en ambos genera secciones
  `.iram1` en conflicto (warning `-Wattributes`). Convención: el atributo va en la definición.

Resultado: compila limpio. La lógica física no cambió ni una constante — la migración es de
entorno, no de diseño.

---

## 2. Exploración de la API moderna (`mcpwm_prelude.h`)

Se evaluó migrar de la API legacy (`mcpwm.h`) a la moderna sin adoptarla todavía. Hallazgos:

- En **prelude** el reloj fuente por defecto del timer es **PLL160M (160 MHz)**, no el APB de
  80 MHz de legacy. El driver fija el divisor internamente según fuente y resolución pedida.
- Si la resolución pedida no divide exacto al reloj fuente, el hardware **da la más cercana
  divisible**, no la exacta. Por eso una capa de configuración debe reportar la resolución *real*
  resultante, no asumir la pedida.
- Ventajas de prelude para este proyecto: el shadow en TEP es un flag nombrado
  (`update_cmp_on_tep`) en vez de escritura de registro cruda; el fault handler es un objeto con
  API clara; desaparecen las direcciones de registro mágicas.
- Costo: reescritura sustancial (cadena de objetos timer→operador→comparador→generador→dead-time→
  fault) y **re-validar toda la base de tiempo** desde cero.

**Decisión:** dejar el legacy validado en producción. Migrar a prelude cuando se aborde la Fase B
del roadmap (dead-time adaptativo, sobrecorriente por fault real), donde prelude da mejores
herramientas y la reescritura se amortiza.

---

## 3. Arquitectura de base de tiempo paramétrica (single source of truth)

Se adoptó una filosofía de diseño estilo Unix: **ninguna constante se expresa en ticks
hardcodeados.** Todo se declara en tiempo físico (ns, µs, Hz) y se deriva a ticks en un único
punto, por fórmula. Cambiar la portadora o el tick recalcula dead-time, guarda, período y amplitud
de forma coherente, con reporte serial de cada conversión.

Esto no solo mantiene la coherencia: **expone las incoherencias escondidas.** Fue exactamente la
herramienta que destapó el problema del dead-time (sección 4). Cuando todo deriva de un punto, un
número equivocado no tiene dónde esconderse.

Verificación de que no rompe nada: la capa reproduce exactamente los valores ya medidos
(período 399 a 20 kHz, amplitud 90%, sampleNum), confirmando que la parametrización es equivalente
al código validado, solo que mantenible.

---

## 4. La saga del dead-time (el hallazgo central del día)

### Acto 1 — el "error" aparente
Al parametrizar, el dead-time de "80 ticks" parecía descalibrado: asumiendo la base del timer
(62.5 ns), 80 × 62.5 = 5000 ns = 5 µs, un 11% del período de portadora. Diez veces lo intencionado.
Se "corrigió" a 8 ticks creyendo que darían 500 ns.

### Acto 2 — "parece 0"
El validador midió y el dead-time casi desapareció (~50 ns). Los 8 ticks no daban 500 ns. Algo no
encajaba. Sospecha: el módulo dead-time no cuenta en la base del timer principal.

### Acto 3 — la medición que lo resolvió
Test con un número fijo y conocido de ticks (48), midiendo el tiempo resultante en el analizador:

```
48 ticks  →  286 ns medidos
286 / 48  =  5.96 ≈ 6.25 ns/tick  →  base del dead-time = 160 MHz
```

**El módulo de dead-time cuenta a 6.25 ns/tick (160 MHz), diez veces más rápido que el timer
principal (62.5 ns/tick).** Son dos relojes distintos en el mismo periférico. La documentación de
ESP-IDF no lo especifica con claridad para la API legacy — razón de más para medirlo.

### El giro irónico
Los "80 ticks" originales eran 80 × 6.25 = **500 ns correctos desde el inicio**. El dead-time nunca
estuvo descalibrado. El error lo introdujimos nosotros al asumir la base equivocada y bajar a 8
ticks (= 50 ns ≈ "parece 0"). La medición nos salvó de "arreglar" algo que ya funcionaba.

### La lección
El error más insidioso viene disfrazado de diligencia: "corregir" lo que no estaba roto. El cimiento
que creíamos torcido estaba a nivel; lo que faltaba era saber en qué regla estaba medido. Desde
entonces el código declara **dos bases de tiempo por separado**, cada una con su conversor
(`NS_TO_TICKS` para el timer, `NS_TO_TICKS_DT` para el dead-time), ambas medidas y verificadas en
el pin.

### Verificación final
Con el conversor calibrado, el dead-time objetivo se traduce a ticks correctos y se confirmó en el
analizador: separación D1→D0 medida **precisa**, con ambos canales en bajo durante la banda muerta
(sin shoot-through). Cerrado en tres niveles: fórmula, base medida, y pin.

---

## 5. Supresión de pulso mínimo (blindaje de la conmutación)

**Problema:** cerca del cruce por cero, el duty se vuelve tan pequeño que el pulso resultante sería
más estrecho que el dead-time total (RED + FED). El hardware no puede formarlo limpio y sale
deforme — un pulso "solitario sin su retorno" que viola el dead-time y solo calienta los
transistores sin entregar energía útil.

**Solución:** si el pulso sería más estrecho que el dead-time total más un margen, se **suprime**
(no se abre). Detalle fino que costó una iteración: el valor de "apagado" depende del **semiciclo**,
porque el duty del lado negativo se escribe con polaridad opuesta en el comparador. Suprimir con un
0 fijo dejaba el canal **en alto** en el semiciclo negativo (lo contrario de lo buscado). Corregido:
positivo → 0, negativo → `tmrRegVal`. Ambos dejan la salida en bajo.

**Control seguro:** el umbral se expone como un factor de margen ajustable (`PULSE_MARGIN`) con un
**piso físico protegido**: `fmaxf(PULSE_MARGIN, 1.0)` garantiza que el umbral nunca baje del
dead-time total, pase lo que pase con el valor que elija el usuario. Bajar de ahí reintroduciría
los pulsos deformes. Flexibilidad donde es seguro, protección donde es física.

Resultado medido: cerca del cruce, los pulsos cortos se suprimen limpiamente, el canal queda en
bajo, sin pulsos huérfanos. *"Se ve tan limpio, sin pulsos solitarios saltando sin su retorno."*

---

## Estado del inversor al cierre de la jornada

Todo medido en el pin, ninguna base de tiempo asumida:

- [x] Cruce por cero limpio (fault handler + GPTimer, guarda estable por enganche de fase a 20 kHz)
- [x] Base de tiempo del **timer** verificada en 3 niveles (cálculo = registro = pin, pulso 39 µs)
- [x] Base de tiempo del **dead-time** medida y calibrada (6.25 ns/tick, 160 MHz) — distinta del timer
- [x] Dead-time de portadora medido preciso en el pin, sin shoot-through
- [x] Supresión de pulso mínimo, con piso físico protegido
- [x] Arquitectura paramétrica (single source of truth) — cambiar un parámetro recalcula todo
- [x] Migración a PlatformIO funcional
- [x] Código documentado: resumen de la saga + comentarios pedagógicos por sección

---

## Lecciones de la jornada

**Un periférico puede tener varios relojes.** El MCPWM cuenta el timer en una base y el dead-time
en otra. Asumir que "ticks" significa lo mismo en todos los submódulos fue la raíz de la confusión.
Cada base de tiempo se mide por separado.

**"Corregir" sin medir es más peligroso que no tocar.** El dead-time funcionaba; nuestra corrección
lo rompió. La señal de alarma ("parece 0") y la disciplina de medir antes de confiar evitaron dejar
el inversor peor que como estaba.

**La parametrización expone, no solo organiza.** Forzar a declarar cada constante en su valor físico
hizo visible una incoherencia que llevaba el proyecto entero escondida. La arquitectura no fue
sobre-ingeniería: fue el instrumento que reveló el error.

**La flexibilidad debe tener pisos físicos.** Dar control al usuario es bueno; dejar que ese control
reintroduzca un bug no. La diferencia entre un parámetro útil y uno peligroso es saber cuál límite
es preferencia y cuál es física.

---

## Pendientes (roadmap)

**Fase A — cierre del núcleo (caminar firme)**
- [ ] Verificar enganche de fase a 50 Hz tras usar la rampa de frecuencia
- [ ] Validar el cruce con carga inductiva real (ruta de freewheeling durante la guarda)

**Fase B — robustez de potencia (trotar)**
- [ ] Dead-time adaptativo por temperatura — ahora trivial: ya existe el conversor a 6.25 ns
- [ ] Protección de sobrecorriente por hardware reutilizando el fault handler
- [ ] Realimentación de tensión de salida (lazo cerrado)
- [ ] *Buen momento para migrar a la API prelude (mejores herramientas para estas funciones)*

**Fase C — sincronización y paralelo (correr)**
- [ ] PLL por software para enganchar la tabla de seno a una referencia externa
- [ ] Paralelo en isla con droop control (reparto de carga sin comunicación)
- [ ] Grid-tie con anti-islanding (opcional, regulado)
- [ ] (por definir)

---

*Bitácora de un día donde el inversor no ganó funciones nuevas, sino algo más valioso: certeza.
Cada tiempo, medido en su regla correcta. Cada cimiento, nivelado de verdad.*
