// GTK4 front end for Rufux. It follows Rufus's main window: same sections,
// same wording, same order. GTK4 draws with the desktop's own theme, icons
// and file dialogs natively (through the freedesktop portal when sandboxed,
// e.g. from inside an AppImage), so unlike the previous Qt build this needs
// no extra plumbing to pick up dark mode or Wayland window decorations.
//
// Anything that touches a disk runs in a separate root process:
// `pkexec rufux create ... --real --yes`, the same worker the command line
// uses. This file builds the command line, shows progress and reports
// errors. The core logic (device scan, ISO probe, partitioning, formatting)
// is unchanged from the Qt build - only the toolkit differs.
#include <gtk/gtk.h>
#include <openssl/evp.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "device.h"
#include "iso_probe.h"
#include "wininstall.h"

#ifndef RUFUX_VERSION
#define RUFUX_VERSION "dev"
#endif

// ===== small helpers, ported 1:1 from the Qt build ========================

// Largest label each file system accepts.
static int label_limit(const char *fs) {
  if (!strcmp(fs, "vfat")) return 11;
  if (!strcmp(fs, "exfat")) return 15;
  if (!strcmp(fs, "ext4")) return 16;
  if (!strcmp(fs, "udf")) return 30;
  return 32;
}

static void sanitize_label(const char *in, const char *fs, char *out, size_t cap) {
  size_t n = 0;
  const int limit = label_limit(fs);
  for (const unsigned char *p = (const unsigned char *)in; *p && (int)n < limit && n + 1 < cap; p++) {
    unsigned char c = *p;
    if (c == ' ') c = '_';
    if (isalnum(c)) out[n++] = (!strcmp(fs, "vfat")) ? (unsigned char)toupper(c) : c;
    else if (c == '_' || c == '-') out[n++] = c;
  }
  out[n] = 0;
  if (n == 0) snprintf(out, cap, "NO_LABEL");
}

// ===== application state ===================================================

typedef struct {
  GtkWidget *win;
  GtkWidget *dev_combo, *boot_combo, *image_combo, *scheme_combo, *target_combo;
  GtkWidget *fs_combo, *cluster_combo, *passes_combo;
  GtkWidget *select_btn, *hash_btn, *start_btn, *close_btn;
  GtkWidget *about_btn, *log_btn;
  GtkWidget *persist_scale, *persist_value, *dev_count, *image_info;
  GtkWidget *label_entry;
  GtkWidget *chk_fixed, *chk_quick, *chk_ext, *chk_bad;
  GtkWidget *bar;
  GtkWidget *drive_image_row, *drive_persist_row;
  GtkWidget *log_win, *log_view;
  GtkTextBuffer *log_buffer;
  GPtrArray *log_lines;  // owned char*

  // state
  char iso_path[4096];
  int iso_windows, iso_hybrid, iso_valid;
  char iso_label[64];
  RufuxDevice devs[64];
  int ndevs;
  char dev_signature[8192];
  GSubprocess *worker;
  char last_error[1024];
  guint poll_id;
} App;

static const char *fs_key(App *a) {
  const char *t = gtk_string_object_get_string(
      GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(GTK_DROP_DOWN(a->fs_combo))));
  if (!strcmp(t, "NTFS")) return "ntfs";
  if (!strcmp(t, "exFAT")) return "exfat";
  if (!strcmp(t, "UDF")) return "udf";
  if (!strcmp(t, "ext4")) return "ext4";
  return "vfat";
}

static gboolean busy(App *a) { return a->worker != NULL; }
static gboolean iso_mode(App *a) { return gtk_drop_down_get_selected(GTK_DROP_DOWN(a->boot_combo)) == 2; }

static void log_line(App *a, const char *s) {
  GDateTime *now = g_date_time_new_now_local();
  char *stamp = g_date_time_format(now, "%H:%M:%S  ");
  char *full = g_strconcat(stamp, s, "\n", NULL);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(a->log_buffer, &end);
  gtk_text_buffer_insert(a->log_buffer, &end, full, -1);
  g_ptr_array_add(a->log_lines, g_strdup(s));
  g_free(stamp);
  g_free(full);
  g_date_time_unref(now);
}

// A single string that combines every logged line, for "Save log" and the
// detailed-error view.
static char *joined_log(App *a) {
  GString *g = g_string_new(NULL);
  for (guint i = 0; i < a->log_lines->len; i++) {
    if (i) g_string_append_c(g, '\n');
    g_string_append(g, (const char *)g_ptr_array_index(a->log_lines, i));
  }
  return g_string_free(g, FALSE);
}

// ---- "Show advanced ..." row: an arrow toggle that reveals a block -------
static void advanced_toggled(GtkToggleButton *btn, GtkWidget *body) {
  const gboolean on = gtk_toggle_button_get_active(btn);
  gtk_widget_set_visible(body, on);
  gtk_button_set_icon_name(GTK_BUTTON(btn), on ? "pan-down-symbolic" : "pan-end-symbolic");
}

static GtkWidget *advanced_row(const char *text, GtkWidget *body) {
  GtkWidget *btn = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(btn), "pan-end-symbolic");
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *arrow = gtk_image_new_from_icon_name("pan-end-symbolic");
  GtkWidget *lbl = gtk_label_new(text);
  gtk_box_append(GTK_BOX(box), arrow);
  gtk_box_append(GTK_BOX(box), lbl);
  gtk_button_set_child(GTK_BUTTON(btn), box);
  gtk_widget_add_css_class(btn, "flat");
  gtk_widget_set_halign(btn, GTK_ALIGN_START);
  gtk_widget_set_visible(body, FALSE);
  g_signal_connect(btn, "toggled", G_CALLBACK(advanced_toggled), body);
  // keep the little arrow row's own icon in sync too
  g_object_set_data(G_OBJECT(btn), "arrow", arrow);
  g_signal_connect_swapped(btn, "toggled", G_CALLBACK(gtk_widget_queue_draw), body);
  return btn;
}

// A labelled row: caption above the control, the way Rufus stacks them
// (this file uses a two-column form grid instead; see form_row below).
static GtkWidget *form_row(GtkWidget *grid, int row, const char *caption, GtkWidget *w) {
  GtkWidget *lbl = gtk_label_new(caption);
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);
  gtk_widget_set_size_request(lbl, 170, -1);
  gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
  gtk_widget_set_hexpand(w, TRUE);
  gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
  return lbl;
}

// ===== device list ==========================================================

static void refresh_devices(App *a, gboolean force) {
  if (busy(a)) return;
  RufuxDevice raw[64];
  const int n = rufux_list_devices(raw, 64,
      gtk_check_button_get_active(GTK_CHECK_BUTTON(a->chk_fixed)) ? 1 : 0);
  GString *sig = g_string_new(NULL);
  for (int i = 0; i < n; i++) g_string_append_printf(sig, "%s:%llu;", raw[i].devnode, raw[i].size_bytes);
  if (!force && !strcmp(sig->str, a->dev_signature)) { g_string_free(sig, TRUE); return; }
  snprintf(a->dev_signature, sizeof a->dev_signature, "%s", sig->str);
  g_string_free(sig, TRUE);

  GtkStringList *keepList = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(a->dev_combo)));
  char keep[128] = {0};
  guint cur = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->dev_combo));
  if (keepList && cur != GTK_INVALID_LIST_POSITION && a->ndevs > 0 && (int)cur < a->ndevs)
    snprintf(keep, sizeof keep, "%s", a->devs[cur].devnode);

  GtkStringList *model = gtk_string_list_new(NULL);
  a->ndevs = n;
  int keepIdx = -1;
  for (int i = 0; i < n; i++) {
    a->devs[i] = raw[i];
    const char *name = raw[i].model[0] ? raw[i].model : (raw[i].vendor[0] ? raw[i].vendor : "NO_LABEL");
    char sizebuf[32];
    rufux_human_size(raw[i].size_bytes, sizebuf, sizeof sizebuf);
    char *item = g_strdup_printf("%s (%s) [%s]", name, raw[i].sysname, sizebuf);
    gtk_string_list_append(model, item);
    g_free(item);
    if (keep[0] && !strcmp(keep, raw[i].devnode)) keepIdx = i;
  }
  int shown = n;
  if (n == 0 && g_getenv("RUFUX_GUI_DEMO")) {
    gtk_string_list_append(model, "SanDisk Ultra (sdb) [14.9 GB]");
    shown = 1;
  } else if (n == 0) {
    gtk_string_list_append(model, "No USB drive found");
  }
  gtk_drop_down_set_model(GTK_DROP_DOWN(a->dev_combo), G_LIST_MODEL(model));
  g_object_unref(model);
  if (keepIdx >= 0) gtk_drop_down_set_selected(GTK_DROP_DOWN(a->dev_combo), (guint)keepIdx);

  char countbuf[64];
  snprintf(countbuf, sizeof countbuf, shown == 1 ? "1 device found" : "%d devices found", shown);
  gtk_label_set_text(GTK_LABEL(a->dev_count), countbuf);
}

// The device combo has no "data" slot like Qt's, so the selected device's
// path is looked up by index into a->devs, which refresh_devices keeps in
// lock-step with the model.
static const char *selected_device(App *a) {
  guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->dev_combo));
  if (i == GTK_INVALID_LIST_POSITION || (int)i >= a->ndevs) return "";
  return a->devs[i].devnode;
}

// ===== enable/disable, matching Rufus's own rules ==========================

static void sync_enabled(App *a) {
  const gboolean run = busy(a);
  const gboolean have = a->iso_path[0] != 0;
  gtk_widget_set_visible(a->drive_image_row, iso_mode(a) && have && a->iso_windows);
  gtk_widget_set_visible(a->drive_persist_row, iso_mode(a) && have && !a->iso_windows && a->iso_valid);
  gtk_widget_set_sensitive(a->dev_combo, !run);
  gtk_widget_set_sensitive(a->boot_combo, !run);
  gtk_widget_set_sensitive(a->select_btn, !run);
  gtk_widget_set_sensitive(a->scheme_combo, !run);
  gtk_widget_set_sensitive(a->target_combo, !run);
  gtk_widget_set_sensitive(a->fs_combo, !run);
  gtk_widget_set_sensitive(a->label_entry, !run);
  const char *fs = fs_key(a);
  gtk_widget_set_sensitive(a->cluster_combo, !run && (!strcmp(fs, "vfat") || !strcmp(fs, "ntfs")));
  gtk_widget_set_sensitive(a->hash_btn, !run && have);
  gtk_widget_set_sensitive(a->close_btn, !run);
  gtk_button_set_label(GTK_BUTTON(a->start_btn), run ? "CANCEL" : "START");
}

// Windows media can be FAT32 or NTFS; the other file systems are for
// everything else. Rebuilding the model is simplest with GtkDropDown.
static void set_fs_choices(App *a, gboolean windows, const char *keep) {
  GtkStringList *model = windows
      ? gtk_string_list_new((const char *[]){"FAT32", "NTFS", NULL})
      : gtk_string_list_new((const char *[]){"FAT32", "NTFS", "exFAT", "UDF", "ext4", NULL});
  gtk_drop_down_set_model(GTK_DROP_DOWN(a->fs_combo), G_LIST_MODEL(model));
  g_object_unref(model);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(model));
  guint idx = 0;
  for (guint i = 0; i < n; i++) {
    GtkStringObject *o = GTK_STRING_OBJECT(g_list_model_get_item(G_LIST_MODEL(model), i));
    if (!strcmp(gtk_string_object_get_string(o), keep)) idx = i;
    g_object_unref(o);
  }
  gtk_drop_down_set_selected(GTK_DROP_DOWN(a->fs_combo), idx);
}

static void relabel(App *a) {
  const char *cur = gtk_editable_get_text(GTK_EDITABLE(a->label_entry));
  char out[64];
  sanitize_label(cur, fs_key(a), out, sizeof out);
  gtk_editable_set_text(GTK_EDITABLE(a->label_entry), out);
}

// ===== a small synchronous alert helper ====================================
// GTK4 has no application-modal "run and block" call any more; every dialog
// is asynchronous. Rufus's own flow (confirm before destroying a disk, pick
// ISO vs DD mode) reads far more clearly as a function that returns an
// answer, so this pumps the default main context until the async callback
// has filled in a result - scoped to just the one dialog, nothing global.
typedef struct { volatile int done; int index; } SyncChoice;

static void sync_choice_cb(GObject *dlg, GAsyncResult *res, gpointer data) {
  SyncChoice *c = data;
  c->index = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dlg), res, NULL);
  c->done = 1;
}

// buttons is a NULL-terminated array; returns the chosen index, or -1 on
// error/dismissal. cancel_idx (or -1) marks which button Esc/window-close
// maps to.
static int alert_choose_sync(GtkWindow *parent, const char *msg, const char *const *buttons, int cancel_idx) {
  GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", msg);
  gtk_alert_dialog_set_buttons(dlg, buttons);
  if (cancel_idx >= 0) gtk_alert_dialog_set_cancel_button(dlg, cancel_idx);
  SyncChoice c = {0, -1};
  gtk_alert_dialog_choose(dlg, parent, NULL, sync_choice_cb, &c);
  GMainContext *ctx = g_main_context_default();
  while (!c.done) g_main_context_iteration(ctx, TRUE);
  g_object_unref(dlg);
  return c.index;
}



typedef struct {
  App *a;
  char path[4096];
  RufuxIsoInfo info;
  int ok, windows;
} ProbeJob;

static gboolean probe_apply(gpointer data) {
  ProbeJob *j = data;
  App *a = j->a;
  snprintf(a->iso_path, sizeof a->iso_path, "%s", j->path);
  a->iso_windows = j->windows > 0;
  a->iso_valid = j->ok;
  a->iso_hybrid = j->ok && j->info.bootable;
  snprintf(a->iso_label, sizeof a->iso_label, "%s", (j->ok && j->info.label[0]) ? j->info.label : "");

  const char *base = strrchr(j->path, '/');
  base = base ? base + 1 : j->path;
  gtk_drop_down_set_model(GTK_DROP_DOWN(a->boot_combo),
      G_LIST_MODEL(gtk_string_list_new((const char *[]){"Non bootable", "FreeDOS", base, NULL})));
  g_object_unref(gtk_drop_down_get_model(GTK_DROP_DOWN(a->boot_combo)));  // drop our extra ref
  gtk_drop_down_set_selected(GTK_DROP_DOWN(a->boot_combo), 2);

  char sizebuf[32];
  rufux_human_size(j->ok ? j->info.size_bytes : 0, sizebuf, sizeof sizebuf);
  char *info = g_strdup_printf("Using image: %s", base);
  gtk_label_set_text(GTK_LABEL(a->image_info), info);
  char *logmsg = g_strdup_printf("Using image: %s (%s)", j->path, sizebuf);
  log_line(a, logmsg);
  g_free(info);
  g_free(logmsg);

  if (a->iso_windows) {
    set_fs_choices(a, TRUE, "NTFS");
    gtk_drop_down_set_selected(GTK_DROP_DOWN(a->scheme_combo), 1);  // GPT
  } else {
    set_fs_choices(a, FALSE, "FAT32");
    gtk_drop_down_set_selected(GTK_DROP_DOWN(a->scheme_combo), 0);  // MBR
  }
  char lab[64];
  sanitize_label(a->iso_label[0] ? a->iso_label : "NO_LABEL", fs_key(a), lab, sizeof lab);
  gtk_editable_set_text(GTK_EDITABLE(a->label_entry), lab);
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(a->bar), "READY");
  sync_enabled(a);
  g_free(j);
  return G_SOURCE_REMOVE;
}

static gpointer probe_thread(gpointer data) {
  ProbeJob *j = data;
  j->ok = rufux_probe_iso_detail(j->path, &j->info) == 0;
  char err[256] = {0};
  j->windows = rufux_is_windows_iso(j->path, err, sizeof err);
  g_idle_add(probe_apply, j);
  return NULL;
}

static void load_image(App *a, const char *path) {
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(a->bar), "Reading image...");
  ProbeJob *j = g_new0(ProbeJob, 1);
  j->a = a;
  snprintf(j->path, sizeof j->path, "%s", path);
  g_thread_new("probe", probe_thread, j);
}

static void on_file_chosen(GObject *chooser, GAsyncResult *res, gpointer data) {
  App *a = data;
  GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(chooser), res, NULL);
  if (!f) return;
  char *path = g_file_get_path(f);
  if (path) load_image(a, path);
  g_free(path);
  g_object_unref(f);
}

static void on_select_clicked(GtkButton *btn, gpointer data) {
  (void)btn;
  App *a = data;
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Select a disk image");
  gtk_file_dialog_open(dlg, GTK_WINDOW(a->win), NULL, on_file_chosen, a);
  g_object_unref(dlg);
}

// ===== checksums dialog =====================================================

typedef struct { char path[4096]; char *result; } HashJob;

static gboolean hash_apply(gpointer data) {
  HashJob *j = data;
  GtkWidget *view = g_object_get_data(G_OBJECT(j), "view");
  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  gtk_text_buffer_set_text(buf, j->result, -1);
  g_free(j->result);
  g_free(j);
  return G_SOURCE_REMOVE;
}

static gpointer hash_thread(gpointer data) {
  HashJob *j = data;
  FILE *f = fopen(j->path, "rb");
  if (!f) { j->result = g_strdup("Cannot open the image.\n"); g_idle_add(hash_apply, j); return NULL; }
  EVP_MD_CTX *c_md5 = EVP_MD_CTX_new(), *c_1 = EVP_MD_CTX_new(), *c_256 = EVP_MD_CTX_new(), *c_512 = EVP_MD_CTX_new();
  EVP_DigestInit_ex(c_md5, EVP_md5(), NULL);
  EVP_DigestInit_ex(c_1, EVP_sha1(), NULL);
  EVP_DigestInit_ex(c_256, EVP_sha256(), NULL);
  EVP_DigestInit_ex(c_512, EVP_sha512(), NULL);
  unsigned char buf[1 << 20];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    EVP_DigestUpdate(c_md5, buf, n); EVP_DigestUpdate(c_1, buf, n);
    EVP_DigestUpdate(c_256, buf, n); EVP_DigestUpdate(c_512, buf, n);
  }
  fclose(f);
  unsigned char h1[EVP_MAX_MD_SIZE], h2[EVP_MAX_MD_SIZE], h3[EVP_MAX_MD_SIZE], h4[EVP_MAX_MD_SIZE];
  unsigned l1, l2, l3, l4;
  EVP_DigestFinal_ex(c_md5, h1, &l1); EVP_DigestFinal_ex(c_1, h2, &l2);
  EVP_DigestFinal_ex(c_256, h3, &l3); EVP_DigestFinal_ex(c_512, h4, &l4);
  EVP_MD_CTX_free(c_md5); EVP_MD_CTX_free(c_1); EVP_MD_CTX_free(c_256); EVP_MD_CTX_free(c_512);
  GString *g = g_string_new(NULL);
  const char *names[4] = {"MD5     ", "SHA-1   ", "SHA-256 ", "SHA-512 "};
  unsigned char *hs[4] = {h1, h2, h3, h4};
  unsigned ls[4] = {l1, l2, l3, l4};
  for (int i = 0; i < 4; i++) {
    g_string_append_printf(g, "%s ", names[i]);
    for (unsigned k = 0; k < ls[i]; k++) g_string_append_printf(g, "%02x", hs[i][k]);
    g_string_append_c(g, '\n');
  }
  j->result = g_string_free(g, FALSE);
  g_idle_add(hash_apply, j);
  return NULL;
}

static void on_hash_clicked(GtkButton *btn, gpointer data) {
  (void)btn;
  App *a = data;
  GtkWidget *dlg = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dlg), "Checksums");
  gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(a->win));
  gtk_window_set_default_size(GTK_WINDOW(dlg), 620, 160);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8);
  gtk_widget_set_margin_top(box, 8); gtk_widget_set_margin_bottom(box, 8);
  GtkWidget *view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), "Computing...\n", -1);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(box), scroll);
  GtkWidget *close = gtk_button_new_with_label("Close");
  g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy), dlg);
  gtk_widget_set_halign(close, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(box), close);
  gtk_window_set_child(GTK_WINDOW(dlg), box);
  gtk_window_present(GTK_WINDOW(dlg));

  HashJob *j = g_new0(HashJob, 1);
  snprintf(j->path, sizeof j->path, "%s", a->iso_path);
  g_object_set_data(G_OBJECT(j), "view", view);
  g_thread_new("hash", hash_thread, j);
}

// ===== log window ===========================================================

static void save_log_response(GObject *chooser, GAsyncResult *res, gpointer data) {
  App *a = data;
  GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(chooser), res, NULL);
  if (!f) return;
  char *path = g_file_get_path(f);
  if (path) {
    char *joined = joined_log(a);
    GFile *out = g_file_new_for_path(path);
    g_file_replace_contents(out, joined, strlen(joined), NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, NULL);
    g_object_unref(out);
    g_free(joined);
  }
  g_free(path);
  g_object_unref(f);
}

static void on_log_save(GtkButton *b, gpointer data) {
  (void)b;
  App *a = data;
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Save log");
  gtk_file_dialog_set_initial_name(dlg, "rufux.log");
  gtk_file_dialog_save(dlg, GTK_WINDOW(a->log_win), NULL, save_log_response, a);
  g_object_unref(dlg);
}

static void on_log_clear(GtkButton *b, gpointer data) {
  (void)b;
  App *a = data;
  g_ptr_array_set_size(a->log_lines, 0);
  gtk_text_buffer_set_text(a->log_buffer, "", -1);
}

static gboolean on_log_close_request(GtkWindow *win, gpointer data) {
  (void)data;
  gtk_widget_set_visible(GTK_WIDGET(win), FALSE);
  return TRUE;  // handled: hide instead of destroy, so log_win can be reused
}

static void on_log_close_clicked(GtkButton *b, gpointer data) {
  (void)b;
  App *a = data;
  gtk_widget_set_visible(a->log_win, FALSE);
}

static void show_log(App *a) {
  if (!a->log_win) {
    a->log_win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(a->log_win), "Log");
    gtk_window_set_transient_for(GTK_WINDOW(a->log_win), GTK_WINDOW(a->win));
    gtk_window_set_default_size(GTK_WINDOW(a->log_win), 640, 380);
    g_signal_connect(a->log_win, "close-request", G_CALLBACK(on_log_close_request), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8); gtk_widget_set_margin_bottom(box, 8);
    a->log_view = gtk_text_view_new_with_buffer(a->log_buffer);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(a->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(a->log_view), TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), a->log_view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), scroll);
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(foot, GTK_ALIGN_END);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    GtkWidget *save = gtk_button_new_with_label("Save");
    GtkWidget *close = gtk_button_new_with_label("Close");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_log_clear), a);
    g_signal_connect(save, "clicked", G_CALLBACK(on_log_save), a);
    g_signal_connect(close, "clicked", G_CALLBACK(on_log_close_clicked), a);
    gtk_box_append(GTK_BOX(foot), clear);
    gtk_box_append(GTK_BOX(foot), save);
    gtk_box_append(GTK_BOX(foot), close);
    gtk_box_append(GTK_BOX(box), foot);
    gtk_window_set_child(GTK_WINDOW(a->log_win), box);
  }
  gtk_window_present(GTK_WINDOW(a->log_win));
}

// ===== About dialog ==========================================================

static void show_about(App *a) {
  GtkWidget *dlg = gtk_about_dialog_new();
  gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(a->win));
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dlg), "Rufux");
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dlg), RUFUX_VERSION);
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dlg), "Create bootable USB drives on Linux.");
  gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dlg),
      "A Linux port of Rufus by Pete Batard. License: GNU General Public License, version 3.");
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dlg), "https://github.com/Hultwl/Rufux");
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dlg), "github.com/Hultwl/Rufux");
  gtk_window_present(GTK_WINDOW(dlg));
}

// ===== Windows User Experience dialog (Rufus's wording, verbatim) =========

typedef struct {
  GtkWidget *bypass, *nro, *user_on, *user_entry, *locale, *privacy, *bitlocker, *qol;
} WueWidgets;

static char *wue_config_path(void) {
  char *dir = g_build_filename(g_get_user_config_dir(), "rufux", NULL);
  g_mkdir_with_parents(dir, 0700);
  char *path = g_build_filename(dir, "wue.conf", NULL);
  g_free(dir);
  return path;
}

static gboolean wue_get_bool(GKeyFile *kf, const char *key, gboolean def) {
  GError *e = NULL;
  gboolean v = g_key_file_get_boolean(kf, "wue", key, &e);
  if (e) { g_error_free(e); return def; }
  return v;
}

static void wue_set_result_ok(GtkButton *b, gpointer data) { (void)b; *(int *)data = 1; }
static void wue_set_result_cancel(GtkButton *b, gpointer data) { (void)b; *(int *)data = -1; }

// Runs the dialog; on OK fills *wue and appends any extra CLI args (locale,
// timezone) to extra. Returns FALSE if the user cancelled.
static gboolean ask_windows_options(App *a, char *wue_out, size_t wue_cap, GPtrArray *extra) {
  char *cfgpath = wue_config_path();
  GKeyFile *kf = g_key_file_new();
  g_key_file_load_from_file(kf, cfgpath, G_KEY_FILE_NONE, NULL);

  GtkWidget *dlg = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dlg), "Windows User Experience");
  gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(a->win));
  gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
  gtk_box_append(GTK_BOX(box), gtk_label_new("Customize Windows installation?"));

  WueWidgets w = {0};
  w.bypass = gtk_check_button_new_with_label("Remove requirement for 4GB+ RAM, Secure Boot and TPM 2.0");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(w.bypass), wue_get_bool(kf, "bypass", FALSE));
  gtk_box_append(GTK_BOX(box), w.bypass);

  w.nro = gtk_check_button_new_with_label("Remove requirement for an online Microsoft account");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(w.nro), wue_get_bool(kf, "nro", FALSE));
  gtk_box_append(GTK_BOX(box), w.nro);

  GtkWidget *user_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  w.user_on = gtk_check_button_new_with_label("Create a local account with username:");
  gboolean user_on_default = wue_get_bool(kf, "user_on", FALSE);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(w.user_on), user_on_default);
  char *saved_user = g_key_file_get_string(kf, "wue", "user", NULL);
  w.user_entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(w.user_entry), saved_user && *saved_user ? saved_user : g_get_user_name());
  g_free(saved_user);
  gtk_widget_set_sensitive(w.user_entry, user_on_default);
  g_signal_connect_swapped(w.user_on, "toggled", G_CALLBACK(gtk_widget_set_sensitive), w.user_entry);
  gtk_widget_set_hexpand(w.user_entry, TRUE);
  gtk_box_append(GTK_BOX(user_row), w.user_on);
  gtk_box_append(GTK_BOX(user_row), w.user_entry);
  gtk_box_append(GTK_BOX(box), user_row);

  w.locale = gtk_check_button_new_with_label("Set regional options to the same values as this user's");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(w.locale), wue_get_bool(kf, "locale", FALSE));
  gtk_box_append(GTK_BOX(box), w.locale);

  w.privacy = gtk_check_button_new_with_label("Disable data collection (Skip privacy questions)");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(w.privacy), wue_get_bool(kf, "privacy", FALSE));
  gtk_box_append(GTK_BOX(box), w.privacy);

  w.bitlocker = gtk_check_button_new_with_label("Disable BitLocker automatic device encryption");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(w.bitlocker), wue_get_bool(kf, "bitlocker", FALSE));
  gtk_box_append(GTK_BOX(box), w.bitlocker);

  w.qol = gtk_check_button_new_with_label(
      "QoL improvements (Don't force Copilot, OneDrive, Outlook, Fast Startup, etc.)");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(w.qol), wue_get_bool(kf, "qol", FALSE));
  gtk_box_append(GTK_BOX(box), w.qol);

  GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(foot, GTK_ALIGN_END);
  GtkWidget *cancel = gtk_button_new_with_label("Cancel");
  GtkWidget *ok = gtk_button_new_with_label("OK");
  gtk_widget_add_css_class(ok, "suggested-action");
  gtk_box_append(GTK_BOX(foot), cancel);
  gtk_box_append(GTK_BOX(foot), ok);
  gtk_box_append(GTK_BOX(box), foot);
  gtk_window_set_child(GTK_WINDOW(dlg), box);
  gtk_window_present(GTK_WINDOW(dlg));

  // Same pump-the-main-context pattern as alert_choose_sync, but this is a
  // hand-built dialog (checkboxes, an entry), not a GtkAlertDialog, so the
  // result is tracked by hand rather than through its finish() call.
  int result = 0;  // 0 pending, 1 ok, -1 cancel
  g_signal_connect(ok, "clicked", G_CALLBACK(wue_set_result_ok), &result);
  g_signal_connect(cancel, "clicked", G_CALLBACK(wue_set_result_cancel), &result);
  g_signal_connect_swapped(ok, "clicked", G_CALLBACK(gtk_window_close), dlg);
  g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_close), dlg);
  GMainContext *ctx = g_main_context_default();
  while (result == 0) g_main_context_iteration(ctx, TRUE);


  gboolean accepted = (result == 1);
  gboolean bypass_on = gtk_check_button_get_active(GTK_CHECK_BUTTON(w.bypass));
  gboolean nro_on = gtk_check_button_get_active(GTK_CHECK_BUTTON(w.nro));
  gboolean user_on = gtk_check_button_get_active(GTK_CHECK_BUTTON(w.user_on));
  gboolean locale_on = gtk_check_button_get_active(GTK_CHECK_BUTTON(w.locale));
  gboolean privacy_on = gtk_check_button_get_active(GTK_CHECK_BUTTON(w.privacy));
  gboolean bitlocker_on = gtk_check_button_get_active(GTK_CHECK_BUTTON(w.bitlocker));
  gboolean qol_on = gtk_check_button_get_active(GTK_CHECK_BUTTON(w.qol));
  const char *uname = gtk_editable_get_text(GTK_EDITABLE(w.user_entry));
  char uname_buf[256];
  snprintf(uname_buf, sizeof uname_buf, "%s", uname ? uname : "");
  g_strstrip(uname_buf);

  gtk_window_destroy(GTK_WINDOW(dlg));

  if (!accepted) { g_key_file_free(kf); g_free(cfgpath); return FALSE; }
  if (user_on && !uname_buf[0]) {
    GtkAlertDialog *warn = gtk_alert_dialog_new("Please enter a user name.");
    gtk_alert_dialog_show(warn, GTK_WINDOW(a->win));
    g_object_unref(warn);
    g_key_file_free(kf); g_free(cfgpath);
    return ask_windows_options(a, wue_out, wue_cap, extra);  // ask again, as Rufux always has
  }

  g_key_file_set_boolean(kf, "wue", "bypass", bypass_on);
  g_key_file_set_boolean(kf, "wue", "nro", nro_on);
  g_key_file_set_boolean(kf, "wue", "user_on", user_on);
  g_key_file_set_string(kf, "wue", "user", uname_buf);
  g_key_file_set_boolean(kf, "wue", "locale", locale_on);
  g_key_file_set_boolean(kf, "wue", "privacy", privacy_on);
  g_key_file_set_boolean(kf, "wue", "bitlocker", bitlocker_on);
  g_key_file_set_boolean(kf, "wue", "qol", qol_on);
  g_key_file_save_to_file(kf, cfgpath, NULL);
  g_key_file_free(kf);
  g_free(cfgpath);

  GString *v = g_string_new(NULL);
  if (bypass_on) g_string_append(v, v->len ? ",bypass" : "bypass");
  if (nro_on) g_string_append(v, v->len ? ",nro" : "nro");
  if (privacy_on) g_string_append(v, v->len ? ",privacy" : "privacy");
  if (bitlocker_on) g_string_append(v, v->len ? ",bitlocker" : "bitlocker");
  if (qol_on) g_string_append(v, v->len ? ",qol" : "qol");
  if (locale_on) {
    g_string_append(v, v->len ? ",locale" : "locale");
    const char *loc = g_getenv("LANG");
    char tag[16] = "en-US";
    if (loc && *loc) {
      snprintf(tag, sizeof tag, "%s", loc);
      char *dot = strchr(tag, '.'); if (dot) *dot = 0;
      for (char *p = tag; *p; p++) if (*p == '_') *p = '-';
      if (!strcmp(tag, "C")) snprintf(tag, sizeof tag, "en-US");
    }
    GTimeZone *tz = g_time_zone_new_local();
    const char *tzid = g_time_zone_get_identifier(tz);
    g_ptr_array_add(extra, g_strdup("--locale"));
    g_ptr_array_add(extra, g_strdup(tag));
    g_ptr_array_add(extra, g_strdup("--timezone"));
    g_ptr_array_add(extra, g_strdup(tzid));
    g_time_zone_unref(tz);
  }
  if (user_on) { g_string_append(v, v->len ? "," : ""); g_string_append_printf(v, "user=%s", uname_buf); }
  snprintf(wue_out, wue_cap, "%s", v->len ? v->str : "none");
  g_string_free(v, TRUE);
  return TRUE;
}

// ===== ISOHybrid mode question, Rufus's wording verbatim ===================

// Returns "extract", "dd", or "" (cancelled).
static const char *ask_iso_mode_sync(App *a) {
  static char out[16];
  const char *msg =
      "The image you have selected is an 'ISOHybrid' image. This means it can be written either in "
      "ISO Image (file copy) mode or DD Image (disk image) mode.\n"
      "Rufux recommends using ISO Image mode, so that you always have full access to the drive after writing it.\n"
      "However, if you encounter issues during boot, you can try writing this image again in DD Image mode.\n\n"
      "Please select the mode that you want to use to write this image:";
  const char *buttons[] = {"Write in ISO Image mode (Recommended)", "Write in DD Image mode", "Cancel", NULL};
  int idx = alert_choose_sync(GTK_WINDOW(a->win), msg, buttons, 2);
  if (idx == 0) snprintf(out, sizeof out, "extract");
  else if (idx == 1) snprintf(out, sizeof out, "dd");
  else out[0] = 0;
  return out;
}

// ===== worker process (create) =============================================

static void worker_finished(App *a, int exit_status, gboolean normal);

static void on_worker_line(GObject *stream, GAsyncResult *res, gpointer data);

static void read_next_line(App *a, GDataInputStream *dis) {
  g_data_input_stream_read_line_async(dis, G_PRIORITY_DEFAULT, NULL, on_worker_line, a);
}

static void on_worker_line(GObject *stream, GAsyncResult *res, gpointer data) {
  App *a = data;
  GDataInputStream *dis = G_DATA_INPUT_STREAM(stream);
  gsize len = 0;
  GError *err = NULL;
  char *line = g_data_input_stream_read_line_finish(dis, res, &len, &err);
  if (err) g_error_free(err);
  if (!line) { g_object_unref(dis); return; }  // EOF: process exit is handled separately
  // A worker line may carry \r-separated progress updates (dd-style).
  char *save = NULL;
  for (char *part = strtok_r(line, "\r", &save); part; part = strtok_r(NULL, "\r", &save)) {
    g_strstrip(part);
    if (!*part || *part == '+') continue;
    int pct = -1, consumed = 0;
    if (sscanf(part, "%d%%%n", &pct, &consumed) == 1 && pct >= 0 && pct <= 100) {
      gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->bar), pct / 100.0);
      char *rest = part + consumed;
      g_strstrip(rest);
      if (!*rest) continue;
      part = rest;
    }
    log_line(a, part);
    if (strcmp(part, "Done.") != 0) snprintf(a->last_error, sizeof a->last_error, "%s", part);
  }
  g_free(line);
  read_next_line(a, dis);
}

static void on_worker_exit(GObject *proc, GAsyncResult *res, gpointer data) {
  App *a = data;
  GError *err = NULL;
  gboolean ok = g_subprocess_wait_check_finish(G_SUBPROCESS(proc), res, &err);
  int code = g_subprocess_get_exit_status(G_SUBPROCESS(proc));
  g_object_unref(a->worker);
  a->worker = NULL;
  sync_enabled(a);
  if (ok) {
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->bar), 1.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(a->bar), "READY");
    log_line(a, "Done.");
    if (err) g_error_free(err);
    return;
  }
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(a->bar), "READY");
  char why[1200];
  if (code == 126 || code == 127) snprintf(why, sizeof why, "Authorization was cancelled or pkexec is unavailable.");
  else if (a->last_error[0]) snprintf(why, sizeof why, "%s", a->last_error);
  else snprintf(why, sizeof why, "The worker exited with code %d.", code);
  char *msg = g_strdup_printf("Failed: %s", why);
  log_line(a, msg);
  g_free(msg);
  GtkAlertDialog *box = gtk_alert_dialog_new("%s", why);
  gtk_alert_dialog_show(box, GTK_WINDOW(a->win));
  g_object_unref(box);
  if (err) g_error_free(err);
}

static void cancel_worker(App *a) {
  if (!a->worker) return;
  log_line(a, "Cancel requested.");
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(a->bar), "Cancelling...");
  g_subprocess_send_signal(a->worker, SIGTERM);
}

static void on_start_clicked(GtkButton *btn, gpointer data) {
  (void)btn;
  App *a = data;
  if (busy(a)) { cancel_worker(a); return; }
  const char *dst = selected_device(a);
  if (!dst[0]) return;

  const char *mode;
  guint boot_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->boot_combo));
  if (boot_idx == 1) mode = "dos";
  else if (boot_idx == 0) mode = "format";
  else {
    if (!a->iso_path[0]) {
      GtkAlertDialog *w = gtk_alert_dialog_new("Please select a disk or ISO image.");
      gtk_alert_dialog_show(w, GTK_WINDOW(a->win));
      g_object_unref(w);
      return;
    }
    if (a->iso_windows) mode = "windows";
    else if (!a->iso_valid) mode = "dd";
    else if (a->iso_hybrid) {
      const char *m = ask_iso_mode_sync(a);
      if (!m[0]) return;
      mode = m;
    } else mode = "extract";
  }

  char wue[256] = "none";
  GPtrArray *extra = g_ptr_array_new_with_free_func(g_free);
  if (!strcmp(mode, "windows") && !ask_windows_options(a, wue, sizeof wue, extra)) {
    g_ptr_array_free(extra, TRUE);
    return;
  }

  const char *devname = gtk_string_object_get_string(
      GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(GTK_DROP_DOWN(a->dev_combo))));
  char destroy_warn[600];
  snprintf(destroy_warn, sizeof destroy_warn,
      "WARNING: ALL DATA ON DEVICE '%s' WILL BE DESTROYED.\n"
      "To continue with this operation, click OK. To quit click CANCEL.", devname);
  const char *ok_cancel[] = {"Cancel", "OK", NULL};
  int confirmed = alert_choose_sync(GTK_WINDOW(a->win), destroy_warn, ok_cancel, 0);
  if (confirmed != 1) { g_ptr_array_free(extra, TRUE); return; }

  const char *fs = fs_key(a);
  char label[64];
  sanitize_label(gtk_editable_get_text(GTK_EDITABLE(a->label_entry)), fs, label, sizeof label);
  static const int cluster_sectors[] = {1, 2, 4, 0, 16, 32, 64, 128};
  guint cluster_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->cluster_combo));
  int csectors = gtk_widget_get_sensitive(a->cluster_combo) ? cluster_sectors[cluster_idx] : 0;
  int persist_mb = !strcmp(mode, "extract")
      ? (int)gtk_range_get_value(GTK_RANGE(a->persist_scale)) * 512 : 0;
  int badpasses = gtk_check_button_get_active(GTK_CHECK_BUTTON(a->chk_bad))
      ? (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(a->passes_combo)) + 1 : 0;
  gboolean gpt = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->scheme_combo)) == 1;

  GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
  char *self_path = g_file_read_link("/proc/self/exe", NULL);
  if (!self_path) self_path = g_strdup("rufux");
  const char *appimage_env = g_getenv("APPIMAGE");
  char *prog;
  if (geteuid() != 0) {
    prog = g_strdup("pkexec");
    char *pk = g_find_program_in_path("pkexec");
    if (!pk) {
      GtkAlertDialog *w = gtk_alert_dialog_new("pkexec was not found. Install polkit, or run Rufux as root.");
      gtk_alert_dialog_show(w, GTK_WINDOW(a->win));
      g_object_unref(w);
      g_free(prog); g_free(self_path);
      g_ptr_array_free(extra, TRUE);
      return;
    }
    g_free(pk);
    g_ptr_array_add(args, g_strdup(prog));
    g_ptr_array_add(args, g_strdup((appimage_env && *appimage_env) ? appimage_env : self_path));
  } else {
    prog = (appimage_env && *appimage_env) ? g_strdup(appimage_env) : g_strdup(self_path);
    g_ptr_array_add(args, g_strdup(prog));
  }
  g_free(self_path);
  g_ptr_array_add(args, g_strdup("create"));
  g_ptr_array_add(args, g_strdup((!strcmp(mode, "dos") || !strcmp(mode, "format")) ? "none" : a->iso_path));
  g_ptr_array_add(args, g_strdup(dst));
  g_ptr_array_add(args, g_strdup("--mode")); g_ptr_array_add(args, g_strdup(mode));
  g_ptr_array_add(args, g_strdup("--scheme")); g_ptr_array_add(args, g_strdup(gpt ? "gpt" : "dos"));
  g_ptr_array_add(args, g_strdup("--fs")); g_ptr_array_add(args, g_strdup(fs));
  g_ptr_array_add(args, g_strdup("--label")); g_ptr_array_add(args, g_strdup(label));
  g_ptr_array_add(args, g_strdup("--persist-mb")); g_ptr_array_add(args, g_strdup_printf("%d", persist_mb));
  g_ptr_array_add(args, g_strdup("--cluster-sectors")); g_ptr_array_add(args, g_strdup_printf("%d", csectors));
  g_ptr_array_add(args, g_strdup("--badblock-passes")); g_ptr_array_add(args, g_strdup_printf("%d", badpasses));
  g_ptr_array_add(args, g_strdup(gtk_check_button_get_active(GTK_CHECK_BUTTON(a->chk_quick)) ? "--quick" : "--full"));
  if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(a->chk_ext))) g_ptr_array_add(args, g_strdup("--no-autorun"));
  if (!strcmp(mode, "windows")) {
    g_ptr_array_add(args, g_strdup("--wue")); g_ptr_array_add(args, g_strdup(wue));
    for (guint i = 0; i < extra->len; i++) g_ptr_array_add(args, g_strdup(g_ptr_array_index(extra, i)));
  }
  g_ptr_array_add(args, g_strdup("--verify"));
  if (gtk_check_button_get_active(GTK_CHECK_BUTTON(a->chk_fixed))) g_ptr_array_add(args, g_strdup("--allow-fixed"));
  g_ptr_array_add(args, g_strdup("--real"));
  g_ptr_array_add(args, g_strdup("--yes"));
  g_ptr_array_add(args, NULL);
  g_ptr_array_free(extra, TRUE);
  g_free(prog);

  a->last_error[0] = 0;
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->bar), 0);
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(a->bar), "%");
  char *startmsg = g_strdup_printf("Starting: %s -> %s", mode, dst);
  log_line(a, startmsg);
  g_free(startmsg);

  GError *err = NULL;
  GSubprocess *proc = g_subprocess_newv((const char *const *)args->pdata,
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, &err);
  g_ptr_array_free(args, TRUE);
  if (!proc) {
    log_line(a, err ? err->message : "Could not start the worker process.");
    if (err) g_error_free(err);
    return;
  }
  a->worker = proc;
  GInputStream *raw = g_subprocess_get_stdout_pipe(proc);
  GDataInputStream *dis = g_data_input_stream_new(raw);
  read_next_line(a, dis);
  g_subprocess_wait_check_async(proc, NULL, on_worker_exit, a);
  sync_enabled(a);
}

// ===== window construction ==================================================

static void on_close_clicked(GtkButton *b, gpointer data) { (void)b; App *a = data; gtk_window_close(GTK_WINDOW(a->win)); }

static gboolean on_close_request(GtkWindow *win, gpointer data) {
  (void)win;
  App *a = data;
  if (busy(a)) {
    GtkAlertDialog *w = gtk_alert_dialog_new("An operation is in progress. Press CANCEL first.");
    gtk_alert_dialog_show(w, GTK_WINDOW(a->win));
    g_object_unref(w);
    return TRUE;  // stop the close
  }
  return FALSE;
}

static gboolean poll_tick(gpointer data) {
  App *a = data;
  refresh_devices(a, FALSE);
  return G_SOURCE_CONTINUE;
}

static void on_chk_fixed_toggled(GtkCheckButton *b, gpointer data) { (void)b; refresh_devices((App *)data, TRUE); }
static void on_chk_bad_toggled(GtkCheckButton *b, gpointer data) {
  gtk_widget_set_sensitive(GTK_WIDGET(data), gtk_check_button_get_active(b));
}
static void on_boot_changed(GObject *o, GParamSpec *p, gpointer data) { (void)o; (void)p; sync_enabled((App *)data); }
static void on_fs_changed(GObject *o, GParamSpec *p, gpointer data) { (void)o; (void)p; App *a = data; relabel(a); sync_enabled(a); }
static void on_persist_changed(GtkRange *r, gpointer data) {
  App *a = data;
  int v = (int)gtk_range_get_value(r);
  char buf[32];
  if (v == 0) snprintf(buf, sizeof buf, "No persistence");
  else if (v % 2) snprintf(buf, sizeof buf, "%d MB", v * 512);
  else snprintf(buf, sizeof buf, "%d GB", v / 2);
  gtk_label_set_text(GTK_LABEL(a->persist_value), buf);
}
static void on_target_changed(GObject *o, GParamSpec *p, gpointer data) {
  (void)o; (void)p; App *a = data;
  guint t = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->target_combo));
  g_signal_handlers_block_matched(a->scheme_combo, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_target_changed, NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(a->scheme_combo), t == 1 ? 1 : 0);
  g_signal_handlers_unblock_matched(a->scheme_combo, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_target_changed, NULL);
}
static void on_scheme_changed(GObject *o, GParamSpec *p, gpointer data) {
  (void)o; (void)p; App *a = data;
  guint s = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->scheme_combo));
  gtk_drop_down_set_selected(GTK_DROP_DOWN(a->target_combo), s == 1 ? 1 : 0);
}

static GtkWidget *string_dropdown(const char *const *items) {
  return gtk_drop_down_new_from_strings(items);
}

static void build_ui(App *a, GtkApplication *gapp) {
  a->win = gtk_application_window_new(gapp);
  char title[64];
  snprintf(title, sizeof title, "Rufux %s", RUFUX_VERSION);
  gtk_window_set_title(GTK_WINDOW(a->win), title);
  gtk_window_set_default_size(GTK_WINDOW(a->win), 540, -1);
  g_signal_connect(a->win, "close-request", G_CALLBACK(on_close_request), a);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(root, 10); gtk_widget_set_margin_end(root, 10);
  gtk_widget_set_margin_top(root, 10); gtk_widget_set_margin_bottom(root, 10);
  gtk_window_set_child(GTK_WINDOW(a->win), root);

  // ---- Drive Properties ----
  GtkWidget *g1 = gtk_frame_new("Drive Properties");
  GtkWidget *v1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(v1, 8); gtk_widget_set_margin_end(v1, 8);
  gtk_widget_set_margin_top(v1, 8); gtk_widget_set_margin_bottom(v1, 8);
  gtk_frame_set_child(GTK_FRAME(g1), v1);
  GtkWidget *drive_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(drive_grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(drive_grid), 10);

  a->dev_combo = gtk_drop_down_new(NULL, NULL);
  form_row(drive_grid, 0, "Device", a->dev_combo);

  GtkWidget *boot_items_holder = string_dropdown((const char *[]){"Non bootable", "FreeDOS",
      "Disk or ISO image (Please select)", NULL});
  a->boot_combo = boot_items_holder;
  gtk_drop_down_set_selected(GTK_DROP_DOWN(a->boot_combo), 2);
  a->select_btn = gtk_button_new_with_label("SELECT");
  a->hash_btn = gtk_button_new_with_label("\xE2\x9C\x93");
  gtk_widget_set_tooltip_text(a->hash_btn, "Compute the checksums of the image");
  GtkWidget *boot_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand(a->boot_combo, TRUE);
  gtk_box_append(GTK_BOX(boot_row), a->boot_combo);
  gtk_box_append(GTK_BOX(boot_row), a->hash_btn);
  gtk_box_append(GTK_BOX(boot_row), a->select_btn);
  form_row(drive_grid, 1, "Boot selection", boot_row);

  a->image_combo = string_dropdown((const char *[]){"Standard Windows installation", NULL});
  a->drive_image_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(a->drive_image_row), a->image_combo);
  GtkWidget *img_lbl = form_row(drive_grid, 2, "Image Option", a->drive_image_row);
  gtk_widget_set_visible(a->drive_image_row, FALSE);
  gtk_widget_set_visible(img_lbl, FALSE);
  g_object_set_data(G_OBJECT(a->drive_image_row), "label", img_lbl);

  a->persist_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 128, 1);
  gtk_scale_set_draw_value(GTK_SCALE(a->persist_scale), FALSE);
  a->persist_value = gtk_label_new("No persistence");
  gtk_widget_set_size_request(a->persist_value, 100, -1);
  GtkWidget *persist_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand(a->persist_scale, TRUE);
  gtk_box_append(GTK_BOX(persist_box), a->persist_scale);
  gtk_box_append(GTK_BOX(persist_box), a->persist_value);
  a->drive_persist_row = persist_box;
  GtkWidget *persist_lbl = form_row(drive_grid, 3, "Persistent partition size", persist_box);
  gtk_widget_set_visible(persist_box, FALSE);
  gtk_widget_set_visible(persist_lbl, FALSE);
  g_object_set_data(G_OBJECT(persist_box), "label", persist_lbl);

  a->scheme_combo = string_dropdown((const char *[]){"MBR", "GPT", NULL});
  form_row(drive_grid, 4, "Partition scheme", a->scheme_combo);
  a->target_combo = string_dropdown((const char *[]){"BIOS (or UEFI-CSM)", "UEFI (non CSM)", NULL});
  form_row(drive_grid, 5, "Target system", a->target_combo);
  gtk_box_append(GTK_BOX(v1), drive_grid);

  GtkWidget *adv_drive = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(adv_drive, 20);
  a->chk_fixed = gtk_check_button_new_with_label("List USB Hard Drives");
  gtk_box_append(GTK_BOX(adv_drive), a->chk_fixed);
  gtk_box_append(GTK_BOX(v1), advanced_row("Show advanced drive properties", adv_drive));
  gtk_box_append(GTK_BOX(v1), adv_drive);
  gtk_box_append(GTK_BOX(root), g1);

  // ---- Format Options ----
  GtkWidget *g2 = gtk_frame_new("Format Options");
  GtkWidget *v2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(v2, 8); gtk_widget_set_margin_end(v2, 8);
  gtk_widget_set_margin_top(v2, 8); gtk_widget_set_margin_bottom(v2, 8);
  gtk_frame_set_child(GTK_FRAME(g2), v2);
  GtkWidget *format_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(format_grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(format_grid), 10);
  a->label_entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(a->label_entry), "NO_LABEL");
  form_row(format_grid, 0, "Volume label", a->label_entry);
  a->fs_combo = string_dropdown((const char *[]){"FAT32", "NTFS", "exFAT", "UDF", "ext4", NULL});
  form_row(format_grid, 1, "File system", a->fs_combo);
  a->cluster_combo = string_dropdown((const char *[]){"512 bytes", "1024 bytes", "2048 bytes",
      "4096 bytes (Default)", "8192 bytes", "16 kilobytes", "32 kilobytes", "64 kilobytes", NULL});
  gtk_drop_down_set_selected(GTK_DROP_DOWN(a->cluster_combo), 3);
  form_row(format_grid, 2, "Cluster size", a->cluster_combo);
  gtk_box_append(GTK_BOX(v2), format_grid);

  GtkWidget *adv_fmt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(adv_fmt, 20);
  a->chk_quick = gtk_check_button_new_with_label("Quick format");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(a->chk_quick), TRUE);
  a->chk_ext = gtk_check_button_new_with_label("Create extended label and icon files");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(a->chk_ext), TRUE);
  GtkWidget *bad_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  a->chk_bad = gtk_check_button_new_with_label("Check device for bad blocks");
  a->passes_combo = string_dropdown((const char *[]){"1 pass", "2 passes", "3 passes", "4 passes", NULL});
  gtk_widget_set_sensitive(a->passes_combo, FALSE);
  gtk_box_append(GTK_BOX(bad_row), a->chk_bad);
  gtk_box_append(GTK_BOX(bad_row), a->passes_combo);
  gtk_box_append(GTK_BOX(adv_fmt), a->chk_quick);
  gtk_box_append(GTK_BOX(adv_fmt), a->chk_ext);
  gtk_box_append(GTK_BOX(adv_fmt), bad_row);
  gtk_box_append(GTK_BOX(v2), advanced_row("Show advanced format options", adv_fmt));
  gtk_box_append(GTK_BOX(v2), adv_fmt);
  gtk_box_append(GTK_BOX(root), g2);

  // ---- Status ----
  GtkWidget *g3 = gtk_frame_new("Status");
  GtkWidget *v3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(v3, 8); gtk_widget_set_margin_end(v3, 8);
  gtk_widget_set_margin_top(v3, 8); gtk_widget_set_margin_bottom(v3, 8);
  gtk_frame_set_child(GTK_FRAME(g3), v3);
  a->bar = gtk_progress_bar_new();
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(a->bar), TRUE);
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(a->bar), "READY");
  gtk_box_append(GTK_BOX(v3), a->bar);
  GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  a->about_btn = gtk_button_new_with_label("i");
  gtk_widget_set_tooltip_text(a->about_btn, "About Rufux");
  a->log_btn = gtk_button_new_with_label("Log");
  gtk_widget_set_tooltip_text(a->log_btn, "Show the log");
  a->start_btn = gtk_button_new_with_label("START");
  gtk_widget_add_css_class(a->start_btn, "suggested-action");
  a->close_btn = gtk_button_new_with_label("CLOSE");
  gtk_box_append(GTK_BOX(foot), a->about_btn);
  gtk_box_append(GTK_BOX(foot), a->log_btn);
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(foot), spacer);
  gtk_box_append(GTK_BOX(foot), a->start_btn);
  gtk_box_append(GTK_BOX(foot), a->close_btn);
  gtk_box_append(GTK_BOX(v3), foot);
  gtk_box_append(GTK_BOX(root), g3);

  GtkWidget *info = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  a->dev_count = gtk_label_new("");
  a->image_info = gtk_label_new("");
  gtk_widget_set_halign(a->image_info, GTK_ALIGN_END);
  gtk_widget_set_hexpand(a->image_info, TRUE);
  gtk_box_append(GTK_BOX(info), a->dev_count);
  gtk_box_append(GTK_BOX(info), a->image_info);
  gtk_box_append(GTK_BOX(root), info);

  // ---- wiring ----
  g_signal_connect(a->select_btn, "clicked", G_CALLBACK(on_select_clicked), a);
  g_signal_connect(a->hash_btn, "clicked", G_CALLBACK(on_hash_clicked), a);
  g_signal_connect(a->start_btn, "clicked", G_CALLBACK(on_start_clicked), a);
  g_signal_connect(a->close_btn, "clicked", G_CALLBACK(on_close_clicked), a);
  g_signal_connect_swapped(a->log_btn, "clicked", G_CALLBACK(show_log), a);
  g_signal_connect_swapped(a->about_btn, "clicked", G_CALLBACK(show_about), a);
  g_signal_connect(a->chk_bad, "toggled", G_CALLBACK(on_chk_bad_toggled), a->passes_combo);
  g_signal_connect(a->chk_fixed, "toggled", G_CALLBACK(on_chk_fixed_toggled), a);
  g_signal_connect(a->boot_combo, "notify::selected", G_CALLBACK(on_boot_changed), a);
  g_signal_connect(a->fs_combo, "notify::selected", G_CALLBACK(on_fs_changed), a);
  g_signal_connect(a->persist_scale, "value-changed", G_CALLBACK(on_persist_changed), a);
  g_signal_connect(a->target_combo, "notify::selected", G_CALLBACK(on_target_changed), a);
  g_signal_connect(a->scheme_combo, "notify::selected", G_CALLBACK(on_scheme_changed), a);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(a->scheme_combo), 1);  // GPT, UEFI (non CSM)
  gtk_drop_down_set_selected(GTK_DROP_DOWN(a->target_combo), 1);
}

// ===== screenshot support (RUFUX_GUI_SNAPSHOT=path.png, for CI/docs) ======

static gboolean save_snapshot(gpointer data) {
  App *a = data;
  GtkNative *native = gtk_widget_get_native(a->win);
  GskRenderer *renderer = gtk_native_get_renderer(native);
  int w = gtk_widget_get_width(a->win), h = gtk_widget_get_height(a->win);
  GtkSnapshot *snap = gtk_snapshot_new();
  gtk_widget_snapshot_child(a->win, gtk_widget_get_first_child(a->win), snap);
  GskRenderNode *node = gtk_snapshot_free_to_node(snap);
  if (node && renderer) {
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, w, h);
    GdkTexture *tex = gsk_renderer_render_texture(renderer, node, &bounds);
    if (tex) {
      const char *path = g_getenv("RUFUX_GUI_SNAPSHOT");
      gdk_texture_save_to_png(tex, path);
      g_object_unref(tex);
    }
    gsk_render_node_unref(node);
  }
  g_application_quit(g_application_get_default());
  return G_SOURCE_REMOVE;
}

// ===== entry point ==========================================================

static void activate(GtkApplication *gapp, gpointer data) {
  App *a = data;
  build_ui(a, gapp);
  a->log_lines = g_ptr_array_new_with_free_func(g_free);
  a->log_buffer = gtk_text_buffer_new(NULL);
  char welcome[64];
  snprintf(welcome, sizeof welcome, "Rufux %s", RUFUX_VERSION);
  log_line(a, welcome);
  refresh_devices(a, TRUE);
  a->poll_id = g_timeout_add(2000, poll_tick, a);
  sync_enabled(a);
  gtk_window_present(GTK_WINDOW(a->win));

  const char *snap = g_getenv("RUFUX_GUI_SNAPSHOT");
  if (snap) g_timeout_add(400, save_snapshot, a);
}

int rufux_gui_run(int argc, char **argv) {
  // Our own options and a bare image path are consumed before GTK sees argv.
  char open_file[4096] = {0};
  int gargc = 1;
  char **gargv = g_new0(char *, argc + 1);
  gargv[0] = argv[0];
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--gui") || !strcmp(argv[i], "gui")) continue;
    if (argv[i][0] != '-' && g_file_test(argv[i], G_FILE_TEST_IS_REGULAR)) {
      snprintf(open_file, sizeof open_file, "%s", argv[i]);
      continue;
    }
    gargv[gargc++] = argv[i];
  }

  GtkApplication *gapp = gtk_application_new("io.github.hultwl.rufux", G_APPLICATION_DEFAULT_FLAGS);
  App a = {0};
  g_signal_connect(gapp, "activate", G_CALLBACK(activate), &a);
  int rc = g_application_run(G_APPLICATION(gapp), gargc, gargv);
  if (open_file[0]) load_image(&a, open_file);
  g_object_unref(gapp);
  g_free(gargv);
  return rc;
}
