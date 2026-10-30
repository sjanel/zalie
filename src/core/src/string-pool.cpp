#include "zalie/string-pool.hpp"

#include <aeronet/memory-utils-sv.hpp>
#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace zalie {

std::string_view StringPool::store(std::string_view str) {
  if (str.empty()) {
    return {};
  }
  char* pData = _pool.allocateAndDefaultConstruct(str.size());
  aeronet::Copy(str, pData);
  return {pData, str.size()};
}

std::string_view StringPool::concat(std::initializer_list<std::string_view> parts) {
  std::size_t size = 0;
  for (std::string_view part : parts) {
    size += part.size();
  }
  if (size == 0) {
    return {};
  }
  char* pData = _pool.allocateAndDefaultConstruct(size);
  char* pOut = pData;
  for (std::string_view part : parts) {
    pOut = aeronet::Append(part, pOut);
  }
  return {pData, size};
}

void StringList::pushBackUnique(std::string_view str) {
  if (!contains(str)) {
    push_back(str);
  }
}

bool StringList::contains(std::string_view str) const noexcept { return std::ranges::contains(_items, str); }

}  // namespace zalie
