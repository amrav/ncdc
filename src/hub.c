/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011 Yoran Heling

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/


#include "ncdc.h"
#include <string.h>
#include <stdlib.h>


#if INTERFACE

struct hub_user {
  gboolean hasinfo : 1;
  gboolean isop : 1;
  gboolean isjoined : 1; // managed by ui_hub_userchange()
  gboolean active : 1;
  unsigned char h_norm;
  unsigned char h_reg;
  unsigned char h_op;
  unsigned char slots;
  unsigned int as;         // auto-open slot if upload is below n bytes/s
  char *name;     // UTF-8
  char *name_hub; // hub-encoded (NMDC)
  char *desc;
  char *conn;
  char *mail;
  char *client;
  int sid;        // for ADC
  char cid[24];   // for ADC
  guint64 sharesize;
  GSequenceIter *iter; // used by ui_userlist_*
}


struct hub {
  gboolean adc; // TRUE = ADC, FALSE = NMDC protocol.
  int state;    // ADC_S_* (ADC only)
  struct ui_tab *tab; // to get name (for config) and for logging & setting of title
  struct net *net;
  // nick as used in this connection, NULL when not sent yet
  char *nick_hub; // in hub encoding (NMDC only)
  char *nick;     // UTF-8
  int sid;        // session ID (ADC only)
  // TRUE is the above nick has also been validated (and we're properly logged in)
  gboolean nick_valid;
  gboolean isreg; // whether we used a password to login
  gboolean isop;  // whether we're an OP or not
  char *hubname;  // UTF-8, or NULL when unknown
  char *hubname_hub; // in hub encoding (NMDC only)
  // user list, key = username (in hub encoding for NMDC), value = struct hub_user *
  GHashTable *users;
  GHashTable *sessions; // user list, with sid as index (ADC only)
  // list of users who have been granted a slot. key = username (in hub
  // encoding), value = (void *)1. A user will stay in this table for as long
  // as the hub tab is open, I guess that's good enough.
  GHashTable *grants;
  int sharecount;
  guint64 sharesize;
  // what we and the hub support
  gboolean supports_nogetinfo;
  // MyINFO send timer (event loop source id)
  guint nfo_timer;
  // reconnect timer (30 sec.)
  guint reconnect_timer;
  // last info we sent to the hub
  char *nfo_desc, *nfo_conn, *nfo_mail;
  unsigned char nfo_slots, nfo_h_norm, nfo_h_reg, nfo_h_op;
  guint64 nfo_share;
  unsigned short nfo_active;
  // whether we've fetched the complete user list (and their $MyINFO's)
  gboolean received_first; // true if one precondition for joincomplete is satisfied.
  gboolean joincomplete;
};

#endif




// struct hub_user related functions

static struct hub_user *user_add(struct hub *hub, const char *name) {
  struct hub_user *u = g_hash_table_lookup(hub->users, name);
  if(u)
    return u;
  u = g_slice_new0(struct hub_user);
  if(hub->adc)
    u->name = g_strdup(name);
  else {
    u->name_hub = g_strdup(name);
    u->name = charset_convert(hub, TRUE, name);
  }
  g_hash_table_insert(hub->users, hub->adc ? u->name : u->name_hub, u);
  ui_hub_userchange(hub->tab, UIHUB_UC_JOIN, u);
  return u;
}


static void user_free(gpointer dat) {
  struct hub_user *u = dat;
  g_free(u->name_hub);
  g_free(u->name);
  g_free(u->desc);
  g_free(u->conn);
  g_free(u->mail);
  g_free(u->client);
  g_slice_free(struct hub_user, u);
}


// Get a user by a UTF-8 string. May fail for NMDC if the UTF-8 -> hub encoding
// is not really one-to-one.
struct hub_user *hub_user_get(struct hub *hub, const char *name) {
  if(hub->adc)
    return g_hash_table_lookup(hub->users, name);
  char *name_hub = charset_convert(hub, FALSE, name);
  struct hub_user *u = g_hash_table_lookup(hub->users, name_hub);
  g_free(name_hub);
  return u;
}


// Auto-complete suggestions for hub_user_get()
void hub_user_suggest(struct hub *hub, char *str, char **sug) {
  GHashTableIter iter;
  struct hub_user *u;
  int i=0, len = strlen(str);
  g_hash_table_iter_init(&iter, hub->users);
  while(i<20 && g_hash_table_iter_next(&iter, NULL, (gpointer *)&u))
    if(g_ascii_strncasecmp(u->name, str, len) == 0 && strlen(u->name) != len)
      sug[i++] = g_strdup(u->name);
  qsort(sug, i, sizeof(char *), cmpstringp);
}


char *hub_user_tag(struct hub_user *u) {
  if(!u->client || !u->slots)
    return NULL;
  GString *t = g_string_new("");
  g_string_printf(t, "<%s,M:%c,H:%d/%d/%d,S:%d", u->client,
    u->active ? 'A' : 'P', u->h_norm, u->h_reg, u->h_op, u->slots);
  if(u->as)
    g_string_append_printf(t, ",O:%d", u->as/1024);
  g_string_append_c(t, '>');
  return g_string_free(t, FALSE);
}


#define cleanspace(str) do {\
    while(*(str) == ' ')\
      (str)++;\
    while((str)[0] && (str)[strlen(str)-1] == ' ')\
      (str)[strlen(str)-1] = 0;\
  } while(0)

static void user_nmdc_nfo(struct hub *hub, struct hub_user *u, char *str) {
  // these all point into *str. *str is modified to contain zeroes in the correct positions
  char *next, *tmp;
  char *desc = NULL;
  char *client = NULL;
  char *conn = NULL;
  char *mail = NULL;
  gboolean active = FALSE;
  unsigned char h_norm = 0;
  unsigned char h_reg = 0;
  unsigned char h_op = 0;
  unsigned char slots = 0;
  unsigned int as = 0;
  guint64 share = 0;

  if(!(next = index(str, '$')) || strlen(next) < 3 || next[2] != '$')
    return;
  *next = 0; next += 3;

  // tag
  if(str[0] && str[strlen(str)-1] == '>' && (tmp = rindex(str, '<'))) {
    *tmp = 0;
    tmp++;
    tmp[strlen(tmp)-1] = 0;
    // tmp now points to the contents of the tag
    char *t;

#define L(s) do {\
    if(!client)\
      client = tmp;\
    else if(strcmp(tmp, "M:A") == 0)\
      active = TRUE;\
    else\
      (void) (sscanf(tmp, "H:%hhu/%hhu/%hhu", &h_norm, &h_reg, &h_op)\
      || sscanf(tmp, "S:%hhu", &slots)\
      || sscanf(tmp, "O:%u", &as));\
  } while(0)

    while((t = index(tmp, ','))) {
      *t = 0;
      L(tmp);
      tmp = t+1;
    }
    L(tmp);
  }

  // description
  desc = str;
  cleanspace(desc);

  // connection and flag
  str = next;
  if(!(next = index(str, '$')))
    return;
  *next = 0; next++;

  // we currently ignore the flag
  str[strlen(str)-1] = 0;

  conn = str;
  cleanspace(conn);

  // email
  str = next;
  if(!(next = index(str, '$')))
    return;
  *next = 0; next++;

  mail = str;
  cleanspace(mail);

  // share
  str = next;
  if(!(next = index(str, '$')))
    return;
  *next = 0;
  share = g_ascii_strtoull(str, NULL, 10);

  // If we still haven't 'return'ed yet, that means we have a correct $MyINFO. Now we can update the struct.
  g_free(u->desc);
  g_free(u->client);
  g_free(u->conn);
  g_free(u->mail);
  u->sharesize = share;
  u->desc = desc[0] ? nmdc_unescape_and_decode(hub, desc) : NULL;
  u->client = client && client[0] ? g_strdup(client) : NULL;
  u->conn = conn[0] ? nmdc_unescape_and_decode(hub, conn) : NULL;
  u->mail = mail[0] ? nmdc_unescape_and_decode(hub, mail) : NULL;
  u->h_norm = h_norm;
  u->h_reg = h_reg;
  u->h_op = h_op;
  u->slots = slots;
  u->as = as*1024;
  u->hasinfo = TRUE;
  u->active = active;
  ui_hub_userchange(hub->tab, UIHUB_UC_NFO, u);
}

#undef cleanspace


#define P(a,b) (((a)<<8) + (b))

static void user_adc_nfo(struct hub *hub, struct hub_user *u, struct adc_cmd *cmd) {
  u->hasinfo = TRUE;
  // sid
  if(!u->sid)
    g_hash_table_insert(hub->sessions, GINT_TO_POINTER(cmd->source), u);
  u->sid = cmd->source;

  // This is faster than calling adc_getparam() each time
  char **n;
  for(n=cmd->argv; n&&*n; n++) {
    if(strlen(*n) < 2)
      continue;
    char *p = *n+2;
    switch(P(**n, (*n)[1])) {
    case P('N','I'): // nick
      g_hash_table_steal(hub->users, u->name);
      g_free(u->name);
      u->name = g_strdup(p);
      g_hash_table_insert(hub->users, u->name, u);
      break;
    case P('D','E'): // description
      g_free(u->desc);
      u->desc = p[0] ? g_strdup(p) : NULL;
      break;
    case P('V','E'): // client name + version
      g_free(u->client);
      u->client = p[0] ? g_strdup(p) : NULL;
      break;
    case P('E','M'): // mail
      g_free(u->mail);
      u->mail = p[0] ? g_strdup(p) : NULL;
      break;
    case P('I','D'): // CID
      // Note that the ADC spec allows hashes of varying length. I'm limiting
      // myself to 39 here since that is more memory-efficient.
      if(strlen(p) == 39)
        base32_decode(p, u->cid);
      break;
    case P('S','S'): // share size
      u->sharesize = g_ascii_strtoull(p, NULL, 10);
      break;
    case P('H','N'): // h_norm
      u->h_norm = strtol(p, NULL, 10);
      break;
    case P('H','R'): // h_reg
      u->h_reg = strtol(p, NULL, 10);
      break;
    case P('H','O'): // h_op
      u->h_op = strtol(p, NULL, 10);
      break;
    case P('S','L'): // slots
      u->slots = strtol(p, NULL, 10);
      break;
    case P('A','S'): // as
      u->slots = strtol(p, NULL, 10);
      break;
    case P('S','U'): // supports
      u->active = !!strstr(p, "TCP4") || !!strstr(p, "TCP6");
      break;
    case P('C','T'): // client type (only used to figure out u->isop)
      u->isop = strtol(p, NULL, 10) >= 4;
      break;
    }
  }

  ui_hub_userchange(hub->tab, UIHUB_UC_NFO, u);
}

#undef P





// hub stuff


void hub_password(struct hub *hub, char *pass) {
  g_return_if_fail(!hub->nick_valid);
  char *rpass = !pass ? g_key_file_get_string(conf_file, hub->tab->name, "password", NULL) : g_strdup(pass);
  if(!rpass)
    ui_m(hub->tab, UIP_HIGH,
      "\nPassword required. Type '/password <your password>' to log in without saving your password."
      "\nOr use '/set password <your password>' to log in and save your password in the config file (unencrypted!).\n");
  else {
    net_sendf(hub->net, "$MyPass %s", rpass); // Password is sent raw, not encoded. Don't think encoding really matters here.
    hub->isreg = TRUE;
  }
  g_free(rpass);
}


void hub_kick(struct hub *hub, struct hub_user *u) {
  g_return_if_fail(hub->nick_valid && u);
  net_sendf(hub->net, "$Kick %s", u->name_hub);
}


void hub_grant(struct hub *hub, struct hub_user *u) {
  if(!g_hash_table_lookup(hub->grants, u->name_hub))
    g_hash_table_insert(hub->grants, g_strdup(u->name_hub), (void *)1);
  // TODO: open a connection to the user?
}


#define streq(a) ((!a && !hub->nfo_##a) || (a && hub->nfo_##a && strcmp(a, hub->nfo_##a) == 0))
#define eq(a) (a == hub->nfo_##a)

void hub_send_nfo(struct hub *hub) {
  // get info, to be compared with hub->nfo_
  char *desc, *conn, *mail;
  unsigned char slots, h_norm, h_reg, h_op;
  guint64 share;
  unsigned short active;

  desc = conf_hub_get(string, hub->tab->name, "description");
  conn = conf_hub_get(string, hub->tab->name, "connection");
  mail = conf_hub_get(string, hub->tab->name, "email");

  h_norm = h_reg = h_op = 0;
  GList *n;
  for(n=ui_tabs; n; n=n->next) {
    struct ui_tab *t = n->data;
    if(t->type != UIT_HUB)
      continue;
    if(t->hub->isop)
      h_op++;
    else if(t->hub->isreg)
      h_reg++;
    else if(t->hub->nick_valid)
      h_norm++;
  }
  if(!hub->nick_valid)
    h_norm++;
  slots = conf_slots();
  active = cc_listen ? cc_listen_port : 0;
  share = fl_local_list_size;

  // check whether we need to make any further effort
  if(hub->nick_valid && streq(desc) && streq(conn) && streq(mail)
      && eq(slots) && eq(h_norm) && eq(h_reg) && eq(h_op) && eq(share) && eq(active)) {
    g_free(desc);
    g_free(conn);
    g_free(mail);
    return;
  }

  char *nfo;
  // ADC
  if(hub->adc) { // TODO: US,DS,SS,SF, SU=TCP4 on active
    GString *cmd = adc_generate('B', ADCC_INF, hub->sid, 0);
    // send non-changing stuff in the IDENTIFY state
    gboolean f = hub->state == ADC_S_IDENTIFY;
    if(f) {
      char *cid = g_key_file_get_string(conf_file, "global", "cid", NULL);
      char *pid = g_key_file_get_string(conf_file, "global", "pid", NULL);
      g_string_append_printf(cmd, " ID%s PD%s I40.0.0.0 VEncdc\\s%s", cid, pid, VERSION);
      g_free(cid);
      g_free(pid);
      adc_append(cmd, "NI", hub->nick);
    }
    if(f || !eq(slots))
      g_string_append_printf(cmd, " SL%d", slots);
    if(f || !eq(h_norm))
      g_string_append_printf(cmd, " HN%d", h_norm);
    if(f || !eq(h_reg))
      g_string_append_printf(cmd, " HR%d", h_reg);
    if(f || !eq(h_op))
      g_string_append_printf(cmd, " HO%d", h_op);
    if(f || !streq(desc))
      adc_append(cmd, "DE", desc?desc:"");
    if(f || !streq(mail))
      adc_append(cmd, "EM", mail?mail:"");
    nfo = g_string_free(cmd, FALSE);

  // NMDC
  } else {
    char *ndesc = nmdc_encode_and_escape(hub, desc?desc:"");
    char *nconn = nmdc_encode_and_escape(hub, conn?conn:"");
    char *nmail = nmdc_encode_and_escape(hub, mail?mail:"");
    nfo = g_strdup_printf("$MyINFO $ALL %s %s<ncdc V:%s,M:%c,H:%d/%d/%d,S:%d>$ $%s\01$%s$%"G_GUINT64_FORMAT"$",
      hub->nick_hub, ndesc, VERSION, active ? 'A' : 'P', h_norm, h_reg, h_op,
      slots, nconn, nmail, share);
    g_free(ndesc);
    g_free(nconn);
    g_free(nmail);
  }

  // send
  net_send(hub->net, nfo);
  g_free(nfo);

  // update
  g_free(hub->nfo_desc); hub->nfo_desc = desc;
  g_free(hub->nfo_conn); hub->nfo_conn = conn;
  g_free(hub->nfo_mail); hub->nfo_mail = mail;
  hub->nfo_slots = slots;
  hub->nfo_h_norm = h_norm;
  hub->nfo_h_reg = h_reg;
  hub->nfo_h_op = h_op;
  hub->nfo_share = share;
  hub->nfo_active = active;
}

#undef eq
#undef streq


void hub_say(struct hub *hub, const char *str) {
  if(!hub->nick_valid)
    return;
  char *msg = nmdc_encode_and_escape(hub, str);
  net_sendf(hub->net, "<%s> %s", hub->nick_hub, msg);
  g_free(msg);
}


void hub_msg(struct hub *hub, struct hub_user *user, const char *str) {
  char *msg = nmdc_encode_and_escape(hub, str);
  net_sendf(hub->net, "$To: %s From: %s $<%s> %s", user->name_hub, hub->nick_hub, hub->nick_hub, msg);
  g_free(msg);
  // emulate protocol echo
  msg = g_strdup_printf("<%s> %s", hub->nick, str);
  ui_hub_msg(hub->tab, user, msg);
  g_free(msg);
}


static void adc_handle(struct hub *hub, char *msg) {
  struct adc_cmd cmd;
  GError *err = NULL;

  if(!msg[0])
    return;

  adc_parse(msg, &cmd, &err);
  if(err) {
    g_warning("ADC parse error from %s: %s. --> %s", net_remoteaddr(hub->net), err->message, msg);
    g_error_free(err);
    return;
  }

  switch(cmd.cmd) {
  case ADCC_SID:
    if(hub->state != ADC_S_PROTOCOL || cmd.type != 'I' || cmd.argc != 1 || strlen(cmd.argv[0]) != 4)
      g_warning("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    else {
      hub->sid = ADC_DFCC(cmd.argv[0]);
      hub->state = ADC_S_IDENTIFY;
      hub->nick = conf_hub_get(string, hub->tab->name, "nick");
      hub_send_nfo(hub);
    }
    break;

  case ADCC_SUP:
    // TODO: do something with it.
    // For C-C connections, this enables the IDENTIFY state, but for hubs it's the SID command that does this.
    break;

  case ADCC_INF:
    // inf from hub
    if(cmd.type == 'I') {
      // Get hub name. Some hubs (PyAdc) send multiple 'NI's, ignore the first one in that case. :-/
      char **left = NULL;
      char *hname = adc_getparam(cmd.argv, "NI", &left);
      if(left)
        hname = adc_getparam(left, "NI", NULL);
      if(hname) {
        g_free(hub->hubname);
        hub->hubname = g_strdup(hname);
      }
      // set state
      if(hub->state == ADC_S_IDENTIFY || hub->state == ADC_S_VERIFY) {
        hub->state = ADC_S_NORMAL;
        hub->nick_valid = TRUE;
      }
    } else if(cmd.type == 'B') {
      char *nick = adc_getparam(cmd.argv, "NI", NULL);
      struct hub_user *u = g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(cmd.source));
      if(!u && nick)
        u = user_add(hub, nick);
      if(!u)
        g_warning("INF for user who is not on the hub (%s): %s", net_remoteaddr(hub->net), msg);
      else {
        if(!u->hasinfo)
          hub->sharecount++;
        else
          hub->sharesize -= u->sharesize;
        user_adc_nfo(hub, u, &cmd);
        hub->sharesize += u->sharesize;
        // if we received our own INF, that means the user list is complete.
        if(u->sid == hub->sid) {
          hub->joincomplete = hub->received_first;
          hub->received_first = TRUE;
        }
      }
    }
    break;

  case ADCC_QUI:
    if(cmd.type != 'I' || cmd.argc < 1 || strlen(cmd.argv[0]) != 4)
      g_warning("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    else {
      int sid = ADC_DFCC(cmd.argv[0]);
      struct hub_user *u = g_hash_table_lookup(hub->sessions, GINT_TO_POINTER(sid));
      if(sid == hub->sid) // TODO: properly handle TL, RD and do something with MS
        hub_disconnect(hub, TRUE);
      else if(u) { // TODO: handle DI, and perhaps do something with MS
        ui_hub_userchange(hub->tab, UIHUB_UC_QUIT, u);
        hub->sharecount--;
        hub->sharesize -= u->sharesize;
        g_hash_table_remove(hub->users, u->name);
      } else
        g_warning("QUI for user who is not on the hub (%s): %s", net_remoteaddr(hub->net), msg);
    }
    break;

  case ADCC_STA:
    if(cmd.argc < 2 || strlen(cmd.argv[0]) != 3)
      g_warning("Invalid message from %s: %s", net_remoteaddr(hub->net), msg);
    else {
      int code = (cmd.argv[0][1]-'0')*10 + (cmd.argv[0][2]-'0');
      if(!code)
        ui_m(hub->tab, 0, cmd.argv[1]);
      if(cmd.argv[0][1] == '1')
        g_message("ADC Error (recoverable): %d %s", code, cmd.argv[1]);
      if(cmd.argv[0][1] == '2') {
        g_warning("ADC Error (fatal): %d %s", code, cmd.argv[1]);
        hub_disconnect(hub, FALSE); // TODO: whether we should reconnect or not depends on the error code
      }
    }
    break;

  default:
    g_message("Unknown command from %s: %s", net_remoteaddr(hub->net), msg);
  }

  g_strfreev(cmd.argv);
}


static void nmdc_search(struct hub *hub, char *from, int size_m, guint64 size, int type, char *query) {
  static char *exts[][10] = { { },
    { "mp3", "mp2", "wav", "au", "rm", "mid", "sm" },
    { "zip", "arj", "rar", "lzh", "gz", "z", "arc", "pak" },
    { "doc", "txt", "wri", "pdf", "ps", "tex" },
    { "pm", "exe", "bat", "com" },
    { "gif", "jpg", "jpeg", "bmp", "pcx", "png", "wmf", "psd" },
    { "mpg", "mpeg", "avi", "asf", "mov" },
    { }, { }, { }
  };
  int max = from[0] == 'H' ? 5 : 10;
  struct fl_list *res[max];
  int filedir = type == 1 ? 3 : type == 8 ? 2 : 1;
  char **ext = exts[type-1];
  char **inc = NULL;
  int i = 0;

  // TTH lookup (YAY! this is fast!)
  if(type == 9) {
    if(strncmp(query, "TTH:", 4) != 0 || strlen(query) != 4+39) {
      g_warning("Invalid TTH $Search for %s", from);
      return;
    }
    char root[24];
    base32_decode(query+4, root);
    GSList *l = fl_local_from_tth(root);
    // it still has to match the other requirements...
    for(; i<max && l; l=l->next) {
      struct fl_list *c = l->data;
      if(fl_list_search_matches(c, size_m, size, filedir, ext, inc))
        res[i++] = c;
    }

  // Advanced lookup (Noo! This is slooow!)
  } else {
    char *tmp = query;
    for(; *tmp; tmp++)
      if(*tmp == '$')
        *tmp = ' ';
    tmp = nmdc_unescape_and_decode(hub, query);
    inc = g_strsplit(tmp, " ", 0);
    g_free(tmp);
    i = fl_list_search(fl_local_list, size_m, size, filedir, ext, inc, res, max);
    g_strfreev(inc);
  }

  // reply
  if(!i)
    return;

  char *hubaddr = net_remoteaddr(hub->net);
  int slots = conf_slots();
  int slots_free = slots - cc_slots_in_use(NULL);
  if(slots_free < 0)
    slots_free = 0;
  char tth[44] = "TTH:";
  tth[43] = 0;

  for(i--; i>=0; i--) {
    char *fl = fl_list_path(res[i]);
    // Windows style path delimiters... why!?
    char *tmp = fl;
    char *size = NULL;
    for(; *tmp; tmp++)
      if(*tmp == '/')
        *tmp = '\\';
    // TODO: In what encoding should the path really be? UTF-8 or hub?
    tmp = nmdc_encode_and_escape(hub, fl);
    if(res[i]->isfile) {
      base32_encode(res[i]->tth, tth+4);
      size = g_strdup_printf("\05%"G_GUINT64_FORMAT, res[i]->size);
    }
    char *msg = g_strdup_printf("$SR %s %s%s %d/%d\05%s (%s)",
      hub->nick_hub, tmp, size ? size : "", slots_free, slots, res[i]->isfile ? tth : hub->hubname_hub, hubaddr);
    if(from[0] == 'H')
      net_sendf(hub->net, "%s\05%s", msg, from+4);
    else
      net_udp_sendf(from, "%s|", msg);
    g_free(fl);
    g_free(msg);
    g_free(size);
    g_free(tmp);
  }
}


static void nmdc_handle(struct hub *hub, char *cmd) {
  GMatchInfo *nfo;

  // create regexes (declared statically, allocated/compiled on first call)
#define CMDREGEX(name, regex) \
  static GRegex * name = NULL;\
  if(!name) name = g_regex_new("\\$" regex, G_REGEX_OPTIMIZE|G_REGEX_ANCHORED|G_REGEX_DOTALL|G_REGEX_RAW, 0, NULL)

  CMDREGEX(lock, "Lock ([^ $]+) Pk=[^ $]+");
  CMDREGEX(supports, "Supports (.+)");
  CMDREGEX(hello, "Hello ([^ $]+)");
  CMDREGEX(quit, "Quit ([^ $]+)");
  CMDREGEX(nicklist, "NickList (.+)");
  CMDREGEX(oplist, "OpList (.+)");
  CMDREGEX(myinfo, "MyINFO \\$ALL ([^ $]+) (.+)");
  CMDREGEX(hubname, "HubName (.+)");
  CMDREGEX(to, "To: ([^ $]+) From: ([^ $]+) \\$(.+)");
  CMDREGEX(forcemove, "ForceMove (.+)");
  CMDREGEX(connecttome, "ConnectToMe ([^ $]+) ([0-9]{1,3}(?:\\.[0-9]{1,3}){3}:[0-9]+)"); // TODO: IPv6
  CMDREGEX(revconnecttome, "RevConnectToMe ([^ $]+) ([^ $]+)");
  CMDREGEX(search, "Search (Hub:(?:[^ $]+)|(?:[0-9]{1,3}(?:\\.[0-9]{1,3}){3}:[0-9]+)) ([TF])\\?([TF])\\?([0-9]+)\\?([1-9])\\?(.+)");

  // $Lock
  if(g_regex_match(lock, cmd, 0, &nfo)) { // 1 = lock
    char *lock = g_match_info_fetch(nfo, 1);
    if(strncmp(lock, "EXTENDEDPROTOCOL", 16) == 0)
      net_send(hub->net, "$Supports NoGetINFO NoHello");
    char *key = nmdc_lock2key(lock);
    net_sendf(hub->net, "$Key %s", key);
    hub->nick = conf_hub_get(string, hub->tab->name, "nick");
    hub->nick_hub = charset_convert(hub, FALSE, hub->nick);
    net_sendf(hub->net, "$ValidateNick %s", hub->nick_hub);
    g_free(key);
    g_free(lock);
  }
  g_match_info_free(nfo);

  // $Supports
  if(g_regex_match(supports, cmd, 0, &nfo)) { // 1 = list
    char *list = g_match_info_fetch(nfo, 1);
    if(strstr(list, "NoGetINFO"))
      hub->supports_nogetinfo = TRUE;
    // we also support NoHello, but no need to check for that
    g_free(list);
  }
  g_match_info_free(nfo);

  // $Hello
  if(g_regex_match(hello, cmd, 0, &nfo)) { // 1 = nick
    char *nick = g_match_info_fetch(nfo, 1);
    if(strcmp(nick, hub->nick_hub) == 0) {
      // some hubs send our $Hello twice (like verlihub)
      // just ignore the second one
      if(!hub->nick_valid) {
        ui_m(hub->tab, 0, "Nick validated.");
        net_send(hub->net, "$Version 1,0091");
        hub_send_nfo(hub);
        net_send(hub->net, "$GetNickList");
        hub->nick_valid = TRUE;
      }
    } else {
      struct hub_user *u = user_add(hub, nick);
      if(!u->hasinfo && !hub->supports_nogetinfo)
        net_sendf(hub->net, "$GetINFO %s", nick);
    }
    g_free(nick);
  }
  g_match_info_free(nfo);

  // $Quit
  if(g_regex_match(quit, cmd, 0, &nfo)) { // 1 = nick
    char *nick = g_match_info_fetch(nfo, 1);
    struct hub_user *u = g_hash_table_lookup(hub->users, nick);
    if(u) {
      ui_hub_userchange(hub->tab, UIHUB_UC_QUIT, u);
      if(u->hasinfo) {
        hub->sharecount--;
        hub->sharesize -= u->sharesize;
      }
      g_hash_table_remove(hub->users, nick);
    }
    g_free(nick);
  }
  g_match_info_free(nfo);

  // $NickList
  if(g_regex_match(nicklist, cmd, 0, &nfo)) { // 1 = list of users
    // not really efficient, but does the trick
    char *str = g_match_info_fetch(nfo, 1);
    char **list = g_strsplit(str, "$$", 0);
    g_free(str);
    char **cur;
    for(cur=list; *cur&&**cur; cur++) {
      struct hub_user *u = user_add(hub, *cur);
      if(!u->hasinfo && !hub->supports_nogetinfo)
        net_sendf(hub->net, "$GetINFO %s %s", *cur, hub->nick_hub);
    }
    hub->received_first = TRUE;
    g_strfreev(list);
  }
  g_match_info_free(nfo);

  // $OpList
  if(g_regex_match(oplist, cmd, 0, &nfo)) { // 1 = list of ops
    // not really efficient, but does the trick
    char *str = g_match_info_fetch(nfo, 1);
    char **list = g_strsplit(str, "$$", 0);
    g_free(str);
    char **cur;
    // Actually, we should be going through the entire user list and set
    // isop=FALSE when the user is not listed here. I consider this to be too
    // inefficient and not all that important at this point.
    hub->isop = FALSE;
    for(cur=list; *cur&&**cur; cur++) {
      struct hub_user *u = user_add(hub, *cur);
      if(!u->isop) {
        u->isop = TRUE;
        ui_hub_userchange(hub->tab, UIHUB_UC_NFO, u);
      } else
        u->isop = TRUE;
      if(strcmp(hub->nick_hub, *cur) == 0)
        hub->isop = TRUE;
    }
    hub->received_first = TRUE;
    g_strfreev(list);
  }
  g_match_info_free(nfo);

  // $MyINFO
  if(g_regex_match(myinfo, cmd, 0, &nfo)) { // 1 = nick, 2 = info string
    char *nick = g_match_info_fetch(nfo, 1);
    char *str = g_match_info_fetch(nfo, 2);
    struct hub_user *u = user_add(hub, nick);
    if(!u->hasinfo)
      hub->sharecount++;
    else
      hub->sharesize -= u->sharesize;
    user_nmdc_nfo(hub, u, str);
    if(!u->hasinfo)
      hub->sharecount--;
    else
      hub->sharesize += u->sharesize;
    if(hub->received_first && !hub->joincomplete && hub->sharecount == g_hash_table_size(hub->users))
      hub->joincomplete = TRUE;
    g_free(str);
    g_free(nick);
  }
  g_match_info_free(nfo);

  // $HubName
  if(g_regex_match(hubname, cmd, 0, &nfo)) { // 1 = name
    g_free(hub->hubname_hub);
    g_free(hub->hubname);
    hub->hubname_hub = g_match_info_fetch(nfo, 1);
    hub->hubname = nmdc_unescape_and_decode(hub, hub->hubname_hub);
  }
  g_match_info_free(nfo);

  // $To
  if(g_regex_match(to, cmd, 0, &nfo)) { // 1 = to, 2 = from, 3 = msg
    char *to = g_match_info_fetch(nfo, 1);
    char *from = g_match_info_fetch(nfo, 2);
    char *msg = g_match_info_fetch(nfo, 3);
    struct hub_user *u = g_hash_table_lookup(hub->users, from);
    if(!u)
      g_warning("[hub: %s] Got a $To from `%s', who is not on this hub!", hub->tab->name, from);
    else {
      char *msge = nmdc_unescape_and_decode(hub, msg);
      ui_hub_msg(hub->tab, u, msge);
      g_free(msge);
    }
    g_free(from);
    g_free(to);
    g_free(msg);
  }
  g_match_info_free(nfo);

  // $ForceMove
  if(g_regex_match(forcemove, cmd, 0, &nfo)) { // 1 = addr
    char *addr = g_match_info_fetch(nfo, 1);
    char *eaddr = nmdc_unescape_and_decode(hub, addr);
    ui_mf(hub->tab, UIP_HIGH, "\nThe hub is requesting you to move to %s.\nType `/connect %s' to do so.\n", eaddr, eaddr);
    hub_disconnect(hub, FALSE);
    g_free(eaddr);
    g_free(addr);
  }
  g_match_info_free(nfo);

  // $ConnectToMe
  if(g_regex_match(connecttome, cmd, 0, &nfo)) { // 1 = me, 2 = addr
    char *me = g_match_info_fetch(nfo, 1);
    char *addr = g_match_info_fetch(nfo, 2);
    if(strcmp(me, hub->nick_hub) != 0)
      g_warning("Received a $ConnectToMe for someone else (to %s from %s)", me, addr);
    else
      cc_connect(cc_create(hub), addr);
    g_free(me);
    g_free(addr);
  }
  g_match_info_free(nfo);

  // $RevConnectToMe
  if(g_regex_match(revconnecttome, cmd, 0, &nfo)) { // 1 = other, 2 = me
    char *other = g_match_info_fetch(nfo, 1);
    char *me = g_match_info_fetch(nfo, 2);
    if(strcmp(me, hub->nick_hub) != 0)
      g_warning("Received a $RevConnectToMe for someone else (to %s from %s)", me, other);
    else if(cc_listen) {
      net_sendf(hub->net, "$ConnectToMe %s %s:%d", other, cc_listen_ip, cc_listen_port);
      cc_expect_add(hub, other);
    } else
      g_message("Received a $RevConnectToMe, but we're not active.");
    g_free(me);
    g_free(other);
  }
  g_match_info_free(nfo);

  // $Search
  if(g_regex_match(search, cmd, 0, &nfo)) { // 1=from, 2=sizerestrict, 3=ismax, 4=size, 5=type, 6=query
    char *from = g_match_info_fetch(nfo, 1);
    char *sizerestrict = g_match_info_fetch(nfo, 2);
    char *ismax = g_match_info_fetch(nfo, 3);
    char *size = g_match_info_fetch(nfo, 4);
    char *type = g_match_info_fetch(nfo, 5);
    char *query = g_match_info_fetch(nfo, 6);
    nmdc_search(hub, from, sizerestrict[0] == 'F' ? 0 : ismax[0] == 'T' ? -1 : 1, g_ascii_strtoull(size, NULL, 10), type[0]-'0', query);
    g_free(from);
    g_free(sizerestrict);
    g_free(ismax);
    g_free(size);
    g_free(type);
    g_free(query);
  }
  g_match_info_free(nfo);

  // $GetPass
  if(strncmp(cmd, "$GetPass", 8) == 0)
    hub_password(hub, NULL);

  // $BadPass
  if(strncmp(cmd, "$BadPass", 8) == 0) {
    if(g_key_file_has_key(conf_file, hub->tab->name, "password", NULL))
      ui_m(hub->tab, 0, "Wrong password. Use '/set password <password>' to edit your password or '/unset password' to reset it.");
    else
      ui_m(hub->tab, 0, "Wrong password. Type /reconnect to try again.");
    hub_disconnect(hub, FALSE);
  }

  // $ValidateDenide
  if(strncmp(cmd, "$ValidateDenide", 15) == 0) {
    ui_m(hub->tab, 0, "Username invalid or already taken.");
    hub_disconnect(hub, TRUE);
  }

  // $HubIsFull
  if(strncmp(cmd, "$HubIsFull", 10) == 0) {
    ui_m(hub->tab, 0, "Hub is full.");
    hub_disconnect(hub, TRUE);
  }

  // global hub message
  if(cmd[0] != '$')
    ui_m(hub->tab, UIM_PASS|UIM_CHAT|UIP_MED, nmdc_unescape_and_decode(hub, cmd));
}


static gboolean check_nfo(gpointer data) {
  hub_send_nfo(data);
  return TRUE;
}


static gboolean reconnect_timer(gpointer dat) {
  hub_connect(dat);
  ((struct hub *)dat)->reconnect_timer = 0;
  return FALSE;
}


static void handle_cmd(struct net *n, char *cmd) {
  struct hub *hub = n->handle;
  if(hub->adc)
    adc_handle(hub, cmd);
  else
    nmdc_handle(hub, cmd);
}


static void handle_error(struct net *n, int action, GError *err) {
  struct hub *hub = n->handle;

  if(err->code == G_IO_ERROR_CANCELLED)
    return;

  switch(action) {
  case NETERR_CONN:
    ui_mf(hub->tab, 0, "Could not connect to hub: %s. Wating 30 seconds before retrying.", err->message);
    hub->reconnect_timer = g_timeout_add_seconds(30, reconnect_timer, hub);
    break;
  case NETERR_RECV:
    ui_mf(hub->tab, 0, "Read error: %s", err->message);
    hub_disconnect(hub, TRUE);
    break;
  case NETERR_SEND:
    ui_mf(hub->tab, 0, "Write error: %s", err->message);
    hub_disconnect(hub, TRUE);
    break;
  }
}


struct hub *hub_create(struct ui_tab *tab) {
  struct hub *hub = g_new0(struct hub, 1);
  // actual separator is set in handle_connect()
  hub->net = net_create('|', hub, TRUE, handle_cmd, handle_error);
  hub->tab = tab;
  hub->users = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, user_free);
  hub->grants = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  hub->sessions = g_hash_table_new(g_direct_hash, g_direct_equal);
  hub->nfo_timer = g_timeout_add_seconds(5*60, check_nfo, hub);
  return hub;
}


static void handle_connect(struct net *n) {
  struct hub *hub = n->handle;
  ui_mf(hub->tab, 0, "Connected to %s.", net_remoteaddr(n));
  // we can safely change the separator here, since command processing only
  // starts *after* this callback.
  hub->net->eom[0] = hub->adc ? '\n' : '|';

  if(hub->adc)
    net_send(hub->net, "HSUP ADBASE ADTIGR");
}


void hub_connect(struct hub *hub) {
  char *oaddr = conf_hub_get(string, hub->tab->name, "hubaddr");
  char *addr = oaddr;
  g_assert(addr);
  // The address should be in the form of "dchub://hostname:port/", but older
  // ncdc versions saved it simply as "hostname:port" or even "hostname", so we
  // need to handle both. No protocol indicator is assumed to be NMDC. No port
  // is assumed to indicate 411.
  hub->adc = FALSE;
  if(strncmp(addr, "dchub://", 8) == 0)
    addr += 8;
  else if(strncmp(addr, "adc://", 6) == 0) {
    addr += 6;
    hub->adc = TRUE;
  }
  if(addr[strlen(addr)-1] == '/')
    addr[strlen(addr)-1] = 0;

  if(hub->reconnect_timer) {
    g_source_remove(hub->reconnect_timer);
    hub->reconnect_timer = 0;
  }

  ui_mf(hub->tab, 0, "Connecting to %s...", addr);
  net_connect(hub->net, addr, 411, handle_connect);
  g_free(oaddr);
}


void hub_disconnect(struct hub *hub, gboolean recon) {
  net_disconnect(hub->net);
  g_hash_table_remove_all(hub->sessions);
  g_hash_table_remove_all(hub->users);
  g_free(hub->nick);     hub->nick = NULL;
  g_free(hub->nick_hub); hub->nick_hub = NULL;
  g_free(hub->hubname);  hub->hubname = NULL;
  g_free(hub->hubname_hub);  hub->hubname_hub = NULL;
  hub->nick_valid = hub->isreg = hub->isop = hub->received_first =
    hub->joincomplete =  hub->sharecount = hub->sharesize =
    hub->supports_nogetinfo = hub->state = 0;
  if(!recon) {
    ui_m(hub->tab, 0, "Disconnected.");
    if(hub->reconnect_timer) {
      g_source_remove(hub->reconnect_timer);
      hub->reconnect_timer = 0;
    }
  } else {
    ui_m(hub->tab, 0, "Connection lost. Waiting 30 seconds before reconnecting.");
    hub->reconnect_timer = g_timeout_add_seconds(30, reconnect_timer, hub);
  }
}


void hub_free(struct hub *hub) {
  cc_remove_hub(hub);
  hub_disconnect(hub, FALSE);
  net_unref(hub->net);
  g_free(hub->nfo_desc);
  g_free(hub->nfo_conn);
  g_free(hub->nfo_mail);
  g_hash_table_unref(hub->users);
  g_hash_table_unref(hub->sessions);
  g_hash_table_unref(hub->grants);
  g_source_remove(hub->nfo_timer);
  g_free(hub);
}

