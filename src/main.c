#include <glib/gi18n.h>
#include <locale.h>

#include "gnalk-config.h"
#include "gnalk-application.h"
#include "gnalk-log.h"

int
main (int argc, char *argv[])
{
  g_autoptr(GnalkApplication) app = NULL;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  gnalk_log_init ();

  app = gnalk_application_new ();

  int status = g_application_run (G_APPLICATION (app), argc, argv);

  gnalk_log_shutdown ();

  return status;
}
