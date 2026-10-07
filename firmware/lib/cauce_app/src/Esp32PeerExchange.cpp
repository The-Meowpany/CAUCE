#include "cauce/app/Esp32PeerExchange.h"

#include <cstdio>
#include <cstring>

namespace cauce::app {

namespace {

// The merge's apply-context. A struct rather than a cast from \Esp32PeerExchange*\, so if the
// exchange's members move, this is the one place that has to move with them - and the compiler
// says so, which is the whole point of a struct over a cast.
struct MergeApplyContext {
  LogStorageRepository* store{nullptr};
  PeerWatermarks* watermarks{nullptr};
  uint32_t storageLastSequence{0};
};

}  // namespace

Esp32PeerExchange::Esp32PeerExchange(LogStorageRepository& store, PeerWatermarks& watermarks,
                           hal::IPeerRadio* radio, hal::IPeerDiscovery* discovery)
    : store_(&store), watermarks_(&watermarks), radio_(radio), discovery_(discovery) {}

bool Esp32PeerExchange::enable() {
  enabled_ = radio_ != nullptr && discovery_ != nullptr;
  return enabled_;
}

void Esp32PeerExchange::disable() {
  enabled_ = false;
}

bool Esp32PeerExchange::toMeasurement(const ReplicatedRecord& in, Measurement& out) {
  // The node id must round-trip. An empty id or one that fills the field with no terminator
  // would be stored as a key that reads past itself forever, so both are refused rather than
  // truncated into something that looks valid.
  if (in.nodeId[0] == '\0') return false;
  const size_t idLen = std::strlen(in.nodeId);
  if (idLen >= sizeof(out.nodeId)) return false;

  std::memset(&out, 0, sizeof(out));
  std::memcpy(out.nodeId, in.nodeId, idLen);
  out.sequence = in.sequence;
  out.timestampUtcMs = in.timestampUtcMs;
  out.value = in.value;
  out.reasonBits = in.reasonBits;
  out.timeUncertain = in.timeUncertain;

  // The lossy part, made explicit: an unrecognised name becomes Unknown and the measurement is
  // still stored. Losing the *meaning* is recoverable; losing the node id would corrupt another
  // node's history, which is why the check above is enforced and this is only documented.
  out.variable = parseVariable(in.variable);

  // `quality` is a uint8_t on the wire and an enum here. Clamped rather than cast: an
  // out-of-range value from a peer is not a valid Quality, and casting it produces an enum whose
  // name is whatever happens to sit at that index.
  // Valid..Missing is 0..6. Out of range means the peer sent something this build does not
  // define, and Invalid is the honest landing: the measurement was received and cannot be
  // trusted, rather than Suspect, which implies a real measurement that was doubted.
  const uint8_t raw = in.quality;
  out.quality = (raw <= static_cast<uint8_t>(Quality::Missing)) ? static_cast<Quality>(raw)
                                                              : Quality::Invalid;

  // A peer's record belongs to the peer's sensor. Using this node's sensor name would attribute
  // another node's measurement to this node's hardware, which is worse than a generic name.
  std::snprintf(out.sensorId, sizeof(out.sensorId), "%s", "peer");
  return true;
}

MergeOutcome Esp32PeerExchange::applyToStore(const ReplicatedRecord& record, void* context) {
  auto* ctx = static_cast<MergeApplyContext*>(context);
  if (ctx == nullptr || ctx->store == nullptr) return MergeOutcome::Rejected;

  Measurement converted{};
  if (!toMeasurement(record, converted)) {
    // Not a duplicate and not a conflict: the record could not be represented honestly, so it
    // was not merged. Reporting it as any other outcome would make a lossy conversion look like
    // a working one.
    return MergeOutcome::Rejected;
  }

  // The primitive this exchange was blocked on: "does the replica already hold this?" for a
  // record from *another* node, answered with the per-peer watermark so the scan is bounded by
  // how far behind that peer is rather than by store size.
  const uint32_t hint = ctx->watermarks != nullptr
                            ? ctx->watermarks->get(record.nodeId, ctx->storageLastSequence)
                            : 0;

  // At or below the watermark means already merged - the watermark *is* the summary of what is
  // held - so this is a duplicate without consulting storage.
  //
  // This ordering is not an optimisation, it is a correctness fix, and the host suite found it.
  // `containsRecord` short-circuits a sequence at or below its hint to "not present" *because*
  // that is the contract for a hint the caller trusts. Letting it answer first therefore said
  // "not present" about a record that was present, and the merge appended it a second time - two
  // rows that look like two measurements of the same thing.
  if (record.sequence <= hint) {
    return MergeOutcome::Duplicate;
  }
  if (ctx->store->containsRecord(record.nodeId, record.sequence, hint)) {
    return MergeOutcome::Duplicate;
  }
  if (!ctx->store->append(converted)) {
    // The store said no - full, or a write failure. Collapsing this into "duplicate" would make
    // a full node report a healthy merge that dropped data.
    return MergeOutcome::Rejected;
  }
  return MergeOutcome::Accepted;
}

MergeReport Esp32PeerExchange::mergeVerified(const ReplicatedRecord* records, size_t count) {
  MergeReport report{};
  if (records == nullptr || count == 0) return report;
  if (store_ == nullptr) return report;

  MergeApplyContext ctx{};
  ctx.store = store_;
  ctx.watermarks = watermarks_;
  // Refreshed per merge rather than cached: storage changes under us as records are appended,
  // and a stale `lastSequence` would clamp a watermark against a number that has moved on.
  ctx.storageLastSequence = store_->lastSequence();

  report = mergeRecords(records, count, &applyToStore, &ctx);
  counters_.recordsOffered += report.recordsOffered;
  counters_.recordsAccepted += report.recordsAccepted;
  counters_.duplicatesIgnored += report.duplicatesIgnored;
  counters_.conflictsObserved += report.conflictsObserved;

  // Whether anything was refused. `mergeRecords` has no `recordsRejected` field, so it is
  // derived: offered minus the three outcomes that mean "the record was handled".
  const uint32_t handled = report.recordsAccepted + report.duplicatesIgnored +
                           report.conflictsObserved;
  const bool anyRefused = report.recordsOffered > handled;

  if (anyRefused) {
    // The watermark does not advance. Advancing past a record that was refused would mark it as
    // merged when it is not held, and the peer would never offer it again - silent data loss
    // that no counter would show.
    ++counters_.mergesBlocked;
    return report;
  }

  // Advance per distinct peer, not for the whole batch: a frame can carry records from more than
  // one node if a gateway aggregates, and advancing only the first would leave the rest behind
  // to be rescanned forever.
  for (size_t i = 0; i < count; ++i) {
    if (records[i].nodeId[0] == '\0') continue;
    if (watermarks_ == nullptr) break;
    if (!watermarks_->advance(records[i].nodeId, records[i].sequence)) {
      // Refused because the table is full. Reported rather than silently dropped: the next
      // frame from that peer will be rescanned from zero, which is slow rather than wrong, but
      // a silent drop is indistinguishable from a bug.
      ++counters_.mergesBlocked;
    }
  }
  if (watermarks_ != nullptr) (void)watermarks_->persist();
  return report;
}

ExchangeReport Esp32PeerExchange::exchange(const uint8_t peerAddress[6]) {
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
    // Busy is "retry later", which is a fact about the link rather than about the data, and
    // `linkFailed` is what lets a caller tell the two apart.
    report.linkFailed = true;
    return report;
  }

  ReplicatedRecord outgoing[kMaxRecordsPerFrame];
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
    report.linkFailed = true;
    ++counters_.framesRefused;
  }
  return report;
}

}  // namespace cauce::app