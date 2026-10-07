#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/hal/IFileSystem.h"

namespace cauce::app {

class PeerWatermarks {
 public:
  // Bounded, and not by taste: four peers is enough for a site this size, and the bound is what
  // stops a node that has ever seen twenty neighbours from growing a file on every boot.
  static constexpr size_t kMaxPeers = 4;

  explicit PeerWatermarks(hal::IFileSystem& fs, const char* path) : fs_(fs), path_(path) {}

  // The highest sequence merged from `nodeId`, or 0 if never seen. Passing a stored value that
  // is ahead of storage would make the merge store duplicates, so it is clamped on the way out.
  uint32_t get(const char* nodeId, uint32_t storageLastSequence) const;

  // Records progress. Ignored for a peer beyond the bound, and reported: silently dropping a
  // watermark means the next run rescans from zero, which is slow rather than wrong, but a
  // silent drop is indistinguishable from a bug.
  bool advance(const char* nodeId, uint32_t sequence);

  // Writes the file. Called by `advance` and available for an explicit flush; a node that
  // merges once an hour does not need a flash write per frame.
  bool persist() const;

  // Reads the file. A malformed entry is skipped rather than aborting the load: a truncated
  // write must not cost every peer its watermark.
  void load();

  size_t peerCount() const { return count_; }

  // Forgets one peer, for a node that has been gone long enough to be presumed replaced.
  bool forget(const char* nodeId);

 private:
  struct Entry {
    char nodeId[16]{};
    uint32_t sequence{0};
    bool inUse{false};
  };

  hal::IFileSystem& fs_;
  const char* path_;
  Entry entries_[kMaxPeers]{};
  size_t count_{0};
};

}  // namespace cauce::app
// Per-peer replication watermarks.
//
// WHY THIS EXISTS AND WHY IT IS NOT IN THE MERGE
//
// `containsRecord(nodeId, sequence, afterSequenceHint)` takes a watermark so its scan is bounded
// by how far behind a peer is rather than by store size. Without somewhere to *keep* that
// watermark the only caller can pass zero, and every received record pays a full scan - so this
// is what makes the primitive affordable rather than merely correct.
//
// It is deliberately not part of `Replication.h`. The merge is a pure function over records and
// a callback, and giving it storage would make it untestable with a fake and would couple the
// merge semantics to a filesystem. The watermark is the *transport's* state: it knows which
// peers it has talked to and how far each got.
//
// THE FORMAT
//
// One line per peer: `node_id=sequence`, semicolon separated, matching `CommandExecutor`'s
// applied-state file so there is one convention in the codebase rather than two. A plain text
// file is right here for the reason it is right there: it can be read during support with
// `cat`, which is the only thing anyone ever actually does with a state file on a node in the
// field.
//
// A watermark that is *ahead* of what storage holds is worse than one that is behind, because it
// makes the merge answer "not present" for records that are present and store duplicates. So a
// load never raises a stored watermark beyond the store's own last sequence.
