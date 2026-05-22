/*
  ============================================================
  Báscula Clínica — 4 Celdas en Puente Wheatstone
  HX711 + ESP32  |  Arduino IDE
  ============================================================
  CONEXIÓN FÍSICA:
    4 celdas en puente Wheatstone completo:
      - Las 4 celdas forman los brazos R1-R4 del puente
      - Excitación (+) → E+ del HX711   (cable rojo combinado)
      - Excitación (−) → E− del HX711   (cable negro combinado)
      - Señal (+)      → A+ del HX711   (punto medio del puente)
      - Señal (−)      → A− del HX711   (punto medio opuesto)

  HX711 → ESP32:
      VCC → 3.3 V
      GND → GND
      DT  → GPIO 4
      SCK → GPIO 5

  COMANDOS (Monitor Serie 115200):
      t  → Tara
      c  → Iniciar calibración (luego escribe el peso en kg + Enter)
      r  → Leer peso una vez
      s  → Guardar calibración en flash
      m  → Mostrar menú
  ============================================================
*/

#include <HX711.h>
#include <Preferences.h>

// ── Pines ────────────────────────────────────────────────
// GPIO 32/33: propósito general, no son strapping pins → arranque estable
// Alternativa igualmente segura: PIN_DT 25 / PIN_SCK 26
#define PIN_DT   32
#define PIN_SCK  33

// ── Filtro de mediana ────────────────────────────────────
#define FILTRO_N  9          // muestras para mediana (impar)

// ── Objetos ──────────────────────────────────────────────
HX711       scale;
Preferences prefs;

// ── Variables de calibración ─────────────────────────────
float calibration_factor = 1.0f;
long  zero_offset        = 0L;
bool  calibrado          = false;

// ── Máquina de estados no bloqueante ────────────────────
enum Estado {
  IDLE,
  ESPERANDO_CAL_PESO,  // esperó comando 'c', aguarda que el usuario escriba kg
  LEYENDO_TARA,
  LEYENDO_CAL
};
Estado estado = IDLE;

String inputBuffer = "";         // buffer de entrada sin bloquear
float  peso_referencia = 0.0f;

// ── Temporización ─────────────────────────────────────────
unsigned long t_accion = 0;
#define ESPERA_MS  3000          // 3 s antes de leer tras comando

// ─────────────────────────────────────────────────────────
// Valida que el HX711 esté listo con timeout para evitar
// bloqueo infinito en el while(!is_ready()) interno de la librería
bool hx711Listo(unsigned long timeoutMs = 500) {
  unsigned long inicio = millis();
  while (!scale.is_ready()) {
    if (millis() - inicio > timeoutMs) return false;
    delay(10);
  }
  return true;
}

// ─────────────────────────────────────────────────────────
// Mediana de N lecturas ya convertidas a unidades de peso
float leerPesoMediana(int n = FILTRO_N) {
  float muestras[FILTRO_N];
  int nn = constrain(n, 1, FILTRO_N);

  for (int i = 0; i < nn; i++) {
    muestras[i] = scale.get_units(1);
    delay(20);
  }

  // Ordenar burbuja (N pequeño, OK)
  for (int i = 0; i < nn - 1; i++)
    for (int j = i + 1; j < nn; j++)
      if (muestras[j] < muestras[i]) {
        float tmp = muestras[i];
        muestras[i] = muestras[j];
        muestras[j] = tmp;
      }

  return muestras[nn / 2];   // mediana
}

// ─────────────────────────────────────────────────────────
void guardarFlash() {
  prefs.begin("bascula", false);
  prefs.putFloat("factor", calibration_factor);
  prefs.putLong("offset",  zero_offset);
  prefs.end();
  Serial.println("[FLASH] Guardado: factor=" + String(calibration_factor, 4) +
                 "  offset=" + String(zero_offset));
}

void cargarFlash() {
  prefs.begin("bascula", true);
  if (prefs.isKey("factor")) {
    calibration_factor = prefs.getFloat("factor", 1.0f);
    zero_offset        = prefs.getLong("offset",  0L);
    calibrado = true;
    Serial.println("[FLASH] Calibración cargada: factor=" +
                   String(calibration_factor, 4) +
                   "  offset=" + String(zero_offset));
  } else {
    Serial.println("[FLASH] Sin calibración previa. Realiza tara + calibración.");
  }
  prefs.end();
}

void mostrarMenu() {
  Serial.println(F("\n===================================="));
  Serial.println(F("  Báscula Clínica — HX711 + ESP32"));
  Serial.println(F("===================================="));
  Serial.println(F("  t  → Tara (sin peso encima)"));
  Serial.println(F("  c  → Calibrar con peso conocido"));
  Serial.println(F("  r  → Leer peso actual"));
  Serial.println(F("  s  → Guardar en flash"));
  Serial.println(F("  m  → Mostrar este menú"));
  Serial.println(F("====================================\n"));
}

// ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  scale.begin(PIN_DT, PIN_SCK);

  if (!hx711Listo(2000)) {
    Serial.println(F("[ERROR] HX711 no responde al iniciar. Revisa cableado/alimentación."));
  } else {
    scale.set_gain(128);  // canal A, ganancia 128 — llama read() internamente, solo si HX711 listo
  }

  cargarFlash();

  if (calibrado) {
    scale.set_offset(zero_offset);
    scale.set_scale(calibration_factor);
  }

  mostrarMenu();
}

// ─────────────────────────────────────────────────────────
void loop() {

  // ── Leer Serial sin bloquear ────────────────────────────
  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\n' || c == '\r') {
      // Procesar buffer acumulado (para ingreso de número de kg)
      if (estado == ESPERANDO_CAL_PESO && inputBuffer.length() > 0) {
        peso_referencia = inputBuffer.toFloat();
        inputBuffer = "";

        if (peso_referencia <= 0.0f) {
          Serial.println("[CAL] Peso inválido. Cancela. Ingresa 'c' para reintentar.");
          estado = IDLE;
        } else {
          Serial.print("[CAL] Peso de referencia: ");
          Serial.print(peso_referencia, 2);
          Serial.println(" kg");
          Serial.println("[CAL] Coloca el peso y espera 3 s...");
          t_accion = millis();
          estado = LEYENDO_CAL;
        }
      }
      inputBuffer = "";

    } else if (estado == ESPERANDO_CAL_PESO) {
      // Acumular dígitos del peso
      inputBuffer += c;

    } else {
      // Comandos de una letra (sólo en IDLE)
      if (estado == IDLE) {
        switch (c) {

          case 't': case 'T':
            Serial.println(F("\n[TARA] Retira todo el peso. Esperando 3 s..."));
            t_accion = millis();
            estado = LEYENDO_TARA;
            break;

          case 'c': case 'C':
            Serial.println(F("\n[CAL] Ingresa el peso de referencia en kg y presiona Enter:"));
            inputBuffer = "";
            estado = ESPERANDO_CAL_PESO;
            break;

          case 'r': case 'R': {
            if (!calibrado) {
              Serial.println(F("[READ] Primero realiza tara + calibración."));
              break;
            }
            if (!hx711Listo()) {
              Serial.println(F("[ERROR] HX711 no está listo. Revisa cableado/alimentación."));
              break;
            }
            float w = leerPesoMediana();
            Serial.print(F("[READ] Peso: "));
            Serial.print(w, 1);
            Serial.println(F(" kg"));
            break;
          }

          case 's': case 'S':
            if (!calibrado) {
              Serial.println(F("[FLASH] No se puede guardar: primero realiza tara + calibración."));
            } else {
              guardarFlash();
            }
            break;

          case 'm': case 'M':
            mostrarMenu();
            break;

          default:
            break;
        }
      }
    }
  }

  // ── Lógica de estados temporizados (sin delay largo) ────
  unsigned long ahora = millis();

  if (estado == LEYENDO_TARA && (ahora - t_accion >= ESPERA_MS)) {
    if (!hx711Listo()) {
      Serial.println(F("[ERROR] HX711 no está listo. Revisa cableado/alimentación."));
      estado = IDLE;
    } else {
      // Tomar tara: promedia 10 lecturas crudas
      scale.set_scale(1.0f);      // factor neutro para capturar raw
      scale.tare(10);
      zero_offset = scale.get_offset();
      scale.set_scale(calibration_factor);   // restaurar factor

      Serial.print(F("[TARA] Offset: "));
      Serial.println(zero_offset);
      Serial.println(F("[TARA] Listo. Ahora calibra con 'c'.\n"));
      estado = IDLE;
    }
  }

  if (estado == LEYENDO_CAL && (ahora - t_accion >= ESPERA_MS)) {
    if (!hx711Listo()) {
      Serial.println(F("[ERROR] HX711 no está listo. Revisa cableado/alimentación."));
      estado = IDLE;
    } else {
      // Leer raw con factor = 1 para calcular el factor real
      scale.set_scale(1.0f);
      long raw = (long)scale.get_units(10);    // promedio 10 lecturas (ya descuenta offset)

      if (abs(raw) < 100) {
        Serial.println(F("[CAL] Lectura demasiado baja. Revisa puente, cableado o peso de referencia."));
        scale.set_scale(calibration_factor);   // restaurar factor anterior
        estado = IDLE;
        return;
      }

      calibration_factor = (float)raw / peso_referencia;
      scale.set_scale(calibration_factor);
      calibrado = true;

      Serial.print(F("[CAL] Raw leído: "));
      Serial.println(raw);
      Serial.print(F("[CAL] Factor calculado: "));
      Serial.println(calibration_factor, 4);

      // Verificar
      float verif = leerPesoMediana(5);
      Serial.print(F("[CAL] Verificación → "));
      Serial.print(verif, 1);
      Serial.println(F(" kg  (debería ser ≈ peso de referencia)"));
      Serial.println(F("[CAL] ¡Listo! Guarda con 's'.\n"));

      estado = IDLE;
    }
  }
}
