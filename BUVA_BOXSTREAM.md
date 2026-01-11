# Buva Boxstream RF Protocol

## Speed Mapping

De Buva Boxstream hoofdbediening heeft 5 fysieke standen, maar stuurt slechts **4 unieke presets** via RF.

| Fysieke Stand | Naam (handleiding) | RF Preset | HA Speed |
|---------------|-------------------|-----------|----------|
| 1 (laagste) | "niet aanwezig" | 1 | 25% |
| 2 | "aanwezig en nacht voor klein huishouden" | 2 | 50% |
| 3 | "nacht" | 3 | 75% |
| 4 | "aanwezig" | 3 | 75% |
| 5 (hoogste) | "maximale ventilatie" | 4 | 100% |

**Let op**: Stand 3 ("nacht") en stand 4 ("aanwezig") sturen **exact dezelfde** RF frames (preset 3). Het verschil zit alleen in de lokale bediening.

### Home Assistant Mapping

| HA | RF Preset | Voltage |
|----|-----------|---------|
| OFF | 0 | 0.0V |
| Speed 1 (25%) | 1 (Low) | 3.0V |
| Speed 2 (50%) | 2 (Medium) | 5.0V |
| Speed 3 (75%) | 3 (High) | 9.0V |
| Speed 4 (100%) | 4 (Max) | 10.0V |

**Opmerking**: De fysieke bediening kan de fan niet uitzetten (geen preset 0). Via Home Assistant kan dit wel.

## RF Protocol Details

- **Frequentie**: 868.2 MHz (BOXSTREAM/BUVA network)
- **Network ID**: 0xFE75FD9B
- **Device Types**:
  - 0x01: MAIN_UNIT (ventilator)
  - 0x0E: MAIN_CONTROL (bedrade hoofdbediening)
  - 0x0F: RF_REMOTE (badkamer afstandsbediening)

## Commands

| Command | Hex | Beschrijving |
|---------|-----|--------------|
| SETVOLTAGE | 0x01 | Directe spanning instellen (0-127 = 0.0-12.7V) |
| SETSPEED | 0x02 | Preset instellen (0-4) |
| SETTIMER | 0x03 | Timer met snelheid |
| FAN_SETTINGS | 0x07 | Response met huidige instellingen |
| QUERY_DEVICE | 0x10 | Opvragen huidige status |

## Badkamer Remote met Timer

De badkamer afstandsbediening (type 0x0F) stuurt altijd preset 4 (maximale ventilatie) wanneer de timer wordt geactiveerd.
