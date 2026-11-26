#include "StorySaveManager.h"
#include "InkDisplayManager.h"
#include "InkStoryManager.h"
#include <cstdio>
#include <cstring>

StorySaveManager::StorySaveManager() {}

StorySaveManager::~StorySaveManager() {}

void StorySaveManager::init(const std::string &saveFilePath,
                            uint32_t storyHash) {
  _saveFilePath = saveFilePath;
  _storyHash = storyHash;
  _hasMainProgress = false;
  _mainSnapshot.clear();
  _mainHistory.clear();
  _mainHistorySerialized.clear();
  _mainHistorySerialized.shrink_to_fit();
  _borrowedMainSnapshot = nullptr;
  _borrowedMainSnapLen = 0;
  _mainStreamFn = nullptr;
  _checkpoints.clear();
  _checkpoints.shrink_to_fit();
  _undoFilePath = saveFilePath.empty() ? "" : saveFilePath + ".undo";
  _hasUndo = false;
  _undoSnapLen = 0;
  _undoCpCount = 0;
  _undoStreamFn = nullptr;
  _undoHistory.clear();
  _undoHistoryBorrowed = nullptr;
}

bool StorySaveManager::serializeHistory(const std::deque<WrappedLine> &history,
                                        IFileWriter &writer) {
  uint16_t historySize = static_cast<uint16_t>(history.size());
  uint8_t szBuf[2] = {static_cast<uint8_t>(historySize & 0xFF),
                      static_cast<uint8_t>((historySize >> 8) & 0xFF)};
  if (!writer.write(szBuf, 2))
    return false;

  for (const auto &line : history) {
    std::string text = line.block.getText();
    if (line.isImage) {
      text = "\x1B[IMG:" + line.imagePath + "]";
    }
    uint16_t len = static_cast<uint16_t>(text.length());
    uint8_t lenBuf[2] = {static_cast<uint8_t>(len & 0xFF),
                         static_cast<uint8_t>((len >> 8) & 0xFF)};
    if (!writer.write(lenBuf, 2))
      return false;

    if (len > 0) {
      if (!writer.write(text.data(), len))
        return false;
    }

    uint8_t flags = (line.isOld ? 1 : 0) | (line.endOfParagraph ? 2 : 0);
    if (!writer.write(&flags, 1))
      return false;
  }
  return true;
}

void StorySaveManager::serializeHistory(const std::deque<WrappedLine> &history,
                                        std::vector<uint8_t> &out) {
  out.clear();
  // Reserve once to avoid repeated reallocations + fragmentation during
  // save (300-line history ~20KB). Estimate 64B/line average.
  size_t estimate = 2 + history.size() * 64;
  // Cap the reservation sanity (historySize is uint16 on the wire).
  if (estimate > 256 * 1024) {
    estimate = 256 * 1024;
  }
  out.reserve(estimate);
  uint16_t historySize = static_cast<uint16_t>(history.size());
  out.push_back(static_cast<uint8_t>(historySize & 0xFF));
  out.push_back(static_cast<uint8_t>((historySize >> 8) & 0xFF));

  for (const auto &line : history) {
    std::string text = line.block.getText();
    if (line.isImage) {
      text = "\x1B[IMG:" + line.imagePath + "]";
    }
    uint16_t len = static_cast<uint16_t>(text.length());
    out.push_back(static_cast<uint8_t>(len & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));

    const uint8_t *textBytes =
        reinterpret_cast<const uint8_t *>(text.c_str());
    out.insert(out.end(), textBytes, textBytes + len);

    uint8_t flags = (line.isOld ? 1 : 0) | (line.endOfParagraph ? 2 : 0);
    out.push_back(flags);
  }
}

bool StorySaveManager::deserializeHistory(const uint8_t *&ptr,
                                          size_t &remaining,
                                          std::deque<WrappedLine> &out,
                                          InkStoryManager *storyMgr) {
  out.clear();
  if (remaining < 2)
    return false;

  uint16_t historySize = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
  ptr += 2;
  remaining -= 2;

  for (uint16_t i = 0; i < historySize; i++) {
    if (remaining < 2)
      return false;
    uint16_t lineLen = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
    ptr += 2;
    remaining -= 2;

    if (remaining < lineLen)
      return false;
    std::string str(reinterpret_cast<const char *>(ptr), lineLen);
    ptr += lineLen;
    remaining -= lineLen;

    if (remaining < 1)
      return false;
    uint8_t flags = *ptr++;
    remaining -= 1;

    bool isOld = (flags & 1) != 0;
    bool endOfParagraph = (flags & 2) != 0;
    bool isImage = false;
    std::string imagePath = "";

    if (str.length() > 7 && str.substr(0, 6) == "\x1B[IMG:") {
      isImage = true;
      imagePath = str.substr(6, str.length() - 7);
      str = "";
    }

    WrappedLine wl;
    wl.block = TextBlock(str);
    wl.isOld = isOld;
    wl.isImage = isImage;
    wl.imagePath = imagePath;
    wl.imageHeight = (isImage && storyMgr)
                         ? storyMgr->getImageHeight(imagePath.c_str())
                         : (isImage ? 280 : 0);
    wl.endOfParagraph = endOfParagraph;
    out.push_back(wl);
  }

  return true;
}

bool StorySaveManager::deserializeHistory(IFileReader &reader,
                                          std::deque<WrappedLine> &out,
                                          InkStoryManager *storyMgr) {
  out.clear();
  uint8_t szBuf[2];
  if (reader.read(szBuf, 2) != 2)
    return false;

  uint16_t historySize = szBuf[0] | (static_cast<uint16_t>(szBuf[1]) << 8);

  for (uint16_t i = 0; i < historySize; i++) {
    uint8_t lenBuf[2];
    if (reader.read(lenBuf, 2) != 2)
      return false;
    uint16_t lineLen = lenBuf[0] | (static_cast<uint16_t>(lenBuf[1]) << 8);

    std::string str;
    if (lineLen > 0) {
      str.resize(lineLen);
      if (reader.read(&str[0], lineLen) != lineLen)
        return false;
    }

    uint8_t flags = 0;
    if (reader.read(&flags, 1) != 1)
      return false;

    bool isOld = (flags & 1) != 0;
    bool endOfParagraph = (flags & 2) != 0;
    bool isImage = false;
    std::string imagePath = "";

    if (str.length() > 7 && str.substr(0, 6) == "\x1B[IMG:") {
      isImage = true;
      imagePath = str.substr(6, str.length() - 7);
      str = "";
    }

    WrappedLine wl;
    wl.block = TextBlock(str);
    wl.isOld = isOld;
    wl.isImage = isImage;
    wl.imagePath = imagePath;
    wl.imageHeight = (isImage && storyMgr)
                         ? storyMgr->getImageHeight(imagePath.c_str())
                         : (isImage ? 280 : 0);
    wl.endOfParagraph = endOfParagraph;
    out.push_back(wl);
  }

  return true;
}

bool StorySaveManager::skipHistory(IFileReader &reader) {
  uint8_t szBuf[2];
  if (reader.read(szBuf, 2) != 2) {
    return false;
  }
  uint16_t historySize = szBuf[0] | (static_cast<uint16_t>(szBuf[1]) << 8);
  for (uint16_t i = 0; i < historySize; ++i) {
    uint8_t lenBuf[2];
    if (reader.read(lenBuf, 2) != 2) {
      return false;
    }
    uint16_t lineLen = lenBuf[0] | (static_cast<uint16_t>(lenBuf[1]) << 8);
    size_t next = reader.tell() + lineLen + 1; // text + flags
    if (next > reader.size()) {
      return false;
    }
    if (!reader.seek(next)) {
      return false;
    }
  }
  return true;
}

size_t StorySaveManager::copyHistoryBytes(IFileReader &oldReader,
                                           size_t historyOffset,
                                           IFileWriter &writer) {
  size_t bytesCopied = 0;
  auto countedWrite = [&](const void *data, size_t len) -> bool {
    if (!writer.write(data, len)) {
      return false;
    }
    bytesCopied += len;
    return true;
  };
  if (!oldReader.seek(historyOffset)) {
    return 0;
  }
  uint8_t szBuf[2];
  if (oldReader.read(szBuf, 2) != 2) {
    return 0;
  }
  if (!countedWrite(szBuf, 2)) {
    return 0;
  }
  uint16_t historySize = szBuf[0] | (static_cast<uint16_t>(szBuf[1]) << 8);
  uint8_t chunk[256];
  for (uint16_t i = 0; i < historySize; ++i) {
    uint8_t lenBuf[2];
    if (oldReader.read(lenBuf, 2) != 2) {
      return 0;
    }
    if (!countedWrite(lenBuf, 2)) {
      return 0;
    }
    uint16_t lineLen = lenBuf[0] | (static_cast<uint16_t>(lenBuf[1]) << 8);
    size_t remaining = lineLen;
    while (remaining > 0) {
      size_t toCopy = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
      if (oldReader.read(chunk, toCopy) != toCopy) {
        return 0;
      }
      if (!countedWrite(chunk, toCopy)) {
        return 0;
      }
      remaining -= toCopy;
    }
    uint8_t flags = 0;
    if (oldReader.read(&flags, 1) != 1) {
      return 0;
    }
    if (!countedWrite(&flags, 1)) {
      return 0;
    }
  }
  return bytesCopied;
}

bool StorySaveManager::loadCheckpointHistoryFromDisk(
    IStorage &storage, const std::string &savePath, size_t historyOffset,
    std::deque<WrappedLine> &out, InkStoryManager *storyMgr) {
  bool ok = false;
  storage.readStream(savePath.c_str(), [&](IFileReader &reader) -> bool {
    if (historyOffset >= reader.size()) {
      return false;
    }
    if (!reader.seek(historyOffset)) {
      return false;
    }
    if (!deserializeHistory(reader, out, storyMgr)) {
      return false;
    }
    ok = true;
    return true;
  });
  return ok;
}

bool StorySaveManager::loadSaveFile(IStorage &storage,
                                    InkStoryManager *storyMgr) {
  if (_saveFilePath.empty() || !storage.fileExists(_saveFilePath.c_str())) {
    // No main save (fresh story): the undo slot is still probed so a
    // surviving sidecar from the same story version stays usable.
    probeUndoFile(storage);
    return false;
  }

  _hasMainProgress = false;
  _mainSnapshot.clear();
  _mainHistory.clear();
  _checkpoints.clear();

  bool result = storage.readStream(
      _saveFilePath.c_str(), [&](IFileReader &reader) -> bool {
        size_t fileSize = reader.size();
        if (fileSize < 8)
          return false;

        auto readU8 = [&](uint8_t &val) -> bool {
          return reader.read(&val, 1) == 1;
        };
        auto readU16 = [&](uint16_t &val) -> bool {
          uint8_t buf[2];
          if (reader.read(buf, 2) != 2)
            return false;
          val = buf[0] | (static_cast<uint16_t>(buf[1]) << 8);
          return true;
        };
        auto readU32 = [&](uint32_t &val) -> bool {
          uint8_t buf[4];
          if (reader.read(buf, 4) != 4)
            return false;
          val = buf[0] | (static_cast<uint32_t>(buf[1]) << 8) |
                (static_cast<uint32_t>(buf[2]) << 16) |
                (static_cast<uint32_t>(buf[3]) << 24);
          return true;
        };

        uint32_t magic = 0;
        if (!readU32(magic))
          return false;

        uint32_t fileStoryHash = 0;
        if (!readU32(fileStoryHash))
          return false;

        if (_storyHash != 0 && fileStoryHash != _storyHash) {
          printf("[StorySaveManager] Story hash mismatch! Expected 0x%08X, got "
                 "0x%08X\n",
                 (unsigned)_storyHash, (unsigned)fileStoryHash);
          return false;
        }

        if (magic == MAGIC_ENK1) {
          uint32_t snapSize = 0;
          if (!readU32(snapSize))
            return false;
          if (snapSize > 8u * 1024u * 1024u) {
            return false;
          }
          if (snapSize > 0) {
            _mainSnapshot.resize(snapSize);
            if (reader.read(_mainSnapshot.data(), snapSize) != snapSize)
              return false;
            _hasMainProgress = true;
            deserializeHistory(reader, _mainHistory, storyMgr);
          }
          return _hasMainProgress;
        } else if (magic == MAGIC_ENK2) {
          // Section 1: Main progress
          uint8_t hasMain = 0;
          if (!readU8(hasMain))
            return false;
          _hasMainProgress = (hasMain != 0);

          if (_hasMainProgress) {
            uint32_t snapSize = 0;
            if (!readU32(snapSize))
              return false;
            // Cap absurd sizes (corrupt file) before allocating.
            if (snapSize > 8u * 1024u * 1024u) {
              return false;
            }
            _mainSnapshot.resize(snapSize);
            if (snapSize > 0) {
              if (reader.read(_mainSnapshot.data(), snapSize) != snapSize)
                return false;
            }
            if (!deserializeHistory(reader, _mainHistory, storyMgr))
              return false;
          }

          // Section 2: Unified checkpoints list
          if (reader.tell() + 2 <= fileSize) {
            uint16_t cpCount = 0;
            if (readU16(cpCount)) {
              _checkpoints.reserve(cpCount);
              for (uint16_t i = 0; i < cpCount; i++) {
                uint16_t titleLen = 0;
                if (!readU16(titleLen))
                  break;
                std::string title;
                if (titleLen > 0) {
                  // Cap absurd title lengths (corrupt file).
                  if (titleLen > 512) {
                    break;
                  }
                  title.resize(titleLen);
                  if (reader.read(&title[0], titleLen) != titleLen)
                    break;
                }

                uint32_t snapLen = 0;
                if (!readU32(snapLen))
                  break;
                if (snapLen > 8u * 1024u * 1024u) {
                  break;
                }
                size_t snapOffset = reader.tell();

                // Skip snapshot payload in stream! Avoids allocating snapshot in heap.
                if (!reader.seek(snapOffset + snapLen))
                  break;

                // Disk-backed histories: skip without allocating a deque.
                // History lives on SD (offset = snapOffset + snapLen) and is
                // loaded on demand in restoreCheckpoint. Saves ~30KB baseline
                // RAM on PSRAM-less devices.
                (void)storyMgr;
                if (!skipHistory(reader))
                  break;

                CheckpointEntry cp;
                cp.title = std::move(title);
                cp.fileOffset = snapOffset;
                cp.snapshotLen = snapLen;
                // cp.history stays empty (disk-backed).
                _checkpoints.push_back(std::move(cp));
              }
            }
          }
          return true;
        }

        return false;
      });
  probeUndoFile(storage);
  return result;
}

bool StorySaveManager::writeSaveData(IFileWriter &writer,
                                     IFileReader *oldReader,
                                     std::vector<size_t> *outNewOffsets) {
  size_t currentOffset = 0;

  auto writeData = [&](const void *data, size_t size) -> bool {
    if (!writer.write(data, size))
      return false;
    currentOffset += size;
    return true;
  };

  auto writeU8 = [&](uint8_t val) -> bool { return writeData(&val, 1); };
  auto writeU16 = [&](uint16_t val) -> bool {
    uint8_t buf[2] = {static_cast<uint8_t>(val & 0xFF),
                      static_cast<uint8_t>((val >> 8) & 0xFF)};
    return writeData(buf, 2);
  };
  auto writeU32 = [&](uint32_t val) -> bool {
    uint8_t buf[4] = {static_cast<uint8_t>(val & 0xFF),
                      static_cast<uint8_t>((val >> 8) & 0xFF),
                      static_cast<uint8_t>((val >> 16) & 0xFF),
                      static_cast<uint8_t>((val >> 24) & 0xFF)};
    return writeData(buf, 4);
  };

  // Header: magic (4), storyHash (4)
  if (!writeU32(MAGIC_ENK2))
    return false;
  if (!writeU32(_storyHash))
    return false;

  // Section 1: Main progress
  if (!writeU8(_hasMainProgress ? 1 : 0))
    return false;
  if (_hasMainProgress) {
    uint32_t snapLen = static_cast<uint32_t>(
        _mainStreamFn ? _borrowedMainSnapLen :
        (_borrowedMainSnapshot != nullptr ? _borrowedMainSnapLen : _mainSnapshot.size()));
    if (!writeU32(snapLen))
      return false;
    if (snapLen > 0) {
      if (_mainStreamFn) {
        size_t written = _mainStreamFn(writer);
        if (written != snapLen)
          return false;
        currentOffset += written;
      } else {
        const uint8_t *dataPtr =
            _borrowedMainSnapshot != nullptr ? _borrowedMainSnapshot : _mainSnapshot.data();
        if (!writeData(dataPtr, snapLen))
          return false;
      }
    }
    
    if (!_mainHistorySerialized.empty()) {
      if (!writeData(_mainHistorySerialized.data(), _mainHistorySerialized.size()))
        return false;
    } else {
      uint16_t historySize = static_cast<uint16_t>(_mainHistory.size());
      if (!writeU16(historySize))
        return false;
      for (const auto &line : _mainHistory) {
        std::string text = line.block.getText();
        if (line.isImage) {
          text = "\x1B[IMG:" + line.imagePath + "]";
        }
        uint16_t len = static_cast<uint16_t>(text.length());
        if (!writeU16(len))
          return false;
        if (len > 0) {
          if (!writeData(text.data(), len))
            return false;
        }
        uint8_t flags = (line.isOld ? 1 : 0) | (line.endOfParagraph ? 2 : 0);
        if (!writeU8(flags))
          return false;
      }
    }
  }

  // Section 2: Unified Checkpoints List
  uint16_t cpCount = static_cast<uint16_t>(_checkpoints.size());
  if (!writeU16(cpCount))
    return false;

  if (outNewOffsets) {
    outNewOffsets->clear();
    outNewOffsets->reserve(_checkpoints.size());
  }

  for (auto &cp : _checkpoints) {
    uint16_t titleLen = static_cast<uint16_t>(cp.title.length());
    if (!writeU16(titleLen))
      return false;
    if (titleLen > 0) {
      if (!writeData(cp.title.data(), titleLen))
        return false;
    }

    uint32_t snapLen = static_cast<uint32_t>(
        cp.streamFn ? cp.snapshotLen :
        (cp.borrowedSnapshot != nullptr ? cp.snapshotLen :
        (!cp.snapshotData.empty() ? cp.snapshotData.size() : cp.snapshotLen)));
    if (!writeU32(snapLen))
      return false;

    size_t newOffset = currentOffset;

    if (cp.streamFn) {
      size_t written = cp.streamFn(writer);
      if (written != snapLen)
        return false;
      currentOffset += written;
    } else if (cp.borrowedSnapshot != nullptr) {
      if (!writeData(cp.borrowedSnapshot, snapLen))
        return false;
    } else if (!cp.snapshotData.empty()) {
      if (!writeData(cp.snapshotData.data(), snapLen))
        return false;
    } else if (oldReader && cp.fileOffset > 0 && snapLen > 0) {
      if (!oldReader->seek(cp.fileOffset))
        return false;
      uint8_t chunkBuf[512];
      size_t remaining = snapLen;
      while (remaining > 0) {
        size_t toRead = std::min(remaining, sizeof(chunkBuf));
        if (oldReader->read(chunkBuf, toRead) != toRead)
          return false;
        if (!writeData(chunkBuf, toRead))
          return false;
        remaining -= toRead;
      }
    } else if (snapLen > 0) {
      // No payload source for a non-empty snapshot — broken entry from a
      // failed previous save. Fail rather than writing corrupt offsets
      // (header would claim snapLen bytes but 0 would follow).
      printf("[StorySaveManager] Missing snapshot payload for checkpoint "
             "'%s' (%u bytes claimed)\n",
             cp.title.c_str(), (unsigned)snapLen);
      return false;
    }

    // Capture old offset before assigning the new one (needed for
    // disk-backed history copy below when outNewOffsets is null).
    size_t oldFileOffset = cp.fileOffset;
    if (outNewOffsets) {
      outNewOffsets->push_back(newOffset);
    } else {
      cp.fileOffset = newOffset;
    }
    cp.snapshotLen = snapLen;

    // History source (in priority order):
    // 1. borrowedHistory (new zero-copy checkpoint, no RAM duplication)
    // 2. cp.history (RAM: tests, legacy, or unsaved entry)
    // 3. disk copy from old file (persisted checkpoint, truncation rewrite)
    // 4. empty (named milestone with no history)
    const std::deque<WrappedLine> *histSrc = nullptr;
    if (cp.borrowedHistory != nullptr) {
      histSrc = cp.borrowedHistory;
    } else if (!cp.history.empty()) {
      histSrc = &cp.history;
    }
    if (histSrc != nullptr) {
      uint16_t cpHistSize = static_cast<uint16_t>(histSrc->size());
      if (!writeU16(cpHistSize))
        return false;
      for (const auto &line : *histSrc) {
        std::string text = line.block.getText();
        if (line.isImage) {
          text = "\x1B[IMG:" + line.imagePath + "]";
        }
        uint16_t len = static_cast<uint16_t>(text.length());
        if (!writeU16(len))
          return false;
        if (len > 0) {
          if (!writeData(text.data(), len))
            return false;
        }
        uint8_t flags = (line.isOld ? 1 : 0) | (line.endOfParagraph ? 2 : 0);
        if (!writeU8(flags))
          return false;
      }
    } else if (oldReader && oldFileOffset > 0) {
      // Disk-backed: historyOffset in the OLD file.
      size_t oldHistOffset = oldFileOffset + snapLen;
      size_t copied = copyHistoryBytes(*oldReader, oldHistOffset, writer);
      if (copied == 0) {
        return false;
      }
      currentOffset += copied;
    } else {
      if (!writeU16(0))
        return false;
    }
  }

  return true;
}

bool StorySaveManager::writeSaveFile(IStorage &storage) {
  if (_saveFilePath.empty())
    return false;

  bool hasDiskCheckpoints = false;
  for (const auto &cp : _checkpoints) {
    // Disk-backed = no new snapshot payload (stream/borrowed/data all empty)
    // but a valid on-disk location. Borrowed histories alone don't force the
    // slow path (they are RAM, written directly).
    bool hasNewSnapshot =
        (cp.streamFn != nullptr) || (cp.borrowedSnapshot != nullptr) ||
        !cp.snapshotData.empty();
    if (!hasNewSnapshot && cp.fileOffset > 0 && cp.snapshotLen > 0) {
      hasDiskCheckpoints = true;
      break;
    }
  }

  std::vector<size_t> newOffsets;

  if (hasDiskCheckpoints && storage.fileExists(_saveFilePath.c_str())) {
    std::string tmpPath = _saveFilePath + ".tmp";

    bool ok = storage.readStream(
        _saveFilePath.c_str(), [&](IFileReader &oldReader) -> bool {
          return storage.writeStream(
              tmpPath.c_str(), [&](IFileWriter &writer) -> bool {
                return writeSaveData(writer, &oldReader, &newOffsets);
              });
        });

    if (!ok) {
      storage.deleteFile(tmpPath.c_str());
      for (auto &cp : _checkpoints) {
        cp.streamFn = nullptr;
        cp.borrowedHistory = nullptr;
      }
      // Prune broken entries (failed new saves with no payload and no file)
      // so a later write cannot emit corrupt offsets.
      for (auto it = _checkpoints.begin(); it != _checkpoints.end();) {
        bool hasPayload = (it->streamFn != nullptr) ||
                          (it->borrowedSnapshot != nullptr) ||
                          !it->snapshotData.empty();
        bool hasFile = (it->fileOffset > 0 && it->snapshotLen > 0);
        if (!hasPayload && !hasFile && it->snapshotLen > 0) {
          it = _checkpoints.erase(it);
        } else {
          ++it;
        }
      }
      _mainStreamFn = nullptr;
      return false;
    }

    storage.deleteFile(_saveFilePath.c_str());
    if (!storage.renameFile(tmpPath.c_str(), _saveFilePath.c_str())) {
      for (auto &cp : _checkpoints) {
        cp.streamFn = nullptr;
        cp.borrowedHistory = nullptr;
      }
      for (auto it = _checkpoints.begin(); it != _checkpoints.end();) {
        bool hasPayload = (it->streamFn != nullptr) ||
                          (it->borrowedSnapshot != nullptr) ||
                          !it->snapshotData.empty();
        bool hasFile = (it->fileOffset > 0 && it->snapshotLen > 0);
        if (!hasPayload && !hasFile && it->snapshotLen > 0) {
          it = _checkpoints.erase(it);
        } else {
          ++it;
        }
      }
      _mainStreamFn = nullptr;
      return false;
    }

    for (size_t i = 0; i < _checkpoints.size(); ++i) {
      if (i < newOffsets.size()) {
        _checkpoints[i].fileOffset = newOffsets[i];
      }
      _checkpoints[i].borrowedSnapshot = nullptr;
      _checkpoints[i].streamFn = nullptr;
      _checkpoints[i].snapshotData.clear();
      _checkpoints[i].snapshotData.shrink_to_fit();
      // Disk-backed from here on: drop RAM histories (frees ~30KB).
      // Restores load histories from SD on demand.
      _checkpoints[i].borrowedHistory = nullptr;
      _checkpoints[i].history.clear();
      _checkpoints[i].history.shrink_to_fit();
    }
    _borrowedMainSnapshot = nullptr;
    _borrowedMainSnapLen = 0;
    _mainStreamFn = nullptr;
    return true;
  }

  bool ok = storage.writeStream(
      _saveFilePath.c_str(),
      [&](IFileWriter &writer) -> bool { return writeSaveData(writer, nullptr, &newOffsets); });

  for (size_t i = 0; i < _checkpoints.size(); ++i) {
    if (ok && i < newOffsets.size()) {
      _checkpoints[i].fileOffset = newOffsets[i];
    }
    _checkpoints[i].borrowedSnapshot = nullptr;
    _checkpoints[i].streamFn = nullptr;
    // Always drop the transient borrow. Snapshot bytes + RAM histories are
    // dropped only on success (disk-backed); on failure they are kept for
    // retry.
    _checkpoints[i].borrowedHistory = nullptr;
    if (ok) {
      _checkpoints[i].snapshotData.clear();
      _checkpoints[i].snapshotData.shrink_to_fit();
      _checkpoints[i].history.clear();
      _checkpoints[i].history.shrink_to_fit();
    }
  }
  if (!ok) {
    // Prune broken entries (failed new saves with no payload and no file).
    for (auto it = _checkpoints.begin(); it != _checkpoints.end();) {
      bool hasPayload = (it->streamFn != nullptr) ||
                        (it->borrowedSnapshot != nullptr) ||
                        !it->snapshotData.empty();
      bool hasFile = (it->fileOffset > 0 && it->snapshotLen > 0);
      if (!hasPayload && !hasFile && it->snapshotLen > 0) {
        it = _checkpoints.erase(it);
      } else {
        ++it;
      }
    }
  }
  _borrowedMainSnapshot = nullptr;
  _borrowedMainSnapLen = 0;
  _mainStreamFn = nullptr;
  return ok;
}

void StorySaveManager::saveMainProgress(
    const uint8_t *snapData, size_t snapLen,
    const std::deque<WrappedLine> &history,
    bool borrowSnapshot) {
  if (snapData && snapLen > 0) {
    if (borrowSnapshot) {
      _borrowedMainSnapshot = snapData;
      _borrowedMainSnapLen = snapLen;
      _mainSnapshot.clear();
      _mainSnapshot.shrink_to_fit();
    } else {
      _borrowedMainSnapshot = nullptr;
      _borrowedMainSnapLen = 0;
      _mainSnapshot.assign(snapData, snapData + snapLen);
    }
    _mainStreamFn = nullptr;
    _mainHistory = history;
    _hasMainProgress = true;
  }
}

void StorySaveManager::saveMainProgressStreaming(
    size_t snapLen,
    const std::deque<WrappedLine> &history,
    SnapshotStreamFn streamFn) {
  if (snapLen > 0 && streamFn) {
    _hasMainProgress = true;
    _mainStreamFn = std::move(streamFn);
    _borrowedMainSnapshot = nullptr;
    _borrowedMainSnapLen = snapLen;
    _mainSnapshot.clear();
    _mainSnapshot.shrink_to_fit();
    _mainHistory = history;
  }
}

void StorySaveManager::saveMainProgressStreaming(
    size_t snapLen, std::deque<WrappedLine> &&history,
    SnapshotStreamFn streamFn) {
  _hasMainProgress = true;
  _mainSnapshot.clear();
  _borrowedMainSnapshot = nullptr;
  _borrowedMainSnapLen = snapLen;
  _mainHistorySerialized.clear();
  _mainHistory = std::move(history);
  _mainStreamFn = streamFn;
}

void StorySaveManager::saveMainProgressStreaming(
    size_t snapLen, std::vector<uint8_t> &&serializedHistory,
    SnapshotStreamFn streamFn) {
  _hasMainProgress = true;
  _mainSnapshot.clear();
  _borrowedMainSnapshot = nullptr;
  _borrowedMainSnapLen = snapLen;
  _mainHistory.clear();
  _mainHistorySerialized = std::move(serializedHistory);
  _mainStreamFn = streamFn;
}

void StorySaveManager::clearMainProgress() {
  _hasMainProgress = false;
  _borrowedMainSnapshot = nullptr;
  _borrowedMainSnapLen = 0;
  _mainStreamFn = nullptr;
  _mainSnapshot.clear();
  _mainSnapshot.shrink_to_fit();
  _mainHistory.clear();
  _mainHistory.shrink_to_fit();
  _mainHistorySerialized.clear();
  _mainHistorySerialized.shrink_to_fit();
}

bool StorySaveManager::restoreMainProgress(InkStoryManager &story,
                                           InkDisplayManager &display,
                                           IStorage *storage) {
  const uint8_t *dataPtr =
      _borrowedMainSnapshot != nullptr ? _borrowedMainSnapshot : _mainSnapshot.data();
  size_t dataLen =
      _borrowedMainSnapshot != nullptr ? _borrowedMainSnapLen : _mainSnapshot.size();

  if (!_hasMainProgress || dataLen == 0) {
    if (!_checkpoints.empty()) {
      return restoreCheckpoint(_checkpoints.size() - 1, story, display, storage);
    }
    return false;
  }

  display.clearHistory();
  display.clearChoices();
  display.setScrollY(0);
  display.clearFontCache();

  if (!story.loadSnapshot(dataPtr, dataLen)) {
    if (!story.runner()) {
      story.createFreshRunner();
    }
    return false;
  }

  for (const auto &l : _mainHistory) {
    display.addWrappedLine(l);
  }

  // Free restored snapshot payload from memory; inkcpp runner and display now own the state
  clearMainProgress();

  return true;
}

void StorySaveManager::saveCheckpoint(
    const std::string &title, const uint8_t *snapData, size_t snapLen,
    const std::deque<WrappedLine> &history,
    bool borrowSnapshot) {
  if (!snapData || snapLen == 0)
    return;

  // 1. Deduplicate: remove any existing entry matching this key
  for (auto it = _checkpoints.begin(); it != _checkpoints.end(); ++it) {
    if (it->title == title) {
      _checkpoints.erase(it);
      break;
    }
  }

  // 2. Append new checkpoint snapshot at the end (latest chronological tail)
  // Named checkpoints (chapters/milestones) do not need display history, saving significant RAM
  CheckpointEntry cp;
  cp.title = title;
  cp.snapshotLen = snapLen;
  if (borrowSnapshot) {
    cp.borrowedSnapshot = snapData;
  } else {
    cp.snapshotData.assign(snapData, snapData + snapLen);
  }
  if (title.empty()) {
    cp.history = history;
  }
  _checkpoints.push_back(std::move(cp));
}

void StorySaveManager::saveCheckpointStreaming(
    const std::string &title, size_t snapLen,
    const std::deque<WrappedLine> &history,
    SnapshotStreamFn streamFn) {
  if (snapLen == 0 || !streamFn)
    return;

  // 1. Deduplicate: remove any existing entry matching this key
  for (auto it = _checkpoints.begin(); it != _checkpoints.end(); ++it) {
    if (it->title == title) {
      _checkpoints.erase(it);
      break;
    }
  }

  // 2. Append new checkpoint snapshot at the end (latest chronological tail)
  CheckpointEntry cp;
  cp.title = title;
  cp.snapshotLen = snapLen;
  cp.streamFn = std::move(streamFn);
  if (title.empty()) {
    cp.history = history;
  }
  _checkpoints.push_back(std::move(cp));
}

void StorySaveManager::saveCheckpointBorrowed(
    const std::string &title, size_t snapLen,
    const std::deque<WrappedLine> &history, SnapshotStreamFn streamFn) {
  if (snapLen == 0 || !streamFn)
    return;
  for (auto it = _checkpoints.begin(); it != _checkpoints.end(); ++it) {
    if (it->title == title) {
      _checkpoints.erase(it);
      break;
    }
  }
  CheckpointEntry cp;
  cp.title = title;
  cp.snapshotLen = snapLen;
  cp.streamFn = std::move(streamFn);
  // Zero-copy: borrow display history (no deque duplication). Named
  // milestones keep no history at all.
  if (title.empty()) {
    cp.borrowedHistory = &history;
  }
  _checkpoints.push_back(std::move(cp));
}

bool StorySaveManager::hasUnnamedCheckpoint() const {
  return getUnnamedCheckpointIndex() >= 0;
}

int StorySaveManager::getUnnamedCheckpointIndex() const {
  for (int i = static_cast<int>(_checkpoints.size()) - 1; i >= 0; --i) {
    if (_checkpoints[i].title.empty()) {
      return i;
    }
  }
  return -1;
}

bool StorySaveManager::hasNamedCheckpoints() const {
  for (const auto &cp : _checkpoints) {
    if (!cp.title.empty())
      return true;
  }
  return false;
}

void StorySaveManager::getNamedCheckpoints(
    std::vector<std::pair<size_t, std::string>> &outNamed) const {
  outNamed.clear();
  for (size_t i = 0; i < _checkpoints.size(); ++i) {
    if (!_checkpoints[i].title.empty()) {
      outNamed.push_back({i, _checkpoints[i].title});
    }
  }
}

bool StorySaveManager::restoreCheckpoint(size_t index, InkStoryManager &story,
                                         InkDisplayManager &display,
                                         IStorage *storage) {
  if (index >= _checkpoints.size())
    return false;

  // Capture disk location + RAM history presence before reclaim (display
  // clear does not touch _checkpoints, but be explicit).
  size_t cpFileOffset = _checkpoints[index].fileOffset;
  size_t cpSnapLen = _checkpoints[index].snapshotLen;
  bool hasRamHistory = !_checkpoints[index].history.empty();
  const std::deque<WrappedLine> *borrowedHist =
      _checkpoints[index].borrowedHistory;
  bool hasInMemSnapshot = !_checkpoints[index].snapshotData.empty();
  const uint8_t *memSnapData =
      hasInMemSnapshot ? _checkpoints[index].snapshotData.data() : nullptr;
  size_t memSnapLen =
      hasInMemSnapshot ? _checkpoints[index].snapshotData.size() : 0;

  // 1. Reclaim heap memory from active display history, choices, mid-chapter progress,
  //    and the current runner/globals before attempting to allocate snapshot buffers.
  display.clearHistory();
  display.clearChoices();
  display.setScrollY(0);
  display.clearFontCache();
  clearMainProgress();
  story.resetRunner();

  if (hasInMemSnapshot) {
    if (!story.loadSnapshot(memSnapData, memSnapLen)) {
      if (!story.runner()) {
        story.createFreshRunner();
      }
      return false;
    }
  } else if (cpFileOffset > 0 && cpSnapLen > 0 && storage) {
    bool loaded = false;
    storage->readStream(
        _saveFilePath.c_str(), [&](IFileReader &reader) -> bool {
          if (!reader.seek(cpFileOffset))
            return false;
          if (cpSnapLen > 8u * 1024u * 1024u) {
            return false;
          }
          uint8_t *snapBuf = (uint8_t *)malloc(cpSnapLen);
          if (!snapBuf) {
            printf("[StorySaveManager] Failed to allocate %u bytes for checkpoint snapshot\n",
                   (unsigned)cpSnapLen);
            return false;
          }
          if (reader.read(snapBuf, cpSnapLen) != cpSnapLen) {
            free(snapBuf);
            return false;
          }
          bool ok = story.loadSnapshot(snapBuf, cpSnapLen);
          free(snapBuf);
          if (!ok) {
            if (!story.runner()) {
              story.createFreshRunner();
            }
            return false;
          }
          loaded = true;
          return true;
        });
    if (!loaded) {
      if (!story.runner()) {
        story.createFreshRunner();
      }
      return false;
    }
  } else {
    if (!story.runner()) {
      story.createFreshRunner();
    }
    return false;
  }

  // 2. History: RAM first (tests / unsaved), else borrowed (mid-write,
  // defensive), else disk-backed on demand (normal device path).
  if (hasRamHistory) {
    for (const auto &l : _checkpoints[index].history) {
      display.addWrappedLine(l);
    }
  } else if (borrowedHist != nullptr) {
    for (const auto &l : *borrowedHist) {
      display.addWrappedLine(l);
    }
  } else if (cpFileOffset > 0 && cpSnapLen > 0 && storage) {
    std::deque<WrappedLine> diskHist;
    size_t histOffset = cpFileOffset + cpSnapLen;
    if (loadCheckpointHistoryFromDisk(*storage, _saveFilePath, histOffset,
                                      diskHist, &story)) {
      for (const auto &l : diskHist) {
        display.addWrappedLine(l);
      }
    } else {
      // Disk history unreadable (truncated file): continue with empty
      // history rather than failing the rewind. Snapshot already loaded.
      printf("[StorySaveManager] Checkpoint history load failed, "
             "continuing with empty history\n");
    }
  }
  // else: named milestone with no history — display stays empty, story
  // continues from the snapshot. Existing behavior.

  // Pruning: delete all checkpoint entries chronologically after index
  if (index + 1 < _checkpoints.size()) {
    _checkpoints.erase(_checkpoints.begin() + index + 1, _checkpoints.end());
    _checkpoints.shrink_to_fit();
  }

  return true;
}

void StorySaveManager::clearAll(IStorage &storage) {
  _hasMainProgress = false;
  _mainSnapshot.clear();
  _mainSnapshot.shrink_to_fit();
  _mainHistory.clear();
  _mainHistory.shrink_to_fit();
  _mainHistorySerialized.clear();
  _mainHistorySerialized.shrink_to_fit();
  _borrowedMainSnapshot = nullptr;
  _borrowedMainSnapLen = 0;
  _mainStreamFn = nullptr;
  _checkpoints.clear();
  _checkpoints.shrink_to_fit();
  if (!_saveFilePath.empty() && storage.fileExists(_saveFilePath.c_str())) {
    storage.deleteFile(_saveFilePath.c_str());
  }
  clearUndo(&storage);
}

void StorySaveManager::saveUndoStreaming(
    size_t snapLen, const std::deque<WrappedLine> &history,
    SnapshotStreamFn streamFn) {
  if (snapLen == 0 || snapLen > kMaxUndoSnapLen || !streamFn ||
      _undoFilePath.empty()) {
    return;
  }
  _undoSnapLen = snapLen;
  // Checkpoint count at undo time: entries appended later belong to the
  // abandoned future and are truncated on restore.
  _undoCpCount = _checkpoints.size() > 0xFFFF ? 0xFFFF : _checkpoints.size();
  _undoStreamFn = std::move(streamFn);
  _undoHistoryBorrowed = nullptr;
  _undoHistory = history;
}

void StorySaveManager::saveUndoBorrowed(
    size_t snapLen, const std::deque<WrappedLine> &history,
    SnapshotStreamFn streamFn) {
  if (snapLen == 0 || snapLen > kMaxUndoSnapLen || !streamFn ||
      _undoFilePath.empty()) {
    return;
  }
  _undoSnapLen = snapLen;
  _undoCpCount = _checkpoints.size() > 0xFFFF ? 0xFFFF : _checkpoints.size();
  _undoStreamFn = std::move(streamFn);
  // Zero-copy: no deque duplication (~30KB saved during every choice).
  _undoHistory.clear();
  _undoHistoryBorrowed = &history;
}

bool StorySaveManager::writeUndoFile(IStorage &storage) {
  bool ok = false;
  if (!_undoFilePath.empty() && _undoSnapLen > 0 &&
      _undoSnapLen <= kMaxUndoSnapLen && _undoStreamFn) {
    std::string tmpPath = _undoFilePath + ".tmp";
    size_t snapLen = _undoSnapLen;
    uint16_t cpCount = static_cast<uint16_t>(_undoCpCount);
    uint32_t storyHash = _storyHash;
    // Resolve history source: borrowed (device, zero-copy) else RAM copy
    // (tests / legacy). Borrowed must stay alive through this call.
    const std::deque<WrappedLine> *histSrc =
        _undoHistoryBorrowed != nullptr ? _undoHistoryBorrowed : &_undoHistory;
    ok = storage.writeStream(
        tmpPath.c_str(), [&](IFileWriter &writer) -> bool {
          uint8_t hdr[16];
          hdr[0] = static_cast<uint8_t>(MAGIC_ENKU & 0xFF);
          hdr[1] = static_cast<uint8_t>((MAGIC_ENKU >> 8) & 0xFF);
          hdr[2] = static_cast<uint8_t>((MAGIC_ENKU >> 16) & 0xFF);
          hdr[3] = static_cast<uint8_t>((MAGIC_ENKU >> 24) & 0xFF);
          hdr[4] = static_cast<uint8_t>(storyHash & 0xFF);
          hdr[5] = static_cast<uint8_t>((storyHash >> 8) & 0xFF);
          hdr[6] = static_cast<uint8_t>((storyHash >> 16) & 0xFF);
          hdr[7] = static_cast<uint8_t>((storyHash >> 24) & 0xFF);
          hdr[8] = static_cast<uint8_t>(snapLen & 0xFF);
          hdr[9] = static_cast<uint8_t>((snapLen >> 8) & 0xFF);
          hdr[10] = static_cast<uint8_t>((snapLen >> 16) & 0xFF);
          hdr[11] = static_cast<uint8_t>((snapLen >> 24) & 0xFF);
          hdr[12] = static_cast<uint8_t>(cpCount & 0xFF);
          hdr[13] = static_cast<uint8_t>((cpCount >> 8) & 0xFF);
          hdr[14] = 0; // reserved
          hdr[15] = 0; // reserved
          if (!writer.write(hdr, sizeof(hdr)))
            return false;
          if (_undoStreamFn(writer) != snapLen)
            return false;
          return serializeHistory(*histSrc, writer);
        });
    if (ok) {
      storage.deleteFile(_undoFilePath.c_str());
      ok = storage.renameFile(tmpPath.c_str(), _undoFilePath.c_str());
      if (!ok) {
        storage.deleteFile(tmpPath.c_str());
      }
    } else {
      storage.deleteFile(tmpPath.c_str());
    }
  }
  // Single-shot pending state is always cleared. On failure the previous
  // sidecar (if any) is left untouched, so _hasUndo keeps its old value.
  _undoStreamFn = nullptr;
  _undoSnapLen = 0;
  _undoCpCount = 0;
  _undoHistory.clear();
  _undoHistoryBorrowed = nullptr;
  if (ok) {
    _hasUndo = true;
  }
  return ok;
}

void StorySaveManager::probeUndoFile(IStorage &storage) {
  _hasUndo = false;
  if (_undoFilePath.empty()) {
    return;
  }
  // Drop an orphaned tmp file from an interrupted write (power loss
  // mid-choice). The previous sidecar is only replaced by rename after a
  // complete write, so at most one stale tmp can exist.
  std::string tmpPath = _undoFilePath + ".tmp";
  if (storage.fileExists(tmpPath.c_str())) {
    storage.deleteFile(tmpPath.c_str());
  }
  if (!storage.fileExists(_undoFilePath.c_str())) {
    return;
  }
  bool valid = false;
  storage.readStream(_undoFilePath.c_str(), [&](IFileReader &reader) -> bool {
    uint8_t hdr[12];
    if (reader.read(hdr, sizeof(hdr)) != sizeof(hdr)) {
      return false;
    }
    uint32_t magic = static_cast<uint32_t>(hdr[0]) |
                     (static_cast<uint32_t>(hdr[1]) << 8) |
                     (static_cast<uint32_t>(hdr[2]) << 16) |
                     (static_cast<uint32_t>(hdr[3]) << 24);
    uint32_t hash = static_cast<uint32_t>(hdr[4]) |
                    (static_cast<uint32_t>(hdr[5]) << 8) |
                    (static_cast<uint32_t>(hdr[6]) << 16) |
                    (static_cast<uint32_t>(hdr[7]) << 24);
    uint32_t snapLen = static_cast<uint32_t>(hdr[8]) |
                       (static_cast<uint32_t>(hdr[9]) << 8) |
                       (static_cast<uint32_t>(hdr[10]) << 16) |
                       (static_cast<uint32_t>(hdr[11]) << 24);
    if (magic != MAGIC_ENKU) {
      return false;
    }
    if (_storyHash != 0 && hash != _storyHash) {
      return false;
    }
    if (snapLen == 0 || snapLen > kMaxUndoSnapLen) {
      return false;
    }
    valid = true;
    return true;
  });
  if (valid) {
    _hasUndo = true;
  } else {
    // Stale (story updated) or corrupt: drop so it can never shadow a fresh
    // slot or confuse the menu.
    storage.deleteFile(_undoFilePath.c_str());
  }
}

bool StorySaveManager::restoreUndo(InkStoryManager &story,
                                   InkDisplayManager &display,
                                   IStorage *storage) {
  if (!_hasUndo || _undoFilePath.empty() || storage == nullptr) {
    return false;
  }
  if (!storage->fileExists(_undoFilePath.c_str())) {
    _hasUndo = false;
    return false;
  }

  uint16_t cpCount = 0;
  std::deque<WrappedLine> history;
  bool headerValid = false;
  bool loaded = false;
  uint32_t wantHash = _storyHash;
  bool ok = storage->readStream(
      _undoFilePath.c_str(), [&](IFileReader &reader) -> bool {
        uint8_t hdr[16];
        if (reader.read(hdr, sizeof(hdr)) != sizeof(hdr)) {
          return false;
        }
        uint32_t magic = static_cast<uint32_t>(hdr[0]) |
                         (static_cast<uint32_t>(hdr[1]) << 8) |
                         (static_cast<uint32_t>(hdr[2]) << 16) |
                         (static_cast<uint32_t>(hdr[3]) << 24);
        uint32_t hash = static_cast<uint32_t>(hdr[4]) |
                        (static_cast<uint32_t>(hdr[5]) << 8) |
                        (static_cast<uint32_t>(hdr[6]) << 16) |
                        (static_cast<uint32_t>(hdr[7]) << 24);
        uint32_t snapLen = static_cast<uint32_t>(hdr[8]) |
                           (static_cast<uint32_t>(hdr[9]) << 8) |
                           (static_cast<uint32_t>(hdr[10]) << 16) |
                           (static_cast<uint32_t>(hdr[11]) << 24);
        cpCount = static_cast<uint16_t>(hdr[12] | (hdr[13] << 8));
        if (magic != MAGIC_ENKU) {
          return false;
        }
        if (wantHash != 0 && hash != wantHash) {
          return false;
        }
        if (snapLen == 0 || snapLen > kMaxUndoSnapLen) {
          return false;
        }
        headerValid = true;

        // Header is good: reclaim heap (mirrors restoreCheckpoint) before
        // the transient snapshot allocation.
        display.clearHistory();
        display.clearChoices();
        display.setScrollY(0);
        display.clearFontCache();
        clearMainProgress();
        story.resetRunner();

        uint8_t *snapBuf = static_cast<uint8_t *>(malloc(snapLen));
        if (!snapBuf) {
          printf("[StorySaveManager] Failed to allocate %u bytes for undo "
                 "snapshot\n",
                 (unsigned)snapLen);
          return false;
        }
        bool readOk = (reader.read(snapBuf, snapLen) == snapLen);
        bool loadOk = false;
        if (readOk) {
          loadOk = story.loadSnapshot(snapBuf, snapLen);
        }
        free(snapBuf);
        if (!loadOk) {
          if (!story.runner()) {
            story.createFreshRunner();
          }
          return false;
        }
        if (!deserializeHistory(reader, history, &story)) {
          return false;
        }
        loaded = true;
        return true;
      });
  if (!ok || !loaded) {
    if (!headerValid) {
      // Corrupt or stale sidecar: consume so the menu stops offering it.
      clearUndo(storage);
    } else if (!story.runner()) {
      story.createFreshRunner();
    }
    return false;
  }

  for (const auto &l : history) {
    display.addWrappedLine(l);
  }

  // Prune checkpoints saved after the undo point (abandoned future).
  if (cpCount < _checkpoints.size()) {
    _checkpoints.erase(_checkpoints.begin() + cpCount, _checkpoints.end());
    _checkpoints.shrink_to_fit();
  }

  // Single-level slot: consumed on use. The next choice re-arms it.
  clearUndo(storage);
  return true;
}

void StorySaveManager::clearUndo(IStorage *storage) {
  _hasUndo = false;
  _undoSnapLen = 0;
  _undoCpCount = 0;
  _undoStreamFn = nullptr;
  _undoHistory.clear();
  _undoHistory.shrink_to_fit();
  _undoHistoryBorrowed = nullptr;
  if (storage != nullptr && !_undoFilePath.empty() &&
      storage->fileExists(_undoFilePath.c_str())) {
    storage->deleteFile(_undoFilePath.c_str());
  }
}
