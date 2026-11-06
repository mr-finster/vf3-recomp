// The words a room's or a player's name may not have (src/index.js, name_ok). Edit freely: upper case, A-Z only.
//
// A name is matched word by word, after: upper case; 0→O 1→I !→I |→I 3→E 4→A 5→S 7→T $→S @→A; each word's dots and
// other marks out (F.CK is FCK, S.H.I.T is SHIT); words of one letter in a row joined (F U C K is FUCK). Never across
// two real words: SUMMER DAY isn't MERDA, ORBIT CHAMP isn't BITCH, WHO REALLY isn't WHORE.
// Runs of a letter: a word with a double letter (FAGGOT, ASS) is matched with runs of 3 or more cut to 2, others with
// every run cut to 1 (FUUUCK is FUCK, SHIIIT is SHIT; AAS isn't ASS).
// ANYWHERE: blocked inside any word ("XFUCKX", "MOTHERFUCKERS"). Only words that are never part of an innocent one.
// STRONG: blocked as a word of its own, or with one of ENDINGS (SHITS, SHITTING, SHITTER, CUNTS, WANKER), and nothing
//   else around it: MERDE doesn't block MERDEKA, SIBAL doesn't block SIBALD.
// WHOLE: blocked as a word of its own or its plural (-S, -ES): ASS never blocks CLASSIC, PASSION or ASSASSIN, COCK never
//   COCKPIT, DICK never DICKENS; DICKS and ASSES are.
// A blocked room name shows as "<HOST>'S ROOM"; a blocked player name as "PLAYER".

export const ANYWHERE = [
	// English
	'FUCK', 'FCUK', 'PHUCK', 'MOTHERFUCKER', 'FAGGOT', 'NIGGER', 'NIGGA', 'BITCH', 'WHORE', 'ASSHOLE', 'COCKSUCKER', 'PUSSY',
	'RETARD', 'HITLER', 'PORN', 'SHITHEAD', 'BULLSHIT',
	// Korean (romanised)
	'GAESAEKKI', 'GAESEKKI', 'BYEONGSIN', 'BYUNGSIN', 'BYUNGSHIN',
	// Japanese (romanised)
	'KICHIGAI', 'YARIMAN',
	// Spanish
	'MIERDA', 'PENDEJO', 'MARICON', 'CABRON', 'CHINGADA', 'CULERO',
	// Portuguese
	'CARALHO', 'BUCETA', 'ARROMBADO',
	// French
	'PUTAIN', 'CONNARD', 'SALOPE', 'ENCULE',
];

export const STRONG = [
	// English
	'SHIT', 'CUNT', 'TWAT', 'WANK', 'SLUT', 'JIZZ', 'FCK', 'FUK', 'FUQ', 'STFU',
	// Korean (romanised)
	'SIBAL', 'SHIBAL', 'SSIBAL', 'JIRAL',
	// Portuguese, French
	'MERDA', 'MERDE',
];
export const ENDINGS = ['S', 'ES', 'ING', 'INGS', 'IN', 'ER', 'ERS', 'ED', 'Y', 'Z'];

export const WHOLE = [
	// English
	'ASS', 'DICK', 'COCK', 'CUM', 'FAG', 'TITS', 'SEX', 'RAPE', 'PENIS', 'NAZI', 'CHINK', 'SPIC', 'KIKE',
	// Japanese (romanised)
	'CHINKO', 'MANKO', 'CHINPO',
	// Spanish
	'PUTA', 'PUTO', 'CONO', 'VERGA',
	// Portuguese
	'PORRA', 'VIADO', 'FODA',
	// French
	'NIQUE', 'PEDE', 'BATARD',
];
