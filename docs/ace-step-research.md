# ACE-Step 1.5 Turbo in Fugue: research and decisions

Date 2026-11-22. Sources: acestep.cpp `docs/ARCHITECTURE.md` and `tools/ace-server.cpp` at the commit omarchy-local-ai
pins (973517be); omarchy-local-ai `recipes/ace-step-1.5-turbo-acestep-cuda.toml` and `docs/engine.md`. [U] = not
verified against a running server.

## What the server is

omarchy-local-ai runs **acestep.cpp's `ace-server`** (GGUF, CUDA on the GB10) with the **turbo** DiT only (2B, 8 steps,
shift 3), the 4B language model, Qwen3 Embedding 0.6B and the VAE (7.57 GiB; a 180 s song in 31 s on the GB10). A model
is shared to this machine through the omarchy pairing (`gx10-f5f4/ace-step-1.5-turbo-acestep-cuda`), and Fugue's omarchy
provider already finds it as a `music` row: capability `audio.generate`, resource `fugue/music-model`, 10 to 600 s.

API (all jobs are async: POST returns `{"id"}`, then `GET /job?id=N`, `GET /job?id=N&result=1`, `POST /job?id=N&cancel=1`):

| Endpoint | Does |
|---|---|
| `POST /lm` | the 4B LM writes a song: caption, lyrics, bpm, key, time signature, duration, audio codes (`lm_mode` generate; `inspire` = caption to lyrics and metadata, no codes; `format` = caption + lyrics to metadata + lyrics, no codes) |
| `POST /synth` | the DiT and VAE render it: JSON, or multipart (`request`, `audio` = source song, `ref_audio` = timbre reference, `*_latents`) |
| `POST /understand` | multipart `audio`: caption, lyrics, bpm, key from a song (LM reads the FSQ codes) |
| `POST /vae` | encode or decode only |

## Tasks (`task_type`) and what the turbo model can do

| Task | Needs | Turbo | In Fugue |
|---|---|---|---|
| text2music | caption (+ lyrics) | yes | **Generate Song** (before) |
| cover | source audio, `audio_cover_strength` | yes, LM skipped | **Remix Song** (now) |
| cover-nofsq | source audio, keeps tune and timing | yes | Remix Song's "Stay close" (now) |
| repaint | source audio, `repainting_start/end` (negative start or end beyond the song = outpaint) | yes | **Repaint Song** (now; outpaint later) |
| lego, extract, complete | source audio, `track` (vocals, drums, bass, guitar, keyboard...) | **no, base DiT only** | issue; needs the base model in a recipe |

For cover, repaint and the other source tasks the LM is skipped: caption and lyrics reach the DiT verbatim. A cover with
empty lyrics has no words to sing, so Remix and Repaint send the lyrics given, `[Instrumental]` when the box is ticked.

## Request fields vs Generate Song (before this work)

| Field | Server | Generate Song before | Now |
|---|---|---|---|
| caption, lyrics, duration, vocal_language, seed | yes | all five (`[Instrumental]` via the Instrumental box) | same |
| bpm, keyscale, timesignature | LM fills if unset | no | **Tempo, Key, Time signature** (advanced; 0 or empty = model chooses) |
| lm_temperature | 0.85 | no | **Creativity** (advanced, unset = server's) |
| use_cot_caption | true: the LM rewrites the caption | no | **Keep my description** (advanced) |
| inference_steps, guidance_scale, shift, solver | auto (turbo: 8, 1.0, 3.0; guidance over 1 is overridden on turbo) | no | not exposed: nothing to gain on turbo |
| audio_cover_strength, cover_noise_strength | cover | no | Remix Song's Strength (noise strength later) |
| repainting_start / end | repaint | no | Repaint Song's From / To |
| synth_batch_size (up to 9), lm_batch_size | variations | no | issue (a Count input once a node can return a list of songs) |
| output_format `mp3`, `wav16/24/32`, mp3_bitrate | | mp3, default bitrate | **Quality** (advanced: mp3, mp3-320, lossless = wav24; #258) |
| ref_audio | timbre / style reference | no | issue |
| adapter, adapter_scale (LoRA) | `--adapters` dir | no | issue |
| lm_cfg_scale, top_p, top_k, negative prompt | LM sampling | no | not exposed |
| /understand | audio to caption, lyrics, metadata | no | issue (Describe Song) |

Limits worth knowing: duration 10 to 600 s (the FSM clamps the LM's choice; 0 falls to 120 s); KV cache `--max-seq 8192`
tokens bounds caption plus lyrics; `[Instrumental]` is the exact string the DiT was trained with for "no vocals"; lyric
section tags in square brackets ([Verse], [Chorus], [Bridge]) are what the model reads; `vocal_language` is a plain
code (`pt`, not `pt-BR`; Fugue already trims).

## Decisions (overturnable; confidence in brackets)

1. **Generate Song 1.1.0** gets the five advanced inputs above, sent only when set. [high] A minor bump, so a workflow
   that does not use them still validates, and a template that does asks for Fugue 1.1.0.
2. **Remix Song** and **Repaint Song** are two simple nodes (house rule), on the `audio.generate` capability with a
   `task` param: no new capability, because they are served by the same music rows and the provider needs only to
   post a multipart to /synth. A model that cannot do the task (a future one) fails with the server's own message.
   [medium-high] A single "Edit Song" node with a mode input was rejected: its inputs would differ per mode.
3. **Lyrics** are written by any text model through Enhance Prompt's new `lyrics` style (family `audio`), not by the
   ace LM's `inspire` mode: Enhance Prompt already picks the model, shows the answer in a Preview and can be edited
   before it is sung. `inspire` stays a later option. [medium]
4. **Song for a picture** is Describe Image's new `song-prompt` style into Generate Song. [high]
5. Later work is filed as issues, not stubbed in the UI: Describe Song #254, Extend Song #255, stems #256 (base model),
   variations #257, lossless output #258, reference audio #259, adapters #260, Write Lyrics with the ace LM #261.

## Templates (fugue-workflows, category `audio`)

`text-to-song` (description + lyrics, optional), `instrumental` (music bed, length, tempo), `write-lyrics-then-sing`
(an idea becomes lyrics through Enhance Prompt, then a song), `song-for-a-picture`, `remix-song`, `repaint-song`.
Each: Generate Song or Remix/Repaint, a Preview of the lyrics where there are some, Save Audio.

## Live check on the GB10 (2026-11-22, through the omarchy pairing)

- Text to song, Write lyrics then sing, Song for a picture, Instrumental, Remix and Repaint templates all ran on
  `gx10-f5f4/ace-step-1.5-turbo-acestep-cuda` through `fugue run`. A remix or a repaint of a 16 s song took about 3 s;
  a 20 to 40 s song with lyrics about 20 s.
- The multipart `/synth` (`request` and `audio` parts, each sent with a filename) is accepted as the docs say.
- Repaint keeps the outside: the part before the region matches the source at a correlation of 0.99 once the 58 ms the
  VAE round trip adds is lined up; the region itself changes.
- Observed [U, not understood]: with lyrics and `duration` 20 the song came back 40.2 s long (without lyrics, 20.0 s).
  The node reports the length it got (`duration` output). To look at when the length matters (issue to file if it repeats).

## Lossless output and mp3 bitrate (#258)

Source: acestep.cpp docs/ARCHITECTURE.md (output_format: mp3, wav16, wav24, wav32; mp3_bitrate default 128). One
advanced input, **Quality**, on all three song nodes: `mp3` (the default, nothing sent: workflows keep their cache
entries), `mp3-320`, and `lossless`. Decision [medium-high]: lossless is **wav24**. wav16 is CD depth and drops what
the VAE's float output holds; wav32 is float WAV, which some players and editors refuse; 24 bits is what a studio
file carries and what FLAC keeps. One input with three values rather than a format and a bitrate pair, since the
bitrate means nothing for a WAV. The provider saves `audio.wav` or `audio.mp3` (the file's extension follows the
server's answer for the format asked) and Save Audio re-encodes as it does for any audio.
