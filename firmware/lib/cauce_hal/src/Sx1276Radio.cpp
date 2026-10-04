#include "cauce/hal/Sx1276Radio.h"

namespace cauce::hal {

namespace {

// Reset and configuration settle time. The datasheet says 1 ms after reset; the
// margin is because a marginal supply browns out during the current draw of setting
// the LNA, and the symptom is a radio that is intermittently deaf.
constexpr uint32_t kResetSettleMs = 5;
constexpr uint32_t kModeSettleMs = 1;

// Datasheet: RegOptRssi is RSSI in dBm plus 157.
constexpr int16_t kOptRssiOffsetDbm = -157;

// The FIFO size is deliberately not a constant here. It was a uint8_t 256 once,
// which is 0, and it was compared against the received length - so the guard
// rejected every frame including the one-byte ack. The compiler caught it with
// -Woverflow during the ESP32 cross-build; reading the code had not. RxNbBytes is a
// uint8_t and therefore cannot exceed 255 on its own, so the binding constraint on
// receive is the caller's buffer.

}  // namespace

Sx1276Radio::Sx1276Radio(ISpiBus& spi, IRadioControl& control, uint8_t resetPin,
                         uint8_t dio0Pin, uint8_t dio1Pin, uint8_t busyPin)
    : spi_(spi),
      control_(control),
      resetPin_(resetPin),
      dio0Pin_(dio0Pin),
      dio1Pin_(dio1Pin),
      busyPin_(busyPin) {}

uint8_t Sx1276Radio::readRegister(uint8_t reg) {
  spi_.beginTransaction();
  spi_.transfer(static_cast<uint8_t>(reg & 0x7F));
  const uint8_t value = spi_.transfer(0x00);
  spi_.endTransaction();
  return value;
}

void Sx1276Radio::writeRegister(uint8_t reg, uint8_t value) {
  spi_.beginTransaction();
  spi_.transfer(static_cast<uint8_t>(reg | 0x80));
  spi_.transfer(value);
  spi_.endTransaction();
}

// Read-modify-write. Every configuration register shares bits with something else -
// ModemConfig1 holds bandwidth in the high nibble and coding rate in the low - so
// writing a whole register to set one field silently clears the other. This is the
// single most common way a driver like this is wrong.
void Sx1276Radio::writeMasked(uint8_t reg, uint8_t mask, uint8_t value) {
  const uint8_t current = readRegister(reg);
  writeRegister(reg, static_cast<uint8_t>((current & ~mask) | (value & mask)));
}

void Sx1276Radio::setMode(uint8_t mode) {
  writeMasked(kRegOpMode, kModeFieldMask, mode);
  // Poll rather than assume. The datasheet's mode-change latency is in tens of
  // microseconds, but a blocking delay sized for the datasheet is a race on a
  // loaded MCU, and the poll costs nothing.
  const uint32_t deadline = control_.millis() + 100;
  while (control_.millis() < deadline) {
    if ((readRegister(kRegOpMode) & kModeFieldMask) == mode) return;
    control_.delay(1);
  }
}

void Sx1276Radio::dumpRegisters() {
  control_.pinMode(dio0Pin_, false);
  control_.pinMode(dio1Pin_, false);
  control_.pinMode(busyPin_, false);
}

uint16_t Sx1276Radio::maxPayloadBytes() const {
  // Zero until the radio has been configured. The default-constructed config already
  // says SF9, so without this a caller asking before begin() would get a confident
  // number for a radio that has never been told what it is.
  if (!ready_) return 0;
  const uint8_t sf = config_.spreadingFactor;
  if (sf < 8 || sf > 12) return 0;
  return kMaxPayloadBytes[sf];
}

bool Sx1276Radio::begin(const Sx1276Config& config) {
  config_ = config;
  ready_ = false;

  if (config_.spreadingFactor < 8 || config_.spreadingFactor > 12) return false;
  if (config_.bandwidthCode > 0x09) return false;
  if (kMaxPayloadBytes[config_.spreadingFactor] == 0) return false;
  // The datasheet caps SF6 and SF7 to specific bandwidths; rather than encode a
  // table of exceptions, refuse them. This project does not use them.
  if (config_.spreadingFactor == 6 || config_.spreadingFactor == 7) return false;

  control_.pinMode(resetPin_, true);
  control_.digitalWrite(resetPin_, true);
  control_.delay(1);
  control_.digitalWrite(resetPin_, false);
  control_.delay(kResetSettleMs);
  control_.digitalWrite(resetPin_, true);
  control_.delay(kResetSettleMs);

  // LoRa only. The reset default is FSK, and an FSK-configured radio on a LoRa
  // network emits noise the central rejects frame by frame, for hours.
  //
  // The selector is bit 7 and is NOT part of the mode field. It used to be written
  // through the mode mask as 0x03 - which is Transmit - so the radio never left
  // FSK and this comment claimed otherwise. The test
  // test_begin_actually_sets_the_lora_bit is what catches that.
  writeMasked(kRegOpMode, kModeLongRange, kModeLongRange);
  setMode(kModeSleep);

  // 64-bit arithmetic. frequencyHz * 64 is about 5.5e10, which overflows a uint32_t
// and silently produced an FRF of 125 instead of 1736 for 868.1 MHz - a radio
// listening on the wrong frequency, with every functional test still able to pass
// because nothing checked where it ended up listening.
const uint32_t frf = static_cast<uint32_t>(
    (static_cast<uint64_t>(config_.frequencyHz) * 64ULL) / 32000000ULL);
  writeRegister(0x06, static_cast<uint8_t>(frf >> 16));
  writeRegister(0x07, static_cast<uint8_t>(frf >> 8));
  writeRegister(0x08, static_cast<uint8_t>(frf));

  // FIFOs at the ends, so a full 255-byte payload fits either way.
  writeRegister(kSpiFifoTxBaseAddr, 0x00);
  writeRegister(kSpiFifoRxBaseAddr, 0x00);
  writeRegister(kSpiFifoRxCurrentAddr, 0x00);

  // Only TxDone on DIO0. Everything else is polled, because a second interrupt
  // source needs a second path and nothing here needs one.
  writeRegister(kRegIrqFlagsMask, 0x3F);
  writeRegister(kRegDioMapping1, 0x40);  // DIO0 -> RxDone | TxDone

  writeRegister(kRegModemConfig1,
                static_cast<uint8_t>((config_.bandwidthCode << 4) | 0x04));
  writeRegister(kRegModemConfig2,
                static_cast<uint8_t>((config_.spreadingFactor << 4) | 0x04));

  const uint16_t preamble =
      static_cast<uint16_t>(config_.preambleSymbols & 0xFFFF);
  writeRegister(kRegPreambleMsb, static_cast<uint8_t>(preamble >> 8));
  writeRegister(kRegPreambleLsb, static_cast<uint8_t>(preamble & 0xFF));
  writeRegister(kRegPayloadLength, 0x01);

  // SF6 needs a different detect-optimise value. This project refuses SF6, so the
  // SF7-and-up value is written unconditionally rather than branched on.
  writeRegister(kRegDetectOptimize, 0xC5);
  writeRegister(kRegModemConfig3, 0x04);  // Aqc = 0, no low-data-rate optimise
  writeRegister(kRegSyncWord, config_.syncWord);

  setMode(kModeStandby);
  dumpRegisters();

  lastRssiDbm_ = 0;
  lastCrcError_ = false;
  ready_ = true;
  return true;
}

bool Sx1276Radio::canSendNow() {
  if (!ready_) return false;
  const uint8_t mode = readRegister(kRegOpMode) & kModeFieldMask;
  return mode == kModeStandby || mode == kModeReceiveContinuous;
}

bool Sx1276Radio::awaitIrq(uint8_t flag, uint32_t timeoutMs) {
  const uint32_t deadline = control_.millis() + timeoutMs;
  while (control_.millis() < deadline) {
    if ((readRegister(kRegIrqFlags) & flag) != 0) return true;
    control_.delay(1);
  }
  return false;
}

bool Sx1276Radio::send(const uint8_t* data, size_t length) {
  if (!ready_ || !data) return false;
  const uint16_t max = maxPayloadBytes();
  // Refuse rather than truncate. A truncated frame decodes to a short but valid
  // looking record, and the node would acknowledge a measurement it never sent.
  if (length == 0 || length > max || length > 0xFF) return false;

  setMode(kModeStandby);

  writeRegister(kRegIrqFlags, static_cast<uint8_t>(kIrqTxDone | kIrqPayloadCrcError));
  writeRegister(kSpiFifoTxBaseAddr, 0x00);
  writeRegister(kSpiFifoRxCurrentAddr, 0x00);
  writeRegister(kRegPayloadLength, static_cast<uint8_t>(length));

  spi_.beginTransaction();
  spi_.transfer(kRegFifo | 0x80);
  spi_.write(data, length);
  spi_.endTransaction();

  setMode(kModeTransmit);

  if (!awaitIrq(kIrqTxDone, config_.txTimeoutMs)) {
    // Clear and park in standby so a wedged transmit cannot leave the radio in TX,
    // where it would deafen receive and drain the battery.
    writeRegister(kRegIrqFlags, kIrqTxDone);
    setMode(kModeStandby);
    return false;
  }
  writeRegister(kRegIrqFlags, kIrqTxDone);
  setMode(kModeReceiveContinuous);
  return true;
}

int Sx1276Radio::receive(uint8_t* buffer, size_t capacity) {
  if (!ready_ || !buffer) return -1;
  lastCrcError_ = false;

  const uint8_t irq = readRegister(kRegIrqFlags);
  if ((irq & kIrqRxDone) == 0) return 0;

  const uint8_t length = readRegister(kRegRxNbBytes);
  // The FIFO bound is deliberately absent: RxNbBytes is a uint8_t, so it cannot
  // exceed 255 and the check would be dead code the compiler rightly flags. The
  // binding constraint is `capacity`, which is checked.
  if (length == 0 || length > capacity) {
    lastCrcError_ = true;
    writeRegister(kRegIrqFlags, static_cast<uint8_t>(kIrqRxDone | kIrqPayloadCrcError));
    setMode(kModeReceiveContinuous);
    return -1;
  }

  const uint8_t address = readRegister(kSpiFifoRxCurrentAddr);
  spi_.beginTransaction();
  spi_.transfer(static_cast<uint8_t>(kRegFifo | 0x80));
  // Burst read with an incrementing address, which needs the address's high bit
  // replaced by the increment flag. Getting this wrong reads the same byte 255
  // times and looks like a plausible frame of constant noise.
  uint8_t current = address;
  for (uint8_t i = 0; i < length; ++i) {
    buffer[i] = spi_.transfer(static_cast<uint8_t>(current));
    current = static_cast<uint8_t>(current + 1);
  }
  spi_.endTransaction();

  if ((irq & kIrqPayloadCrcError) != 0) {
    lastCrcError_ = true;
    writeRegister(kRegIrqFlags, static_cast<uint8_t>(kIrqRxDone | kIrqPayloadCrcError));
    setMode(kModeReceiveContinuous);
    return -1;
  }

  writeRegister(kRegIrqFlags, static_cast<uint8_t>(kIrqRxDone | kIrqPayloadCrcError));

  const uint8_t optRssi = readRegister(kRegOptRssi);
  lastRssiDbm_ = static_cast<int16_t>(optRssi) + kOptRssiOffsetDbm;

  setMode(kModeReceiveContinuous);
  return length;
}

}  // namespace cauce::hal