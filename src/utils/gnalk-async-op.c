/* gnalk-async-op.c
 *
 * Safe async callback data: GWeakRef to owner + generation counter snapshot.
 */

#include "gnalk-async-op.h"

GnalkAsyncOp *
gnalk_async_op_new (GObject      *owner,
                    guint        *gen_counter,
                    const char   *ctx_data,
                    gint64        ctx_id)
{
  GnalkAsyncOp *op = g_new0 (GnalkAsyncOp, 1);

  g_weak_ref_init (&op->owner_ref, owner);
  op->generation_ptr = gen_counter;
  op->generation = gen_counter ? *gen_counter : 0;
  op->context_data = g_strdup (ctx_data);
  op->context_id = ctx_id;

  return op;
}

gboolean
gnalk_async_op_is_valid (GnalkAsyncOp *op)
{
  GObject *owner = g_weak_ref_get (&op->owner_ref);

  if (!owner)
    return FALSE;

  gboolean valid = (op->generation_ptr == NULL ||
                    *op->generation_ptr == op->generation);

  g_object_unref (owner);
  return valid;
}

GObject *
gnalk_async_op_get_owner (GnalkAsyncOp *op)
{
  return g_weak_ref_get (&op->owner_ref);
}

void
gnalk_async_op_free (GnalkAsyncOp *op)
{
  if (!op)
    return;

  g_weak_ref_clear (&op->owner_ref);
  g_free (op->context_data);
  g_free (op);
}
