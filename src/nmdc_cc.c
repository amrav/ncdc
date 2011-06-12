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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


#if INTERFACE

struct nmdc_cc {
  struct net *net;
  struct nmdc_hub *hub;
  char *nick_raw; // hub encoding
  char *nick;     // UTF-8
  time_t last_action;
  int timeout_src;
  char *last_file;
  guint64 last_size;
  guint64 last_length;
  guint64 last_offset;
  GError *err;
};

#endif

// opened connections - in no particular order
GList *nmdc_cc_list = NULL;


// When a hub tab is closed (not just disconnected), make sure all hub fields
// are reset to NULL - since we won't be able to dereference it anymore.  Note
// that we do keep the connections opened, and things can resume as normal
// without the hub field, since it is only used in the initial phase (with the
// $MyNick's being exchanged.)
// Note that the connection will remain hubless even when the same hub is later
// opened again. I don't think this is a huge problem, however.
void nmdc_cc_remove_hub(struct nmdc_hub *hub) {
  GList *n;
  for(n=nmdc_cc_list; n; n=n->next) {
    struct nmdc_cc *c = n->data;
    if(c->hub == hub)
      c->hub = NULL;
  }
}


// Can be cached if performance is an issue. Note that even file transfers that
// do not require a slot are still counted as taking a slot. For this reason,
// the return value can be larger than the configured number of slots. This
// also means that an upload that requires a slot will not be granted if there
// are many transfers active that don't require a slot.
int nmdc_cc_slots_in_use() {
  int num = 0;
  GList *n;
  for(n=nmdc_cc_list; n; n=n->next)
    if(((struct nmdc_cc *)n->data)->net->file_left)
      num++;
  return num;
}


// Get an already connected user
static struct nmdc_cc *nmdc_cc_get_conn(struct nmdc_hub *hub, const char *user) {
  GList *n;
  for(n=nmdc_cc_list; n; n=n->next) {
    struct nmdc_cc *c = n->data;
    if(c->nick_raw && c->hub == hub && c->net->conn && strcmp(c->nick_raw, user) == 0)
      return c;
  }
  return NULL;
}


// ADC parameter unescaping, required for $ADCGET
static char *adc_unescape(const char *str) {
  char *dest = g_new(char, strlen(str)+1);
  char *tmp = dest;
  while(*str) {
    if(*str == '\\') {
      str++;
      if(*str == 's')
        *tmp = ' ';
      else if(*str == 'n')
        *tmp = '\n';
      else if(*str == '\\')
        *tmp = '\\';
      else {
        g_free(dest);
        return NULL;
      }
    } else
      *tmp = *str;
    tmp++;
    str++;
  }
  *tmp = 0;
  return dest;
}


static char *adc_escape(const char *str) {
  GString *dest = g_string_sized_new(strlen(str)+50);
  while(*str) {
    switch(*str) {
    case ' ':  g_string_append(dest, "\\s"); break;
    case '\n': g_string_append(dest, "\\n"); break;
    case '\\': g_string_append(dest, "\\\\"); break;
    default: g_string_append_c(dest, *str); break;
    }
    str++;
  }
  return g_string_free(dest, FALSE);
}



static void handle_error(struct net *n, int action, GError *err) {
  struct nmdc_cc *cc = n->handle;
  g_propagate_error(&(cc->err), g_error_copy(err));
  nmdc_cc_disconnect(n->handle);
}


// TODO:
// - id = files.xml? (Required by ADC, but I doubt it's used)
// - type = list? (Also required by ADC, but is this used?)

// err->type = 0 -> generic error. otherwise -> no slots
static void handle_adcget(struct nmdc_cc *cc, char *type, char *id, guint64 start, gint64 bytes, GError **err) {
  // tthl
  if(strcmp(type, "tthl") == 0) {
    if(strncmp(id, "TTH/", 4) != 0 || strlen(id) != 4+39 || start != 0 || bytes != -1) {
      g_set_error_literal(err, 1, 0, "Invalid ADCGET arguments");
      return;
    }
    char root[24];
    base32_decode(id+4, root);
    int len;
    char *dat = fl_hashdat_get(root, &len);
    if(!dat)
      g_set_error_literal(err, 1, 0, "File Not Available");
    else {
      // no need to adc_escape(id) here, since it cannot contain any special characters
      net_sendf(cc->net, "$ADCSND tthl %s 0 %d", id, len);
      net_send_raw(cc->net, dat, len);
      free(dat);
    }
    return;
  }

  // file
  if(strcmp(type, "file") != 0) {
    g_set_error_literal(err, 1, 0, "Unsupported ADCGET type");
    return;
  }

  // get path (for file uploads)
  char *path = NULL;
  char *vpath;
  struct fl_list *f = NULL;
  gboolean needslot = TRUE;

  // files.xml.bz2
  if(strcmp(id, "files.xml.bz2") == 0) {
    path = g_strdup(fl_local_list_file);
    vpath = g_strdup("files.xml.bz2");
    needslot = FALSE;
  // / (path in the nameless root - must be UTF-8)
  } else if(id[0] == '/' && fl_local_list && g_utf8_validate(id, -1, NULL)) {
    f = fl_list_from_path(fl_local_list, id);
  // TTH/
  } else if(strncmp(id, "TTH/", 4) == 0 && strlen(id) == 4+39) {
    char root[24];
    base32_decode(id+4, root);
    GSList *l = fl_local_from_tth(root);
    f = l ? l->data : NULL;
  }

  if(f) {
    char *enc_path = fl_local_path(f);
    path = g_filename_from_utf8(enc_path, -1, NULL, NULL, NULL);
    g_free(enc_path);
    vpath = fl_list_path(f);
  }

  // validate
  struct stat st;
  if(!path || stat(path, &st) < 0 || !S_ISREG(st.st_mode) || start > st.st_size) {
    g_set_error_literal(err, 1, 0, "File Not Available");
    g_free(path);
    g_free(vpath);
    return;
  }
  if(bytes < 0 || bytes > st.st_size-start)
    bytes = st.st_size-start;
  if(needslot && st.st_size < 16*1024)
    needslot = FALSE;

  // send
  if(!needslot || nmdc_cc_slots_in_use() < conf_slots()) {
    g_free(cc->last_file);
    cc->last_file = vpath;
    cc->last_length = bytes;
    cc->last_offset = start;
    cc->last_size = st.st_size;
    char *tmp = adc_escape(id);
    net_sendf(cc->net, "$ADCSND %s %s %"G_GUINT64_FORMAT" %"G_GINT64_FORMAT, type, tmp, start, bytes);
    net_sendfile(cc->net, path, start, bytes);
    g_free(tmp);
    g_free(path);
  } else {
    g_set_error_literal(err, 1, 1, "No Slots Available");
    g_free(vpath);
  }
}


static void handle_mynick(struct nmdc_cc *cc, const char *nick) {
  if(cc->nick) {
    g_warning("Received a $MyNick from %s when we have already received one.", cc->nick);
    return;
  }

  // TODO: figure out hub if this is an active session
  if(!cc->hub) {
    nmdc_cc_disconnect(cc);
    return;
  }

  struct nmdc_user *u = g_hash_table_lookup(cc->hub->users, nick);
  if(!u) {
    g_set_error_literal(&(cc->err), 1, 0, "User is not on the hub");
    nmdc_cc_disconnect(cc);
    return;
  }

  // don't allow multiple connections from the same user
  struct nmdc_cc *dup = nmdc_cc_get_conn(cc->hub, nick);

  cc->nick_raw = g_strdup(nick);
  cc->nick = g_strdup(u->name);

  if(dup) {
    g_set_error_literal(&(cc->err), 1, 0, "too many open connections with this user");
    nmdc_cc_disconnect(cc);
    return;
  }
}


static void handle_cmd(struct net *n, char *cmd) {
  struct nmdc_cc *cc = n->handle;
  GMatchInfo *nfo;

  time(&(cc->last_action));
  g_clear_error(&(cc->err));

  // create regexes (declared statically, allocated/compiled on first call)
#define CMDREGEX(name, regex) \
  static GRegex * name = NULL;\
  if(!name) name = g_regex_new("\\$" regex, G_REGEX_OPTIMIZE|G_REGEX_ANCHORED|G_REGEX_DOTALL|G_REGEX_RAW, 0, NULL)

  CMDREGEX(mynick, "MyNick ([^ $]+)");
  CMDREGEX(lock, "Lock ([^ $]+) Pk=[^ $]+");
  CMDREGEX(supports, "Supports (.+)");
  CMDREGEX(adcget, "ADCGET ([^ $]+) ([^ ]+) ([0-9]+) (-?[0-9]+)");

  // $MyNick
  if(g_regex_match(mynick, cmd, 0, &nfo)) { // 1 = nick
    char *nick = g_match_info_fetch(nfo, 1);
    handle_mynick(cc, nick);
    g_free(nick);
  }
  g_match_info_free(nfo);

  // $Lock
  if(g_regex_match(lock, cmd, 0, &nfo)) { // 1 = lock
    char *lock = g_match_info_fetch(nfo, 1);
    // we don't implement the classic NMDC get, so we can't talk with non-EXTENDEDPROTOCOL clients
    if(strncmp(lock, "EXTENDEDPROTOCOL", 16) != 0) {
      g_set_error_literal(&(cc->err), 1, 0, "Client does not support ADCGet");
      g_warning("C-C connection with %s (%s), but it does not support EXTENDEDPROTOCOL.", net_remoteaddr(cc->net), cc->nick);
      nmdc_cc_disconnect(cc);
    } else {
      net_send(cc->net, "$Supports MiniSlots XmlBZList ADCGet TTHL TTHF");
      char *key = nmdc_lock2key(lock);
      net_send(cc->net, "$Direction Upload 0"); // we don't support downloading yet.
      net_sendf(cc->net, "$Key %s", key);
      g_free(key);
      g_free(lock);
    }
  }
  g_match_info_free(nfo);

  // $Supports
  if(g_regex_match(supports, cmd, 0, &nfo)) { // 1 = list
    char *list = g_match_info_fetch(nfo, 1);
    // Client must support ADCGet to download from us, since we haven't implemented the old NMDC $Get.
    if(!strstr(list, "ADCGet")) {
      g_set_error_literal(&(cc->err), 1, 0, "Client does not support ADCGet");
      g_warning("C-C connection with %s (%s), but it does not support ADCGet.", net_remoteaddr(cc->net), cc->nick);
      nmdc_cc_disconnect(cc);
    }
    g_free(list);
  }
  g_match_info_free(nfo);

  // $ADCGET
  if(g_regex_match(adcget, cmd, 0, &nfo)) { // 1 = type, 2 = identifier, 3 = start_pos, 4 = bytes
    char *type = g_match_info_fetch(nfo, 1);
    char *id = g_match_info_fetch(nfo, 2);
    char *start = g_match_info_fetch(nfo, 3);
    char *bytes = g_match_info_fetch(nfo, 4);
    guint64 st = g_ascii_strtoull(start, NULL, 10);
    gint64 by = g_ascii_strtoll(bytes, NULL, 10);
    char *un_id = adc_unescape(id);
    if(!cc->nick) {
      g_set_error_literal(&(cc->err), 1, 0, "Received $ADCGET before $MyNick");
      g_warning("Received $ADCGET before $MyNick, disconnecting client.");
      nmdc_cc_disconnect(cc);
    } else if(un_id) {
      GError *err = NULL;
      handle_adcget(cc, type, un_id, st, by, &err);
      if(err) {
        if(!err->code)
          net_sendf(cc->net, "$Error %s", err->message);
        else
          net_send(cc->net, "$MaxedOut");
        g_propagate_error(&(cc->err), err);
      }
    }
    g_free(un_id);
    g_free(type);
    g_free(id);
    g_free(start);
    g_free(bytes);
  }
  g_match_info_free(nfo);
}


// Hub may be unknown when we start listening on incoming connections.
struct nmdc_cc *nmdc_cc_create(struct nmdc_hub *hub) {
  struct nmdc_cc *cc = g_new0(struct nmdc_cc, 1);
  cc->net = net_create('|', cc, FALSE, handle_cmd, handle_error);
  cc->hub = hub;
  time(&(cc->last_action));
  nmdc_cc_list = g_list_prepend(nmdc_cc_list, cc);
  return cc;
}


static void handle_connect(struct net *n) {
  struct nmdc_cc *cc = n->handle;
  time(&(cc->last_action));
  if(!cc->hub)
    nmdc_cc_disconnect(cc);
  else {
    net_sendf(n, "$MyNick %s", cc->hub->nick_hub);
    net_sendf(n, "$Lock EXTENDEDPROTOCOL/wut? Pk=%s-%s", PACKAGE_NAME, PACKAGE_VERSION);
  }
}


void nmdc_cc_connect(struct nmdc_cc *cc, const char *addr) {
  g_return_if_fail(!cc->timeout_src);
  net_connect(cc->net, addr, 0, handle_connect);
  if(cc->timeout_src) {
    g_source_remove(cc->timeout_src);
    cc->timeout_src = 0;
  }
  g_clear_error(&(cc->err));
}


static gboolean handle_timeout(gpointer dat) {
  nmdc_cc_free(dat);
  return FALSE;
}


void nmdc_cc_disconnect(struct nmdc_cc *cc) {
  time(&(cc->last_action));
  net_disconnect(cc->net);
  cc->timeout_src = g_timeout_add_seconds(30, handle_timeout, cc);
}


void nmdc_cc_free(struct nmdc_cc *cc) {
  nmdc_cc_disconnect(cc);
  nmdc_cc_list = g_list_remove(nmdc_cc_list, cc);
  net_unref(cc->net);
  g_error_free(cc->err);
  g_free(cc->nick_raw);
  g_free(cc->nick);
  g_free(cc->last_file);
  g_free(cc);
}

