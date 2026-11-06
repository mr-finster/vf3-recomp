// This game's string tables (text.h): English from lang/en.inc, each translation from its own file. tools/langtest
// checks them all: every ID in every language once, the same {places} as the English.
#include "text.h"

namespace tr::data {

const char *const KEY[] = {
#define T(id, english) #id,
#include "lang/en.inc"
#undef T
};

const char *const EN[] = {
#define T(id, english) english,
#include "lang/en.inc"
#undef T
};

const size_t COUNT = sizeof EN / sizeof EN[0];

#define T(id, text) { ids::id, text },
static const entry KO_[] = {
#include "lang/ko.inc"
};
static const entry JA_[] = {
#include "lang/ja.inc"
};
static const entry ZH_HANS_[] = {
#include "lang/zh-Hans.inc"
};
static const entry ZH_HANT_[] = {
#include "lang/zh-Hant.inc"
};
static const entry ES_[] = {
#include "lang/es.inc"
};
static const entry PT_BR_[] = {
#include "lang/pt-BR.inc"
};
static const entry FR_[] = {
#include "lang/fr.inc"
};
static const entry RU_[] = {
#include "lang/ru.inc"
};
static const entry DE_[] = {
#include "lang/de.inc"
};
static const entry IT_[] = {
#include "lang/it.inc"
};
static const entry PL_[] = {
#include "lang/pl.inc"
};
static const entry TR_[] = {
#include "lang/tr.inc"
};
#undef T

// (in the order of tr::lang: en has none of its own)
#define N(a) sizeof a / sizeof a[0]
const entry *const TABLE[] = { nullptr, KO_, JA_, ZH_HANS_, ZH_HANT_, ES_, PT_BR_, FR_, RU_, DE_, IT_, PL_, TR_ };
const size_t TABLE_COUNT[] = { 0, N(KO_), N(JA_), N(ZH_HANS_), N(ZH_HANT_), N(ES_), N(PT_BR_), N(FR_), N(RU_), N(DE_), N(IT_), N(PL_), N(TR_) };
#undef N
static_assert(sizeof TABLE / sizeof TABLE[0] == REAL_LANGS, "a table for each language, in tr::lang's order");

} // namespace tr::data
