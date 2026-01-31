#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define GNALK_TYPE_PREFERENCES_DIALOG (gnalk_preferences_dialog_get_type ())
G_DECLARE_FINAL_TYPE (GnalkPreferencesDialog, gnalk_preferences_dialog, GNALK, PREFERENCES_DIALOG, AdwDialog)

GnalkPreferencesDialog *gnalk_preferences_dialog_new (void);

G_END_DECLS
