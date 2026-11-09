#pragma once

#include <aeronet/file.hpp>
#include <aeronet/timedef.hpp>
#include <amc/vector.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>

#include "zalie/load-settings.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {

// What a scan of a data file found, without keeping the file in memory: its columns, its first rows and a sparse index
// of its rows, whatever its size. Immutable once built, so that threads can share it.
class DataFileIndex {
 public:
  static constexpr std::uint32_t kNbPreviewRows = 5;
  // The memory of the index stays under 8 MiB: past this number of offsets, it keeps one row out of two.
  static constexpr std::uint32_t kMaxOffsets = 1U << 20;

  // Scans the file: this can take long on a large file, call it off the UI thread. See valid() for the outcome.
  DataFileIndex(const std::filesystem::path& path, DataFileFormat format, std::uint32_t maxOffsets = kMaxOffsets);

  [[nodiscard]] bool valid() const noexcept { return _error.empty(); }

  // Why the file cannot be used.
  [[nodiscard]] std::string_view error() const noexcept { return _error; }

  // Whether the file still has the size and the date it had when scanned.
  [[nodiscard]] bool isCurrent() const;

  [[nodiscard]] std::string_view path() const noexcept { return _path; }
  [[nodiscard]] DataFileFormat format() const noexcept { return _format; }
  [[nodiscard]] std::uint64_t nbRows() const noexcept { return _nbRows; }
  [[nodiscard]] std::size_t fileSize() const noexcept { return _fileSize; }
  [[nodiscard]] aeronet::SysTimePoint lastModified() const noexcept { return _lastModified; }
  [[nodiscard]] std::span<const RawChars32> columns() const noexcept { return _columns; }

  // Position of the column with this name, or columns().size().
  [[nodiscard]] std::uint32_t findColumn(std::string_view name) const noexcept;

  // The values of the first rows, one row after the other (columns().size() values per row).
  [[nodiscard]] std::span<const RawChars32> previewValues() const noexcept { return _previewValues; }

  // Number of the first malformed line, starting at 1, or 0.
  [[nodiscard]] std::uint64_t firstMalformedLine() const noexcept { return _firstMalformedLine; }

  // Offset in the file of the row number 'rowsPerOffset() * position'.
  [[nodiscard]] std::uint64_t offset(std::uint32_t position) const noexcept { return _offsets[position]; }
  [[nodiscard]] std::uint32_t rowsPerOffset() const noexcept { return _rowsPerOffset; }

 private:
  void scan(const aeronet::File& file);
  void handleRow(std::string_view row, std::uint64_t offset);
  void addRow(std::uint64_t offset);

  RawChars32 _path;
  RawChars32 _error;
  RawChars32 _decoded;
  RawChars32 _keyScratch;
  amc::vector<RawChars32> _columns;
  amc::vector<RawChars32> _previewValues;
  amc::vector<std::string_view> _fields;
  amc::vector<std::uint64_t> _offsets;
  aeronet::SysTimePoint _lastModified;
  std::uint64_t _nbRows{0};
  std::uint64_t _line{1};
  std::uint64_t _firstMalformedLine{0};
  std::size_t _fileSize{0};
  std::uint32_t _rowsPerOffset{16};
  std::uint32_t _maxOffsets;
  DataFileFormat _format;
  bool _headerDone{false};
};

// Reads the rows of an indexed file: one per thread, each with its own descriptor. The rows are read through a window
// of the file, so that nearby rows do not cost a system call each.
class DataFileReader {
 public:
  // 'index' must outlive the reader.
  explicit DataFileReader(const DataFileIndex& index);

  // Why the file cannot be read: it is missing, or changed since it was indexed.
  [[nodiscard]] std::string_view error() const noexcept { return _error; }

  // Loads a row. Returns false if it does not exist any more (the file was truncated), or is malformed.
  [[nodiscard]] bool read(std::uint64_t row);

  // The value of a column of the last loaded row. Valid until the next read().
  [[nodiscard]] std::string_view value(std::uint32_t column) const noexcept { return _values[column]; }

 private:
  [[nodiscard]] bool load(std::uint64_t offset);
  [[nodiscard]] bool findRow(std::uint64_t offset, std::string_view& row, std::uint64_t& next);

  const DataFileIndex* _pIndex;
  aeronet::File _file;
  RawChars _window;
  RawChars32 _decoded;
  RawChars32 _keyScratch;
  RawChars32 _error;
  amc::vector<std::string_view> _values;
  std::uint64_t _windowOffset{0};
  bool _windowAtEnd{false};
};

// The indexes of the data files of a collection, reused between previews and runs while the files do not change.
class DataFileIndexCache {
 public:
  // The index of the file, built if absent or stale: this can take long, call it off the UI thread. Never null: on
  // failure the index is not valid(). Thread safe.
  [[nodiscard]] std::shared_ptr<const DataFileIndex> get(const std::filesystem::path& path, DataFileFormat format);

 private:
  std::mutex _mutex;
  amc::vector<std::shared_ptr<const DataFileIndex>> _indexes;
};

// The file designated by the path of a parameter: a relative path starts from 'baseDirectory'.
[[nodiscard]] std::filesystem::path ResolveDataFilePath(std::string_view path,
                                                        const std::filesystem::path& baseDirectory);

}  // namespace zalie
