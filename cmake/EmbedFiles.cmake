# Embeds binary files (fonts, images) into a target as constant byte arrays, portably (no #embed: GCC 13 and MSVC do
# not support it). Each file becomes a function `std::span<const unsigned char> <SYMBOL>() noexcept` in namespace
# zalie::embedded, generated at build time so that editing an asset regenerates its source.
#
#   zalie_embed_file(<target> <file> <SYMBOL>)

set(ZALIE_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/EmbedFile.cmake")

function(zalie_embed_file target file symbol)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/embedded/${symbol}.cpp")
  add_custom_command(
    OUTPUT "${output}"
    COMMAND ${CMAKE_COMMAND} -D "INPUT=${file}" -D "OUTPUT=${output}" -D "SYMBOL=${symbol}" -P
            "${ZALIE_EMBED_SCRIPT}"
    DEPENDS "${file}" "${ZALIE_EMBED_SCRIPT}"
    COMMENT "Embedding ${symbol}"
    VERBATIM)
  target_sources(${target} PRIVATE "${output}")
endfunction()
