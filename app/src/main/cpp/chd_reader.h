// Cartucho - lets rcheevos identify disc games stored as .chd (compressed CD
// images), which its built-in disc reader does not open. Other disc formats
// (.cue/.bin, .iso...) still go to the built-in reader.
#pragma once

struct rc_hash_cdreader;

namespace cartucho {

// Fills `reader` with handlers that open .chd files through libchdr and hand
// anything else to rcheevos' own reader.
void GetDiscReader(rc_hash_cdreader* reader);

}  // namespace cartucho
