# Source and asset provenance

Audit date: 2026-10-26. License: MIT, Copyright (c) 2026 tappoy.

- Public API, scheduler, oscillator/envelope implementation, echo planning,
  percussion expansion, transition player, tests, build scripts and documentation:
  author-maintained code. The private synthesis implementation was extracted from
  the author's engine; the available source history identifies tappoy as author.
  No independent third-party copyright/license notices were found in these files.
  Existing notices were not removed.
- `assets/kick.dmc` and `src/kick-data.h`: deterministic output of
  `scripts/make-kick.py`, an exponential DAC-step waveform encoded as DPCM.
  Regeneration was compared byte-for-byte. No recorded audio, extracted game
  sample, or musical score is included.
- Standard C headers/functions and platform math libraries are build/runtime
  dependencies, not copied implementation sources in this repository.
- No vendored third-party code, fonts or other external media were found in the
  tracked library tree. Host application fonts/tools and user music are outside
  this license's scope.

This records the checked repository contents and available author history. It
is not a claim about unseen upstream materials. The owner's implementation
specification explicitly authorizes MIT licensing of this library's code and
associated documentation. The standard MIT text is in `LICENSE` and is installed
with the library.
