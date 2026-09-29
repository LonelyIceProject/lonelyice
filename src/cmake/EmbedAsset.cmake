# cmake -DIN=<file> -DOUT=<file.cpp> -DSYMBOL=<name> -P EmbedAsset.cmake
# Writes the file's bytes as a C++ array (portable replacement for Windows resources).
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hexLength)
math(EXPR size "${hexLength} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUT}" "#include <cstddef>
namespace LonelyIce
{
    extern unsigned char const ${SYMBOL}_data[] = {
${bytes}0 };
    extern std::size_t const ${SYMBOL}_size = ${size};
}
")
