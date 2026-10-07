import io

NEW = '''ExchangeReport Esp32PeerExchange::exchange(const uint8_t peerAddress[6]) {
  ExchangeReport report{};
  if (peerAddress == nullptr || radio_ == nullptr) {
    // No address or no radio: the link failed, which is the distinction the report exists for.
    report.linkFailed = true;
    return report;
  }

  // 1. Receive whatever the peer already sent. A frame that fails its CRC or the format is the
  //    normal failure mode of a radio, so it is counted and dropped rather than reported.
  const int got = radio_->receive(scratch_, sizeof(scratch_));
  if (got < 0) {
    report.linkFailed = true;
    ++counters_.framesRefused;
    return report;
  }
  if (got > 0) {
    if (static_cast<size_t>(got) > sizeof(scratch_)) {
      ++counters_.framesRefused;
    } else {
      hal::PeerFrameContents contents{};
      if (!hal::decodePeerFrame(scratch_, static_cast<size_t>(got), contents)) {
        ++counters_.framesRefused;
      } else {
        ++counters_.framesReceived;
        report.receivedFromPeer = contents.recordCount;
        report.conflictsObserved =
            mergeVerified(contents.records, contents.recordCount).conflictsObserved;
      }
    }
  }

  // 2. Offer our own records, one frame's worth. The budget is arithmetic rather than taste: an
  //    ESP-NOW payload is 250 bytes and a record costs 59, so a frame carries three.
  if (local_.records == nullptr || local_.count == 0 || local_.nextSequence == 0) return report;
  if (!radio_->canSendNow()) {
    report.linkFailed = true;
    return report;
  }

  hal::ReplicatedRecord outgoing[kMaxRecordsPerFrame];
  size_t packed = 0;
  for (; packed < kMaxRecordsPerFrame && packed < local_.count; ++packed) {
    const Measurement& m = local_.records[packed];
    const size_t idLen = std::strlen(m.nodeId);
    // A record whose id does not fit the frame is skipped rather than truncated: a truncated id
    // is a different node, and the peer would store it as one.
    if (idLen == 0 || idLen >= sizeof(outgoing[packed].nodeId)) continue;
    std::memset(&outgoing[packed], 0, sizeof(outgoing[packed]));
    std::memcpy(outgoing[packed].nodeId, m.nodeId, idLen);
    std::snprintf(outgoing[packed].variable, sizeof(outgoing[packed].variable), "%s",
                  variableName(m.variable));
    outgoing[packed].sequence = m.sequence;
    outgoing[packed].timestampUtcMs = m.timestampUtcMs;
    outgoing[packed].value = m.value;
    outgoing[packed].quality = static_cast<uint8_t>(m.quality);
    outgoing[packed].reasonBits = m.reasonBits;
    outgoing[packed].timeUncertain = m.timeUncertain;
  }
  if (packed == 0) return report;

  const size_t frameLength = hal::encodePeerFrame(local_.records[0].nodeId, local_.nextSequence,
                                                  outgoing, packed, scratch_, sizeof(scratch_));
  if (frameLength == 0) {
    ++counters_.framesRefused;
    return report;
  }
  report.offeredToPeer = static_cast<uint32_t>(packed);

  const hal::RadioStatus status = radio_->send(scratch_, frameLength, peerAddress);
  if (status == hal::RadioStatus::Ok) {
    ++counters_.framesSent;
    report.acceptedByPeer = static_cast<uint32_t>(packed);
  } else {
    // Busy and NoRoute are both "retry later", so this is not a failure of the data.
    // `linkFailed` says exactly that, which is what lets a caller distinguish a peer being
    // unreachable from a peer having nothing new.
    report.linkFailed = true;
    ++counters_.framesRefused;
  }
  return report;
}

'''

p = 'firmware/lib/cauce_app/src/Esp32PeerExchange.cpp'
s = open(p, encoding='utf-8').read()
i = s.find('void Esp32PeerExchange::tick')
j = s.find('}  // namespace cauce::app')
if i < 0 or j < 0:
    raise SystemExit('anchors not found: tick=%d end=%d' % (i, j))
s = s[:i] + NEW + s[j:]
open(p, 'w', encoding='utf-8', newline='').write(s)
print('rewrote exchange()')