#include "zalie/data-file.hpp"

#include <aeronet/file.hpp>
#include <algorithm>
#include <amc/vector.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>

#include "json-scan.hpp"
#include "zalie/file-io.hpp"
#include "zalie/load-settings.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/string-utils.hpp"

namespace zalie {

namespace {

constexpr std::size_t kNpos = std::string_view::npos;
constexpr std::size_t kScanBlockBytes = 1UL << 20;
constexpr std::size_t kMinWindowBytes = 4UL << 10;
constexpr std::size_t kMaxWindowBytes = 1UL << 20;
// A longer row is refused rather than growing the buffers without limit.
constexpr std::size_t kMaxRowBytes = 256UL << 20;
constexpr std::size_t kMaxCachedIndexes = 16;
constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
constexpr std::string_view kTextColumn = "value";

struct RowEnd {
  std::size_t contentEnd;
  std::size_t next;
  bool complete;
};

// Finds the end of the row starting at 'pos', quotes considered for CSV: a quoted field can hold line breaks.
[[nodiscard]] RowEnd FindRowEnd(DataFileFormat format, std::string_view data, std::size_t pos) noexcept {
  std::size_t lineEnd = kNpos;
  if (format == DataFileFormat::csv) {
    bool inQuotes = false;
    for (std::size_t idx = pos; idx < data.size(); ++idx) {
      if (data[idx] == '"') {
        inQuotes = !inQuotes;
      } else if (data[idx] == '\n' && !inQuotes) {
        lineEnd = idx;
        break;
      }
    }
  } else {
    lineEnd = data.find('\n', pos);
  }
  if (lineEnd == kNpos) {
    const std::size_t end = data.size() > pos && data.back() == '\r' ? data.size() - 1 : data.size();
    return {.contentEnd = end, .next = data.size(), .complete = false};
  }
  const std::size_t end = lineEnd > pos && data[lineEnd - 1] == '\r' ? lineEnd - 1 : lineEnd;
  return {.contentEnd = end, .next = lineEnd + 1, .complete = true};
}

[[nodiscard]] bool IsBlankRow(DataFileFormat format, std::string_view row) noexcept {
  return format == DataFileFormat::jsonLines ? Trim(row).empty() : row.empty();
}

// Splits a CSV row (RFC 4180). The fields are views of 'row' or, when they had to be decoded, of 'decoded'.
[[nodiscard]] bool ParseCsvRow(std::string_view row, RawChars32& decoded, amc::vector<std::string_view>& fields) {
  fields.clear();
  decoded.clear();
  decoded.ensureAvailableCapacity(row.size());  // decoding only shrinks: the views stay valid
  std::size_t pos = 0;
  while (true) {
    if (pos < row.size() && row[pos] == '"') {
      const std::size_t begin = pos + 1;
      const auto decodedBegin = decoded.size();
      std::size_t closing = kNpos;
      bool escaped = false;
      for (std::size_t idx = begin; idx < row.size(); ++idx) {
        if (row[idx] != '"') {
          if (escaped) {
            decoded.unchecked_push_back(row[idx]);
          }
          continue;
        }
        if (idx + 1 < row.size() && row[idx + 1] == '"') {
          if (!escaped) {
            decoded.unchecked_append(row.substr(begin, idx - begin));
            escaped = true;
          }
          decoded.unchecked_push_back('"');
          ++idx;
        } else {
          closing = idx;
          break;
        }
      }
      if (closing == kNpos) {
        return false;
      }
      fields.push_back(escaped ? std::string_view(decoded).substr(decodedBegin) : row.substr(begin, closing - begin));
      pos = closing + 1;
      if (pos < row.size() && row[pos] != ',') {
        return false;
      }
    } else {
      const std::size_t end = std::min(row.find(',', pos), row.size());
      fields.push_back(row.substr(pos, end - pos));
      pos = end;
    }
    if (pos >= row.size()) {
      return true;
    }
    ++pos;  // the comma
  }
}

// The text of a JSON value: strings are decoded, null is empty, anything else (number, object...) is kept as written.
[[nodiscard]] bool DecodeJsonValue(std::string_view value, RawChars32& decoded, std::string_view& out) {
  if (value.starts_with('"')) {
    const std::string_view inner = value.substr(1, value.size() - 2);
    if (!inner.contains('\\')) {
      out = inner;
      return true;
    }
    const auto begin = decoded.size();
    if (!AppendUnescaped(inner, decoded)) {
      return false;
    }
    out = std::string_view(decoded).substr(begin);
    return true;
  }
  out = value == "null" ? std::string_view{} : value;
  return true;
}

// The key of a member, decoded in 'scratch' if needed.
[[nodiscard]] bool DecodeJsonKey(std::string_view rawKey, RawChars32& scratch, std::string_view& out) {
  if (!rawKey.contains('\\')) {
    out = rawKey;
    return true;
  }
  scratch.clear();
  if (!AppendUnescaped(rawKey, scratch)) {
    return false;
  }
  out = scratch;
  return true;
}

// Fills 'values' with the value of each column. Fails on a malformed line, or a column missing from it.
[[nodiscard]] bool ParseJsonRow(std::string_view row, std::span<const RawChars32> columns, RawChars32& decoded,
                                RawChars32& keyScratch, amc::vector<std::string_view>& values) {
  row = Trim(row);
  if (row.size() < 2 || row.front() != '{' || row.back() != '}') {
    return false;
  }
  values.assign(static_cast<std::uint32_t>(columns.size()), std::string_view{});
  decoded.clear();
  decoded.ensureAvailableCapacity(row.size());  // decoding only shrinks: the views stay valid
  std::size_t nbFound = 0;
  const bool wellFormed = ScanJsonObject(row, [&](std::string_view rawKey, std::string_view value) {
    std::string_view key;
    if (!DecodeJsonKey(rawKey, keyScratch, key)) {
      return false;
    }
    const auto column = std::ranges::find(columns, key, [](const RawChars32& name) { return View(name); });
    if (column == columns.end()) {
      return true;
    }
    ++nbFound;
    return DecodeJsonValue(value, decoded, values[static_cast<std::uint32_t>(column - columns.begin())]);
  });
  return wellFormed && nbFound >= columns.size();
}

// Fills 'values' with the value of each column of a data row. Fails on a malformed row.
[[nodiscard]] bool ParseRow(DataFileFormat format, std::string_view row, std::span<const RawChars32> columns,
                            RawChars32& decoded, RawChars32& keyScratch, amc::vector<std::string_view>& values) {
  switch (format) {
    case DataFileFormat::text:
      values.assign(1U, row);
      return true;
    case DataFileFormat::csv:
      return ParseCsvRow(row, decoded, values) && values.size() == columns.size();
    case DataFileFormat::jsonLines:
      return ParseJsonRow(row, columns, decoded, keyScratch, values);
  }
  return false;
}

[[nodiscard]] aeronet::File OpenFile(std::string_view path) noexcept {
  try {
    return aeronet::File(path);
  } catch (const std::exception&) {
    return {};
  }
}

}  // namespace

std::filesystem::path ResolveDataFilePath(std::string_view path, const std::filesystem::path& baseDirectory) {
  std::filesystem::path ret = PathFromUtf8(path);
  return ret.is_absolute() ? ret : baseDirectory / ret;
}

DataFileIndex::DataFileIndex(const std::filesystem::path& path, DataFileFormat format, std::uint32_t maxOffsets)
    : _path(PathToUtf8(path)), _maxOffsets(std::max(maxOffsets, 2U)), _format(format) {
  const aeronet::File file = OpenFile(_path);
  if (!file) {
    _error = Concat({"Cannot open '", _path, "'"});
    return;
  }
  _fileSize = file.size();
  _lastModified = file.lastModified();
  if (format == DataFileFormat::text) {
    _columns.emplace_back(kTextColumn);
    _headerDone = true;
  }
  try {
    scan(file);
  } catch (const std::exception& ex) {
    _error = Concat({"Cannot read '", _path, "': ", ex.what()});
  }
  if (_error.empty() && _nbRows == 0) {
    _error = Concat({"No value in '", _path, "'"});
  }
  amc::vector<std::string_view>().swap(_fields);
  _decoded = RawChars32{};
  _keyScratch = RawChars32{};
}

void DataFileIndex::scan(const aeronet::File& file) {
  RawChars buffer(kScanBlockBytes);
  std::uint64_t bufferOffset = 0;  // offset in the file of the first byte of the buffer
  std::size_t used = 0;
  bool isStart = true;
  while (_error.empty()) {
    const std::size_t nbRead = file.readAt(
        std::as_writable_bytes(std::span<char>(buffer.data() + used, buffer.capacity() - used)), bufferOffset + used);
    if (nbRead == aeronet::File::kError) {
      _error = Concat({"Cannot read '", _path, "'"});
      return;
    }
    const bool atEnd = nbRead == 0;
    used += nbRead;
    const std::string_view data(buffer.data(), used);
    std::size_t pos = 0;
    if (isStart && (used >= kByteOrderMark.size() || atEnd)) {
      pos = data.starts_with(kByteOrderMark) ? kByteOrderMark.size() : 0;
      isStart = false;
    }
    while (pos < used && !isStart && _error.empty()) {
      const RowEnd end = FindRowEnd(_format, data, pos);
      if (!end.complete && !atEnd) {
        break;
      }
      handleRow(data.substr(pos, end.contentEnd - pos), bufferOffset + pos);
      pos = end.next;
    }
    if (atEnd) {
      return;
    }
    std::memmove(buffer.data(), buffer.data() + pos, used - pos);
    bufferOffset += pos;
    used -= pos;
    if (used == buffer.capacity()) {  // one row bigger than the buffer
      if (used >= kMaxRowBytes) {
        _error = Concat({"A line is too long in '", _path, "'"});
        return;
      }
      RawChars larger(buffer.capacity() * 2);
      std::memcpy(larger.data(), buffer.data(), used);
      buffer.swap(larger);
    }
  }
}

void DataFileIndex::handleRow(std::string_view row, std::uint64_t offset) {
  const std::uint64_t line = _line;
  _line += _format == DataFileFormat::csv ? 1U + static_cast<std::uint64_t>(std::ranges::count(row, '\n')) : 1U;
  if (IsBlankRow(_format, row)) {
    return;
  }
  if (!_headerDone) {
    _headerDone = true;
    if (_format == DataFileFormat::csv) {
      if (!ParseCsvRow(row, _decoded, _fields)) {
        _error = Concat({"Malformed header in '", _path, "'"});
        return;
      }
      for (std::string_view field : _fields) {
        _columns.emplace_back(field);
      }
      return;  // the header is not a row of values
    }
    const std::string_view object = Trim(row);
    const bool wellFormed = object.starts_with('{') && ScanJsonObject(object, [this](std::string_view rawKey, auto) {
                              std::string_view key;
                              if (!DecodeJsonKey(rawKey, _keyScratch, key)) {
                                return false;
                              }
                              _columns.emplace_back(key);
                              return true;
                            });
    if (!wellFormed || _columns.empty()) {
      _error = Concat({"The first line of '", _path, "' is not a JSON object"});
      return;
    }
  }
  addRow(offset);
  const bool valid = ParseRow(_format, row, _columns, _decoded, _keyScratch, _fields);
  if (!valid && _firstMalformedLine == 0) {
    _firstMalformedLine = line;
  }
  if (_nbRows <= kNbPreviewRows) {
    for (std::uint32_t column = 0; column < _columns.size(); ++column) {
      _previewValues.emplace_back(valid && column < _fields.size() ? _fields[column] : std::string_view{});
    }
  }
}

void DataFileIndex::addRow(std::uint64_t offset) {
  const auto isIndexed = [this] { return (_nbRows & (std::uint64_t{_rowsPerOffset} - 1U)) == 0; };
  if (isIndexed()) {
    if (_offsets.size() == _maxOffsets) {  // keep one offset out of two
      const std::uint32_t kept = (_offsets.size() + 1U) / 2U;
      for (std::uint32_t idx = 1; idx < kept; ++idx) {
        _offsets[idx] = _offsets[2U * idx];
      }
      _offsets.resize(kept);
      _rowsPerOffset *= 2U;
    }
    if (isIndexed()) {
      _offsets.push_back(offset);
    }
  }
  ++_nbRows;
}

bool DataFileIndex::isCurrent() const {
  const aeronet::File file = OpenFile(_path);
  return file && file.size() == _fileSize && file.lastModified() == _lastModified;
}

std::uint32_t DataFileIndex::findColumn(std::string_view name) const noexcept {
  const std::span<const RawChars32> columns = _columns;
  const auto column = std::ranges::find(columns, name, [](const RawChars32& col) { return View(col); });
  return static_cast<std::uint32_t>(column - columns.begin());
}

DataFileReader::DataFileReader(const DataFileIndex& index) : _pIndex(&index) {
  _file = OpenFile(_pIndex->path());
  if (!_file) {
    _error = Concat({"Cannot open '", _pIndex->path(), "'"});
    return;
  }
  if (_file.size() != _pIndex->fileSize() || _file.lastModified() != _pIndex->lastModified()) {
    _error = Concat({"The file '", _pIndex->path(), "' changed since it was read"});
    return;
  }
  // Enough for the rows between two offsets of the index, so that a read rarely needs more than one system call.
  const std::size_t rowBytes = (_pIndex->fileSize() / std::max<std::uint64_t>(_pIndex->nbRows(), 1U)) + 1U;
  _window = RawChars(
      std::clamp(rowBytes * (std::size_t{_pIndex->rowsPerOffset()} + 1U) * 2U, kMinWindowBytes, kMaxWindowBytes));
}

bool DataFileReader::load(std::uint64_t offset) {
  const std::size_t nbRead =
      _file.readAt(std::as_writable_bytes(std::span<char>(_window.data(), _window.capacity())), offset);
  if (nbRead == aeronet::File::kError) {
    return false;
  }
  _window.setSize(static_cast<RawChars::size_type>(nbRead));
  _windowOffset = offset;
  _windowAtEnd = nbRead < _window.capacity();
  return true;
}

bool DataFileReader::findRow(std::uint64_t offset, std::string_view& row, std::uint64_t& next) {
  const DataFileFormat format = _pIndex->format();
  while (true) {
    const bool inWindow = offset >= _windowOffset && offset < _windowOffset + _window.size();
    if (!inWindow && !load(offset)) {
      return false;
    }
    if (_window.empty()) {
      return false;
    }
    const std::string_view data(_window);
    const auto pos = static_cast<std::size_t>(offset - _windowOffset);
    if (pos >= data.size()) {
      return false;
    }
    const RowEnd end = FindRowEnd(format, data, pos);
    if (!end.complete && !_windowAtEnd) {
      if (pos != 0) {
        if (!load(offset)) {  // the row is cut by the end of the window
          return false;
        }
      } else {
        if (_window.capacity() >= kMaxRowBytes) {
          return false;
        }
        _window = RawChars(_window.capacity() * 2U);  // a row bigger than the window
        if (!load(offset)) {
          return false;
        }
      }
      continue;
    }
    next = _windowOffset + end.next;
    row = data.substr(pos, end.contentEnd - pos);
    if (!IsBlankRow(format, row)) {
      return true;
    }
    offset = next;
  }
}

bool DataFileReader::read(std::uint64_t row) {
  const DataFileIndex& index = *_pIndex;
  if (row >= index.nbRows()) {
    return false;
  }
  std::uint64_t offset = index.offset(static_cast<std::uint32_t>(row / index.rowsPerOffset()));
  std::uint64_t remaining = row % index.rowsPerOffset();
  std::string_view text;
  std::uint64_t next = 0;
  while (true) {
    if (!findRow(offset, text, next)) {
      return false;
    }
    if (remaining == 0) {
      break;
    }
    --remaining;
    offset = next;
  }
  return ParseRow(index.format(), text, index.columns(), _decoded, _keyScratch, _values);
}

std::shared_ptr<const DataFileIndex> DataFileIndexCache::get(const std::filesystem::path& path, DataFileFormat format) {
  const RawChars32 utf8 = PathToUtf8(path);
  {
    std::scoped_lock lock(_mutex);
    const auto it = std::ranges::find_if(_indexes, [&utf8, format](const std::shared_ptr<const DataFileIndex>& pIndex) {
      return pIndex->path() == View(utf8) && pIndex->format() == format;
    });
    if (it != _indexes.end()) {
      if ((*it)->isCurrent()) {
        return *it;
      }
      _indexes.erase(it);
    }
  }
  std::shared_ptr<const DataFileIndex> pIndex = std::make_shared<const DataFileIndex>(path, format);  // can take long
  if (pIndex->valid()) {
    std::scoped_lock lock(_mutex);
    if (_indexes.size() == kMaxCachedIndexes) {
      _indexes.erase(_indexes.begin());
    }
    _indexes.push_back(pIndex);
  }
  return pIndex;
}

}  // namespace zalie
