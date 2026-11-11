#ifndef FBL_TABLE_H
#define FBL_TABLE_H

// Tables, in a frame's slots: making one, reading and writing it as a script does, through its
// metatable, or raw, as rawget and rawset do; its length, its entries, its metatable, and freezing
// it. Each takes the table's slot, which has to hold a table.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Adds a new table to the frame, with room for `array` items from 1 and `hash` others. A status.
FBL_API int32_t fbl_table_new(uint64_t state, int32_t frame, int32_t array, int32_t hash);

// Adds the value of `key_slot`'s key in the table to the frame, as `table[key]` reads it, through
// its metatable's __index. A status.
FBL_API int32_t fbl_table_get(uint64_t state, int32_t frame, int32_t table, int32_t key_slot);

// Sets the table's `key_slot` key to `value_slot`'s value, as `table[key] = value` does, through
// __newindex. A status: a nil or NaN key, or a frozen table, is an error, as in a script.
FBL_API int32_t fbl_table_set(uint64_t state, int32_t frame, int32_t table, int32_t key_slot,
                              int32_t value_slot);

// As fbl_table_get and fbl_table_set, without the metatable.
FBL_API int32_t fbl_table_raw_get(uint64_t state, int32_t frame, int32_t table, int32_t key_slot);
FBL_API int32_t fbl_table_raw_set(uint64_t state, int32_t frame, int32_t table, int32_t key_slot,
                                  int32_t value_slot);

// Adds the table's item `index` to the frame, raw, as rawget(table, index) does. A status.
FBL_API int32_t fbl_table_raw_get_index(uint64_t state, int32_t frame, int32_t table,
                                        int32_t index);

// The table's length, raw, as rawlen gives it: its border from 1. -1 for a slot that isn't a table.
FBL_API int32_t fbl_table_length(uint64_t state, int32_t frame, int32_t table);

// Goes over the table's entries, raw, in no order, from `iterator` 0: adds the next entry's key
// and value to the frame and gives the iterator to the one after, or gives -1, adding nothing,
// past the last. The table mustn't change while it's gone over. -2 for a mistake.
FBL_API int32_t fbl_table_next(uint64_t state, int32_t frame, int32_t table, int32_t iterator);

// Adds the table's metatable to the frame, or nil when it has none. A status.
FBL_API int32_t fbl_table_get_metatable(uint64_t state, int32_t frame, int32_t table);

// Sets the table's metatable to `metatable_slot`'s table, or to none for nil. A status: a frozen
// table's is an error.
FBL_API int32_t fbl_table_set_metatable(uint64_t state, int32_t frame, int32_t table,
                                        int32_t metatable_slot);

// Freezes the table, as table.freeze does: from then on nothing changes it. A status.
FBL_API int32_t fbl_table_freeze(uint64_t state, int32_t frame, int32_t table);

// Whether the table is frozen: 1 or 0, or -1 for a slot that isn't a table.
FBL_API int32_t fbl_table_is_frozen(uint64_t state, int32_t frame, int32_t table);

#ifdef __cplusplus
}
#endif

#endif
