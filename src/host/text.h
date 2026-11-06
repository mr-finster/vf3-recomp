// This game's strings' IDs (tr::ONLINE and the rest), made from src/host/lang/en.inc: each T(ID, "English") there is
// tr::ID here. Their translations: lang/<code>.inc, one a language. How it works: i18n.h; how to add one:
// docs/i18n-design.md.
#pragma once
#include "i18n.h"

namespace tr {

namespace ids {
enum : uint16_t {
#define T(id, english) id,
#include "lang/en.inc"
#undef T
	COUNT_
};
}

#define T(id, english) inline constexpr text id{ ids::id };
#include "lang/en.inc"
#undef T

} // namespace tr
