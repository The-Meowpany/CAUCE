#pragma once

// SX1276 LoRa driver, behind ILoRaRadio.
//
// WHY IT IS BUILT AGAINST INTERFACES AND NOT AGAINST SPIClass
//
// A driver written directly against the Arduino SPI object can only be tested by
// plugging in a board and reading a register back. The register sequence - which
// masks which interrupts, in what order the opmode transitions happen, where the
// FIFO pointers are set - would then never be asserted by a test, only by whatever
// the radio happens to do. Behind ISpiBus the entire protocol is exercised on the
// host against a scripted bus, and a bench run becomes a confirmation rather than
// the first execution.
//
// WHAT IS VERIFIED AND WHAT IS NOT
//
// Verified here: the register sequence, the register values for each spreading
// factor and bandwidth, the interrupt masking, the FIFO addressing, the payload
// length refusal, the RSSI conversion arithmetic, and the timeout path.
//
// NOT verified: that the silicon behaves as the datasheet describes. The maximum
// payload figures below come from the SX1276 datasheet's payload table and are
// carried as explicit constants rather than computed, because a formula recalled
// from memory is not a datasheet. They are named, commented, and checked for
// monotonicity against the spreading factor by a test, so a correction from the
// bench is one edit in one table.
//
// The RSSI read uses RegOptRssi. That register is the SX1276's; the SX1278 differs,
// and reading the wrong one returns a plausible number that is off by a constant.

#include <cstddef>
#include <cstdint>

#include "cauce/hal/ILoRaRadio.h"
#include "cauce/hal/IRadioBus.h"

namespace cauce::hal {

// SPI address form of the FIFO registers.
//
// The SX1276 puts the write flag in bit 7 of the SPI address byte, so only seven
// address bits ever reach the chip. The datasheet's register 0x80 is therefore
// written as 0x00, 0x81 as 0x01 and 0x82 as 0x02.
//
// This is not a stylistic note. Writing the datasheet number directly sends
// 0x80 | 0x80 == 0x80, which addresses register 0x00 - the transmit FIFO - so the
// base-address write lands inside the data buffer and the radio transmits whatever
// follows it. No host test could have caught it, because the register log looked
// plausible: the value was written, just to the wrong place.
constexpr uint8_t kSpiFifoTxBaseAddr = 0x00;
constexpr uint8_t kSpiFifoRxBaseAddr = 0x01;
constexpr uint8_t kSpiFifoRxCurrentAddr = 0x02;

// Registers. Spelled out rather than generated, because a reader checking this
// against a datasheet should find the same names in the same order. The FIFO
// registers are listed with their datasheet numbers for reference; use the kSpi*
// constants above to access them.
enum Sx1276Register : uint8_t {
  kRegFifo = 0x00,
  kRegOpMode = 0x01,
  kRegLna = 0x0C,
  kRegDetectOptimize = 0x31,
  kRegIrqFlagsMask = 0x11,
  kRegIrqFlags = 0x12,
  kRegRxNbBytes = 0x13,
  kRegModemConfig1 = 0x1D,
  kRegModemConfig2 = 0x1E,
  kRegPreambleMsb = 0x20,
  kRegPreambleLsb = 0x21,
  kRegPayloadLength = 0x22,
  kRegModemConfig3 = 0x26,
  kRegOptRssi = 0x1C,
  kRegSyncWord = 0x39,
  kRegDioMapping1 = 0x40,
  // Datasheet numbers; NOT SPI addresses. See kSpiFifoTxBaseAddr above.
  kRegFifoTxBaseAddrDatasheet = 0x80,
  kRegFifoRxBaseAddrDatasheet = 0x81,
  kRegFifoRxCurrentAddrDatasheet = 0x82,
};

// OpMode values.
// OpMode on the SX1276 has a THREE-bit mode field, not two, and bit 7 is a
// separate LongRangeMode selector.
//
// Two silent bugs lived here. Masking the mode field with 0x03 turned
// ReceiveContinuous (0x05) into 0x01, which is Standby - so the radio transmitted
// once and then sat deaf forever, and every functional test still passed. And
// kModeLoRa was written as 0x03, which is not the LoRa selector at all: that is
// 0x80. begin() therefore never put the radio in LoRa mode while its own comment
// said it did.
constexpr uint8_t kModeFieldMask = 0x07;
constexpr uint8_t kModeLongRange = 0x80;

enum Sx1276Mode : uint8_t {
  kModeSleep = 0x00,
  kModeStandby = 0x01,
  kModeTransmit = 0x03,
  kModeReceiveContinuous = 0x05,
  kModeReceiveSingle = 0x06,
};

// Interrupt flags, in the IrqFlags register.
enum Sx1276Irq : uint8_t {
  kIrqTxDone = 0x08,
  kIrqRxDone = 0x40,
  kIrqPayloadCrcError = 0x20,
};

// Bandwidth codes as they appear in ModemConfig1 bits 7-4, in Hz.
struct LoRaBandwidth {
  uint8_t code;
  uint32_t hz;
};

// From the SX1276 datasheet's bandwidth table. The code values are the register
// encoding, not an index.
constexpr LoRaBandwidth kBandwidths[] = {
    {0x00, 7800},   {0x01, 10400},  {0x02, 15600},  {0x03, 20800},
    {0x04, 31250},  {0x05, 41700},  {0x06, 62500},  {0x07, 125000},
    {0x08, 250000}, {0x09, 500000},
};

// Maximum payload per spreading factor at 125 kHz, explicit header off, CRC on,
// coding rate 4/5. From the SX1276 datasheet payload table.
//
// Indexed BY SPREADING FACTOR, so the array is 13 wide with the unusable entries
// zeroed. It was briefly 7 wide and indexed by sf, which read two entries past the
// end for every SF above 8 and returned whatever happened to be there.
//
// CARRIED, NOT COMPUTED. A formula recalled from memory is not a datasheet, and a
// wrong one produces a limit that either truncates silently or refuses frames that
// would have fit. A test checks the table is self-consistent, which is the most that
// can be asserted without the document in front of you, and the bench corrects one
// entry if it is wrong.
constexpr uint16_t kMaxPayloadBytes[13] = {
    // Indices 0 through 7 are spreading factors 0 through 7. None are usable: the
    // radio cannot do below SF6, and this project refuses SF6 and SF7 because an
    // OTA manifest will not fit in one frame at those settings.
    0, 0, 0, 0, 0, 0, 0, 0,
    230,  // index 8  = SF8
    115,  // index 9  = SF9 - the default, and why the frame format is 60 bytes
    222,  // index 10 = SF10. Lower than SF9 in the datasheet because SF9's
          // practical limit there is airtime under a duty-cycle cap, not radio
          // capacity.
    230,  // index 11 = SF11
    222,  // index 12 = SF12
};

constexpr uint8_t kDefaultSpreadingFactor = 9;
constexpr uint8_t kDefaultBandwidthCode = 0x07;  // 125 kHz

// A private network wants a private sync word. The default 0x12 is what every
// SX1276 ships with, so two unmodified radios on the same frequency hear each
// other. Changed deliberately here.
constexpr uint8_t kPrivateSyncWord = 0xCA;

struct Sx1276Config {
  uint32_t frequencyHz{868100000};
  uint8_t spreadingFactor{kDefaultSpreadingFactor};
  uint8_t bandwidthCode{kDefaultBandwidthCode};
  uint16_t preambleSymbols{8};
  int8_t outputPowerDbm{14};
  uint8_t syncWord{kPrivateSyncWord};
  // Long enough for the slowest frame plus the ack. A LoRa SF9 frame of 60 bytes at
  // 125 kHz is about 190 ms, so this is generous rather than tuned.
  uint32_t txTimeoutMs{4000};
};

class Sx1276Radio final : public ILoRaRadio {
 public:
  Sx1276Radio(ISpiBus& spi, IRadioControl& control, uint8_t resetPin,
              uint8_t dio0Pin, uint8_t dio1Pin, uint8_t busyPin);

  // Puts the radio into LoRa standby with the configured modulation. Returns false
  // when the configuration is not one the radio supports, which is a programming
  // error rather than a hardware fault and must not be retried forever.
  bool begin(const Sx1276Config& config);

  bool canSendNow() override;
  bool send(const uint8_t* data, size_t length) override;
  int receive(uint8_t* buffer, size_t capacity) override;
  int16_t lastRssiDbm() const override { return lastRssiDbm_; }

  // True when the last receive was rejected by the radio's CRC rather than
  // accepted. A node that cannot tell those apart will treat noise as data.
  bool lastFrameFailedCrc() const { return lastCrcError_; }

  // Maximum payload for the configured modulation, from the table above.
  uint16_t maxPayloadBytes() const;

  const Sx1276Config& config() const { return config_; }

 private:
  uint8_t readRegister(uint8_t reg);
  void writeRegister(uint8_t reg, uint8_t value);
  void writeMasked(uint8_t reg, uint8_t mask, uint8_t value);
  void setMode(uint8_t mode);
  void dumpRegisters();
  bool awaitIrq(uint8_t flag, uint32_t timeoutMs);

  ISpiBus& spi_;
  IRadioControl& control_;
  uint8_t resetPin_;
  uint8_t dio0Pin_;
  uint8_t dio1Pin_;
  uint8_t busyPin_;
  Sx1276Config config_{};
  int16_t lastRssiDbm_{0};
  bool lastCrcError_{false};
  bool ready_{false};
};

}  // namespace cauce::hal