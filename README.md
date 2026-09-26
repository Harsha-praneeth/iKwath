# 🌿 Smart Kwatha Maker — Embedded ESP32 Firmware

> **Smart India Hackathon (SIH) 2026 Submission**  
> **Team:** COGNIVOLTE | **Domain:** MedTech / IoT Hardware Appliance  
> **Repository:** Production-Grade Firmware for Autonomous Herbal Decoction Extraction

---

## 📌 Executive Summary

Traditional Ayurvedic *Kwatha* (herbal decoction) preparation requires precise, long-duration temperature modulation and exact volume reduction—typically concentrating a raw aqueous mixture from **400 mL down to 100 mL** ($\Delta M = 300\text{ g}$ mass loss). Manual decoction often leads to active phytochemical degradation due to thermal scalding or inaccurate final concentration.

The **Smart Kwatha Maker** is an IoT-enabled, edge-computed hardware appliance powered by an **ESP32 microcontroller**. It automates decoction extraction using:
1. **RFID Pod Authentication:** Auto-selects thermal profiles based on the herbal blend.
2. **Real-Time Differential Mass Tracking:** Uses an HX711 strain-gauge amplifier to track continuous water evaporation.
3. **Closed-Loop PID Temperature Regulation:** Dynamically modulates a Solid-State Relay (SSR) to hold an exact $88^\circ\text{C}$ extraction point without boiling over.
4. **Non-Destructive Stirring:** Continuous magnetic agitation prevents bottom-plate charring.
5. **Standalone UI & Telemetry:** Integrated $0.96''$ OLED step-by-step guidance and wireless Bluetooth Low Energy (BLE) diagnostic streaming.

---

## 🏗️ Hardware Architecture & System Integration

### Microcontroller
* **Board:** ESP32 Dev Module (Dual-Core Xtensa LX6 @ 240 MHz, Integrated Wi-Fi/BLE)

### Pinout Mapping Matrix

| Hardware Module | Protocol / Signal | ESP32 GPIO Pin | Description / Notes |
| :--- | :--- | :--- | :--- |
| **0.96" OLED Display** | $\text{I}^2\text{C}$ SDA | `GPIO 21` | $128 \times 64$ SSD1306 Visual Interface |
| **0.96" OLED Display** | $\text{I}^2\text{C}$ SCL | `GPIO 22` | $128 \times 64$ SSD1306 Clock Line |
| **RC522 RFID Reader** | SPI SCK | `GPIO 18` | Master Clock |
| **RC522 RFID Reader** | SPI MISO | `GPIO 19` | Master In Slave Out |
| **RC522 RFID Reader** | SPI MOSI | `GPIO 23` | Master Out Slave In |
| **RC522 RFID Reader** | SPI SS (SDA) | `GPIO 5` | Chip Select Line |
| **RC522 RFID Reader** | Reset | `GPIO 27` | RFID Hardware Reset |
| **DS18B20 Temp Sensor** | 1-Wire Protocol | `GPIO 4` | Waterproof Probe ($4.7\text{ k}\Omega$ Pull-Up) |
| **HX711 Strain Gauge** | Data (DT) | `GPIO 16` | Mass Tracking Data Line |
| **HX711 Strain Gauge** | Clock (SCK) | `GPIO 17` | Mass Tracking Serial Clock |
| **SSR-25DA Relay** | Digital Output / PWM | `GPIO 2` | Optocoupled Heating Element Driver |
| **DC Magnetic Stirrer** | Digital Output / PWM | `GPIO 14` | High-Torque Agitation Motor Driver |

---

## 🧠 System State Machine & Execution Flow

The firmware operates as a deterministic **8-Stage Finite State Machine (FSM)**:

```text
[STATE 1: INIT] ──► [STATE 2: SCAN POD] ──► [STATE 3: AUTHENTICATED]
                                                     │
[STATE 6: HEATING/STIRRING] ◄── [STATE 5: TARE LOCK] ◄── [STATE 4: ADD WATER]
           │
           ├── (ΔM >= 300g) ────► [STATE 7: PROCESS COMPLETE]
           │
           └── (Temp > 105°C) ──► [STATE 8: EMERGENCY CUTOFF]
