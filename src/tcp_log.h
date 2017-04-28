#pragma once
#include <iostream>

static inline std::ostream &LOG() {
#ifdef NOLOG
    std::cerr.setstate(std::ios_base::badbit);
#endif
    return std::cerr;
}
