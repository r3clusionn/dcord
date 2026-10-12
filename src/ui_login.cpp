// Login window: QR code (phone app), email + password (+ 2FA), or a token.
#include <qrcodegen.h>

#include "app.h"
#include "remote_auth.h"
#include "ui_internal.h"

namespace {
struct Login {
	GtkWidget *win = nullptr;
	GtkWidget *error = nullptr;
	GtkWidget *qr_area = nullptr, *qr_status = nullptr;
	GtkWidget *email = nullptr, *password = nullptr, *login_btn = nullptr;
	GtkWidget *mfa_box = nullptr, *mfa_code = nullptr, *mfa_kind = nullptr;
	GtkWidget *token = nullptr;
	std::string qr_url;
	std::string mfa_ticket;
	std::unique_ptr<RemoteAuth> ra;
};
Login *L;

void set_error(const std::string &e)
{
	gtk_label_set_text(GTK_LABEL(L->error), e.c_str());
	gtk_widget_set_visible(L->error, !e.empty());
}

void finish(const std::string &token)
{
	token_save(token);
	// Called from inside RemoteAuth/API callbacks: tear down the login window later.
	post_main([token]() { app_start(token); });
}

void start_qr()
{
	L->ra = std::make_unique<RemoteAuth>();
	L->qr_url.clear();
	gtk_label_set_text(GTK_LABEL(L->qr_status), "Getting a login code…");
	gtk_widget_queue_draw(L->qr_area);
	L->ra->on_qr = [](const std::string &url) {
		L->qr_url = url;
		gtk_label_set_markup(GTK_LABEL(L->qr_status),
			"Open the Discord app on your phone → <b>Settings → Scan QR Code</b>.");
		gtk_widget_queue_draw(L->qr_area);
	};
	L->ra->on_scanned = [](const std::string &name) {
		std::string m = "Scanned by <b>" + markup_escape(name) + "</b>. Confirm the login on your phone.";
		gtk_label_set_markup(GTK_LABEL(L->qr_status), m.c_str());
		L->qr_url.clear();
		gtk_widget_queue_draw(L->qr_area);
	};
	L->ra->on_token = [](const std::string &t) { finish(t); };
	L->ra->on_error = [](const std::string &e) {
		gtk_label_set_text(GTK_LABEL(L->qr_status), (e + " Click the code area to get a new one.").c_str());
		L->qr_url.clear();
		gtk_widget_queue_draw(L->qr_area);
	};
	L->ra->start();
}

gboolean draw_qr(GtkWidget *w, cairo_t *cr, gpointer)
{
	int aw = gtk_widget_get_allocated_width(w), ah = gtk_widget_get_allocated_height(w);
	if (L->qr_url.empty()) {
		cairo_set_source_rgb(cr, 0.2, 0.21, 0.23);
		cairo_rectangle(cr, (aw - ah) / 2.0, 0, ah, ah);
		cairo_fill(cr);
		return FALSE;
	}
	uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
	if (!qrcodegen_encodeText(L->qr_url.c_str(), tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
				  qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
		return FALSE;
	int n = qrcodegen_getSize(qr);
	int quiet = 3;
	double cell = (double)std::min(aw, ah) / (n + 2 * quiet);
	cell = (int)cell;
	double side = cell * (n + 2 * quiet);
	double ox = (aw - side) / 2, oy = (ah - side) / 2;
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, ox, oy, side, side);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++)
			if (qrcodegen_getModule(qr, x, y))
				cairo_rectangle(cr, ox + (x + quiet) * cell, oy + (y + quiet) * cell, cell, cell);
	cairo_fill(cr);
	return FALSE;
}

void do_password_login()
{
	std::string email = gtk_entry_get_text(GTK_ENTRY(L->email));
	std::string pw = gtk_entry_get_text(GTK_ENTRY(L->password));
	if (email.empty() || pw.empty())
		return;
	set_error("");
	gtk_widget_set_sensitive(L->login_btn, FALSE);
	api_login(email, pw, [](ApiResult &r) {
		if (!L)
			return;
		gtk_widget_set_sensitive(L->login_btn, TRUE);
		if (r.ok() && !jstr(r.body, "token").empty()) {
			finish(jstr(r.body, "token"));
			return;
		}
		if (r.ok() && jbool(r.body, "mfa")) {
			L->mfa_ticket = jstr(r.body, "ticket");
			gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(L->mfa_kind));
			if (jbool(r.body, "totp"))
				gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(L->mfa_kind), "totp", "Authenticator app code");
			if (jbool(r.body, "backup"))
				gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(L->mfa_kind), "backup", "Backup code");
			if (jbool(r.body, "sms")) {
				gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(L->mfa_kind), "sms", "SMS code");
			}
			gtk_combo_box_set_active(GTK_COMBO_BOX(L->mfa_kind), 0);
			gtk_widget_show_all(L->mfa_box);
			gtk_widget_grab_focus(L->mfa_code);
			return;
		}
		if (r.body.contains("captcha_key") || r.body.contains("captcha_sitekey")) {
			set_error("Discord wants a captcha for this login, which only works in a browser.\n"
				  "Use the QR code tab instead (no captcha), or paste a token.");
			return;
		}
		std::string e = api_error_text(r);
		if (jint(r.body, "code") == 70007)
			e += "\nCheck your phone for a verification SMS, then log in from the QR tab.";
		set_error(e.empty() ? "Login failed." : e);
	});
}

void do_mfa()
{
	std::string code = trim(gtk_entry_get_text(GTK_ENTRY(L->mfa_code)));
	const char *kind = gtk_combo_box_get_active_id(GTK_COMBO_BOX(L->mfa_kind));
	if (code.empty() || !kind)
		return;
	api_login_mfa(kind, L->mfa_ticket, code, [](ApiResult &r) {
		if (!L)
			return;
		if (r.ok() && !jstr(r.body, "token").empty())
			finish(jstr(r.body, "token"));
		else
			set_error("2FA failed: " + api_error_text(r));
	});
}

GtkWidget *labeled(const char *label, GtkWidget *w)
{
	GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(2));
	GtkWidget *l = gtk_label_new(label);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_box_pack_start(GTK_BOX(b), l, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(b), w, FALSE, FALSE, 0);
	return b;
}
}  // namespace

void login_window_show(const std::string &error)
{
	if (L) {
		set_error(error);
		gtk_window_present(GTK_WINDOW(L->win));
		return;
	}
	L = new Login;
	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	L->win = win;
	gtk_window_set_title(GTK_WINDOW(win), "dcord — log in");
	gtk_window_set_default_size(GTK_WINDOW(win), S(420), S(560));
	g_signal_connect(win, "delete-event", CB(+[](GtkWidget *, GdkEvent *, gpointer) -> gboolean {
		gtk_main_quit();
		return FALSE;
	}), nullptr);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(8));
	g_object_set(box, "margin", S(16), nullptr);
	GtkWidget *title = gtk_label_new(nullptr);
	gtk_label_set_markup(GTK_LABEL(title), "<span size='x-large' weight='bold'>Log in to Discord</span>");
	gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
	L->error = gtk_label_new("");
	gtk_label_set_line_wrap(GTK_LABEL(L->error), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(L->error), "error");
	gtk_widget_set_no_show_all(L->error, TRUE);
	gtk_box_pack_start(GTK_BOX(box), L->error, FALSE, FALSE, 0);

	GtkWidget *nb = gtk_notebook_new();
	// QR
	GtkWidget *qr = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(8));
	g_object_set(qr, "margin", S(8), nullptr);
	L->qr_area = gtk_drawing_area_new();
	gtk_widget_set_size_request(L->qr_area, S(240), S(240));
	g_signal_connect(L->qr_area, "draw", G_CALLBACK(draw_qr), nullptr);
	gtk_widget_add_events(L->qr_area, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(L->qr_area, "button-press-event", CB(+[](GtkWidget *, GdkEventButton *, gpointer) -> gboolean {
		if (L->qr_url.empty())
			start_qr();
		return TRUE;
	}), nullptr);
	gtk_box_pack_start(GTK_BOX(qr), L->qr_area, TRUE, TRUE, 0);
	L->qr_status = gtk_label_new("");
	gtk_label_set_line_wrap(GTK_LABEL(L->qr_status), TRUE);
	gtk_label_set_justify(GTK_LABEL(L->qr_status), GTK_JUSTIFY_CENTER);
	gtk_box_pack_start(GTK_BOX(qr), L->qr_status, FALSE, FALSE, 0);
	gtk_notebook_append_page(GTK_NOTEBOOK(nb), qr, gtk_label_new("QR code"));

	// Password
	GtkWidget *pw = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(8));
	g_object_set(pw, "margin", S(8), nullptr);
	L->email = gtk_entry_new();
	L->password = gtk_entry_new();
	gtk_entry_set_visibility(GTK_ENTRY(L->password), FALSE);
	gtk_entry_set_input_purpose(GTK_ENTRY(L->password), GTK_INPUT_PURPOSE_PASSWORD);
	gtk_box_pack_start(GTK_BOX(pw), labeled("Email or phone number", L->email), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(pw), labeled("Password", L->password), FALSE, FALSE, 0);
	L->login_btn = gtk_button_new_with_label("Log in");
	gtk_style_context_add_class(gtk_widget_get_style_context(L->login_btn), "suggested-action");
	g_signal_connect(L->login_btn, "clicked", CB(+[](GtkButton *, gpointer) { do_password_login(); }), nullptr);
	g_signal_connect(L->password, "activate", CB(+[](GtkEntry *, gpointer) { do_password_login(); }), nullptr);
	gtk_box_pack_start(GTK_BOX(pw), L->login_btn, FALSE, FALSE, 0);
	L->mfa_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(6));
	L->mfa_kind = gtk_combo_box_text_new();
	L->mfa_code = gtk_entry_new();
	gtk_box_pack_start(GTK_BOX(L->mfa_box), gtk_label_new("Two-factor authentication"), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(L->mfa_box), L->mfa_kind, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(L->mfa_box), L->mfa_code, FALSE, FALSE, 0);
	GtkWidget *sms = gtk_button_new_with_label("Send SMS code");
	g_signal_connect(sms, "clicked", CB(+[](GtkButton *, gpointer) {
		api_send_mfa_sms(L->mfa_ticket, [](ApiResult &r) {
			if (L)
				set_error(r.ok() ? "SMS sent." : api_error_text(r));
		});
	}), nullptr);
	gtk_box_pack_start(GTK_BOX(L->mfa_box), sms, FALSE, FALSE, 0);
	GtkWidget *verify = gtk_button_new_with_label("Verify");
	g_signal_connect(verify, "clicked", CB(+[](GtkButton *, gpointer) { do_mfa(); }), nullptr);
	g_signal_connect(L->mfa_code, "activate", CB(+[](GtkEntry *, gpointer) { do_mfa(); }), nullptr);
	gtk_box_pack_start(GTK_BOX(L->mfa_box), verify, FALSE, FALSE, 0);
	gtk_widget_set_no_show_all(L->mfa_box, TRUE);
	gtk_box_pack_start(GTK_BOX(pw), L->mfa_box, FALSE, FALSE, 0);
	GtkWidget *note = gtk_label_new("If Discord asks for a captcha, use the QR code tab.");
	gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
	gtk_box_pack_end(GTK_BOX(pw), note, FALSE, FALSE, 0);
	gtk_notebook_append_page(GTK_NOTEBOOK(nb), pw, gtk_label_new("Email"));

	// Token
	GtkWidget *tk = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(8));
	g_object_set(tk, "margin", S(8), nullptr);
	L->token = gtk_entry_new();
	gtk_entry_set_visibility(GTK_ENTRY(L->token), FALSE);
	gtk_box_pack_start(GTK_BOX(tk), labeled("Account token", L->token), FALSE, FALSE, 0);
	GtkWidget *use = gtk_button_new_with_label("Use token");
	auto use_token = +[](GtkWidget *, gpointer) {
		std::string t = trim(gtk_entry_get_text(GTK_ENTRY(L->token)));
		if (t.size() > 1 && t.front() == '"' && t.back() == '"')
			t = t.substr(1, t.size() - 2);
		if (!t.empty())
			finish(t);
	};
	g_signal_connect(use, "clicked", G_CALLBACK(use_token), nullptr);
	g_signal_connect(L->token, "activate", G_CALLBACK(use_token), nullptr);
	gtk_box_pack_start(GTK_BOX(tk), use, FALSE, FALSE, 0);
	GtkWidget *tnote = gtk_label_new("The token is stored in ~/.config/dcord/token (mode 600).");
	gtk_label_set_line_wrap(GTK_LABEL(tnote), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(tnote), "dim-label");
	gtk_box_pack_start(GTK_BOX(tk), tnote, FALSE, FALSE, 0);
	gtk_notebook_append_page(GTK_NOTEBOOK(nb), tk, gtk_label_new("Token"));

	gtk_box_pack_start(GTK_BOX(box), nb, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(win), box);
	gtk_widget_show_all(win);
	set_error(error);
	start_qr();
}

void login_window_hide()
{
	if (!L)
		return;
	if (L->ra)
		L->ra->stop();
	GtkWidget *w = L->win;
	delete L;
	L = nullptr;
	gtk_widget_destroy(w);
}
