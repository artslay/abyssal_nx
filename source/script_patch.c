/* script_patch.c -- on-device patches to the game's compiled GDScript, for
 * defaults that only exist in script code (the touch-control settings and
 * the platform file picker).
 *
 * Each target .gdc is read from the user's own assets, decompressed with the
 * engine's zstd (the ZSTD_* forwarders in main.c), patched by rewriting literal
 * constants in its token buffer, re-encoded and written to <save_root>/_ovr, which
 * godot_shim.c serves instead of the original. Nothing is patched blind: the whole
 * buffer has to parse, and each constant/property has to hold the expected value
 * with the expected number of uses -- otherwise that script is left alone and the
 * game runs its original code. A game version that changes these scripts may need
 * the tables below re-derived. No game code is shipped here, only the literals to
 * look for.
 *
 * The copy must be exactly as long as the original: the engine reads every file
 * listed in assets.sparsepck with the size recorded there, so a longer copy would be
 * cut short and a shorter one padded with whatever is in memory. A compressed script
 * is re-compressed at a higher level than Godot's export (level 3), and the spare
 * bytes become a zstd skippable frame, which the decompressor steps over.
 *
 * .gdc layout (Godot 4.5+, tokenizer version 101): "GDSC", u32 version, u32
 * decompressed size (0 = stored uncompressed), then the token buffer: u32 counts of
 * identifiers, constants, lines and tokens; identifiers (u32 length + UTF-32 chars);
 * constants (Variant encoding); line and column tables (line count x 16 bytes each);
 * tokens (5 bytes, or 8 when bit 7 of the first byte is set: type in the low 7 bits,
 * identifier/constant index from bit 8, then 4 bytes of line).
 *
 * MIT license; see LICENSE. */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "util.h"
#include "asset_pack.h"
#include "godot_shim.h"
#include "script_patch.h"

// the engine's zstd, forwarded by main.c
size_t ZSTD_decompress(void *dst, size_t dstCap, const void *src, size_t srcSize);
size_t ZSTD_compress(void *dst, size_t dstCap, const void *src, size_t srcSize, int level);

#define GDSC_HEADER_SIZE 12
#define MAX_TOKEN_BUFFER (16u << 20)

/*
 * Godot 4.7 GDScriptTokenizer::Token::Type:
 *
 * EMPTY      = 0
 * ANNOTATION = 1
 * IDENTIFIER = 2
 * LITERAL    = 3
 *
 * Assignment:
 * EQUAL      = 31
 */
#define TOKEN_IDENTIFIER 2
#define TOKEN_LITERAL    3
#define TOKEN_EQUAL      28

#define ZSTD_SKIPPABLE_MAGIC 0x184D2A50u
#define ZSTD_SKIPPABLE_HEADER_SIZE 8

// Variant::Type ids of GDScript literals, and the 64-bit payload flag.
#define VARIANT_NIL 0
#define VARIANT_BOOL 1
#define VARIANT_INT 2
#define VARIANT_FLOAT 3
#define VARIANT_STRING 4
#define VARIANT_STRING_NAME 21
#define VARIANT_FLAG_64 (1u << 16)

#define MAX_CONST_PATCHES 4
#define MAX_BOOL_PATCHES 4

typedef struct {
  const char *text;     // String literal to find, or NULL for a float literal
  const char *new_text; // same-length replacement
  double value;         // float literal to find
  double new_value;
  uint32_t uses;        // LITERAL tokens that must reference it
} ConstPatch;

typedef struct {
  const char *identifier;
  int expected_value;
  int new_value;
} BoolPropertyPatch;

typedef struct {
  const char *asset;    // path under assets/
  const char *name;     // override file name (the .gdc basename)
  const ConstPatch *patches;
  unsigned count;
  const BoolPropertyPatch *bool_patches;
  unsigned bool_count;
} ScriptPatch;

// Touch controls are meant for phones; the Switch has a controller. touch_controls.gd
// asks OS.has_feature('mobile') twice -- `enabled` defaults to it, and the "TC" on/off
// button uses toggle_anchor.visible = is_mobile or enabled -- and this is the Android
// build, so both were true. 'nx_off' is no feature tag: touch controls start OFF and the
// TC button only shows while they are ON. The overlay opacity default (18%) -> 0%.
static const ConstPatch k_touch_controls[] = {
  { "mobile", "nx_off", 0.0, 0.0, 2 },
  { NULL, NULL, 0.1764706, 0.0, 1 },
};

// save_load.gd _load_touch_settings(): overlay_opacity = clamp(saved, 0.1, 1.0), so the
// 0% default above came back as 10% once the settings had been saved. Lower bound
// 0.1 -> 0.0. (Since 1.00.92 the Touch Controls menu itself stops at 10%.)
static const ConstPatch k_save_load[] = {
  { NULL, NULL, 0.1, 0.0, 1 },
};

/*
 * Godot's FileDialog normally falls back to its own Godot UI when
 * use_native_dialog=false. On Switch there is no Android native picker,
 * so we disable only this property.
 */
static const BoolPropertyPatch k_file_access_bool[] = {
  { "use_native_dialog", 1, 0 },
};

static const ScriptPatch k_scripts[] = {
  {
    "scripts/touch_controls.gdc",
    "touch_controls.gdc",
    k_touch_controls,
    2,
    NULL,
    0
  },
  {
    "scripts/Functions/save_load.gdc",
    "save_load.gdc",
    k_save_load,
    1,
    NULL,
    0
  },
  {
    "native/platform/file_access.gdc",
    "file_access.gdc",
    NULL,
    0,
    k_file_access_bool,
    1
  },
};

static uint32_t read_u32(const uint8_t *p) {
  return (uint32_t)p[0] |
         (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static void write_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

// Encoded size of one constant, for the Variant types GDScript literals use; 0 for
// anything else or truncated data.
static size_t constant_size(const uint8_t *p, size_t avail) {
  if (avail < 4)
    return 0;

  const uint32_t header = read_u32(p);

  size_t size;

  switch (header & 0xFF) {
    case VARIANT_NIL:
      size = 4;
      break;

    case VARIANT_BOOL:
      size = 8;
      break;

    case VARIANT_INT:
    case VARIANT_FLOAT:
      size = (header & VARIANT_FLAG_64) ? 12 : 8;
      break;

    case VARIANT_STRING:
    case VARIANT_STRING_NAME:
      if (avail < 8)
        return 0;

      size =
        8 + (((size_t)read_u32(p + 4) + 3) & ~(size_t)3);
      break;

    default:
      return 0;
  }

  return size <= avail ? size : 0;
}

static int constant_matches(const uint8_t *c,
                            const ConstPatch *patch) {
  const uint32_t header = read_u32(c);

  if (patch->text) {
    const size_t n = strlen(patch->text);

    return
      (header == VARIANT_STRING ||
       header == VARIANT_STRING_NAME) &&
      read_u32(c + 4) == n &&
      !memcmp(c + 8, patch->text, n);
  }

  double v;

  if (header == (VARIANT_FLOAT | VARIANT_FLAG_64)) {
    memcpy(&v, c + 4, sizeof(v));
  } else if (header == VARIANT_FLOAT) {
    float f;

    memcpy(&f, c + 4, sizeof(f));

    v = f;
  } else {
    return 0;
  }

  return fabs(v - patch->value) < 1e-6;
}

static void constant_rewrite(uint8_t *c,
                             const ConstPatch *patch) {
  if (patch->text) {
    memcpy(
      c + 8,
      patch->new_text,
      strlen(patch->new_text)
    );
  } else if (read_u32(c) & VARIANT_FLAG_64) {
    memcpy(
      c + 4,
      &patch->new_value,
      sizeof(patch->new_value)
    );
  } else {
    const float f =
      (float)patch->new_value;

    memcpy(
      c + 4,
      &f,
      sizeof(f)
    );
  }
}

// Applies `patches` to a decompressed token buffer in place. Returns 1 only if the
// buffer parsed exactly to its end and every patch found its constant.
static int patch_token_buffer(uint8_t *buf,
                              size_t len,
                              const ConstPatch *patches,
                              unsigned count) {
  if (len < 16 || count > MAX_CONST_PATCHES)
    return 0;

  const uint32_t identifiers =
    read_u32(buf);

  const uint32_t constants =
    read_u32(buf + 4);

  const uint32_t lines =
    read_u32(buf + 8);

  const uint32_t tokens =
    read_u32(buf + 12);

  size_t p = 16;

  for (uint32_t i = 0; i < identifiers; i++) {
    if (len - p < 4)
      return 0;

    const size_t chars =
      read_u32(buf + p);

    if (chars > (len - p - 4) / 4)
      return 0;

    p += 4 + chars * 4;
  }

  int ok = 0;

  uint32_t target[MAX_CONST_PATCHES] = {0};

  size_t *offsets =
    malloc(
      (constants ? constants : 1) *
      sizeof(*offsets)
    );

  uint32_t *uses =
    calloc(
      constants ? constants : 1,
      sizeof(*uses)
    );

  if (!offsets || !uses)
    goto done;

  for (uint32_t i = 0; i < constants; i++) {
    const size_t size =
      constant_size(
        buf + p,
        len - p
      );

    if (!size)
      goto done;

    offsets[i] = p;
    p += size;
  }

  if (lines > (len - p) / 16)
    goto done;

  p += (size_t)lines * 16;

  for (uint32_t i = 0; i < tokens; i++) {
    if (p >= len)
      goto done;

    const int wide =
      buf[p] & 0x80;

    const size_t token_len =
      wide ? 8 : 5;

    if (len - p < token_len)
      goto done;

    const uint32_t word =
      wide
        ? read_u32(buf + p)
        : buf[p];

    if ((word & 0x7F) == TOKEN_LITERAL) {
      if ((word >> 8) >= constants)
        goto done;

      uses[word >> 8]++;
    }

    p += token_len;
  }

  if (p != len)
    goto done;

  for (unsigned k = 0; k < count; k++) {
    unsigned found = 0;

    for (uint32_t i = 0; i < constants; i++) {
      if (constant_matches(
            buf + offsets[i],
            &patches[k])) {

        target[k] = i;
        found++;
      }
    }

    if (found != 1) {
      debugPrintf(
        "[script]   constant %u: %u matches, expected 1\n",
        k,
        found
      );

      goto done;
    }

    if (uses[target[k]] != patches[k].uses) {
      debugPrintf(
        "[script]   constant %u: %u uses, expected %u\n",
        k,
        uses[target[k]],
        patches[k].uses
      );

      goto done;
    }
  }

  for (unsigned k = 0; k < count; k++) {
    constant_rewrite(
      buf + offsets[target[k]],
      &patches[k]
    );
  }

  ok = 1;

done:
  free(offsets);
  free(uses);

  return ok;
}

static int identifier_matches(const uint8_t *p,
                              size_t chars,
                              const char *name)
{
  const size_t n = strlen(name);

  if (chars != n)
    return 0;

  for (size_t i = 0; i < n; i++) {
    uint8_t encoded[4];

    memcpy(
      encoded,
      p + i * 4,
      4
    );

    for (int b = 0; b < 4; b++)
      encoded[b] ^= 0xB6;

    const uint32_t ch =
      read_u32(encoded);

    if (ch !=
        (uint32_t)(unsigned char)name[i]) {

      return 0;
    }
  }

  return 1;
}

/*
 * Patches a BOOL literal assigned directly to the requested property.
 *
 * We intentionally match the token sequence:
 *
 *     IDENTIFIER(use_native_dialog)
 *     EQUAL
 *     LITERAL(bool constant)
 *
 * This avoids changing unrelated `true`/`false` literals that may exist
 * elsewhere in the same GDScript.
 */
static int patch_bool_property_token_buffer(
  uint8_t *buf,
  size_t len,
  const BoolPropertyPatch *patches,
  unsigned count) {

  if (len < 16 ||
      count > MAX_BOOL_PATCHES) {
    return 0;
  }

  if (count == 0)
    return 1;

  const uint32_t identifiers =
    read_u32(buf);

  const uint32_t constants =
    read_u32(buf + 4);

  const uint32_t lines =
    read_u32(buf + 8);

  const uint32_t tokens =
    read_u32(buf + 12);

  uint32_t property_ids[MAX_BOOL_PATCHES];

  for (unsigned i = 0; i < count; i++)
    property_ids[i] = UINT32_MAX;

  size_t p = 16;

  /*
   * Identifier table.
   */
  for (uint32_t i = 0; i < identifiers; i++) {
    if (len - p < 4)
      return 0;

    const size_t chars =
      read_u32(buf + p);

    if (chars > (len - p - 4) / 4)
      return 0;

    const uint8_t *text =
      buf + p + 4;

    for (unsigned k = 0; k < count; k++) {
      if (property_ids[k] == UINT32_MAX &&
          identifier_matches(
            text,
            chars,
            patches[k].identifier)) {

        property_ids[k] = i;
      }
    }

    p += 4 + chars * 4;
  }

  /*
   * Every requested property must exist.
   */
  for (unsigned k = 0; k < count; k++) {
    if (property_ids[k] == UINT32_MAX) {
      debugPrintf(
        "[script]   bool property \"%s\" not found\n",
        patches[k].identifier
      );

      return 0;
    }
  }

  /*
   * Constant offsets.
   */
  size_t *offsets =
    malloc(
      (constants ? constants : 1) *
      sizeof(*offsets)
    );

  if (!offsets)
    return 0;

  for (uint32_t i = 0; i < constants; i++) {
    const size_t size =
      constant_size(
        buf + p,
        len - p
      );

    if (!size) {
      free(offsets);
      return 0;
    }

    offsets[i] = p;
    p += size;
  }

  /*
   * Godot 4 .gdc line table is 16 bytes per line.
   */
  if (lines > (len - p) / 16) {
    free(offsets);
    return 0;
  }

  p += (size_t)lines * 16;

  int state[MAX_BOOL_PATCHES] = {0};
  unsigned found[MAX_BOOL_PATCHES] = {0};

  for (uint32_t i = 0; i < tokens; i++) {
    if (p >= len) {
      free(offsets);
      return 0;
    }

    const int wide =
      buf[p] & 0x80;

    const size_t token_len =
      wide ? 8 : 5;

    if (len - p < token_len) {
      free(offsets);
      return 0;
    }

    const uint32_t word =
      wide
        ? read_u32(buf + p)
        : buf[p];

    const uint32_t type =
      word & 0x7F;

    const uint32_t index =
      word >> 8;

    for (unsigned k = 0; k < count; k++) {

      /*
       * State 0:
       * Look for the property identifier.
       */
      if (state[k] == 0) {
        if (type == TOKEN_IDENTIFIER &&
            wide &&
            index == property_ids[k]) {

          state[k] = 1;
        }
      }

      /*
       * State 1:
       * Property identifier must be followed by '='.
       */
      else if (state[k] == 1) {
        if (type == TOKEN_EQUAL) {
          state[k] = 2;
        } else {
          state[k] = 0;
        }
      }

      /*
       * State 2:
       * '=' must be followed by a BOOL literal.
       */
      else if (state[k] == 2) {
        if (type != TOKEN_LITERAL ||
            !wide ||
            index >= constants) {

          state[k] = 0;
          continue;
        }

        uint8_t *c =
          buf + offsets[index];

        const uint32_t header =
          read_u32(c);

        if ((header & 0xFF) != VARIANT_BOOL) {
          free(offsets);
          return 0;
        }

        const uint32_t current =
          read_u32(c + 4);

        if ((int)current !=
            patches[k].expected_value) {

          debugPrintf(
            "[script]   bool property \"%s\": value=%u, expected=%d\n",
            patches[k].identifier,
            current,
            patches[k].expected_value
          );

          free(offsets);
          return 0;
        }

        write_u32(
          c + 4,
          (uint32_t)patches[k].new_value
        );

        found[k]++;
        state[k] = 0;
      }
    }

    p += token_len;
  }

  if (p != len) {
    free(offsets);
    return 0;
  }

  for (unsigned k = 0; k < count; k++) {
    if (found[k] != 1) {
      debugPrintf(
        "[script]   bool property \"%s\": %u matches, expected 1\n",
        patches[k].identifier,
        found[k]
      );

      free(offsets);
      return 0;
    }

    debugPrintf(
      "[script]   bool property \"%s\": %d -> %d\n",
      patches[k].identifier,
      patches[k].expected_value,
      patches[k].new_value
    );
  }

  free(offsets);
  return 1;
}

// Returns the patched script, exactly `file_len` bytes long (caller frees), or NULL.
static uint8_t *patch_script(
  const uint8_t *file,
  size_t file_len,
  const ScriptPatch *script) {

  if (file_len < GDSC_HEADER_SIZE ||
      memcmp(file, "GDSC", 4)) {
    return NULL;
  }

  const size_t stored =
    file_len - GDSC_HEADER_SIZE;

  const size_t raw_len =
    read_u32(file + 8);

  if (raw_len > MAX_TOKEN_BUFFER)
    return NULL;

  int ok = 0;

  uint8_t *check = NULL;

  uint8_t *out =
    malloc(file_len);

  uint8_t *raw =
    raw_len ? malloc(raw_len) : NULL;

  if (!out ||
      (raw_len && !raw)) {
    goto done;
  }

  memcpy(
    out,
    file,
    GDSC_HEADER_SIZE
  );

  /*
   * Uncompressed .gdc.
   */
  if (!raw_len) {

    memcpy(
      out + GDSC_HEADER_SIZE,
      file + GDSC_HEADER_SIZE,
      stored
    );

    ok =
      patch_token_buffer(
        out + GDSC_HEADER_SIZE,
        stored,
        script->patches,
        script->count
      );

    if (ok) {
      ok =
        patch_bool_property_token_buffer(
          out + GDSC_HEADER_SIZE,
          stored,
          script->bool_patches,
          script->bool_count
        );
    }

    goto done;
  }

  /*
   * Compressed .gdc.
   */
  if (ZSTD_decompress(
        raw,
        raw_len,
        file + GDSC_HEADER_SIZE,
        stored
      ) != raw_len) {
    goto done;
  }

  if (!patch_token_buffer(
        raw,
        raw_len,
        script->patches,
        script->count
      )) {
    goto done;
  }

  if (!patch_bool_property_token_buffer(
        raw,
        raw_len,
        script->bool_patches,
        script->bool_count
      )) {
    goto done;
  }

  static const int levels[] = {
    19,
    12,
    6
  };

  for (unsigned i = 0;
       i < sizeof(levels) / sizeof(*levels) && !ok;
       i++) {

    const size_t packed =
      ZSTD_compress(
        out + GDSC_HEADER_SIZE,
        stored,
        raw,
        raw_len,
        levels[i]
      );

    if (packed > stored)
      continue;

    const size_t gap =
      stored - packed;

    if (gap == 0) {

      ok = 1;

    } else if (gap >= ZSTD_SKIPPABLE_HEADER_SIZE) {

      uint8_t *pad =
        out + GDSC_HEADER_SIZE + packed;

      write_u32(
        pad,
        ZSTD_SKIPPABLE_MAGIC
      );

      write_u32(
        pad + 4,
        (uint32_t)(
          gap -
          ZSTD_SKIPPABLE_HEADER_SIZE
        )
      );

      memset(
        pad + ZSTD_SKIPPABLE_HEADER_SIZE,
        0,
        gap - ZSTD_SKIPPABLE_HEADER_SIZE
      );

      ok = 1;
    }
  }

  /*
   * A .gdc the engine can't decode crashes the game at boot:
   * only hand over a copy that the engine's own zstd decodes
   * back to exactly the patched tokens.
   */
  if (ok) {

    check =
      malloc(raw_len);

    ok =
      check &&
      ZSTD_decompress(
        check,
        raw_len,
        out + GDSC_HEADER_SIZE,
        stored
      ) == raw_len &&
      !memcmp(
        check,
        raw,
        raw_len
      );
  }

done:
  free(check);
  free(raw);

  if (!ok) {
    free(out);
    return NULL;
  }

  return out;
}

static uint8_t *read_file(
  const char *path,
  size_t *len) {

  FILE *f =
    fopen(path, "rb");

  if (!f)
    return NULL;

  uint8_t *data = NULL;

  long size = -1;

  if (fseek(f, 0, SEEK_END) == 0)
    size = ftell(f);

  if (size > 0 &&
      fseek(f, 0, SEEK_SET) == 0) {

    data =
      malloc((size_t)size);

    if (data &&
        fread(
          data,
          1,
          (size_t)size,
          f
        ) != (size_t)size) {

      free(data);
      data = NULL;
    }
  }

  fclose(f);

  if (data)
    *len = (size_t)size;

  return data;
}

void script_patches_apply(void) {

  for (unsigned i = 0;
       i < sizeof(k_scripts) / sizeof(*k_scripts);
       i++) {

    const ScriptPatch *script =
      &k_scripts[i];

    /*
     * touch_controls=1 means leave the original Android touch scripts alone.
     * Remove old generated overrides so changing the config takes effect.
     */
    if (config.touch_controls) {

      char stale[512];

      snprintf(
        stale,
        sizeof(stale),
        "%s/_ovr/%s",
        config.save_root,
        script->name
      );

      remove(stale);

      debugPrintf(
        "[script] %s left original (touch_controls=1)\n",
        script->asset
      );

      continue;
    }

    char path[512];

    snprintf(
      path,
      sizeof(path),
      "%s/assets/%s",
      config.data_root,
      script->asset
    );

    size_t file_len = 0;

    uint8_t *file =
      read_file(
        path,
        &file_len
      );

    if (!file &&
        asset_pack_active()) {

      void *packed = NULL;

      if (asset_pack_read_all_path(
            path,
            &packed,
            &file_len
          )) {

        file = packed;
      }
    }

    uint8_t *out =
      file
        ? patch_script(
            file,
            file_len,
            script
          )
        : NULL;

    if (out &&
        script_override_write(
          script->name,
          out,
          file_len
        )) {

      debugPrintf(
        "[script] %s patched\n",
        script->asset
      );

    } else {

      debugPrintf(
        "[script] %s left unpatched (%s)\n",
        script->asset,
        !file
          ? "not found"
          : !out
            ? "unexpected contents"
            : "write failed"
      );
    }

    free(out);
    free(file);
  }
}