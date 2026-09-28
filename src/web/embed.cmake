# Turns a file into a C byte array, so web assets live as real files in the repo (no C escaping):
#   cmake -DIN=<file> -DOUT=<c file> -DNAME=<symbol> -P src/web/embed.cmake
# defines `const unsigned char NAME[]` and `const size_t NAME_len`.
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR len "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}"
    "// Generated from ${IN} by src/web/embed.cmake: edit that file instead.\n"
    "#include <stddef.h>\n"
    "const unsigned char ${NAME}[] = {${bytes}};\n"
    "const size_t ${NAME}_len = ${len};\n")
