# Vocalyx — handoff, 29 September 2026

Written at the end of a long session. Some of what follows is verified working
and some is written but unverified; the distinction is marked throughout,
because a chunk of tonight was spent debugging code that turned out never to
have been compiled.

## What the project is now

Three things that were one thing this morning.

1. **The application** — `of_v0.12.1_osx_release/apps/myApps/vocalyx`, an
   openFrameworks C++ app. Loads a recording, analyses it, transforms it,
   displays what it is doing. The Python original lives on in `v1/` as a
   reference implementation.
2. **The neural pipeline** — `_PROJECTS/vocalyx-neural`, a Python sidecar
   wrapping Seed-VC for voice conversion.
3. **Source data** — `_PROJECTS/vocalyx-source-data`, the Hillenbrand 1995
   vowel measurements.

## Verified working

- **Loading and playback.** Drag a WAV onto the window; it loops, with a
  playhead. AudioToolbox handles decoding, since openFrameworks 0.12.1 has no
  file reader.
- **WORLD analysis.** Fundamental frequency, spectral envelope, aperiodicity.
  Measures tract length from formant dispersion, pitch, tilt, jitter, shimmer.
  On the test recording: 16.3 cm, 94.9 Hz for James; 13.3 cm, 193.5 Hz for the
  French collaborator.
- **The spectrogram and formant track display.**
- **The coupled size control**, which moves tract length, pitch and pharynx
  share together along a locus fitted to male-to-female and male-to-child
  anchors. This is the control that finally produced plausible results rather
  than chipmunks.
- **Windowed preview.** Three seconds around the playhead, which brought
  parameter changes from 1,200 ms down to 100–200 ms. WORLD path only.
- **Neural conversion.** Seed-VC converting James's English to the
  collaborator's French voice from 4.2 minutes of reference audio. Result was
  convincing — the clearest success of the session and the thing that settled
  the direction.

## Written but unverified

Everything below compiled for the first time at the very end of the session and
was barely heard.

- **`FilterEngine`** — short-time Fourier transform, cepstral envelope
  estimation, ratio filtering with phase preserved. The point is transparency:
  at identity it returns the input untouched, where WORLD's resynthesis is
  audibly lossy even when doing nothing. **This is the most important unverified
  piece.** If it works, it should replace WORLD for all envelope work.
- **The all-pole formant tracker with path search.** Replaced peak picking on
  the raw envelope. Ran for the first time tonight; James reported the vowel
  space did not look noticeably better, which is worth taking seriously.
- **Speaker profiles** (`bin/data/profiles.txt`, `SpeakerProfiles.cpp`). Group
  averages from Hillenbrand, applied as ratios. `g` cycles the reference group,
  `[` and `]` the target.
- **The morph layer.** Blends toward a neural conversion of the same
  performance, frame by frame, with separate weights for timbre, breath and
  pitch track. `c` loads the conversion. Implemented in both engines.
- **The formant editor.** Draggable handles, one per formant, on a log
  frequency axis. Dragging overrides the anatomy controls.

## Known broken

- **Pausing breaks the filters.** After pause and resume, parameter changes stop
  affecting the sound. Bypass still works, so a processed clip exists and the
  audio path is intact; what is unknown is whether renders are still happening.
  The millisecond readout answers that and was not checked.
- **The formant handles stopped responding** at some point in the same session.
  Unclear whether this is the same bug.
- **The vowel plot is still a shapeless cloud**, after the tracker rewrite that
  was supposed to fix it. Since the rewrite genuinely ran this time, the cause
  lies elsewhere — possibly frames being marked voiced that are not, or
  connected speech simply covering more of the space than expected.
- **Unused variable warning**, `ofApp.cpp` around line 970: a leftover `stats`.

## The trap that cost two hours

Adding files to Xcode with **"Copy items if needed" checked** put duplicates at
the project root. Xcode compiled those; every edit went to `src/`. So the
formant tracker rewrite, the speaker profiles and the first filter engine fixes
were all invisible — the build was using files dated before they were written.

Always uncheck that box. To verify: Build Phases → Compile Sources should show
`in src` beside every file.

Related: **projectGenerator rewrites the Xcode project** and discards the Rubber
Band build settings. `tools/fix-xcode-settings.py` restores them. Better to add
files by dragging them into Xcode and avoid running the generator at all.

## Also worth knowing

- The build targets **x86_64**, so the app has been running under Rosetta the
  whole time. Switching to arm64 should roughly halve render times.
- Rubber Band comes from Homebrew, so the app will not run on a Mac without it.
  Linking `librubberband.a` statically is the fix, when it matters.
- WORLD is vendored under `src/world` and `src/world_src`, modified BSD.
- Seed-VC needed two patches for Apple Silicon: casting the pitch track to
  float32, and skipping autocast on Metal. Both are local edits to
  `seed-vc/inference.py` and will be lost if that repository is updated.

## Where this was heading

The order agreed before stopping:

1. Establish what actually works, now that the build is finally honest.
2. Fix the pause bug and the formant handles.
3. If the filter engine proves transparent, make it the default for all
   envelope work and reserve WORLD for pitch-track morphing and perturbation,
   which need resynthesis.
4. Then the source layer: aryepiglottic narrowing around 3 kHz, the thick-to-thin
   source control, vibrato and drift. These address what the warp cannot touch,
   since every transformation currently leaves the glottal source untouched.
5. Longer term: an expression layer for emotional and social qualities — pitch
   range, declination, terminal contours, rate — which is where most of that
   research actually lives.

## Consent and rights

The French recording is a collaborator's, used with their agreement. Whether
that agreement covers public use of synthetic speech in their voice has not been
settled and should be before anything is shown.
