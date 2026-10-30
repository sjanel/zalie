#include "zalie/exchange.hpp"

#include <string_view>

#include "zalie/json-format.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/syntax.hpp"
#include "zalie/text-document.hpp"

namespace zalie {

void ResponseBody::analyze(std::string_view contentType) {
  kind = DetectContentKind(contentType, raw);
  pretty = RawChars{};
  prettyLines.clear();
  rawLines.clear();
  if (kind == ContentKind::image || kind == ContentKind::binary || kind == ContentKind::empty) {
    return;  // displayed as an image or a hex dump, computed on the fly from 'raw'
  }
  rawLines.build(raw, SyntaxFor(kind));
  if (kind == ContentKind::json) {
    if (PrettyPrintJson(raw, pretty)) {
      prettyLines.build(pretty, SyntaxLanguage::json);
    } else {
      pretty = RawChars{};  // invalid JSON: shown raw
    }
  }
}

}  // namespace zalie
