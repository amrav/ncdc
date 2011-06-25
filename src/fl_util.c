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
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <libxml/xmlreader.h>
#include <libxml/xmlwriter.h>
#include <bzlib.h>


#if INTERFACE

// file list

struct fl_list {
  char *name; // root = NULL
  struct fl_list *parent;
  GSequence *sub;
  guint64 size;   // including sub-items
  char tth[24];
  time_t lastmod; // only used for files in own list
  int hastth;     // for files: 1/0, directories: (number of directories) + (number of files with hastth==1)
  gboolean isfile : 1;
  gboolean incomplete : 1; // when a directory is missing files
};

#endif





// Utility functions

// only frees the given item and its childs. leaves the parent(s) untouched
void fl_list_free(gpointer dat) {
  struct fl_list *fl = dat;
  if(!fl)
    return;
  g_free(fl->name);
  if(fl->sub)
    g_sequence_free(fl->sub);
  g_slice_free(struct fl_list, fl);
}


// Must return 0 if and only if a and b are equal, assuming they do reside in
// the same directory. A name comparison is enough for this. It is assumed that
// names are case-sensitive.  (Actually, this function may not access other
// members than the name, otherwise a file-by-name lookup wouldn't work.)
gint fl_list_cmp(gconstpointer a, gconstpointer b, gpointer dat) {
  return strcmp(((struct fl_list *)a)->name, ((struct fl_list *)b)->name);
}


void fl_list_add(struct fl_list *parent, struct fl_list *cur) {
  cur->parent = parent;
  g_sequence_insert_sorted(parent->sub, cur, fl_list_cmp, NULL);
  if(!cur->isfile || (cur->isfile && cur->hastth))
    parent->hastth++;
  // update parents size
  while(parent) {
    parent->size += cur->size;
    parent = parent->parent;
  }
}


// Removes an item from the file list, making sure to update the parents.
void fl_list_remove(struct fl_list *fl) {
  struct fl_list *par = fl->parent;
  GSequenceIter *iter = NULL;
  if(par) {
    // can't use _lookup(), too new (2.28)
    iter = g_sequence_iter_prev(g_sequence_search(par->sub, fl, fl_list_cmp, NULL));
    g_assert(!g_sequence_iter_is_end(iter));
    g_assert(g_sequence_get(iter) == fl);

    // update parent->hastth
    if(!fl->isfile || (fl->isfile && fl->hastth))
      par->hastth--;
  }
  // update parents size
  while(par) {
    par->size -= fl->size;
    par = par->parent;
  }
  // and free
  if(iter)
    g_sequence_remove(iter); // also frees the item
  else
    fl_list_free(fl);
}


struct fl_list *fl_list_copy(const struct fl_list *fl) {
  struct fl_list *cur = g_slice_dup(struct fl_list, fl);
  cur->name = g_strdup(fl->name);
  cur->parent = NULL;
  if(fl->sub) {
    cur->sub = g_sequence_new(fl_list_free);
    GSequenceIter *iter;
    // No need to use _insert_sorted() here, since we walk through the list in
    // the correct order already.
    for(iter=g_sequence_get_begin_iter(fl->sub); !g_sequence_iter_is_end(iter); iter=g_sequence_iter_next(iter)) {
      struct fl_list *tmp = fl_list_copy(g_sequence_get(iter));
      tmp->parent = cur;
      g_sequence_append(cur->sub, tmp);
    }
  }
  return cur;
}


// get a file by name in a directory
struct fl_list *fl_list_file(const struct fl_list *dir, const char *name) {
  struct fl_list cmp;
  cmp.name = (char *)name;
  GSequenceIter *iter = g_sequence_iter_prev(g_sequence_search(dir->sub, &cmp, fl_list_cmp, NULL));
  return g_sequence_iter_is_end(iter)
    || strcmp(name, ((struct fl_list *)g_sequence_get(iter))->name) != 0 ? NULL : g_sequence_get(iter);
}


gboolean fl_list_is_child(const struct fl_list *parent, const struct fl_list *child) {
  for(child=child->parent; child; child=child->parent)
    if(child == parent)
      return TRUE;
  return FALSE;
}


// Get the virtual path to a file
char *fl_list_path(struct fl_list *fl) {
  if(!fl->parent)
    return g_strdup("/");
  char *tmp, *path = g_strdup(fl->name);
  struct fl_list *cur = fl->parent;
  while(cur->parent) {
    tmp = path;
    path = g_build_filename(cur->name, path, NULL);
    g_free(tmp);
    cur = cur->parent;
  }
  tmp = path;
  path = g_build_filename("/", path, NULL);
  g_free(tmp);
  return path;
}


// Resolves a path string (Either absolute or relative to root). Does not
// support stuff like ./ and ../, and '/' is assumed to refer to the given
// root. (So '/dir' and 'dir' are simply equivalent)
// Case-sensitive, and '/' is the only recognised path separator
struct fl_list *fl_list_from_path(struct fl_list *root, const char *path) {
  while(path[0] == '/')
    path++;
  if(!path[0])
    return root;
  g_assert(root->sub);
  int slash = strcspn(path, "/");
  char *name = g_strndup(path, slash);
  struct fl_list *n = fl_list_file(root, name);
  g_free(name);
  if(!n)
    return NULL;
  if(slash == strlen(path))
    return n;
  if(n->isfile)
    return NULL;
  return fl_list_from_path(n, path+slash+1);
}


// Auto-complete for fl_list_from_path()
void fl_list_suggest(struct fl_list *root, char *opath, char **sug) {
  struct fl_list *parent = root;
  char *path = g_strdup(opath);
  char *name = path;
  char *sep = strrchr(path, '/');
  if(sep) {
    *sep = 0;
    name = sep+1;
    parent = fl_list_from_path(parent, path);
  } else {
    name = path;
    path = "";
  }
  if(parent) {
    int i = 0, len = strlen(name);
    // Note: performance can be improved by using _search() instead
    GSequenceIter *iter;
    for(iter=g_sequence_get_begin_iter(parent->sub); i<20 && !g_sequence_iter_is_end(iter); iter=g_sequence_iter_next(iter)) {
      struct fl_list *n = g_sequence_get(iter);
      if(strncmp(n->name, name, len) == 0)
        sug[i++] = n->isfile ? g_strconcat(path, "/", n->name, NULL) : g_strconcat(path, "/", n->name, "/", NULL);
    }
  }
  if(sep)
    g_free(path);
  else
    g_free(name);
}


gboolean fl_list_search_match_name(struct fl_list *fl, char **ext, char **inc) {
  for(; inc&&*inc; inc++)
    if(G_LIKELY(!str_casestr(fl->name, *inc)))
      return FALSE;
  if(!ext || !*ext)
    return TRUE;
  char *l = rindex(fl->name, '.');
  if(G_UNLIKELY(!l || !l[1]))
    return FALSE;
  l++;
  for(; *ext; ext++)
    if(G_UNLIKELY(g_ascii_strcasecmp(l, *ext) == 0))
      return TRUE;
  return FALSE;
}


#if INTERFACE

#define fl_list_search_matches(fl, size_m, s, filedir, ext, inc) \
  (((((filedir) & 2) && !(fl)->isfile) || (((filedir) & 1) && (fl)->isfile && (fl)->hastth))\
    && (!(size_m) || ((size_m) < 0 && (fl)->size < (s)) || ((size_m) > 0 && (fl)->size > (s))) && fl_list_search_match_name(fl, ext, inc))

#endif


// Recursive depth-first search through the list, used for replying to non-TTH
// $Search requests. Not exactly fast, but what did you expect? :-(
int fl_list_search(struct fl_list *parent, int size_m, guint64 size, int filedir, char **ext, char **inc, struct fl_list **res, int max) {
  if(!parent || !parent->sub)
    return 0;
  GSequenceIter *iter;
  // weed out stuff from inc if it's already matched in parent (I'm assuming
  // that stuff matching the parent of parent has already been removed)
  char *ninc[g_strv_length(inc)];
  int i = 0;
  for(; *inc; inc++)
    if(G_LIKELY(!parent->name || !str_casestr(parent->name, *inc)))
      ninc[i++] = *inc;
  ninc[i] = NULL;
  // loop through the directory
  i = 0;
  for(iter=g_sequence_get_begin_iter(parent->sub); i<max && !g_sequence_iter_is_end(iter); iter=g_sequence_iter_next(iter)) {
    struct fl_list *n = g_sequence_get(iter);
    if(fl_list_search_matches(n, size_m, size, filedir, ext, ninc))
      res[i++] = n;
    if(!n->isfile && i < max)
      i += fl_list_search(n, size_m, size, filedir, ext, ninc, res+i, max-i);
  }
  return i;
}







// Internal structure used by fl_load() and fl_save()

struct fl_loadsave_context {
  char *file;     // some name, for debugging purposes
  BZFILE *fh_bz;  // if BZ2 compression is enabled (implies fh_h!=NULL)
  FILE *fh_f;     // if we're working with a file
  GString *buf;   // if we're working with a buffer (only fl_save() supports this)
  GError **err;
  gboolean stream_end;
};




// Read filelist from an xml file


static int fl_load_input(void *context, char *buf, int len) {
  struct fl_loadsave_context *xc = context;
  if(xc->stream_end)
    return 0;
  int bzerr;
  if(xc->fh_bz) {
    int r = BZ2_bzRead(&bzerr, xc->fh_bz, buf, len);
    if(bzerr != BZ_OK && bzerr != BZ_STREAM_END) {
      g_set_error(xc->err, 1, 0, "bzip2 decompression error. (%d)", bzerr);
      return -1;
    } else {
      xc->stream_end = bzerr == BZ_STREAM_END;
      return r;
    }
  } else if(xc->fh_f) {
    int r = fread(buf, 1, len, xc->fh_f);
    if(r < 0)
      g_set_error(xc->err, 1, 0, "Read error: %s", g_strerror(errno));
    xc->stream_end = r <= 0;
    return r;
  } else
    g_return_val_if_reached(-1);
}


static int fl_load_close(void *context) {
  struct fl_loadsave_context *xc = context;
  int bzerr;
  if(xc->fh_bz)
    BZ2_bzReadClose(&bzerr, xc->fh_bz);
  fclose(xc->fh_f);
  g_free(xc->file);
  g_slice_free(struct fl_loadsave_context, xc);
  return 0;
}


static void fl_load_error(void *arg, const char *msg, xmlParserSeverities severity, xmlTextReaderLocatorPtr locator) {
  struct fl_loadsave_context *xc = arg;
  if(severity == XML_PARSER_SEVERITY_VALIDITY_WARNING || severity == XML_PARSER_SEVERITY_WARNING)
    g_warning("XML parse warning in %s line %d: %s", xc->file, xmlTextReaderLocatorLineNumber(locator), msg);
  else if(xc->err && !*(xc->err))
    g_set_error(xc->err, 1, 0, "XML parse error on input line %d: %s", xmlTextReaderLocatorLineNumber(locator), msg);
}


static int fl_load_handle(xmlTextReaderPtr reader, gboolean *havefl, gboolean *newdir, struct fl_list **cur) {
  struct fl_list *tmp;
  char *attr[3];
  char name[50], *tmpname;

  tmpname = (char *)xmlTextReaderName(reader);
  strncpy(name, tmpname, 50);
  name[49] = 0;
  free(tmpname);

  switch(xmlTextReaderNodeType(reader)) {

  case XML_READER_TYPE_ELEMENT:
    // <FileListing ..>
    // We ignore its attributes (for now)
    if(strcmp(name, "FileListing") == 0) {
      if(*havefl || xmlTextReaderIsEmptyElement(reader))
        return -1;
      *havefl = TRUE;
    // <Directory ..>
    } else if(strcmp(name, "Directory") == 0) {
      if(!*havefl)
        return -1;
      if(!(attr[0] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Name")))
        return -1;
      attr[1] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Incomplete");
      if(attr[1] && strcmp(attr[1], "0") != 0 && strcmp(attr[1], "1") != 0) {
        free(attr[0]);
        return -1;
      }
      tmp = g_slice_new0(struct fl_list);
      tmp->name = g_strdup(attr[0]);
      tmp->isfile = FALSE;
      tmp->incomplete = attr[1] && attr[1][0] == '1';
      tmp->sub = g_sequence_new(fl_list_free);
      fl_list_add(*newdir ? *cur : (*cur)->parent, tmp);
      *cur = tmp;
      *newdir = !xmlTextReaderIsEmptyElement(reader);
      free(attr[0]);
      free(attr[1]);
    // <File .. />
    } else if(strcmp(name, "File") == 0) {
      if(!*havefl || !xmlTextReaderIsEmptyElement(reader))
        return -1;
      if(!(attr[0] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Name")))
        return -1;
      attr[1] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"Size");
      if(!attr[1] || strspn(attr[1], "0123456789") != strlen(attr[1])) {
        free(attr[0]);
        return -1;
      }
      attr[2] = (char *)xmlTextReaderGetAttribute(reader, (xmlChar *)"TTH");
      if(!attr[2] || strlen(attr[2]) != 39 || strspn(attr[2], "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567") != 39) {
        free(attr[0]);
        free(attr[1]);
        return -1;
      }
      tmp = g_slice_new0(struct fl_list);
      tmp->name = g_strdup(attr[0]);
      tmp->isfile = TRUE;
      tmp->size = g_ascii_strtoull(attr[1], NULL, 10);
      tmp->hastth = 1;
      base32_decode(attr[2], tmp->tth);
      fl_list_add(*newdir ? *cur : (*cur)->parent, tmp);
      *newdir = FALSE;
      *cur = tmp;
      free(attr[0]);
      free(attr[1]);
      free(attr[2]);
    }
    break;

  case XML_READER_TYPE_END_ELEMENT:
    // </Directory>
    if(strcmp(name, "Directory") == 0) {
      if(!*newdir)
        *cur = (*cur)->parent;
      else
        *newdir = FALSE;
    // </FileListing>
    } else if(strcmp(name, "FileListing") == 0) {
      return 0; // stop reading
    }
    break;
  }
  return 1;
}


static xmlTextReaderPtr fl_load_open(const char *file, GError **err) {
  gboolean isbz2 = strlen(file) > 4 && strcmp(file+(strlen(file)-4), ".bz2") == 0;

  // open file
  FILE *f = fopen(file, "r");
  if(!f) {
    g_set_error_literal(err, 1, 0, g_strerror(errno));
    return NULL;
  }

  // open BZ2 decompression
  BZFILE *bzf = NULL;
  if(isbz2) {
    int bzerr;
    bzf = BZ2_bzReadOpen(&bzerr, f, 0, 0, NULL, 0);
    if(bzerr != BZ_OK) {
      g_set_error(err, 1, 0, "Unable to open BZ2 file (%d)", bzerr);
      fclose(f);
      return NULL;
    }
  }

  // create reader
  struct fl_loadsave_context *xc = g_slice_new0(struct fl_loadsave_context);
  xc->err = err;
  xc->file = g_strdup(file);
  xc->fh_f = f;
  xc->fh_bz = bzf;
  xmlTextReaderPtr reader = xmlReaderForIO(fl_load_input, fl_load_close, xc, NULL, NULL, XML_PARSE_NOENT);

  if(!reader) {
    fl_load_close(xc);
    if(err && !*err)
      g_set_error_literal(err, 1, 0, "Failed to open file.");
    return NULL;
  }

  xmlTextReaderSetErrorHandler(reader, fl_load_error, &xc);
  return reader;
}


struct fl_list *fl_load(const char *file, GError **err) {
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);

  // open the file
  xmlTextReaderPtr reader = fl_load_open(file, err);
  if(!reader)
    return NULL;

  // parse & read
  struct fl_list *cur, *root;
  gboolean havefl = FALSE, newdir = TRUE;
  int ret;

  root = g_slice_new0(struct fl_list);
  root->sub = g_sequence_new(fl_list_free);
  cur = root;

  while((ret = xmlTextReaderRead(reader)) == 1)
    if((ret = fl_load_handle(reader, &havefl, &newdir, &cur)) <= 0)
      break;

  if(ret < 0) {
    if(err && !*err) // rather uninformative error message as fallback
      g_set_error_literal(err, 1, 0, "Error parsing or validating XML.");
    fl_list_free(root);
    root = NULL;
  }

  // close (ignoring errors)
  xmlTextReaderClose(reader);
  xmlFreeTextReader(reader);

  return root;
}






// Save a filelist to a .xml file

static int fl_save_write(void *context, const char *buf, int len) {
  struct fl_loadsave_context *xc = context;
  if(xc->fh_bz) {
    int bzerr;
    BZ2_bzWrite(&bzerr, xc->fh_bz, (char *)buf, len);
    if(bzerr == BZ_OK)
      return len;
    if(bzerr == BZ_IO_ERROR) {
      g_set_error_literal(xc->err, 1, 0, "bzip2 write error.");
      return -1;
    }
    g_return_val_if_reached(-1);
  } else if(xc->fh_f) {
    int r = fwrite(buf, 1, len, xc->fh_f);
    if(r < 0)
      g_set_error(xc->err, 1, 0, "Write error: %s", g_strerror(errno));
    return r;
  } else if(xc->buf) {
    g_string_append_len(xc->buf, buf, len);
    return len;
  } else
    g_return_val_if_reached(-1);
}


static int fl_save_close(void *context) {
  struct fl_loadsave_context *xc = context;
  int bzerr;
  if(xc->fh_bz)
    BZ2_bzWriteClose(&bzerr, xc->fh_bz, 0, NULL, NULL);
  if(xc->fh_f)
    fclose(xc->fh_f);
  g_free(xc->file);
  g_slice_free(struct fl_loadsave_context, xc);
  return 0;
}


// recursive
static gboolean fl_save_childs(xmlTextWriterPtr writer, struct fl_list *fl, int level) {
  GSequenceIter *iter;
  for(iter=g_sequence_get_begin_iter(fl->sub); !g_sequence_iter_is_end(iter); iter=g_sequence_iter_next(iter)) {
    struct fl_list *cur = g_sequence_get(iter);
#define CHECKFAIL(f) if(f < 0) return FALSE
    if(cur->isfile && cur->hastth) {
      char tth[40];
      base32_encode(cur->tth, tth);
      tth[39] = 0;
      CHECKFAIL(xmlTextWriterStartElement(writer, (xmlChar *)"File"));
      CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Name", (xmlChar *)cur->name));
      CHECKFAIL(xmlTextWriterWriteFormatAttribute(writer, (xmlChar *)"Size", "%"G_GUINT64_FORMAT, cur->size));
      CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"TTH", (xmlChar *)tth));
      CHECKFAIL(xmlTextWriterEndElement(writer));
    }
    if(!cur->isfile) {
      CHECKFAIL(xmlTextWriterStartElement(writer, (xmlChar *)"Directory"));
      CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Name", (xmlChar *)cur->name));
      if(cur->incomplete || cur->hastth != g_sequence_iter_get_position(g_sequence_get_end_iter(cur->sub))
          || (cur->hastth && level < 1))
        CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Incomplete", (xmlChar *)"1"));
      if(level > 0)
        fl_save_childs(writer, cur, level-1);
      CHECKFAIL(xmlTextWriterEndElement(writer));
    }
#undef CHECKFAIL
  }
  return TRUE;
}


static xmlTextWriterPtr fl_save_open(const char *file, gboolean isbz2, GString *buf, GError **err) {
  // open file (if any)
  FILE *f = NULL;
  if(file) {
    f = fopen(file, "w");
    if(!f) {
      g_set_error_literal(err, 1, 0, g_strerror(errno));
      return NULL;
    }
  }

  // open compressor (if needed)
  BZFILE *bzf = NULL;
  if(f && isbz2) {
    int bzerr;
    bzf = BZ2_bzWriteOpen(&bzerr, f, 7, 0, 0);
    if(bzerr != BZ_OK) {
      g_set_error(err, 1, 0, "Unable to create BZ2 file (%d)", bzerr);
      fclose(f);
      return NULL;
    }
  }

  // create writer
  struct fl_loadsave_context *xc = g_slice_new0(struct fl_loadsave_context);
  xc->err = err;
  xc->file = file ? g_strdup(file) : g_strdup("string buffer");
  xc->fh_f = f;
  xc->fh_bz = bzf;
  xc->buf = buf;
  xmlTextWriterPtr writer = xmlNewTextWriter(xmlOutputBufferCreateIO(fl_save_write, fl_save_close, xc, NULL));

  if(!writer) {
    fl_save_close(xc);
    if(err && !*err)
      g_set_error_literal(err, 1, 0, "Failed to open file.");
    return NULL;
  }
  return writer;
}


gboolean fl_save(struct fl_list *fl, const char *file, GString *buf, int level, GError **err) {
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

  // open a temporary file for writing
  gboolean isbz2 = FALSE;
  char *tmpfile = NULL;
  if(file) {
    isbz2 = strlen(file) > 4 && strcmp(file+(strlen(file)-4), ".bz2") == 0;
    tmpfile = g_strdup_printf("%s.tmp-%d", file, rand());
  }

  xmlTextWriterPtr writer = fl_save_open(tmpfile, isbz2, buf, err);
  if(!writer) {
    g_free(tmpfile);
    return FALSE;
  }

  // write
  gboolean success = TRUE;
#define CHECKFAIL(f) if((f) < 0) { success = FALSE; goto fl_save_error; }
  CHECKFAIL(xmlTextWriterSetIndent(writer, 1));
  CHECKFAIL(xmlTextWriterSetIndentString(writer, (xmlChar *)"\t"));
  // <FileListing ..>
  CHECKFAIL(xmlTextWriterStartDocument(writer, NULL, "utf-8", "yes"));
  CHECKFAIL(xmlTextWriterStartElement(writer, (xmlChar *)"FileListing"));
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Version", (xmlChar *)"1"));
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Generator", (xmlChar *)PACKAGE_STRING));

  char *cid = g_key_file_get_string(conf_file, "global", "cid", NULL);
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"CID", (xmlChar *)cid));
  g_free(cid);

  char *path = fl_list_path(fl);
  CHECKFAIL(xmlTextWriterWriteAttribute(writer, (xmlChar *)"Base", (xmlChar *)path));
  g_free(path);

  // all <Directory ..> elements
  if(!fl_save_childs(writer, fl, level-1)) {
    success = FALSE;
    goto fl_save_error;
  }

  CHECKFAIL(xmlTextWriterEndElement(writer));

  // close
fl_save_error:
  xmlTextWriterEndDocument(writer);
  xmlFreeTextWriter(writer);

  // rename or unlink file
  if(file) {
    if(success && rename(tmpfile, file) < 0) {
      if(err && !*err)
        g_set_error_literal(err, 1, 0, g_strerror(errno));
      success = FALSE;
    }
    if(!success)
      unlink(tmpfile);
    g_free(tmpfile);
  }
  return success;
}
