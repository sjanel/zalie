#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace zalie::test {

// A fresh temporary directory, removed with its content at destruction.
class TempDir {
 public:
  TempDir() {
    std::random_device device;
    const std::string name = "zalie-test-" + std::to_string(device()) + "-" + std::to_string(device());
    _path = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(_path);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  ~TempDir() {
    std::error_code errc;
    std::filesystem::remove_all(_path, errc);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return _path; }

 private:
  std::filesystem::path _path;
};

}  // namespace zalie::test
