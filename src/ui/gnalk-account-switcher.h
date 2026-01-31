#pragma once

#include <gtk/gtk.h>
#include "gnalk-session-manager.h"

G_BEGIN_DECLS

#define GNALK_TYPE_ACCOUNT_SWITCHER (gnalk_account_switcher_get_type ())
G_DECLARE_FINAL_TYPE (GnalkAccountSwitcher, gnalk_account_switcher, GNALK, ACCOUNT_SWITCHER, GtkWidget)

/**
 * GnalkAccountSwitcher:
 *
 * A widget for switching between multiple logged-in accounts.
 * Shows the current account and allows switching to other sessions.
 *
 * Signals:
 * - "add-account-requested" ()
 *     Emitted when user wants to add a new account
 * - "sign-out-requested" (gint64 account_id)
 *     Emitted when user wants to sign out of an account
 */

GnalkAccountSwitcher *gnalk_account_switcher_new (void);

void gnalk_account_switcher_set_session_manager (GnalkAccountSwitcher *self,
                                                    GnalkSessionManager  *manager);

G_END_DECLS
