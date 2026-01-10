# BOXSTREAM/BUVA RF Protocol Analysis

## Hardware Configuration

### nRF905 RF Module
- **Frequency:** 868.2 MHz (Channel 117)
- **Network ID:** 0xFE75FD9B (BOXSTREAM/BUVA)
- **CRC:** 16-bit
- **TX Power:** 10 dBm
- **Address Width:** 4 bytes
- **Payload Width:** 16 bytes

### ESP32 GPIO Pin Configuration
- **SPI CLK:** GPIO18
- **SPI MOSI:** GPIO23
- **SPI MISO:** GPIO19
- **CS (Chip Select):** GPIO5
- **CD (Carrier Detect):** GPIO4
- **CE (Chip Enable):** GPIO16
- **PWR (Power):** GPIO17
- **TXEN (TX Enable):** GPIO2

## System Architecture

### Device Types
| Type | Code | Description | ID Examples |
|------|------|-------------|-------------|
| BROADCAST | 0x00 | Broadcast to all devices | 0x00 |
| MAIN_UNIT | 0x01 | Ventilation fan unit | 0x39 |
| REMOTE_CONTROL | 0x03 | Zehnder remote (older?) | - |
| MAIN_CONTROL | 0x0E | Wired main control panel | 0x39 |
| RF_REMOTE | 0x0F | Wireless RF remote (bathroom) | 0xD7 |
| CO2_SENSOR | 0x18 | CO2 sensor | - |

### Known Devices in System
- **RF Remote (Badkamer):** ID=0xD7, Type=0x0F
- **Main Unit (Ventilator):** ID=0x39, Type=0x01
- **Main Control (Bedraad):** ID=0x39, Type=0x0E (No RF transmission)

## RF Commands

### Command Types
| Command | Code | Description | Direction |
|---------|------|-------------|-----------|
| SETVOLTAGE | 0x01 | Set speed by voltage/percentage | Remote → Unit |
| SETSPEED | 0x02 | Set speed preset (1-4) | Remote → Unit |
| SETTIMER_OLD | 0x03 | Set timer (old protocol?) | Remote → Unit |
| JOIN_REQUEST | 0x04 | Network join request | Device → Broadcast |
| SETSPEED_REPLY | 0x05 | Reply to speed command | Unit → Remote |
| JOIN_OPEN | 0x06 | Open network for joining | Unit → Broadcast |
| FAN_SETTINGS | 0x07 | Current fan settings/status | Unit → Remote |
| FRAME_0B | 0x0B | Unknown | ? |
| JOIN_ACK | 0x0C | Join acknowledgment | Unit → Device |
| QUERY_NETWORK | 0x0D | Query network/poll | Remote → Unit |
| QUERY_DEVICE | 0x10 | Query specific device | Device → Device |
| **SETTIMER** | **0x14** | **Set timer (pre-configure)** ✅ | **Remote → Main Control** |
| **STATUS_BROADCAST** | **0x15** | **Broadcast current status** ✅ | **Main Control → Broadcast** |
| SETVOLTAGE_REPLY | 0x1D | Reply to voltage command | Unit → Remote |

## Observed RF Frames

### 1. RF Remote Operation Sequence (Bathroom Remote)
**Example captured on 2026-01-10 21:39**

The RF remote has a two-step operation:
1. **Set timer** (5, 10, or 20 minutes)
2. **Press button** (MAX or OFF)

#### Step 1: Set Timer (Command 0x14 - SETTIMER)
```
RX: 0x0E (MAIN_CONTROL) ID=0x00
TX: 0x0F (RF_REMOTE) ID=0xD7
Command: 0x14 (SETTIMER)
Parameters: 05 0A  → 10 minutes (0x0A = 10 decimal)
Parameters: 05 05  → 5 minutes
Parameters: 05 14  → 20 minutes (0x14 = 20 decimal)
```

**SETTIMER Parameter Format:**
- Byte 1: Always `0x05` (identifier?)
- Byte 2: Timer duration in **decimal minutes** (0x05=5min, 0x0A=10min, 0x14=20min)

**Note:** Timer is sent to MAIN_CONTROL (0x0E), not MAIN_UNIT!

#### Step 2: Press MAX Button (Command 0x02 - SETSPEED)
```
RX: 0x01 (MAIN_UNIT) ID=0x00 (broadcast)
TX: 0x0F (RF_REMOTE) ID=0xD7
Command: 0x02 (SETSPEED)
Parameters: 04  → MAX speed
Parameters: 02  → MEDIUM speed
Parameters: 00  → OFF (?)
```

#### Main Unit Response (Command 0x07 - FAN_SETTINGS)
```
RX: 0x0F (RF_REMOTE) ID=0xD7
TX: 0x01 (MAIN_UNIT) ID=0x39
Command: 0x07 (FAN_SETTINGS)
Parameters: 04 32 00 04
```

**FAN_SETTINGS Parameter Format:**
- Byte 1: **Target speed preset** (02=MEDIUM, 04=MAX)
- Byte 2: **Current voltage %** in hex (0x32=50%, 0x5A=90%)
- Byte 3: Unknown (always 0x00)
- Byte 4: Timer-related (always 0x04?)

**IMPORTANT DISCOVERY: Voltage Ramping**
The fan gradually ramps voltage up/down to target:
- `04 32 00 04` = Target MAX (100%) set, but currently at 50% → Ramping UP
- `02 5A 00 04` = Target MEDIUM (50%) set, but currently at 90% → Ramping DOWN

This explains why we see different voltage values for the same speed preset!

### 2. Periodic Network Query
**Observed every ~60 seconds**

```
RX: 0x01 (MAIN_UNIT) ID=0x00
TX: 0x0F (RF_REMOTE) ID=0xD7
TTL: 0xFA
Command: 0x0D (QUERY_NETWORK)
```

The RF remote periodically polls the network to maintain connection.

### 3. SETSPEED Commands from Main Control Panel
**Observed on 2026-01-10 21:36**

All speed changes from the wired MAIN_CONTROL panel (Type 0x0E, ID 0x39) are transmitted via RF:

#### Speed LOW (01)
```
RX: 0x01 (MAIN_UNIT) ID=0x00 (broadcast)
TX: 0x0E (MAIN_CONTROL) ID=0x39
Command: 0x02 (SETSPEED)
Parameters: 01
```
Each command is retransmitted 4 times with decreasing TTL:
- Frame 1: TTL=0xFA (250)
- Frame 2: TTL=0xAF (175)
- Frame 3: TTL=0x5E (94)
- Frame 4: TTL=0x29 (41)

#### Speed MEDIUM (02)
```
Parameters: 02
```

#### Speed HIGH (03)
```
Parameters: 03
```

**SETSPEED Parameter Mapping:**
- `0x00` = AUTO/OFF (0%)
- `0x01` = LOW (30% / 3.0V)
- `0x02` = MEDIUM (50% / 5.0V)
- `0x03` = HIGH (90% / 9.0V)
- `0x04` = MAX (100% / 10.0V)

**Retransmission Behavior:**
- Every command is sent **4 times** (FAN_TX_FRAMES = 4)
- TTL decreases with each retransmission
- Ensures reliable delivery over RF

### 4. Network Pairing Sequence (RF_REMOTE)
**Captured on 2026-01-10 22:16**

Complete pairing sequence when a new RF_REMOTE joins the network:

#### Step 1: Remote Announces Link ID (JOIN_ACK 0x0C)
```
RX: 0x04 (Not a device type - signals JOIN_REQUEST mode!)
TX: 0x0F (RF_REMOTE) ID=0xD7
Command: 0x0C (JOIN_ACK)

Frame 1 Parameters: A5 5A 5A A5  → NETWORK_LINK_ID (0xA55A5AA5)
Frame 2 Parameters: 9B FD 75 FE  → Network ID (0xFE75FD9B in little endian!)
Frame 3 Parameters: 9B FD 75 FE  → Network ID (repeated)
```

**JOIN_ACK Format:**
- First frame: Link ID `0xA55A5AA5` (4 bytes, already defined in zehnder.h)
- Following frames: Network ID `0xFE75FD9B` in **little endian** byte order
- RX type `0x04` indicates "joining mode", not a device type

#### Step 2: Remote Requests to Join (JOIN_REQUEST 0x04)
```
RX: 0x0E (MAIN_CONTROL) ID=0x39
TX: 0x0F (RF_REMOTE) ID=0xD7
Command: 0x04 (JOIN_REQUEST)
Parameters: 9B FD 75 FE  → Network ID (little endian)
```

**JOIN_REQUEST Format:**
- Sent to MAIN_CONTROL (not MAIN_UNIT!)
- Contains network ID to join (0xFE75FD9B)
- Remote has already chosen its device ID (0xD7)

#### Step 3: Main Control Confirms Pairing (FRAME_0B)
```
RX: 0x0F (RF_REMOTE) ID=0xD7
TX: 0x0E (MAIN_CONTROL) ID=0x39
Command: 0x0B (FRAME_0B)
No parameters
```

**Pairing Handshake:**
- MAIN_CONTROL sends FRAME_0B to confirm pairing
- **Retransmitted 4 times** (TTL: 0xFA → 0xB4 → 0x75 → 0x2E)
- After this, the remote is fully paired and can send commands

#### Step 4: Status Update After Pairing
```
Command: 0x15 (STATUS_BROADCAST)
Parameters: 10 46 00  → Current status (70% voltage, timer off)
```

**Pairing Complete!** Remote can now send SETSPEED/SETTIMER commands.

### 5. STATUS_BROADCAST (Command 0x15)
**Observed on 2026-01-10 21:39**

The MAIN_CONTROL broadcasts its current status to all devices **after every status change**:

```
RX: 0x00 (BROADCAST) ID=0x00
TX: 0x0E (MAIN_CONTROL) ID=0x39
Command: 0x15 (STATUS_BROADCAST)
Parameters: 10 5A 01  → Voltage 90% (0x5A), Timer ON
Parameters: 10 32 00  → Voltage 50% (0x32), Timer OFF
```

**STATUS_BROADCAST Parameter Format:**
- Byte 1: Always `0x10` (identifier?)
- Byte 2: **Current voltage %** in hex (0x32=50%, 0x5A=90%)
- Byte 3: **Timer status** (0x00=OFF, 0x01=ON)

**Purpose:**
- Keeps all devices synchronized with current fan state
- Sent after every status change (speed change, timer change)
- Allows displays/controllers to show current state without polling

## Communication Patterns

### Bidirectional Communication
The system uses **full bidirectional RF communication:**

1. **Remote sends command** → Main unit
2. **Main unit acknowledges** → Remote
3. **Main unit sends current status** → Remote

### Frame Structure
```
Byte 0: RX Device Type
Byte 1: RX Device ID
Byte 2: TX Device Type
Byte 3: TX Device ID
Byte 4: TTL (Time To Live)
Byte 5: Command
Byte 6: Parameter Count
Bytes 7-15: Parameters (up to 9 bytes)
```

## Known Issues & Workarounds

### ESPHome Component Lifecycle Problem
**Issue:** ESPHome does not call `setup()` or `loop()` methods on custom components using the standard inheritance pattern.

**Workaround:**
1. Manual initialization via button press (`manual_init()`)
2. Manually call `nRF905::setup()` and `nRF905::loop()` from Zehnder component
3. Use manual buttons for testing and diagnostics

**Affected Methods:**
- `ZehnderRF::setup()` - Never called by ESPHome
- `ZehnderRF::loop()` - Never called by ESPHome (confirmed NOT the issue - loop IS called)
- `nRF905::setup()` - Never called, GPIO pins not initialized
- `nRF905::loop()` - Must be called manually for frame detection

### Main Control Panel (Wired)
The wired main control panel (Type 0x0E, ID 0x39) **DOES transmit RF signals**.
- ✅ Confirmed: MAIN_CONTROL sends SETSPEED commands via RF
- All speed changes from the wired panel are broadcast over RF to the main unit
- Both wired and wireless control methods use the same RF protocol

## TODO: Further Investigation

- [x] Decode all SETSPEED commands (speed 0-4) ✅ **COMPLETED**
- [x] Map all speed presets to voltage percentages ✅ **COMPLETED**
- [x] Decode SETTIMER parameter encoding ✅ **COMPLETED** (0x14, decimal minutes)
- [x] Document all FAN_SETTINGS parameter fields ✅ **COMPLETED** (target preset, current voltage %, timer)
- [x] Identify STATUS_BROADCAST command ✅ **COMPLETED** (0x15)
- [x] Discover voltage ramping behavior ✅ **COMPLETED**
- [x] Capture network join/pairing process ✅ **COMPLETED** (JOIN_ACK → JOIN_REQUEST → FRAME_0B)
- [x] Investigate command 0x0B purpose ✅ **COMPLETED** (Pairing handshake confirmation)
- [ ] Capture SETVOLTAGE commands (percentage control)
- [ ] Capture and test SETSPEED command transmission from ESPHome
- [ ] Test SETTIMER (0x14) transmission from ESPHome
- [ ] Understand why timer is sent to MAIN_CONTROL instead of MAIN_UNIT
- [ ] Decode FAN_SETTINGS byte 4 (timer-related parameter)

## Development Setup

### Manual Initialization Required
After boot, click **"Zehnder Sniffer Manual Init"** button in Home Assistant to initialize:
1. Load preferences
2. Call nRF905 setup() (GPIO initialization)
3. Configure RF parameters (868.2 MHz, network 0xFE75FD9B)
4. Set RX callbacks
5. Enable RECEIVE mode

### Diagnostic Buttons
- **Manual Init:** Initialize RF subsystem
- **Status Check:** Show current state and force RECEIVE mode
- **High 10min / Medium:** Test speed commands (not yet implemented)

### Debug Logging
All RF frames are logged with:
- Device types and IDs
- Command names
- Parameter values (hex dump)
- Bidirectional communication tracking

## Success Metrics
✅ RF frame reception working (promiscuous mode)
✅ All frames captured reliably - no missed frames
✅ DR (Data Ready) GPIO pin working correctly
✅ Frame parsing and logging functional
✅ Device identification working
✅ Command recognition implemented
✅ SETSPEED commands fully decoded (0x00-0x04)
✅ SETTIMER command fully decoded (0x14)
✅ STATUS_BROADCAST command identified (0x15)
✅ FAN_SETTINGS parameters decoded (target, current voltage, timer)
✅ Voltage ramping behavior discovered
✅ Retransmission behavior documented (4x per command)
✅ Both MAIN_CONTROL and RF_REMOTE confirmed working
✅ RF Remote operation sequence documented (2-step: timer + speed)
⏳ Command transmission (not tested yet)
⏳ Full protocol documentation (90% complete)

## Technical Achievements (2026-01-10)
- **Promiscuous sniffing mode**: Accept all RF frames regardless of address (AM flag ignored)
- **DR pin optimization**: Hardware-based frame detection via GPIO15
- **Reliable frame capture**: Proper DR flag clearing (Idle→Receive toggle)
- **Complete SETSPEED mapping**: All 5 speed levels documented (0x00-0x04)
- **SETTIMER protocol**: Decimal minutes encoding (0x14 command)
- **Voltage ramping discovery**: Fan gradually adjusts from current to target voltage
- **STATUS_BROADCAST**: Main control broadcasts state changes to all devices
- **Retransmission analysis**: 4 frames per command with TTL decay
- **RF Remote workflow**: Two-step operation (set timer → press button)

---
*Last updated: 2026-01-10*
