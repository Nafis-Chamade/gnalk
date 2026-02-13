#include <glib/gi18n.h>
#include <locale.h>

#include "gnalk-config.h"
#include "gnalk-application.h"
#include "gnalk-log.h"

int
main (int argc, char *argv[])
{
  g_autoptr(GnalkApplication) app = NULL;

  /* Override locale if user chose a specific language in preferences */
  GSettings *settings = g_settings_new ("net.skilpa.gnalk");
  g_autofree char *lang = g_settings_get_string (settings, "preferred-language");
  if (lang && *lang)
    g_setenv ("LANGUAGE", lang, TRUE);
  g_object_unref (settings);

  setlocale (LC_ALL, "");

  /* Use GNALK_LOCALE_DIR if set (dev builds via run.sh), else compiled LOCALEDIR */
  const char *locale_dir = g_getenv ("GNALK_LOCALE_DIR");
  if (!locale_dir)
    locale_dir = LOCALEDIR;
  bindtextdomain (GETTEXT_PACKAGE, locale_dir);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  gnalk_log_init ();

  app = gnalk_application_new ();

  int status = g_application_run (G_APPLICATION (app), argc, argv);

  gnalk_log_shutdown ();

  return status;
}
