# The menus in other languages (i18n)

Virtua Fighter 3 Recomp's menus, notices, online screens, its own dialogs (choosing the ROM the first time, a start
that failed) and the Online site page are in 13 languages: English, Korean, Japanese, Simplified and Traditional
Chinese, Spanish, Brazilian Portuguese, French, Russian, German, Italian, Polish and Turkish. A pseudo-locale is there for testing. This page covers:

- how it works;
- how to add a string or a language;
- the fonts;
- the tests;
- the glossary and what still needs a native speaker;
- what Virtua Fighter and Virtua Fighter 2 must do to use the same system.

What is not translated: the game's own text (the ROM's: its title screen, HUD and so on), players' and rooms' names,
codes, addresses, key and button names as printed (Enter, Esc, A, LB, Start), the games' names and "VS".

The language never touches the game or online play:
- it is not sent in the protocol and not part of a save or the game's state;
- the log stays in English;
- the HUD's names stay as they are ("PLAYER 1");
- a room without a name is "NAME'S ROOM" in English everywhere, because everyone sees the same name.

## The mechanism (src/host/i18n.h, i18n.cpp, text.h, text.cpp, lang/*.inc)

- **One table per language, whole sentences.** `src/host/lang/en.inc` is the list of strings, `T(ID, "English")`, each
  with a comment saying where it is shown and how much room it has. Every other language has its own file
  (`ko.inc`, `ja.inc`, `zh-Hans.inc`, ...) with the same IDs in any order. A string is a whole sentence. What
  changes in it is a named place, `{name}`, `{code}`, `{n}`, and a translation puts the place where its language
  needs it. There is no concatenation of pieces: the word order is the translation's.
- **IDs.** `text.h` turns each line of `en.inc` (an X-macro) into a constant: `tr::ONLINE`, `tr::D_SCALE_HELP`. A
  `tr::text` converts to `std::string` in the language currently on (`operator std::string`, `c_str()`), so it goes
  anywhere a string did. A missing translation falls back to English, which `langtest` catches.
- **Filling places.** `tr::fill(tr::IN_LINE, {{"code", c}, {"n", "2"}, {"of", "3"}})` fills the places in one pass;
  what is filled in is never filled again.
- **Messages kept for later.** A notice, a failure's why and an error's line are a `tr::line`: an ID with its places,
  or a raw string. `.str()` renders it in the language on at the time it is drawn, so a language switch re-renders
  it. `.en()` renders it in English for the log.
- **src/net's sentences.** `tr::from_net(english)` maps the shared net code's English sentences (version refusals, a
  desync at a frame, socket errors) to `NET_*` IDs, their places pulled out, so src/net stays the same in every game
  and the menus still show them translated. An unknown one is shown as it came.
- **The setting.** Game > Language: Auto, then each language in its own name and script ("한국어", "Русский").
  Auto follows the system (`SDL_GetPreferredLocales`). The system's first language that the game has wins:
  - zh-TW, zh-HK and zh-MO (or a script tag Hant) give Traditional Chinese; any other Chinese gives Simplified;
  - any Portuguese gives pt-BR;
  - anything else that isn't covered gives English.

  `settings.ini` stores it as `language = auto|en|ko|ja|zh-Hans|zh-Hant|es|pt-BR|fr|ru|de|it|pl|tr`. Left or right on
  the row switches the language at once (`set_language` in main.cpp): `tr::set`, then `game_menu::set_language()`,
  which re-orders the fallback fonts, sets the documents' class (`lang-<code>`, plus `cjk`), re-renders the key hints
  and rebuilds the page. main.cpp's `menus_relabelled` then remakes the title, the main page and Game, with Language
  still selected.
- **Set early.** The language is set as soon as settings.ini is read, before the window: the first run's dialogs
  (choosing the ROM, `ROMS_*`) and a start that fails (`FAIL_*`) are in it too. A failure is a `tr::line`, so the
  log still says it in English. What a ROM check or the system says (the `{why}`) stays as it came.
- **For tests.** `VF_LANG=<code>` (or `pseudo`) overrides the setting for a run.
- **Pseudo-locale.** `VF_LANG=pseudo` gives accented letters, about 40% longer, in brackets: "[Ŕéñđéŕ šçáĺé ~~~]".
  It finds text that isn't going through the tables (it stays plain) and the boxes that are too tight.

### How to add a string

1. Add `T(MY_ID, "The English sentence with a {place}.")   // where it shows, how much room` to `en.inc`, in its
   section.
2. Use `tr::MY_ID` in the code, or `tr::fill(tr::MY_ID, {{"place", value}})`. For a message drawn later, use a
   `tr::line(tr::MY_ID, {{...}})`.
3. Add the line to every other `lang/*.inc`. If there is no translation yet, add the English: it is listed, not
   hidden. Keep the glossary's terms (`docs/i18n-glossary.md`). For CJK, use full-width punctuation, but keep ASCII
   for codes and numbers.
4. Build and run `langtest`. It fails on:
   - an ID missing, doubled or extra in any language;
   - a translation whose `{places}` differ from the English;
   - a letter that is in no font for that language (re-run `tools/fonts/subset.py`, see Fonts);
   - a string that starts or ends with a space.
5. Run `tools/langcheck.py`. It runs menutest in every language and lists every box the text overflows where the
   English doesn't, and every check the English passes that this language fails. Shorten the translation, or give
   the layout room without changing the English picture.

There are no plural rules. Where a count changes the word, the strings are written so that they don't need one. The
Polish and Russian ones say "pokoje: {n}" and "зрителей: {n}"; Korean, Japanese and Chinese have no plural. A
language that needs real plurals would add an ID per form, chosen in the code.

### How to add a language

1. Add it to the `tr::lang` enum (before `pseudo`), to `LANGS` and `REAL_LANGS`, to `info()` (its code, its name in
   its own script, its English name, CJK or not), and to `from_locale` if a system locale needs mapping.
2. Add `lang/<code>.inc` and add it to `text.cpp` (an `#include` and the `TABLE` and `TABLE_COUNT` entries; a
   `static_assert` counts them).
3. Fonts: if Barlow Condensed doesn't have its letters, add a subset (`tools/fonts/subset.py`) and put the face in
   `FALLBACK_FONTS` (menu.cpp) and `fallback_order()`.
4. Add it to `tools/langcheck.py`'s list and to the site's `LANGS` and `TR` (`tools/online/site/online.html`; run
   `node tools/online/sitelang.mjs`).
5. Run `langtest`, `langcheck`, and look at the pictures.

## Fonts and their size

Barlow Condensed stays the face for every Latin letter and number in every language. Its Latin Extended covers
Spanish, Portuguese, French, German, Italian, Polish and Turkish: the ą ę ł ń ś ź ż ğ ı İ ş letters were checked by
langtest. Everything else comes from Noto Sans subsets, used as RmlUi fallback faces:

| script | files | bytes | letters |
|---|---|---|---|
| Korean | NotoSansKR-Medium, -Bold | 142,776 | 450 (every Hangul syllable the strings use) |
| Japanese | NotoSansJP-Medium, -Bold | 350,908 | 529 (the kanji used, plus all the kana) |
| Simplified Chinese | NotoSansSC-Medium, -Bold | 314,280 | 599 |
| Traditional Chinese | NotoSansTC-Medium, -Bold | 367,856 | 602 |
| Cyrillic | NotoSansCyr-Medium, -SemiBold, -BoldItalic (condensed, wdth 75) | 31,196 | 66 |
| **total** | 11 files | **1,207,016 (1.15 MB)** | |

Each language's fonts are added to the download, not loaded unless that language is on. The licences
(`assets/fonts/OFL-NotoSansCJK.txt`, `OFL-NotoSans.txt`) and the credit (`assets/fonts/README.txt`, README.md) go
with them, and the release kit's `licenses.sh` includes them. To remake the fonts after a translation adds a letter:

```
cd vf3-recomp
pip install fonttools
python3 tools/fonts/subset.py <folder with NotoSansKR[wght].ttf, NotoSansJP[wght].ttf, NotoSansSC[wght].ttf,
                               NotoSansTC[wght].ttf, NotoSans[wdth,wght].ttf, NotoSans-Italic[wdth,wght].ttf>
```

(These are the variable fonts from google/fonts' `ofl/notosans*`.) The script reads every `lang/*.inc`, keeps only
the letters Barlow lacks, instances the weights and writes `assets/fonts/NotoSans*.ttf`.

**RmlUi changes** (third_party/rmlui, all small):

- `FontProvider` orders fallback faces by family priority, then style match, then nearest weight
  (`Rml::SetFallbackFontOrder`). The current language's script comes first, so a character shared by Chinese and
  Japanese is drawn in that language's own form.
- Faces remember their style and weight (`FontFaceHandleDefault`).
- `ElementText` breaks lines between CJK characters, with the usual kinsoku rules: no break before 、。）」ー or small
  kana, and no break after （「. Without this, RmlUi breaks only at spaces, and Japanese and Chinese ran off the
  right edge.

CJK is never slanted: there is no synthetic italic, and the title's italic falls back to the upright face. English is
unchanged: with `language = en`, menutest's 64 pictures are pixel-identical to the ones before this change.

**Layout.**

- In languages other than English, a page's help note (`#note`) shrinks from 17dp down to 14dp until it fits.
- Translations were shortened wherever `langcheck` found a box that the English fits.
- English's own long lines were left alone, so that its pictures stay the same: about 49 lines, such as Window
  size's help, which is over four lines, and the stress values menutest uses on purpose, such as Smooth motion's
  "On (60 a second for now: ...)" and CRT colours' "BT.601 PAL (...) (not with this renderer)". The same rows in
  other languages overflow the same way.

## The Online site page (tools/online/site/online.html)

- **Strings.** The same 13 languages are in `EN` and `TR`, with the same rules: whole lines and `{places}`.
  `node tools/online/sitelang.mjs` checks that every language has every line with the same places.
- **Choosing the language.** The page uses the language chosen in the switch at the top right, kept in this browser
  (`localStorage` "vf-lang"). Otherwise it uses the browser's languages, mapped as the game maps the system's.
  Otherwise English.
- **Fonts.** Korean, Japanese, Chinese and Russian load Noto Sans KR, JP, SC, TC or Noto Sans (condensed) from Google
  Fonts, after Barlow, which still draws the Latin letters and numbers. `_headers`' Content-Security-Policy now
  allows `fonts.googleapis.com` (styles) and `fonts.gstatic.com` (fonts).
- **Static text.** The title, the description, the note and "Flags:" are translated too. Column heads that the CSS
  capitalises are written in capitals by hand for Turkish (İZLEYEN, not a transform).

## Tests

- **`langtest`** (`make langtest`): the tables, the places, plain place names, every letter of every language in its
  fonts, the language names in their fonts, `tr::line` and `from_net`, `fill`, the setting's round trip (`language =`
  key ↔ choice for all 14 choices, an unknown code refused, Left and Right wrap), the locale mapping (zh-TW, zh-HK,
  zh-SG, zh-Hant, pt-PT and so on), and pseudo's length.
- **`menutest`** in any language (`VF_LANG=ko`):
  - every Online screen, the in-match menu, the notices, the VS screen, the watching cells;
  - replicas of every main.cpp page (`pages_*.png`), each row selected so each help is measured;
  - every line checked against its boxes;
  - the rows' left edges checked to be in line (`game_menu::misaligned`);
  - a runtime switch with a page open: en, then ko, ja, ru and tr, each drawn at once, then back to en, which must be
    the identical picture.
- **`tools/langcheck.py`**: menutest in all 13 languages plus pseudo, each compared with English. All 12 translations
  now report 0 boxes overflowed that the English fits and 0 extra failed checks. Pseudo is listed, not failed: 264
  tight spots, all in the replicas' stress values and the key-hint line. With `VF_LOBBY` set (tools/lobby's
  `test/serve.mjs`), the room screens are included, and langcheck runs one language at a time so that Quick match
  doesn't pair two of them.
- **The game itself, headless, per language** (`VF3_SCRIPT` scripted random play for 3000 frames with
  `language = <code>` in settings.ini, the software renderer): the game's frames at 900, 1500, 2400, 2900 and the
  last are byte-identical in all 13 languages and pseudo, and the in-game menu is drawn in each.
- **The site**: Playwright, all 13 languages at 1280 wide and as a phone (390 wide, touch):
  - no sideways scroll;
  - the Google font fetched for each script;
  - the browser-language pick for 13 locales;
  - the switch, and that its choice is kept.
- **Online**: the language isn't in any packet. nettest, delaytest, watchsim 0/1/50 and the lobby's `npm test`
  all pass as before (roomtest needs a TURN server, not run here). src/net isn't changed.

## Glossary and what needs a native speaker

The glossary, the main terms in all 13 languages side by side, is `docs/i18n-glossary.md`, made from the tables by
`tools/glossary.py`. Every translation here was written without a native reviewer. The points to check first, by
language:

- **Korean**: 해요체 throughout. 방장 (host), 퀵 매치, 관전, 참가, 체력 (Energy), 치트.
  - Particles after a place are written as 은(는) and 을(를) (`M_ONLINE_NOT_READY`, the site's "opening"); is that
    acceptable, or should those be reworded?
  - The Display values were shortened to fit ("CPU (GPU 오류: 골라서 재시도)").
- **Japanese**: です・ます. ホスト, ルーム, 募集中 (open), 観戦, 体力, エスケープ.
  - 挑戦者 for the test menu's CHALLENGER: is the ROM's own katakana チャレンジャー better? It was too long for its
    row.
  - Shortened to fit: the crumb "アーケード設定：変更はメニューを閉じると再起動", Window size's value "3x
    1536x1152 ウィンドウ", "クイックマッチをルーム一覧に公開".
- **Simplified Chinese**: 联机 (Online, vs 在线), 房主, 快速匹配, 观战, 场地 (stage, vs 舞台/场景), 乱入方 (Challenger),
  回滚 (rollback), 作弊.
- **Traditional Chinese**: made from the Simplified with OpenCC (s2twp), then Taiwan's terms: 連線, 線上服務, 手把,
  全螢幕, 快速配對, 代碼, 回報. Hong Kong readers may prefer 手掣 and others: which do players expect?
- **Spanish**: neutral, for Spain and Latin America (no vosotros; "equipo", not "ordenador"). Anfitrión, Partida
  rápida, Retador, Trucos, "relay" kept in English. Is "online" fine, or should it be "en línea"?
- **Brazilian Portuguese**: anfitrião, Partida rápida, Desafiante, Trapaças (or just "Cheats"?), "relay" in English.
- **French**: tutoiement. Salon (room), match rapide, relais, Astuces (or "Codes"?), "Public" for the watchers' cell
  (SPECTATEURS didn't fit), stage. Non-breaking spaces before : ; ? and %.
- **Russian**: ты.
  - хост, Быстрый матч, Уклон (Escape).
  - ОТК for the rollback cell: or keep "RB"?
  - Dural is in Latin letters.
  - "автомат" (the arcade machine) vs "Аркада" (Play style): one term, or keep both?
  - "D-pad" in the key hints (Крестовина didn't fit).
- **German**: du. Host, Schnelles Match, Automat (arcade), Ton (Sound: or "Audio"?), Stage, Relay.
- **Italian**: tu. Stanza, Partita rapida, Sfidante, Trucchi, "D-pad" in the key hints (Croce direzionale didn't
  fit), relay.
- **Polish**: ty. Pokój, Szybki mecz, Kody (Cheats), Automat. Counts are written as "pokoje: {n}" to need no plural
  forms.
- **Turkish**: sen.
  - Kurucu for host (or "Ev sahibi"?), Salon for arcade (or "Atari"/"Arcade"?), Aktarıcı (relay), Kol (pad), and
    Guard / Punch / Kick kept as the buttons' names.
  - No suffix ever follows a place: a sentence is reworded with a colon or "ile" instead (`{name} ile bağlanılıyor`).
    Do these read naturally?
- **Every language**: the newest strings, the first run's dialogs (`ROMS_CHOOSE`, `ROMS_COPIED_INTO`,
  `ROMS_NO_PICKER`, `ROMS_NOT_A_SET`, `ROMS_NO_COPY`) and a failed start's (`FAIL_VIDEO`, `FAIL_WINDOW`,
  `FAIL_GRAPHICS`, `FAIL_TEXTURE`). They're system message boxes, seen once, so they read as plain sentences; the
  set's names ("vf3", vf3.zip) stay as they are.

## What Virtua Fighter and Virtua Fighter 2 must do

Their host code is laid out the same way (main.cpp's pages, menu.cpp on RmlUi, online.cpp), so the system carries over
file for file. Once their rooms and watching patches are in, so that online.cpp matches VF3's:

1. **Copy the shared parts unchanged**:
   - `src/host/i18n.h`, `i18n.cpp`, `text.h`, `text.cpp`;
   - `tools/langtest.cpp`, `tools/langcheck.py`, `tools/fonts/subset.py`, `tools/glossary.py`;
   - the RmlUi changes (FontProvider, FontFaceHandleDefault, FontFace, Core.h and Core.cpp's
     `SetFallbackFontOrder`, ElementText's CJK breaking): VF1 and VF2 have the same RmlUi, so the diffs apply as
     they are;
   - `assets/fonts/OFL-NotoSans*.txt` and the README lines.
2. **The tables**: copy `lang/*.inc`. The online, room, watching, settings, controls and net strings are the same.
   Then, per game:
   - delete what isn't there (Team Battle's rows, Dural, the GPU renderer's notices if that game hasn't got them);
   - add that game's own rows (VF2's backgrounds, VF1's random fighters and so on) to `en.inc` and to every
     language, with the glossary's terms;
   - `langtest` lists what's missing.
3. **The code**:
   - replace each English literal in main.cpp, menu.cpp and online.cpp with its `tr::` ID, as VF3's diff does: page
     names, rows, helps, values, notices, the key hints, the net cells, the VS screen, the banners;
   - make notices and failures `tr::line` (log them with `.en()`);
   - delete `online_text.h` if that game has one;
   - add the Language row to Game, `cfg.language` (`language =` in settings.ini), `tr::set(tr::resolve(...))` before
     `menu.init`, `set_language` with `menus_relabelled`;
   - add the `FALLBACK_FONTS` table, `fallback_order()` and `game_menu::set_language()`, `overflow()` and
     `misaligned()` to menu.cpp, and the note's auto-fit in `show_help`.
4. **Fonts**: run `tools/fonts/subset.py` against that game's tables (about 1.2 MB again; less if it has fewer
   strings).
5. **Tests**:
   - port menutest's `VF_LANG`, the `pages_*` replicas and the switch test;
   - run `langtest`, `langcheck.py`, the scripted ROM run in each language (the frames identical), and that game's
     online tests.
6. **The site** is shared (one page for all three games): nothing more to do there.

## Adding Arabic, Hindi or Thai (not done)

- **Arabic** (and Persian, Hebrew) needs right to left:
  - RmlUi doesn't lay out RTL or shape Arabic. Each letter's form depends on its neighbours, and without HarfBuzz
    the letters come out unjoined.
  - It would need HarfBuzz shaping (or pre-shaped presentation forms, which are poor), a bidi pass (FriBidi or
    SheenBidi) to reorder mixed runs, since names, codes and numbers stay left to right, and mirrored layouts: the
    rows' values on the left, arrows reversed, the key hints' order.
  - Plus Noto Sans Arabic (about 100 KB subset).
- **Hindi** (Devanagari) needs shaping too: conjuncts, matras that reorder, the headline joined across letters.
  RmlUi's FreeType path draws one glyph per code point, so it would need HarfBuzz. Line breaking is at spaces,
  which RmlUi already does. Plus Noto Sans Devanagari (about 60 KB subset).
- **Thai** needs:
  - line breaking without spaces (a dictionary, e.g. ICU's or libthai's, since the CJK rule here doesn't apply:
    Thai breaks between words, not characters);
  - vowel and tone marks stacked above and below (positioned by HarfBuzz or the font's GPOS, which RmlUi doesn't
    apply);
  - Noto Sans Thai (about 30 KB).

So all three need HarfBuzz in RmlUi's font engine (RmlUi 6 has an optional HarfBuzz font engine plugin; switching to
it is the largest step). Arabic also needs bidi and a mirrored layout. Then the tables, fonts and tests work as for
any other language. The site page needs less: the browser shapes them all, so only `dir="rtl"` handling for Arabic
and the fonts.
