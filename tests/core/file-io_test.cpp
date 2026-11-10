#include "zalie/file-io.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>

#include "test-utils.hpp"
#include "zalie/raw-chars.hpp"

namespace zalie {
namespace {

TEST(FileIo, WriteThenRead) {
  const test::TempDir dir;
  const std::filesystem::path path = dir.path() / "sub" / "dir" / "file.txt";  // the directories are created
  ASSERT_TRUE(WriteFileAtomically(path, "content").has_value());
  ASSERT_TRUE(WriteFileAtomically(path, "replaced").has_value());
  auto content = ReadFile(path);
  ASSERT_TRUE(content.has_value());
  EXPECT_EQ(std::string_view(*content), "replaced");
  EXPECT_FALSE(std::filesystem::exists(dir.path() / "sub" / "dir" / "file.txt.tmp"));
  ASSERT_TRUE(WriteFileAtomically(path, "").has_value());
  content = ReadFile(path);
  ASSERT_TRUE(content.has_value());
  EXPECT_TRUE(content->empty());
}

TEST(FileIo, ErrorsNameTheFile) {
  const test::TempDir dir;
  auto missing = ReadFile(dir.path() / "missing");
  ASSERT_FALSE(missing.has_value());
  EXPECT_TRUE(View(missing.error()).starts_with("cannot open '")) << View(missing.error());
  auto directory = ReadFile(dir.path());  // opens on Linux: its size is the error
  ASSERT_FALSE(directory.has_value());
  EXPECT_TRUE(View(directory.error()).contains(PathToUtf8(dir.path()))) << View(directory.error());

  // A directory where a file is: its parent cannot be created, its temporary file neither.
  ASSERT_TRUE(WriteFileAtomically(dir.path() / "file", "x").has_value());
  auto underFile = WriteFileAtomically(dir.path() / "file" / "child", "x");
  ASSERT_FALSE(underFile.has_value());
  EXPECT_TRUE(View(underFile.error()).starts_with("cannot create the directory of")) << View(underFile.error());
  std::filesystem::create_directories(dir.path() / "blocked.tmp");
  auto blocked = WriteFileAtomically(dir.path() / "blocked", "x");
  ASSERT_FALSE(blocked.has_value());
  EXPECT_TRUE(View(blocked.error()).starts_with("cannot create '")) << View(blocked.error());
  // The destination is a directory: the temporary file is removed.
  std::filesystem::create_directories(dir.path() / "target" / "inside");
  auto onDirectory = WriteFileAtomically(dir.path() / "target", "x");
  ASSERT_FALSE(onDirectory.has_value());
  EXPECT_FALSE(std::filesystem::exists(dir.path() / "target.tmp"));
}

TEST(FileIo, Utf8Paths) {
  const std::filesystem::path path = PathFromUtf8("dossier/\xC3\xA9t\xC3\xA9.json");
  EXPECT_EQ(View(PathToUtf8(path)), "dossier/\xC3\xA9t\xC3\xA9.json");
}

}  // namespace
}  // namespace zalie
