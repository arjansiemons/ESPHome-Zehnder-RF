# Zehnder RF Fan Control - Status Tracking

## Huidige Status (2026-01-11)

### ✅ WAT WERKT
- **Status synchronisatie van fan → HA**: Als je de fan handmatig bedient (fysieke remote/wandschakelaar), komt de status update wel door naar Home Assistant
- **RF ontvangst**: De nRF905 ontvangt wel broadcasts (STATUS_BROADCAST, SETSPEED frames)
- **nRF905 component**: Basis RF communicatie werkt

### ❌ WAT NIET WERKT
- **Fan control HA → fan**: Bedienen vanuit Home Assistant werkt NIET
- **Pairing**: Automatische pairing werkt niet meer
- **Handmatige pairing**: "Pair as Remote" button werkt niet

## Recente Commits (laatste → oudste)

| Commit | Beschrijving | Impact |
|--------|-------------|--------|
| 2fbd1c5 | Fix: RF_REMOTE must target MAIN_CONTROL (0x0E), not MAIN_UNIT (0x01) | ⚠️ Config validatie veranderd - oude configs worden verworpen |
| 46f35ee | Fix preset mapping: Direct 1:1 mapping (preset = speed) | Preset mapping aangepast |
| 453e107 | Correct preset mapping: 5 speeds on physical controls + OFF in HA | Speed count aangepast |
| 8782441 | Fix preset-to-speed mapping and eliminate 100%→75% jump | Preset logic |
| 733056b | Add automatic pairing on first boot | ✅ Auto-pairing toegevoegd |
| 353cc3c | Add comprehensive debug logging for SETSPEED broadcasts | Debug logging |
| a6d225d | Add SETSPEED broadcast handler and fix state machine stuck issue | ✅ SETSPEED broadcasts werken |

## Waarschijnlijke Oorzaak

De commit **2fbd1c5** veranderde de pairing target van `MAIN_UNIT (0x01)` naar `MAIN_CONTROL (0x0E)`.

**Probleem**: De config validatie in `zehnder.cpp:139-143` verwerpt nu oude configs die nog `MAIN_UNIT` als target hebben:
```cpp
if (this->config_.fan_networkId == 0xFE75FD9B &&
    this->config_.fan_my_device_type == FAN_TYPE_RF_REMOTE &&
    this->config_.fan_my_device_id != 0 &&
    this->config_.fan_main_unit_type == FAN_TYPE_MAIN_CONTROL &&  // ← Moet 0x0E zijn!
    this->config_.fan_main_unit_id != 0) {
```

Als de validatie faalt:
- Device blijft in `StateStartup` state
- Fan control werkt alleen in `StateIdle` state
- → Fan control werkt dus niet!

## Systematische Aanpak

### Stap 1: Verstaan Waarom Het Werkte
Commit **733056b** (Add automatic pairing on first boot) werkte waarschijnlijk:
- ✅ Auto-pairing was geïmplementeerd
- ✅ Config werd opgeslagen na pairing
- ✅ Fan control werkte direct na pairing

### Stap 2: Identificeer Breaking Changes
- Commit **2fbd1c5**: Config validatie te strict → oude configs worden verworpen
- Mogelijke issue: Pairing sequence faalt nu?

### Stap 3: Te Onderzoeken
1. Controleer of pairing sequence nog correct is
2. Check of TX berichten daadwerkelijk verzonden worden
3. Verify of device in Idle state komt na pairing
4. Test of setSpeed() commando's verzonden worden

### Stap 4: Oplossingsplan
1. Check logs om te zien in welke state device blijft hangen
2. Als StateStartup: pairing faalt → debug pairing sequence
3. Als StateIdle maar geen TX: debug setSpeed() / startTransmit()
4. Mogelijk teruggaan naar werkende commit en incrementeel debugging

## Log Analyse (10:36-10:40)

### ✅ WAT WEL WERKT
1. **Pairing werkt!** - FRAME_0B wordt ontvangen, config wordt opgeslagen
2. **RX ontvangst werkt** - SETSPEED broadcasts van MAIN_CONTROL worden ontvangen
3. **TX verzending werkt** - Frames worden verzonden (state RfStateRxWait)
4. **Manual Init werkt** - State gaat naar Idle

### ❌ KERNPROBLEEM GEVONDEN
**Fan reageert NIET op onze SETSPEED commands!**

```
[10:37:39.851] Sending to fan: HA speed 1 (state=ON) → Zehnder preset 1
[10:37:40.873] RfStateRxWait: Receive timeout (waited 1007ms)
[10:37:51.155] !!! SET SPEED TIMEOUT - NO RESPONSE FROM FAN !!!
```

**MAAR**: Fan stuurt WEL SETSPEED broadcasts als je handmatig bedient:
```
[10:38:06.407] TX: 0x0E (MAIN_CONTROL) ID=0x39
[10:38:06.409] Command Name: SETSPEED
[10:38:06.704] !!! SETSPEED BROADCAST HANDLER TRIGGERED !!!
```

### Hypothese
**Probleem: RF_REMOTE (0x0F) stuurt naar MAIN_CONTROL (0x0E), maar zou naar MAIN_UNIT (0x01) moeten sturen!**

Bewijs:
- SETSPEED broadcasts zijn: `RX=MAIN_UNIT (0x01), TX=MAIN_CONTROL (0x0E)`
- Dit betekent: MAIN_CONTROL stuurt broadcast NAAR alle MAIN_UNIT devices
- Dus wij moeten ook sturen NAAR MAIN_UNIT, niet naar MAIN_CONTROL!

In commit 2fbd1c5 werd dit VERKEERD aangepast:
```cpp
this->config_.fan_main_unit_type = FAN_TYPE_MAIN_CONTROL;  // 0x0E ← FOUT!
```

Zou moeten zijn:
```cpp
this->config_.fan_main_unit_type = FAN_TYPE_MAIN_UNIT;  // 0x01 ← GOED!
```

## FIX GEÏMPLEMENTEERD

### Wijzigingen
1. ✅ Target type terug naar `FAN_TYPE_MAIN_UNIT (0x01)` in setup()
2. ✅ Target type terug naar `FAN_TYPE_MAIN_UNIT (0x01)` in manual_init()
3. ✅ Pairing stuurt JOIN_REQUEST naar MAIN_UNIT in plaats van MAIN_CONTROL
4. ✅ Config validatie accepteert nu BEIDE targets (0x01 en 0x0E) voor backwards compatibility

### Logica
- **SETSPEED commands**: Van RF_REMOTE (0x0F) → NAAR MAIN_UNIT (0x01)
- **SETSPEED broadcasts**: Van MAIN_CONTROL (0x0E) → NAAR MAIN_UNIT (0x01) (broadcast)
- **Status ontvangst**: Van MAIN_CONTROL (0x0E) → Broadcasts worden ontvangen

### Test Instructies
1. Upload nieuwe firmware naar ESP32
2. Druk op "Clear Config" button in HA
3. Reboot device (of druk "Restart" button)
4. Wacht 5 seconden voor automatische pairing
5. Test fan control vanuit Home Assistant
6. Test of handmatige bediening nog steeds updates geeft naar HA

## Test Resultaten (na e1daae0)

### ✅ WAT WERKT
- **Fan control HA → fan**: ✅ **WERKT** (alleen na Manual Init button!)
- **Status updates fan → HA**: ✅ Werken nog steeds

### ❌ WAT NOG NIET WERKT
- **Automatische setup**: Fan control werkt NIET direct na boot
- **Pairing**: Werkt niet automatisch
- **Vereist workaround**: "Manual Init" button moet gedrukt worden

### Analyse: setup() vs manual_init()

**Wat manual_init() WEL doet (en werkt):**
```cpp
this->rf_->setup();  // ← Roept nRF905 setup() handmatig aan!
```

**Wat setup() NIET doet:**
```cpp
// NOTE: Do NOT call rf_->setup() - ESPHome calls it automatically!
// ← Dit commentaar is FOUT! ESPHome roept het blijkbaar NIET aan!
```

**ECHTE OORZAAK GEVONDEN:**
Setup priorities waren verkeerd!
```cpp
// nRF905: AFTER_CONNECTION (400)
// Zehnder: AFTER_CONNECTION - 1.0f (399) ← TE LAAG!
```

Zehnder::setup() liep VOOR nRF905::setup(), dus:
1. Zehnder registreert callbacks
2. ESPHome roept nRF905::setup() aan → reset callbacks?
3. Callbacks werken niet meer!

**FIX**: Zehnder priority verhogen naar DATA - 1.0f (599) zodat het NA nRF905 loopt.

## FIX #2: Setup Priority

### Probleem
Setup priority was te laag (399), dus Zehnder::setup() liep VOOR nRF905::setup()

### Oplossing
```cpp
// Was: setup_priority::AFTER_CONNECTION - 1.0f  (399)
// Nu:  setup_priority::DATA - 1.0f               (599)
```

Nu loopt Zehnder::setup() NA nRF905::setup(), dus callbacks blijven werken!

## Test Resultaten Fix #2 (commit 9c8dca8)

### ❌ WERKT NOG STEEDS NIET
Upload gedaan, fan control geeft nog steeds timeouts:
```
[10:59:21.238] Sending to fan: HA speed 1 (state=ON) → Zehnder preset 1
[10:59:32.549] !!! SET SPEED TIMEOUT - NO RESPONSE FROM FAN !!!
```

**Setup() logs ontbreken** - setup() liep waarschijnlijk VOOR API verbinding, dus logs niet zichtbaar.

### Hypothese
Setup priority aanpassing (599) is niet genoeg. Mogelijke problemen:
1. nRF905::setup() wordt nog steeds niet aangeroepen door ESPHome
2. Callbacks worden nog steeds gereset na onze setup()
3. Setup volgorde is anders dan verwacht

### Volgende Stap
Probeer de oude werkende aanpak van manual_init():
**Roep rf_->setup() EXPLICIET aan in setup(), net als manual_init() doet**

## FIX #3: Expliciet rf_->setup() aanroepen

### Conclusie
Setup priority helpt niet - ESPHome roept `rf_->setup()` gewoon NOOIT automatisch aan voor referenced components!

### Oplossing
Simpel: doe wat manual_init() doet - roep rf_->setup() EXPLICIET aan VOOR het registreren van callbacks:

```cpp
// In setup():
this->rf_->setup();  // ← Initialiseer hardware EERST
this->rf_->setOnRxComplete(...);  // ← DAN callbacks registreren
```

Dit is exact wat manual_init() doet en dat werkt perfect!

## Test Resultaten Fix #3 (commit ab38346)

### ❌ WERKT NOG STEEDS NIET
Gebruiker rapporteert: nog steeds hetzelfde probleem, setup logs niet zichtbaar.

**Mogelijk probleem:**
De firmware upload gaat te snel en we zien setup() logs niet omdat die VOOR API verbinding gebeuren.

### Debug Strategie
1. Vraag gebruiker om VOLLEDIGE logs vanaf boot
2. Check of setup() überhaupt wordt aangeroepen
3. Check of er compiler errors zijn
4. Mogelijk: voeg delay toe NA setup() zodat logs zichtbaar zijn
5. Of: gebruik ESP_LOGE ipv ESP_LOGI voor setup logs (ERROR level altijd zichtbaar)

## Volgende Acties
- [x] Fix #3 geïmplementeerd
- [ ] Logs van gebruiker opvragen
- [ ] Verificeren dat firmware correct gecompileerd is
- [ ] Mogelijk: ESP_LOGE gebruiken voor alle setup logs
