// control/stdio_lines.hpp
//
// C-stdio NDJSON line I/O. The transport TUs must not pull in <iostream>:
// its static std::ios_base::Init object allocates cout/cin/cerr + locale
// before main -- a static-init alloc outside the budgeted CPU block. FILE*
// is buffered; reads are unbounded (NDJSON command lines can be long). Not
// a hot path -- one line per command.

#pragma once

#include <cstdio>
#include <string>

namespace cairns::control {

// Reads one '\n'-terminated line (newline stripped, '\r' dropped) from `f` into
// `line`. Returns false at EOF with no further data -- mirrors std::getline's
// stream-bool so the caller's `while (ReadLine(...))` keeps the same shape.
inline bool ReadLine(std::FILE* f, std::string& line) {
    line.clear();
    int c = std::fgetc(f);
    if (c == EOF) {
        return false;
    }
    while (c != EOF) {
        if (c == '\n') {
            break;
        }
        if (c != '\r') {
            line.push_back(static_cast<char>(c));
        }
        c = std::fgetc(f);
    }
    return true;
}

inline void WriteLine(std::FILE* out, const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), out);
    std::fputc('\n', out);
}

}  // namespace cairns::control
