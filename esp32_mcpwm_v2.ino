/*==============================================================================
  INVERSOR MONOFASICO SPWM DE PUENTE H — ESP32 (MCPWM legacy)
  Sintesis de cruce por cero limpio + base de tiempo parametrica autoverificable
  ==============================================================================

  QUE HACE ESTE PROGRAMA
  ----------------------
  Sintetiza una onda senoidal de 60 Hz (la "fundamental") usando modulacion SPWM
  unipolar sobre un puente H, controlado por un ESP32 clasico. La onda de salida
  se construye conmutando 4 transistores: dos rapidos (modulan la portadora de
  20 kHz) y dos lentos (relevan la polaridad cada semiciclo de la fundamental).

  El reto central NO es generar la SPWM (eso es relativamente directo), sino que
  el CRUCE POR CERO sea limpio: sin cortocircuito de rama (shoot-through), sin
  pulsos de polaridad equivocada, y con bandas muertas correctas. Este programa
  es el resultado de depurar ese cruce hasta entender cada microsegundo.

  ============================================================================
  RESUMEN DE LA SAGA (para quien estudia este codigo)
  ============================================================================
  Este programa no se escribio de una vez: es el destilado de dias de depuracion
  midiendo en analizador logico (Saleae). El metodo que lo hizo posible, repetido
  en cada problema: MEDIR ANTES DE CREER. Aislar una variable, descartar hipotesis
  por evidencia fisica, nunca asumir el comportamiento del hardware. Cada hallazgo
  abajo costo horas y se resolvio con un numero medido en un pin, no con teoria.

  PROBLEMA 1 — APAGAR EL BRAZO RAPIDO EN EL CRUCE (fault handler)
    Para limpiar el cruce hay que apagar HO1/LO1 (D0/D1) momentaneamente. Se probo
    duty=0 -> FALLO: el dead-time regenera el complemento aguas abajo. Se probo
    out_w1tc sobre el pin -> FALLO: el pin lo controla el MCPWM, no el registro GPIO.
    SOLUCION: el FAULT HANDLER en modo cycle-by-cycle actua en la ETAPA DE SALIDA,
    DESPUES del dead-time -> es el unico punto del pipeline que apaga D1 de verdad:
         generador -> dead-time -> [FAULT HANDLER] -> pin
    Se dispara enrutando un GPIO a la entrada de falla F0 por la matriz interna.

  PROBLEMA 2 — LA GUARDA FINA DEL CRUCE (GPTimer metodo libre)
    Se necesitaba una banda de guarda ajustable de ~microsegundos en el cruce.
    El GPTimer en modo one-shot (stop/reconfig/start) metia ~10us de latencia de
    arranque -> guarda imprecisa. SOLUCION: metodo "libre": el GPTimer arranca UNA
    vez y nunca se detiene; en cada cruce solo se mueve la alarma (now+ticks).
    Bajo la latencia de arranque a casi cero. (6 hipotesis previas cayeron al
    medirlas: residuo de contador, callback fuera de IRAM, reloj a 500kHz, etc.)

  PROBLEMA 3 — EL JITTER QUE ERA RESONANCIA (enganche de fase)
    La guarda "bailaba" entre dos valores (bimodal). NO era bug: era BATIDO DE FASE
    entre tres osciladores inconmensurables (portadora, fundamental, GPTimer). A
    23 kHz las fases deslizaban. Bajar la portadora a 20 kHz las ENGANCHO (relacion
    armonica) -> guarda estable. Bonus: 20 kHz es inaudible. El "bug" era armonia.

  PROBLEMA 4 — LA BASE DE TIEMPO DEL TIMER (verificada en 3 niveles)
    Se sospecho que el calculo de tmrRegVal usaba base equivocada (16 vs 80 MHz).
    Se MIDIO: calculo=registro=pin, los tres coinciden. Era FALSA ALARMA: el tick
    de 62.5ns viene de 80MHz/prescaler5, coincidencia numerica con 16MHz/1.
    Pulso pico medido: 39us = 90% del periodo. Confirmado en silicio.

  PROBLEMA 5 — EL DEAD-TIME Y SU BASE DE TIEMPO OCULTA (el giro)
    Al parametrizar, "80 ticks" de dead-time parecian ser 5us (asumiendo base del
    timer, 62.5ns). Se "corrigio" a 8 ticks creyendo que darian 500ns -> el
    dead-time casi desaparecio (~50ns, "parece 0"). Algo no encajaba.
    Test decisivo: 48 ticks fijos -> el analizador midio 286 ns.
         286 / 48 = 5.96 ~ 6.25 ns/tick  ->  EL DEAD-TIME CUENTA A 160 MHz.
    El modulo dead-time NO usa la base del timer: cuenta 10x mas rapido (6.25ns).
    GIRO IRONICO: los "80 ticks" originales eran 80x6.25 = 500ns CORRECTOS desde
    el inicio. Nunca estuvieron mal. El error lo introdujimos NOSOTROS al asumir
    la base equivocada. La medicion nos salvo de "arreglar" algo que ya funcionaba.
    LECCION: el cimiento que creiamos torcido estaba a nivel; faltaba saber en
    que regla estaba medido. Hay DOS bases de tiempo distintas en este periferico.

  PROBLEMA 6 — PULSOS ESTRECHOS QUE VIOLAN EL DEAD-TIME (supresion de pulso minimo)
    Cerca del cruce el duty se vuelve tan pequeno que el pulso seria mas estrecho
    que el dead-time total (RED+FED). El hardware no puede encajarlo -> sale un
    pulso deforme "solitario sin su retorno". SOLUCION: si el pulso seria menor
    que el dead-time total + margen, se SUPRIME (no se abre). Detalle fino: el
    valor de "apagado" depende del SEMICICLO (polaridad opuesta en el comparador),
    medido y corregido para que la salida quede en BAJO, no en alto.

  ============================================================================
  ARQUITECTURA: "SINGLE SOURCE OF TRUTH" (estilo Unix)
  ============================================================================
  Filosofia de diseno adoptada tras el problema 5: NINGUNA constante se expresa
  en "ticks" hardcodeados. Todo se declara en TIEMPO FISICO (ns, us, Hz) y se
  deriva a ticks en UN solo punto, por formula. Cambias la portadora o el tick
  arriba, y dead-time, guarda, periodo y amplitud se recalculan solos, coherentes.
  Esto no solo mantiene la coherencia: EXPONE las mentiras escondidas (asi se
  descubrio el problema 5). Cuando todo deriva de un punto, no hay donde esconder
  un numero equivocado. Hay DOS bases de tiempo, declaradas por separado:
    - TIMER principal : 62.5 ns/tick  (APB 80MHz / prescaler 5)
    - DEAD-TIME       : 6.25 ns/tick  (160MHz, MEDIDO, distinto del timer)

  ============================================================================
  COMO VALIDAR EN ANALIZADOR LOGICO (Saleae)
  ============================================================================
    D0=HO1  D1=LO1 (complemento)  D2=HO2  D3=LO2 (brazos lentos)  D7=guarda
    - Dead-time D0<->D1: ~300ns de solapamiento en BAJO en cada conmutacion.
    - Cruce: durante D7 (guarda) los 4 canales D0-D3 en BAJO, sin pulso espurio.
    - Guarda estable (no oscilando) gracias al enganche de fase a 20kHz.
    - Cerca del cruce: pulsos estrechos SUPRIMIDOS (canal en bajo), sin pulsos
      solitarios deformes.

  HARDWARE: ESP32 clasico (WROOM/WROVER), APB 80MHz. Puente H externo.
  ENTORNO : Arduino IDE o PlatformIO (requiere core 3.x / IDF v5.x por gptimer).
 =============================================================================*/

#include "soc/gpio_struct.h"
#include "driver/gpio.h"
#include "driver/mcpwm.h"
#include "driver/gptimer.h"
#include "soc/rtc.h"
#include "soc/gpio_sig_map.h"      // PWM0_F0_IN_IDX (entrada de falla F0)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// gpio_matrix_in: enruta un GPIO a una senal interna del MCPWM (la entrada de
// falla F0) sin cable externo. Declarada extern "C" porque vive en el ROM.
extern "C" void gpio_matrix_in(uint32_t gpio, uint32_t signal_idx, bool inv);

/*==============================================================================
  ===================  CAPA DE BASE DE TIEMPO (single source of truth)  ========
  ============================================================================
  EDITA SOLO ESTA SECCION. Todo lo de abajo se deriva por formula.
  Para cambiar la portadora, el dead-time o la amplitud, cambia el valor FISICO
  (en Hz, ns, %) y el resto se recalcula y se reporta por serial al arrancar.
==============================================================================*/

/* --- FUENTE DE TIEMPO DEL TIMER PRINCIPAL ---------------------------------
   El MCPWM legacy cuenta sobre el reloj APB (80 MHz). Con un prescaler de 5,
   cada tick del contador dura 62.5 ns. Esta es la "regla" del timer principal,
   verificada en 3 niveles (calculo = registro = pin) en el problema 4.        */
#define APB_HZ        80000000.0f      // reloj APB del MCPWM (verificado en silicio)
#define PRESCALER     5                // divisor del timer principal
#define TICK_NS       ((1.0f / APB_HZ) * PRESCALER * 1e9f)   // = 62.5 ns

/* --- CONVERSORES DEL TIMER (punto UNICO de conversion tiempo->ticks) -------
   Cualquier tiempo del timer (periodo, etc.) se convierte aqui, no a mano.    */
#define NS_TO_TICKS(ns)  ((uint32_t)((float)(ns) / TICK_NS + 0.5f))
#define US_TO_TICKS(us)  NS_TO_TICKS((float)(us) * 1000.0f)
#define TICKS_TO_NS(t)   ((float)(t) * TICK_NS)

/* --- BASE DE TIEMPO DEL DEAD-TIME (DISTINTA, medida en el problema 5) -------
   HALLAZGO CLAVE: el modulo dead-time NO cuenta en la base del timer. Cuenta a
   160 MHz -> 6.25 ns/tick (10x mas rapido). Se midio: 48 ticks dieron 286 ns,
   286/48 = 5.96 ~ 6.25. Por eso el dead-time tiene su PROPIO conversor.
   Olvidar esto fue lo que causo toda la confusion del problema 5.             */
#define DT_TICK_NS          6.25f      // base REAL del dead-time (160MHz), MEDIDA
#define NS_TO_TICKS_DT(ns)  ((uint32_t)((float)(ns) / DT_TICK_NS + 0.5f))
#define DT_TICKS_TO_NS(t)   ((float)(t) * DT_TICK_NS)

/* --- CONSTANTES DE DISENO, EN TIEMPO FISICO (lo que el ingeniero quiere) ----
   Estos son los unicos numeros que normalmente tocaras. Todo se deriva de aqui.*/
#define FREQ_CARR_HZ  20000.0f         // portadora. 20kHz: engancha la fase (guarda
                                       //   estable, problema 3) y es inaudible.
#define FREQ_MOD_HZ   60.0f            // fundamental de salida (la onda senoidal)
#define DEADTIME_NS   300.0f           // dead-time de portadora. A 6.25ns/tick = 48 ticks.
                                       //   Protege HO1/LO1 contra shoot-through en cada
                                       //   conmutacion. Ajusta segun el t_off de tus FETs.
#define AMP_PERCENT   0.90f            // amplitud del seno (90% del periodo de portadora)

/* --- SUPRESION DE PULSO MINIMO (problema 6) --------------------------------
   Cerca del cruce el duty se vuelve diminuto. Si el pulso resultante seria mas
   estrecho que el dead-time total (RED+FED), el hardware no puede formarlo bien
   y sale deforme. Lo SUPRIMIMOS (no lo abrimos). El umbral se deriva del
   dead-time total + 20% de margen, expresado en ticks del COMPARADOR (timer).
     dead-time total = 2 * DEADTIME_NS ; +20% ; / TICK_NS                      */
#define MIN_PULSE_NS    (2.0f * DEADTIME_NS * 1.2f)              // = 720 ns con 300ns DT
#define MIN_PULSE_TICKS ((int)(MIN_PULSE_NS / TICK_NS + 0.5f))   // en ticks del timer

/* --- DERIVADOS (NO EDITAR: se recalculan solos desde lo de arriba) ----------
   period_ticks: medio periodo del contador up-down a la frecuencia de portadora.
   deadtime_ticks: usa el conversor del DEAD-TIME (6.25ns), no el del timer.
   amplitude: 90% del periodo. sampleNum: muestras del seno por ciclo de 60Hz.  */
#define PERIOD_TICKS    ((int)( (1.0f/FREQ_CARR_HZ)/2.0f/(TICK_NS*1e-9f) - 1.0f + 0.5f ))
#define DEADTIME_TICKS  NS_TO_TICKS_DT(DEADTIME_NS)       // base 6.25ns
#define AMPLITUDE_TICKS ((int)(AMP_PERCENT * PERIOD_TICKS))
#define REAL_FREQ_CARR  ( 1.0f / ((PERIOD_TICKS + 1) * TICK_NS * 1e-9f * 2.0f) )
#define SAMPLE_NUM_CALC ((int)(REAL_FREQ_CARR / FREQ_MOD_HZ))

/*------------------------------------------------------------------------------
  PINES DEL PUENTE H
  --------------------------------------------------------------------------
  HO1/LO1 (rama izquierda): los RAPIDOS. HO1 modula la portadora; LO1 es su
  complemento generado por el dead-time del MCPWM.
  HO2/LO2 (rama derecha): los LENTOS. Relevan la polaridad cada semiciclo (60Hz),
  controlados por GPIO directo.
  DIAG_CRUCE (D7): refleja la guarda en el analizador (sube al inicio, baja al fin).
  FAULT_DRIVE: GPIO enrutado a la entrada de falla F0 (apaga D0/D1 en el cruce).  */
const int HO1 = 23, LO1 = 22, HO2 = 21, LO2 = 19;
const int DIAG_CRUCE = 18;          // GPIO18 -> D7
#define DIAG_BIT (1 << 18)
const int FAULT_DRIVE = 5;          // GPIO5 -> falla F0 por matriz interna
#define FAULT_BIT (1 << 5)
#define HO2_BIT (1 << 21)
#define LO2_BIT (1 << 19)
#define HO1_BIT (1 << 23)
#define LO1_BIT (1 << 22)

/* Registros del MCPWM (ESP32 clasico) accedidos directamente por direccion.
   Se usan registros crudos donde la API legacy no expone el control fino que
   necesitamos (escritura de duty desde la ISR, shadow en TEP, etc.).          */
#define MCPWM_CMPR0_REG    0x3FF5E040   // valor de comparacion (duty) del operador 0
#define MCPWM_INT_CLR_REG  0x3FF5E11C   // limpiar interrupcion
#define MCPWM_CLK_CFG      0x3FF5E000   // config de reloj de grupo
#define MCPWM_TIMER0_CFG0  0x3FF5E004   // prescaler + periodo del timer 0
#define MCPWM_GEN0_STMP    0x3FF5E03C   // momento de actualizacion del shadow (TEP)
#define MCPWM_INT_ENA      0x3FF5E110   // habilitar interrupcion

/* Copias en variables de los derivados, para uso dentro de la ISR (mas rapido
   que evaluar macros con float en cada interrupcion).                          */
const int   tmrRegVal = PERIOD_TICKS;
float real_freqCarr   = REAL_FREQ_CARR;
const int   sampleNum = SAMPLE_NUM_CALC;
const float radVal    = 2 * PI / SAMPLE_NUM_CALC;   // paso angular del seno por muestra
const int   amplitude = AMPLITUDE_TICKS;

/*------------------------------------------------------------------------------
  GUARDA DEL CRUCE (zero-crossing / polarity-transition delay)
  --------------------------------------------------------------------------
  GUARD_TICKS ajusta la duracion de la guarda (en ticks del GPTimer, ~1us c/u,
  sumados a la latencia natural de ~7.6us). Valor segun la carga:
    resistiva: 2-5us basta ; inductiva (motor/trafo): 8-20us (corriente de
    freewheeling necesita recircular). GUARD_TICKS=10 ~ 13us (carga inductiva).  */
#define GUARD_TICKS   10
#define GUARD_CORE    1            // mismo nucleo que la ISR de portadora (evita
                                  //   escritura cruzada entre nucleos)

/* Estado compartido entre la ISR (productor) y el callback del GPTimer (consumidor).
   'volatile' porque se modifican en una ISR y se leen en otra.                 */
volatile int      i           = 0;   // indice de la muestra del seno actual
volatile int      prevSign    = 0;   // signo del seno en la muestra anterior
volatile int      pendingSign = 0;   // signo entrante, para el brazo lento del cruce
volatile uint32_t guard_ticks = GUARD_TICKS;
volatile int      guard_ready = 0;   // bandera: GPTimer inicializado y corriendo

gptimer_handle_t gt = NULL;
void initGuardTimer(void* arg);

/*==============================================================================
  CALLBACK DEL GPTIMER — se ejecuta al FINAL de la guarda.
  Coordina el fin del cruce: suelta la falla (D0/D1 vuelven al PWM) Y enciende
  el brazo lento entrante (D2 o D3), en el MISMO instante. Asi los brazos rapido
  y lento se reincorporan juntos, sin el pulso espurio de polaridad.
  IRAM_ATTR: vive en RAM para latencia deterministica (no en flash).            */
static bool IRAM_ATTR onGuardElapsed(gptimer_handle_t timer,
                                     const gptimer_alarm_event_data_t *edata,
                                     void *user_ctx) {
  GPIO.out_w1tc = FAULT_BIT;                       // soltar falla -> D0/D1 reanudan PWM
  if (pendingSign > 0) GPIO.out_w1ts = HO2_BIT;    // semiciclo positivo: enciende D2
  else                 GPIO.out_w1ts = LO2_BIT;    // semiciclo negativo: enciende D3
  GPIO.out_w1tc = DIAG_BIT;                         // D7 abajo = fin de la guarda
  return false;
}

/*==============================================================================
  ISR DEL MCPWM — se dispara cada muestra de la portadora (~20 kHz).
  Hace dos cosas: (1) calcula y escribe el duty del seno para esta muestra;
  (2) si detecta cruce de polaridad, congela el puente y arma la guarda.
  Todo sin bloquear (la guarda la cierra el GPTimer, no un busy-wait).          */
void IRAM_ATTR MCPWM_ISR(void*) {
  WRITE_PERI_REG(MCPWM_INT_CLR_REG, BIT(3));       // limpiar el flag de interrupcion

  // Valor del seno para esta muestra, escalado a la amplitud (en ticks de duty).
  int sineVal = int(amplitude * sin(radVal * i));
  int sign    = (sineVal > 0) ? 1 : -1;            // polaridad del semiciclo actual

  /* --- SUPRESION DE PULSO MINIMO (problema 6) ---
     Si el pulso seria mas estrecho que el dead-time total, no tiene caso abrirlo
     (saldria deforme y solo calentaria los FETs). Marcamos para suprimir.
     OJO: 'sign' se calcula ANTES de suprimir, para que el cruce de polaridad
     use el signo real del seno (la supresion no debe alterar la deteccion).    */
  bool suppress = (sineVal > -MIN_PULSE_TICKS && sineVal < MIN_PULSE_TICKS);

  /* --- DETECCION DE CRUCE POR CERO ---
     Si el signo cambio respecto a la muestra anterior, estamos en el cruce:
     congelamos TODO el puente y armamos la guarda.                            */
  if (sign != prevSign) {
    GPIO.out_w1ts = FAULT_BIT;                     // falla ON -> D0/D1 a LOW (etapa salida)
    GPIO.out_w1tc = HO2_BIT | LO2_BIT;             // apaga ambos brazos lentos
    GPIO.out_w1ts = DIAG_BIT;                       // D7 arriba = inicio de la guarda
    WRITE_PERI_REG(MCPWM_CMPR0_REG, 0);            // duty 0 (limpieza)
    pendingSign   = sign;                           // recordar polaridad entrante

    // Armar la guarda: GPTimer metodo libre, alarma = cuenta_actual + guard_ticks.
    uint64_t now = 0;
    gptimer_get_raw_count(gt, &now);
    gptimer_alarm_config_t al = { .alarm_count = now + guard_ticks,
                                  .reload_count = 0,
                                  .flags = { .auto_reload_on_alarm = false } };
    gptimer_set_alarm_action(gt, &al);
    prevSign = sign;
  }

  /* --- ESCRITURA DEL DUTY ---
     El valor de "apagado" depende del SEMICICLO porque el duty del lado negativo
     se escribe como (tmrRegVal + sineVal), polaridad opuesta en el comparador.
     Por eso, al suprimir: positivo -> 0 ; negativo -> tmrRegVal. Ambos dejan la
     salida en BAJO (problema 6: corregido para que no quede en alto).          */
  if (suppress) {
    if (sign > 0) WRITE_PERI_REG(MCPWM_CMPR0_REG, 0);
    else          WRITE_PERI_REG(MCPWM_CMPR0_REG, tmrRegVal);
  } else if (sineVal > 0) {
    WRITE_PERI_REG(MCPWM_CMPR0_REG, sineVal);           // semiciclo positivo
  } else {
    WRITE_PERI_REG(MCPWM_CMPR0_REG, tmrRegVal + sineVal); // semiciclo negativo
  }

  i++;
  if (i >= sampleNum) i = 0;                        // reiniciar el ciclo del seno
}

/*==============================================================================
  INICIALIZACION DEL GPTIMER (problema 2: metodo libre)
  Corre en una tarea anclada a GUARD_CORE. Arranca el GPTimer UNA vez y lo deja
  corriendo para siempre; en cada cruce la ISR solo mueve la alarma. Esto evita
  la latencia de arranque (~10us) que tenia el modo one-shot.                   */
void initGuardTimer(void* arg) {
  gptimer_config_t tcfg = {
    .clk_src       = GPTIMER_CLK_SRC_APB,
    .direction     = GPTIMER_COUNT_UP,
    .resolution_hz = 1000000,           // 1 MHz -> 1 tick = 1 us (aprox)
    .flags = { .intr_shared = false },
  };
  esp_err_t err = gptimer_new_timer(&tcfg, &gt);
  if (err != ESP_OK) Serial.printf(">>> FALLO gptimer_new_timer: %s\n", esp_err_to_name(err));

  // Calibracion informativa: mide la resolucion real del GPTimer (en el problema 2
  // se vio que pedir 1MHz daba ~994kHz; medir > asumir).
  gptimer_enable(gt);
  gptimer_set_raw_count(gt, 0);
  int64_t t0 = esp_timer_get_time();
  gptimer_start(gt);
  while (esp_timer_get_time() - t0 < 2000) { }
  uint64_t tk = 0; gptimer_get_raw_count(gt, &tk);
  int64_t t1 = esp_timer_get_time();
  gptimer_stop(gt);
  float res = (float)tk / ((float)(t1 - t0) * 1e-6f);
  Serial.printf("GPTimer res: %.0f Hz (%.4f us/tick)  GUARD_TICKS=%u\n",
                res, 1e6f / res, guard_ticks);
  gptimer_disable(gt);

  gptimer_event_callbacks_t cbs = { .on_alarm = onGuardElapsed };
  gptimer_register_event_callbacks(gt, &cbs, NULL);
  gptimer_enable(gt);
  gptimer_set_raw_count(gt, 0);
  gptimer_start(gt);                    // metodo libre: corre indefinidamente

  guard_ready = 1;
  vTaskDelete(NULL);                    // la tarea muere; el timer queda corriendo
}

/*==============================================================================
  REPORTE DE BASE DE TIEMPO — se imprime al arrancar.
  Muestra cada constante en su valor FISICO pedido y en ticks, para las DOS
  bases de tiempo. Es la herramienta que destapo el problema 5: al ver el valor
  fisico junto a los ticks, las incoherencias saltan a la vista.               */
void reportarBaseDeTiempo() {
  Serial.println();
  Serial.println("========= BASE DE TIEMPO (single source of truth) =========");
  Serial.printf("TIMER    : APB=%.0f MHz / presc %d -> TICK = %.3f ns\n",
                APB_HZ/1e6, PRESCALER, TICK_NS);
  Serial.printf("DEAD-TIME: base MEDIDA = %.3f ns/tick (160MHz, distinta del timer!)\n",
                DT_TICK_NS);
  Serial.println("-----------------------------------------------------------");
  Serial.printf("portadora : %.0f Hz  -> period_ticks=%d  -> real %.1f Hz\n",
                FREQ_CARR_HZ, PERIOD_TICKS, REAL_FREQ_CARR);
  Serial.printf("dead-time : %.0f ns   -> %d ticks (base 6.25ns) -> real %.1f ns\n",
                DEADTIME_NS, DEADTIME_TICKS, DT_TICKS_TO_NS(DEADTIME_TICKS));
  Serial.printf("amplitud  : %.0f%%    -> %d ticks (%.1f%% del periodo)\n",
                AMP_PERCENT*100, AMPLITUDE_TICKS, 100.0f*AMPLITUDE_TICKS/PERIOD_TICKS);
  Serial.printf("sampleNum : %d muestras por ciclo de %.0f Hz\n", SAMPLE_NUM_CALC, FREQ_MOD_HZ);
  Serial.printf("pulso min : %d ticks (%.0f ns) -> pulsos mas cortos se SUPRIMEN\n",
                MIN_PULSE_TICKS, MIN_PULSE_NS);
  Serial.println("===========================================================");
}

/*==============================================================================
  SETUP — configura el MCPWM, el dead-time, el fault handler y el GPTimer.
==============================================================================*/
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("==== INVERSOR SPWM PUENTE H — cruce limpio + base parametrica ====");

  reportarBaseDeTiempo();             // imprime ambas bases de tiempo y derivados

  // Pines: brazos lentos y diagnostico como salida GPIO; todo en bajo al inicio.
  pinMode(LO2, OUTPUT);
  pinMode(HO2, OUTPUT);
  pinMode(DIAG_CRUCE, OUTPUT);
  pinMode(FAULT_DRIVE, OUTPUT);
  GPIO.out_w1tc = FAULT_BIT | HO2_BIT | LO2_BIT | DIAG_BIT;

  // Pines del MCPWM: HO1 = salida A (PWM), LO1 = salida B (complemento).
  mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0A, HO1);
  mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0B, LO1);

  // Config base del MCPWM. counter UP_DOWN = PWM centrado (simetrico).
  mcpwm_config_t pwm_config;
  pwm_config.frequency    = real_freqCarr * 2;   // x2 por el modo up-down
  pwm_config.cmpr_a       = 0;
  pwm_config.cmpr_b       = 0;
  pwm_config.counter_mode = MCPWM_UP_DOWN_COUNTER;
  pwm_config.duty_mode    = MCPWM_DUTY_MODE_0;
  mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &pwm_config);

  // Configuracion fina por registro: prescaler + periodo, y shadow en TEP.
  WRITE_PERI_REG(MCPWM_CLK_CFG, 0);
  uint32_t reg_val = ((PRESCALER - 1) & 0xFF) | ((tmrRegVal & 0xFFFF) << 8);
  WRITE_PERI_REG(MCPWM_TIMER0_CFG0, reg_val);
  WRITE_PERI_REG(MCPWM_GEN0_STMP, 2);   // shadow update en TEP (transfiere duty en el pico)

  // Registrar la ISR de la portadora, en IRAM para latencia deterministica.
  esp_intr_alloc(ETS_PWM0_INTR_SOURCE, ESP_INTR_FLAG_IRAM, MCPWM_ISR, NULL, NULL);

  /* --- DEAD-TIME (problema 5: base 6.25ns, MEDIDA) ---
     red = fed = DEADTIME_TICKS. El conversor NS_TO_TICKS_DT usa la base real del
     modulo (6.25ns), no la del timer. Por eso 300ns -> 48 ticks correctos.      */
  Serial.printf(">>> dead-time: %d ticks x 6.25ns = %.0f ns (red=fed)\n",
                DEADTIME_TICKS, DT_TICKS_TO_NS(DEADTIME_TICKS));
  mcpwm_deadtime_enable(MCPWM_UNIT_0, MCPWM_TIMER_0,
                        MCPWM_ACTIVE_HIGH_COMPLIMENT_MODE,
                        DEADTIME_TICKS, DEADTIME_TICKS);

  /* --- FAULT HANDLER (problema 1) ---
     Enruta FAULT_DRIVE (GPIO5) a la entrada de falla F0 por la matriz interna.
     Falla en nivel ALTO, modo cycle-by-cycle, fuerza A y B a LOW en la salida.
     Mientras FAULT_BIT este alto, HO1/LO1 se apagan DESPUES del dead-time.       */
  gpio_matrix_in(FAULT_DRIVE, PWM0_F0_IN_IDX, false);
  mcpwm_fault_init(MCPWM_UNIT_0, MCPWM_HIGH_LEVEL_TGR, MCPWM_SELECT_F0);
  mcpwm_fault_set_cyc_mode(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_SELECT_F0,
                           MCPWM_FORCE_MCPWMXA_LOW,
                           MCPWM_FORCE_MCPWMXB_LOW);

  // GPTimer de la guarda, en GUARD_CORE (mismo nucleo que la ISR).
  TaskHandle_t h = NULL;
  xTaskCreatePinnedToCore(initGuardTimer, "guardInit", 4096, NULL, 5, &h, GUARD_CORE);
  while (guard_ready == 0) { delay(1); }   // esperar a que el GPTimer este listo

  WRITE_PERI_REG(MCPWM_INT_ENA, BIT(3));   // habilitar la interrupcion de portadora

  Serial.println("Listo. Cruce limpio: fault apaga D0/D1, GPTimer da la guarda,");
  Serial.println("callback suelta fault y enciende el brazo lento, coordinados.");
}

void loop() {
  // Todo el trabajo ocurre en interrupciones. El loop queda libre.
  // (Aqui irian, en versiones extendidas: soft-start, rampa de frecuencia,
  //  comandos serial, lazo cerrado, etc.)
  delay(1000);
}
