#ifndef LEVARI_TIER_RENAME_H
#define LEVARI_TIER_RENAME_H
/* Codegen-tier symbol renaming for the decoder translation units.
   When LEVAR_DECODE_SUFFIX is set (e.g. _avx2), every linkable symbol that
   the decode tier objects DEFINE gets suffixed, so several codegen tiers of
   decode.c + stb_vorbis.c can coexist in one binary; src/decode/dispatch.c
   owns the unsuffixed public API and picks a tier at runtime.
   Included by decode.c itself (guarded) and force-included (-include) into
   the per-tier stb_vorbis.c compiles. dr_flac/dr_wav need no entries here:
   their tiers compile with DRFLAC_API/DRWAV_API/..._PRIVATE=static, making
   every drflac_/drwav_ symbol local to the tier object. */

#if defined(LEVAR_DECODE_SUFFIX)

#define LEV_TIER_CAT2(a, b) a##b
#define LEV_TIER_CAT(a, b) LEV_TIER_CAT2(a, b)
#define LEV_TIER(name) LEV_TIER_CAT(name, LEVAR_DECODE_SUFFIX)

/* public decoder API: the per-tier objects export these suffixed names,
   dispatch.c references them and exposes the plain ones */
#define dec_open           LEV_TIER(dec_open)
#define dec_rate           LEV_TIER(dec_rate)
#define dec_channels       LEV_TIER(dec_channels)
#define dec_total_frames   LEV_TIER(dec_total_frames)
#define dec_read           LEV_TIER(dec_read)
#define dec_seek           LEV_TIER(dec_seek)
#define dec_close          LEV_TIER(dec_close)
#define dec_ffmpeg_probe   LEV_TIER(dec_ffmpeg_probe)

/* minimp3: plain extern functions, the vendored header has no API macro */
#define mp3dec_init              LEV_TIER(mp3dec_init)
#define mp3dec_f32_to_s16        LEV_TIER(mp3dec_f32_to_s16)
#define mp3dec_decode_frame      LEV_TIER(mp3dec_decode_frame)
#define mp3dec_detect_buf        LEV_TIER(mp3dec_detect_buf)
#define mp3dec_detect_cb         LEV_TIER(mp3dec_detect_cb)
#define mp3dec_load_buf          LEV_TIER(mp3dec_load_buf)
#define mp3dec_load_cb           LEV_TIER(mp3dec_load_cb)
#define mp3dec_iterate_buf       LEV_TIER(mp3dec_iterate_buf)
#define mp3dec_iterate_cb        LEV_TIER(mp3dec_iterate_cb)
#define mp3dec_ex_open_buf       LEV_TIER(mp3dec_ex_open_buf)
#define mp3dec_ex_open_cb        LEV_TIER(mp3dec_ex_open_cb)
#define mp3dec_ex_close          LEV_TIER(mp3dec_ex_close)
#define mp3dec_ex_seek           LEV_TIER(mp3dec_ex_seek)
#define mp3dec_ex_read_frame     LEV_TIER(mp3dec_ex_read_frame)
#define mp3dec_ex_read           LEV_TIER(mp3dec_ex_read)
#define mp3dec_detect            LEV_TIER(mp3dec_detect)
#define mp3dec_load              LEV_TIER(mp3dec_load)
#define mp3dec_iterate           LEV_TIER(mp3dec_iterate)
#define mp3dec_ex_open           LEV_TIER(mp3dec_ex_open)
#ifdef _WIN32
#define mp3dec_detect_w          LEV_TIER(mp3dec_detect_w)
#define mp3dec_load_w            LEV_TIER(mp3dec_load_w)
#define mp3dec_iterate_w         LEV_TIER(mp3dec_iterate_w)
#define mp3dec_ex_open_w         LEV_TIER(mp3dec_ex_open_w)
#endif

/* stb_vorbis: full public set (the tier object compiles the implementation,
   internal code calls public entry points, so every name must move) */
#define stb_vorbis_close                       LEV_TIER(stb_vorbis_close)
#define stb_vorbis_comment                     LEV_TIER(stb_vorbis_comment)
#define stb_vorbis_decode_filename             LEV_TIER(stb_vorbis_decode_filename)
#define stb_vorbis_decode_frame_pushdata       LEV_TIER(stb_vorbis_decode_frame_pushdata)
#define stb_vorbis_decode_memory               LEV_TIER(stb_vorbis_decode_memory)
#define stb_vorbis_flush_pushdata              LEV_TIER(stb_vorbis_flush_pushdata)
#define stb_vorbis_get_comment                 LEV_TIER(stb_vorbis_get_comment)
#define stb_vorbis_get_error                   LEV_TIER(stb_vorbis_get_error)
#define stb_vorbis_get_file_offset             LEV_TIER(stb_vorbis_get_file_offset)
#define stb_vorbis_get_frame_float             LEV_TIER(stb_vorbis_get_frame_float)
#define stb_vorbis_get_frame_short             LEV_TIER(stb_vorbis_get_frame_short)
#define stb_vorbis_get_frame_short_interleaved LEV_TIER(stb_vorbis_get_frame_short_interleaved)
#define stb_vorbis_get_info                    LEV_TIER(stb_vorbis_get_info)
#define stb_vorbis_get_sample_offset           LEV_TIER(stb_vorbis_get_sample_offset)
#define stb_vorbis_get_samples_float           LEV_TIER(stb_vorbis_get_samples_float)
#define stb_vorbis_get_samples_float_interleaved LEV_TIER(stb_vorbis_get_samples_float_interleaved)
#define stb_vorbis_get_samples_short           LEV_TIER(stb_vorbis_get_samples_short)
#define stb_vorbis_get_samples_short_interleaved LEV_TIER(stb_vorbis_get_samples_short_interleaved)
#define stb_vorbis_info                        LEV_TIER(stb_vorbis_info)
#define stb_vorbis_open_file                   LEV_TIER(stb_vorbis_open_file)
#define stb_vorbis_open_filename               LEV_TIER(stb_vorbis_open_filename)
#define stb_vorbis_open_file_section           LEV_TIER(stb_vorbis_open_file_section)
#define stb_vorbis_open_memory                 LEV_TIER(stb_vorbis_open_memory)
#define stb_vorbis_open_pushdata               LEV_TIER(stb_vorbis_open_pushdata)
#define stb_vorbis_seek                        LEV_TIER(stb_vorbis_seek)
#define stb_vorbis_seek_frame                  LEV_TIER(stb_vorbis_seek_frame)
#define stb_vorbis_seek_start                  LEV_TIER(stb_vorbis_seek_start)
#define stb_vorbis_stream_length_in_samples    LEV_TIER(stb_vorbis_stream_length_in_samples)
#define stb_vorbis_stream_length_in_seconds    LEV_TIER(stb_vorbis_stream_length_in_seconds)

#endif /* LEVAR_DECODE_SUFFIX */
#endif /* LEVARI_TIER_RENAME_H */
