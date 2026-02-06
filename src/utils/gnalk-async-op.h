/* gnalk-async-op.h
 *
 * Safe async callback data: GWeakRef to owner + generation counter snapshot.
 * Prevents use-after-free when owner is destroyed before callback fires,
 * and discards stale responses via generation counter comparison.
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct {
  GWeakRef  owner_ref;       /* Weak reference to owner GObject */
  guint     generation;      /* Snapshot of *generation_ptr at creation */
  guint    *generation_ptr;  /* Points to owner's counter (NULL = skip check) */
  char     *context_data;    /* Optional context string (e.g., reference_id, emoji) */
  gint64    context_id;      /* Optional context integer (e.g., message_id) */
} GnalkAsyncOp;

/**
 * gnalk_async_op_new:
 * @owner: The GObject that owns this operation (weak-referenced)
 * @gen_counter: (nullable): Pointer to owner's generation counter. If non-NULL,
 *   the current value is snapshotted for later comparison. Pass NULL if only
 *   the weak-ref check is needed (no stale response detection).
 * @ctx_data: (nullable): Optional context string (copied)
 * @ctx_id: Optional context integer
 *
 * Creates a new async operation handle. Snapshots the current value
 * of *@gen_counter (if non-NULL) for later comparison.
 *
 * Returns: A new GnalkAsyncOp. Free with gnalk_async_op_free().
 */
GnalkAsyncOp *gnalk_async_op_new       (GObject      *owner,
                                         guint        *gen_counter,
                                         const char   *ctx_data,
                                         gint64        ctx_id);

/**
 * gnalk_async_op_is_valid:
 * @op: The async operation
 *
 * Checks whether the operation is still valid:
 * - Owner must still be alive (GWeakRef resolves)
 * - If a generation counter was provided, it must still match
 *
 * Returns: TRUE if the owner is alive and generation matches.
 */
gboolean      gnalk_async_op_is_valid  (GnalkAsyncOp *op);

/**
 * gnalk_async_op_get_owner:
 * @op: The async operation
 *
 * Returns a strong reference to the owner, or NULL if it was destroyed.
 * Caller must g_object_unref() the returned object when done.
 *
 * Returns: (transfer full) (nullable): The owner GObject, or NULL.
 */
GObject      *gnalk_async_op_get_owner (GnalkAsyncOp *op);

/**
 * gnalk_async_op_free:
 * @op: The async operation
 *
 * Frees the operation: clears the weak ref, frees context_data and the struct.
 */
void          gnalk_async_op_free      (GnalkAsyncOp *op);

G_END_DECLS
