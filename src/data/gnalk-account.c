#include "gnalk-account.h"

struct _GnalkAccount
{
  GObject parent_instance;

  gint64  id;
  char   *server_url;
  char   *user_id;
  char   *display_name;
  char   *avatar_url;
  gboolean enabled;
  gboolean autologin;
  char   *goa_account_id;
};

G_DEFINE_FINAL_TYPE (GnalkAccount, gnalk_account, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_ID,
  PROP_SERVER_URL,
  PROP_USER_ID,
  PROP_DISPLAY_NAME,
  PROP_AVATAR_URL,
  PROP_ENABLED,
  PROP_AUTOLOGIN,
  PROP_GOA_ACCOUNT_ID,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
gnalk_account_finalize (GObject *object)
{
  GnalkAccount *self = GNALK_ACCOUNT (object);

  g_free (self->server_url);
  g_free (self->user_id);
  g_free (self->display_name);
  g_free (self->avatar_url);
  g_free (self->goa_account_id);

  G_OBJECT_CLASS (gnalk_account_parent_class)->finalize (object);
}

static void
gnalk_account_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  GnalkAccount *self = GNALK_ACCOUNT (object);

  switch (prop_id)
    {
    case PROP_ID:
      g_value_set_int64 (value, self->id);
      break;
    case PROP_SERVER_URL:
      g_value_set_string (value, self->server_url);
      break;
    case PROP_USER_ID:
      g_value_set_string (value, self->user_id);
      break;
    case PROP_DISPLAY_NAME:
      g_value_set_string (value, self->display_name);
      break;
    case PROP_AVATAR_URL:
      g_value_set_string (value, self->avatar_url);
      break;
    case PROP_ENABLED:
      g_value_set_boolean (value, self->enabled);
      break;
    case PROP_AUTOLOGIN:
      g_value_set_boolean (value, self->autologin);
      break;
    case PROP_GOA_ACCOUNT_ID:
      g_value_set_string (value, self->goa_account_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gnalk_account_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  GnalkAccount *self = GNALK_ACCOUNT (object);

  switch (prop_id)
    {
    case PROP_ID:
      self->id = g_value_get_int64 (value);
      break;
    case PROP_SERVER_URL:
      g_free (self->server_url);
      self->server_url = g_value_dup_string (value);
      break;
    case PROP_USER_ID:
      g_free (self->user_id);
      self->user_id = g_value_dup_string (value);
      break;
    case PROP_DISPLAY_NAME:
      g_free (self->display_name);
      self->display_name = g_value_dup_string (value);
      break;
    case PROP_AVATAR_URL:
      g_free (self->avatar_url);
      self->avatar_url = g_value_dup_string (value);
      break;
    case PROP_ENABLED:
      self->enabled = g_value_get_boolean (value);
      break;
    case PROP_AUTOLOGIN:
      self->autologin = g_value_get_boolean (value);
      break;
    case PROP_GOA_ACCOUNT_ID:
      g_free (self->goa_account_id);
      self->goa_account_id = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gnalk_account_class_init (GnalkAccountClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnalk_account_finalize;
  object_class->get_property = gnalk_account_get_property;
  object_class->set_property = gnalk_account_set_property;

  properties[PROP_ID] =
    g_param_spec_int64 ("id", NULL, NULL,
                        G_MININT64, G_MAXINT64, 0,
                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  properties[PROP_SERVER_URL] =
    g_param_spec_string ("server-url", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  properties[PROP_USER_ID] =
    g_param_spec_string ("user-id", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  properties[PROP_DISPLAY_NAME] =
    g_param_spec_string ("display-name", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_AVATAR_URL] =
    g_param_spec_string ("avatar-url", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_ENABLED] =
    g_param_spec_boolean ("enabled", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_AUTOLOGIN] =
    g_param_spec_boolean ("autologin", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_GOA_ACCOUNT_ID] =
    g_param_spec_string ("goa-account-id", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
gnalk_account_init (GnalkAccount *self)
{
  self->enabled = TRUE;
  self->autologin = FALSE;
}

GnalkAccount *
gnalk_account_new (gint64      id,
                     const char *server_url,
                     const char *user_id)
{
  return g_object_new (GNALK_TYPE_ACCOUNT,
                       "id", id,
                       "server-url", server_url,
                       "user-id", user_id,
                       NULL);
}

GnalkAccount *
gnalk_account_new_full (gint64      id,
                           const char *server_url,
                           const char *user_id,
                           const char *display_name,
                           const char *avatar_url,
                           gboolean    enabled,
                           gboolean    autologin,
                           const char *goa_account_id)
{
  return g_object_new (GNALK_TYPE_ACCOUNT,
                       "id", id,
                       "server-url", server_url,
                       "user-id", user_id,
                       "display-name", display_name,
                       "avatar-url", avatar_url,
                       "enabled", enabled,
                       "autologin", autologin,
                       "goa-account-id", goa_account_id,
                       NULL);
}

gint64
gnalk_account_get_id (GnalkAccount *self)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT (self), 0);
  return self->id;
}

const char *
gnalk_account_get_server_url (GnalkAccount *self)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT (self), NULL);
  return self->server_url;
}

const char *
gnalk_account_get_user_id (GnalkAccount *self)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT (self), NULL);
  return self->user_id;
}

const char *
gnalk_account_get_display_name (GnalkAccount *self)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT (self), NULL);
  return self->display_name;
}

const char *
gnalk_account_get_avatar_url (GnalkAccount *self)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT (self), NULL);
  return self->avatar_url;
}

gboolean
gnalk_account_get_enabled (GnalkAccount *self)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT (self), FALSE);
  return self->enabled;
}

gboolean
gnalk_account_get_autologin (GnalkAccount *self)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT (self), FALSE);
  return self->autologin;
}

void
gnalk_account_set_display_name (GnalkAccount *self,
                                  const char    *display_name)
{
  g_return_if_fail (GNALK_IS_ACCOUNT (self));

  if (g_strcmp0 (self->display_name, display_name) == 0)
    return;

  g_free (self->display_name);
  self->display_name = g_strdup (display_name);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_DISPLAY_NAME]);
}

void
gnalk_account_set_avatar_url (GnalkAccount *self,
                                const char    *avatar_url)
{
  g_return_if_fail (GNALK_IS_ACCOUNT (self));

  if (g_strcmp0 (self->avatar_url, avatar_url) == 0)
    return;

  g_free (self->avatar_url);
  self->avatar_url = g_strdup (avatar_url);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_AVATAR_URL]);
}

void
gnalk_account_set_enabled (GnalkAccount *self,
                             gboolean       enabled)
{
  g_return_if_fail (GNALK_IS_ACCOUNT (self));

  if (self->enabled == enabled)
    return;

  self->enabled = enabled;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ENABLED]);
}

void
gnalk_account_set_autologin (GnalkAccount *self,
                               gboolean       autologin)
{
  g_return_if_fail (GNALK_IS_ACCOUNT (self));

  if (self->autologin == autologin)
    return;

  self->autologin = autologin;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_AUTOLOGIN]);
}

char *
gnalk_account_get_unique_key (GnalkAccount *self)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT (self), NULL);
  return g_strdup_printf ("%s@%s", self->user_id, self->server_url);
}

gboolean
gnalk_account_matches (GnalkAccount *self,
                         const char    *server_url,
                         const char    *user_id)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT (self), FALSE);

  return g_strcmp0 (self->server_url, server_url) == 0 &&
         g_strcmp0 (self->user_id, user_id) == 0;
}

const char *
gnalk_account_get_goa_account_id (GnalkAccount *self)
{
  g_return_val_if_fail (GNALK_IS_ACCOUNT (self), NULL);
  return self->goa_account_id;
}

void
gnalk_account_set_goa_account_id (GnalkAccount *self,
                                    const char    *goa_account_id)
{
  g_return_if_fail (GNALK_IS_ACCOUNT (self));

  if (g_strcmp0 (self->goa_account_id, goa_account_id) == 0)
    return;

  g_free (self->goa_account_id);
  self->goa_account_id = g_strdup (goa_account_id);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_GOA_ACCOUNT_ID]);
}
