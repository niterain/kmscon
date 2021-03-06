/*
 * User Interface
 *
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * User Interface
 * Implementation of the user interface.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "eloop.h"
#include "log.h"
#include "shl_dlist.h"
#include "terminal.h"
#include "ui.h"
#include "uterm.h"

#define LOG_SUBSYSTEM "ui"

struct ui_video {
	struct shl_dlist list;
	struct kmscon_ui *ui;
	struct uterm_video *video;
};

struct kmscon_ui {
	struct ev_eloop *eloop;
	struct uterm_input *input;
	struct shl_dlist video_list;
	struct kmscon_terminal *term;
	bool awake;
};

static void video_activate(struct ui_video *vid, struct uterm_display *disp)
{
	int ret;

	if (!uterm_video_is_awake(vid->video))
		return;

	if (uterm_display_get_state(disp) == UTERM_DISPLAY_INACTIVE) {
		ret = uterm_display_activate(disp, NULL);
		if (ret) {
			log_warning("cannot activate display");
			return;
		}
		ret = uterm_display_set_dpms(disp, UTERM_DPMS_ON);
		if (ret) {
			log_warning("cannot set DPMS state to on for display");
			return;
		}
	}
}

static void video_event(struct uterm_video *video,
			struct uterm_video_hotplug *ev,
			void *data)
{
	struct ui_video *vid = data;
	struct kmscon_ui *ui = vid->ui;
	struct uterm_display *disp;

	if (ev->action == UTERM_NEW) {
		video_activate(vid, ev->display);
		kmscon_terminal_add_display(ui->term, ev->display);
	} else if (ev->action == UTERM_GONE) {
		kmscon_terminal_remove_display(ui->term, ev->display);
	} else if (ev->action == UTERM_WAKE_UP) {
		disp = uterm_video_get_displays(video);
		while (disp) {
			video_activate(vid, disp);
			kmscon_terminal_add_display(ui->term, disp);
			kmscon_terminal_redraw(ui->term);
			disp = uterm_display_next(disp);
		}
	}
}

static void video_new(struct kmscon_ui *ui, struct uterm_video *video)
{
	struct shl_dlist *iter;
	struct ui_video *vid;
	int ret;

	shl_dlist_for_each(iter, &ui->video_list) {
		vid = shl_dlist_entry(iter, struct ui_video, list);
		if (vid->video == video)
			return;
	}

	log_debug("adding video device");

	vid = malloc(sizeof(*vid));
	if (!vid)
		return;
	memset(vid, 0, sizeof(*vid));
	vid->ui = ui;
	vid->video = video;

	ret = uterm_video_register_cb(vid->video, video_event, vid);
	if (ret)
		goto err_free;

	shl_dlist_link(&ui->video_list, &vid->list);
	uterm_video_ref(vid->video);

	return;

err_free:
	free(vid);
}

static void video_free(struct ui_video *vid)
{
	struct uterm_display *disp;
	struct kmscon_ui *ui = vid->ui;

	log_debug("removing video device");
	shl_dlist_unlink(&vid->list);

	disp = uterm_video_get_displays(vid->video);
	while (disp) {
		kmscon_terminal_remove_display(ui->term, disp);
		kmscon_terminal_redraw(ui->term);
		disp = uterm_display_next(disp);
	}

	uterm_video_unregister_cb(vid->video, video_event, vid);
	uterm_video_unref(vid->video);
	free(vid);
}

static void video_free_all(struct kmscon_ui *ui)
{
	struct ui_video *vid;
	struct shl_dlist *iter, *tmp;

	shl_dlist_for_each_safe(iter, tmp, &ui->video_list) {
		vid = shl_dlist_entry(iter, struct ui_video, list);
		video_free(vid);
	}
}

static void input_event(struct uterm_input *input,
			struct uterm_input_event *ev,
			void *data)
{
	struct kmscon_ui *ui = data;

	if (!ui->awake)
		return;
}

static void terminal_event(struct kmscon_terminal *term,
			   enum kmscon_terminal_etype type,
			   void *data)
{
	if (type == KMSCON_TERMINAL_HUP)
		kmscon_terminal_open(term, terminal_event, data);
}

int kmscon_ui_new(struct kmscon_ui **out,
			struct ev_eloop *eloop,
			struct uterm_input *input)
{
	struct kmscon_ui *ui;
	int ret;

	if (!out || !eloop || !input)
		return -EINVAL;

	ui = malloc(sizeof(*ui));
	if (!ui)
		return -ENOMEM;
	memset(ui, 0, sizeof(*ui));
	ui->eloop = eloop;
	ui->input = input;
	shl_dlist_init(&ui->video_list);

	ret = kmscon_terminal_new(&ui->term, eloop, ui->input);
	if (ret)
		goto err_free;

	ret = uterm_input_register_cb(ui->input, input_event, ui);
	if (ret)
		goto err_video;

	ret = kmscon_terminal_open(ui->term, terminal_event, NULL);
	if (ret)
		goto err_input;

	ev_eloop_ref(ui->eloop);
	uterm_input_ref(ui->input);
	*out = ui;
	return 0;

err_input:
	uterm_input_unregister_cb(ui->input, input_event, ui);
err_video:
	video_free_all(ui);
	kmscon_terminal_unref(ui->term);
err_free:
	free(ui);
	return ret;
}

void kmscon_ui_free(struct kmscon_ui *ui)
{
	if (!ui)
		return;

	uterm_input_unregister_cb(ui->input, input_event, ui);
	video_free_all(ui);
	kmscon_terminal_unref(ui->term);
	uterm_input_unref(ui->input);
	ev_eloop_unref(ui->eloop);
	free(ui);
}

void kmscon_ui_add_video(struct kmscon_ui *ui, struct uterm_video *video)
{
	if (!ui || !video)
		return;

	video_new(ui, video);
}

void kmscon_ui_remove_video(struct kmscon_ui *ui, struct uterm_video *video)
{
	struct ui_video *vid;
	struct shl_dlist *iter;

	if (!ui || !video)
		return;

	shl_dlist_for_each(iter, &ui->video_list) {
		vid = shl_dlist_entry(iter, struct ui_video, list);
		if (vid->video == video) {
			video_free(vid);
			return;
		}
	}
}

void kmscon_ui_wake_up(struct kmscon_ui *ui)
{
	if (!ui || ui->awake)
		return;

	ui->awake = true;
	kmscon_terminal_wake_up(ui->term);
}

void kmscon_ui_sleep(struct kmscon_ui *ui)
{
	if (!ui || !ui->awake)
		return;

	ui->awake = false;
	kmscon_terminal_sleep(ui->term);
}

bool kmscon_ui_is_awake(struct kmscon_ui *ui)
{
	return ui && ui->awake;
}
