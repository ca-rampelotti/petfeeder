# PetFeeder IoT

Sistema inteligente de alimentação automática para pets — PMR3402 (Sistemas Embarcados), Escola Politécnica da USP.

## Descrição

Alimentador automático baseado em ESP32 que libera ração em horários programados ou por acionamento manual, com a porção controlada pelo tempo de abertura de uma comporta acionada por servomotor. A configuração e o monitoramento são feitos remotamente via Firebase Realtime Database, com interface web para o usuário.

## Arquitetura

O firmware é estruturado em **FreeRTOS**, com 5 tarefas concorrentes:

- **Task_Comunicacao** — mantém WiFi, sincroniza horário via NTP e inicia o Firebase.
- **Task_GestorEventos** — lê a configuração remota e os gatilhos (manual e programado).
- **Task_Balanca** — módulo de aferição de peso da dosagem.
- **Task_ControleDosagem** — máquina de estados que controla a dosagem.
- **Task_Telemetria** — lê o sensor ultrassônico e publica o nível de ração.

A comunicação entre tarefas usa **fila** (comandos) e **mutex** (proteção do acesso ao Firebase e ao peso compartilhado).

### Máquina de estados (dosagem)

`SERVO_IDLE → SERVO_ABRINDO → SERVO_ABERTO → SERVO_FECHANDO → SERVO_FECHADO → SERVO_IDLE`

## Casos de uso

- **UC1 — Servir porção (programado):** horários definidos pelo usuário, disparados via NTP.
- **UC2 — Configurar porção:** gramagem e horários ajustados remotamente.
- **UC3 — Alimentação manual:** acionamento sob demanda pela interface.
- **UC4 — Monitorar nível:** medição por sensor ultrassônico, publicada no Firebase.

## Hardware

- ESP32 DevKit
- Servomotor SG90 (comporta)
- Sensor ultrassônico HC-SR04 (nível de ração)
- Estrutura impressa em 3D (reservatório, comporta, suporte)

## Configuração

1. Abra `petfeeder.ino` na Arduino IDE.
2. Preencha as credenciais no topo do arquivo (WiFi e Firebase).
3. Instale as bibliotecas: `ESP32Servo`, `Firebase Arduino Client Library for ESP8266 and ESP32` (Mobizt).
4. Selecione a placa **ESP32 Dev Module** e faça o upload.
5. Abra `app/petfeeder.html` no navegador e preencha o secret do Firebase para usar a interface.

## Estrutura do Firebase (Realtime Database)

```
config/gramasManual     (int)     porção do acionamento manual
config/horarios         (string)  "HH:MM:gramas,HH:MM:gramas"
comando/abrirPorta      (bool)    gatilho manual
nivelPercentual         (int)     nível de ração (%)
status                  (string)  estado atual
ultimaPorcaoGramas      (int)     última porção servida
ultimaPorcaoHora        (string)  horário da última porção
alerta                  (string)  "ok" / "sem racao"
alertaNivel             (string)  "ok" / "baixo"
```