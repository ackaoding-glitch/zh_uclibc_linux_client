#ifndef ZH_CORE_JSON_COMPAT_H
#define ZH_CORE_JSON_COMPAT_H

#include "bithion_core.h"

#define mg_str_n(buf, len) bithion_core_json_view_make((buf), (len))
#define mg_json_get_str(json, path) bithion_core_json_get_str((json), (path))
#define mg_json_get_long(json, path, dflt) bithion_core_json_get_long((json), (path), (dflt))
#define mg_json_get_num(json, path, out_num) bithion_core_json_get_num((json), (path), (out_num))
#define mg_json_get_tok(json, path) bithion_core_json_get_tok((json), (path))
#define mg_json_next(json, ofs, _key, out_val) bithion_core_json_next((json), (ofs), (out_val))

#endif
