// Copyright (C) 2004-2024 Artifex Software, Inc.
//
// This file is part of MuPDF.
//
// MuPDF is free software: you can redistribute it and/or modify it under the
// terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// MuPDF is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with MuPDF. If not, see <https://www.gnu.org/licenses/agpl-3.0.en.html>
//
// Alternative licensing terms are available from the licensor.
// For commercial licensing, see <https://www.artifex.com/> or contact
// Artifex Software, Inc., 39 Mesa Street, Suite 108A, San Francisco,
// CA 94129, USA, for further information.

#include "mupdf/fitz.h"
#include "html-imp.h"

#include <string.h>

enum { T, R, B, L };

static int is_internal_uri(const char *uri)
{
	while (*uri >= 'a' && *uri <= 'z')
		++uri;
	if (uri[0] == ':' && uri[1] == '/' && uri[2] == '/')
		return 0;
	return 1;
}

static fz_link *load_link_flow(fz_context *ctx, fz_html_flow *flow, fz_link *head, int page, float page_h, const char *dir, const char *file)
{
	fz_link *link;
	fz_html_flow *next;
	char path[2048];
	fz_rect bbox;
	const char *dest;
	const char *href;
	float end;

	float page_y0 = page * page_h;
	float page_y1 = (page + 1) * page_h;

	while (flow)
	{
		next = flow->next;
		if (flow->y >= page_y0 && flow->y <= page_y1)
		{
			href = flow->box->href;
			if (href)
			{
				/* Coalesce contiguous flow boxes into one link node */
				end = flow->x + flow->w;
				while (next &&
					next->y == flow->y &&
					next->h == flow->h &&
					next->box->href == href)
				{
					end = next->x + next->w;
					next = next->next;
				}

				bbox.x0 = flow->x;
				bbox.y0 = flow->y - page * page_h;
				bbox.x1 = end;
				bbox.y1 = bbox.y0 + flow->h;
				if (flow->type != FLOW_IMAGE)
				{
					/* flow->y is the baseline, adjust bbox appropriately */
					bbox.y0 -= 0.8f * flow->h;
					bbox.y1 -= 0.8f * flow->h;
				}

				if (is_internal_uri(href))
				{
					if (href[0] == '#')
					{
						fz_strlcpy(path, file, sizeof path);
						fz_strlcat(path, href, sizeof path);
					}
					else
					{
						fz_strlcpy(path, dir, sizeof path);
						fz_strlcat(path, "/", sizeof path);
						fz_strlcat(path, href, sizeof path);
					}
					fz_urldecode(path);
					fz_cleanname(path);

					dest = path;
				}
				else
				{
					dest = href;
				}

				link = fz_new_derived_link(ctx, fz_link, bbox, dest);
				link->next = head;
				head = link;
			}
		}
		flow = next;
	}
	return head;
}

static fz_link *load_link_box(fz_context *ctx, fz_html_box *box, fz_link *head, int page, float page_h, const char *dir, const char *file)
{
	while (box)
	{
		if (box->type == BOX_FLOW)
			head = load_link_flow(ctx, box->u.flow.head, head, page, page_h, dir, file);
		if (box->down)
			head = load_link_box(ctx, box->down, head, page, page_h, dir, file);
		box = box->next;
	}
	return head;
}

fz_link *
fz_load_html_links(fz_context *ctx, fz_html *html, int page, const char *file)
{
	fz_link *link, *head;
	char dir[2048];
	fz_dirname(dir, file, sizeof dir);

	head = load_link_box(ctx, html->tree.root, NULL, page, html->page_h, dir, file);

	for (link = head; link; link = link->next)
	{
		/* Adjust for page margins */
		link->rect.x0 += html->page_margin[L];
		link->rect.x1 += html->page_margin[L];
		link->rect.y0 += html->page_margin[T];
		link->rect.y1 += html->page_margin[T];
	}

	return head;
}

static fz_html_flow *
find_first_content(fz_html_box *box)
{
	while (box)
	{
		if (box->type == BOX_FLOW)
			return box->u.flow.head;
		box = box->down;
	}
	return NULL;
}

static float
find_flow_target(fz_html_flow *flow, const char *id)
{
	while (flow)
	{
		if (flow->box->id && !strcmp(id, flow->box->id))
			return flow->y;
		flow = flow->next;
	}
	return -1;
}

static float
find_box_target(fz_html_box *box, const char *id)
{
	float y;
	while (box)
	{
		if (box->id && !strcmp(id, box->id))
		{
			fz_html_flow *flow = find_first_content(box);
			if (flow)
				return flow->y;
			return box->s.layout.y;
		}
		if (box->type == BOX_FLOW)
		{
			y = find_flow_target(box->u.flow.head, id);
			if (y >= 0)
				return y;
		}
		else
		{
			y = find_box_target(box->down, id);
			if (y >= 0)
				return y;
		}
		box = box->next;
	}
	return -1;
}

float
fz_find_html_target(fz_context *ctx, fz_html *html, const char *id)
{
	return find_box_target(html->tree.root, id);
}

/* SumatraPDF Plus: element id -> line rectangles, for EPUB media overlay highlighting. */

static fz_html_box *
find_box_by_id(fz_html_box *box, const char *id)
{
	fz_html_box *found;
	while (box)
	{
		if (box->id && !strcmp(id, box->id))
			return box;
		if (box->down)
		{
			found = find_box_by_id(box->down, id);
			if (found)
				return found;
		}
		if (box->type == BOX_FLOW)
		{
			fz_html_flow *flow;
			for (flow = box->u.flow.head; flow; flow = flow->next)
			{
				fz_html_box *b;
				for (b = flow->box; b && b != box; b = b->up)
					if (b->id && !strcmp(id, b->id))
						return b;
			}
		}
		box = box->next;
	}
	return NULL;
}

static int
box_is_within(fz_html_box *b, fz_html_box *target)
{
	for (; b; b = b->up)
		if (b == target)
			return 1;
	return 0;
}

typedef struct
{
	fz_html *html;
	fz_html_box *target;
	int *pages;
	fz_rect *rects;
	int max;
	int n;
	int open; /* last rect can still grow along the current line */
	float line_y;
} target_rects_state;

static void
add_flow_rect(target_rects_state *st, fz_html_flow *flow)
{
	float y0 = 0, ml = 0, mt = 0;
	float top, bottom;
	int page;
	fz_rect r;

	if (flow->w <= 0 && flow->type != FLOW_IMAGE)
		return;
	if (flow->type == FLOW_IMAGE)
	{
		top = flow->y;
		bottom = flow->y + flow->h;
	}
	else
	{
		/* words: y is the baseline and h the em size (see layout_line) */
		top = flow->y - flow->h * 0.9f;
		bottom = flow->y + flow->h * 0.3f;
	}
	page = fz_html_page_number_at_y(st->html, (top + bottom) * 0.5f);
	fz_html_page_box(st->html, page, &y0, NULL, NULL, NULL, &ml, &mt, NULL);
	r.x0 = flow->x + ml;
	r.x1 = flow->x + flow->w + ml;
	r.y0 = top - y0 + mt;
	r.y1 = bottom - y0 + mt;

	if (st->open && st->n > 0 && st->pages[st->n - 1] == page && flow->y == st->line_y)
	{
		st->rects[st->n - 1] = fz_union_rect(st->rects[st->n - 1], r);
		return;
	}
	if (st->n >= st->max)
		return;
	st->pages[st->n] = page;
	st->rects[st->n] = r;
	st->n++;
	st->open = 1;
	st->line_y = flow->y;
}

static void
collect_target_rects(target_rects_state *st, fz_html_box *box, int siblings)
{
	while (box)
	{
		if (box->type == BOX_FLOW)
		{
			fz_html_flow *flow;
			st->open = 0;
			for (flow = box->u.flow.head; flow; flow = flow->next)
			{
				if (!box_is_within(flow->box, st->target))
				{
					st->open = 0;
					continue;
				}
				if (flow->type == FLOW_WORD || flow->type == FLOW_IMAGE)
					add_flow_rect(st, flow);
			}
			st->open = 0;
		}
		else if (box->down)
			collect_target_rects(st, box->down, 1);
		if (!siblings)
			break;
		box = box->next;
	}
}

/* Returns the number of rects written. Rects are in page coordinates of the chapter page given in pages[i]. */
int
fz_html_target_rects(fz_context *ctx, fz_html *html, const char *id, int *pages, fz_rect *rects, int max)
{
	target_rects_state st = { 0 };
	fz_html_box *scan;

	if (!html || !id || !*id || max <= 0)
		return 0;
	st.target = find_box_by_id(html->tree.root, id);
	if (!st.target)
		return 0;
	st.html = html;
	st.pages = pages;
	st.rects = rects;
	st.max = max;

	scan = st.target;
	if (scan->type == BOX_INLINE)
		while (scan && scan->type != BOX_FLOW)
			scan = scan->up;
	if (!scan)
		return 0;
	collect_target_rects(&st, scan, 0);
	return st.n;
}

/* Locate a reading anchor without drawing/extracting every preceding page. */
typedef struct {
    int needle[80], prefix[80], len, matched, seen;
    fz_html_flow *recent[80], *hit;
} reflow_anchor_state;

static int reflow_anchor_skip(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x3000 || c == 0xAD || c == 0x200B;
}

static void find_reflow_anchor_box(reflow_anchor_state* st, fz_html_box* box) {
    for (; box && !st->hit; box = box->next) {
        if (box->type == BOX_FLOW) {
            fz_html_flow* flow;
            for (flow = box->u.flow.head; flow && !st->hit; flow = flow->next) {
                const char* p;
                if (flow->type != FLOW_WORD || flow->box->style->visibility != V_VISIBLE) continue;
                p = flow->content.text;
                while (*p && !st->hit) {
                    int c;
                    p += fz_chartorune(&c, p);
                    if (reflow_anchor_skip(c)) continue;
                    st->recent[st->seen % st->len] = flow;
                    st->seen++;
                    while (st->matched && st->needle[st->matched] != c) st->matched = st->prefix[st->matched - 1];
                    if (st->needle[st->matched] == c) st->matched++;
                    if (st->matched == st->len) st->hit = st->recent[(st->seen - st->len) % st->len];
                }
            }
        } else
            find_reflow_anchor_box(st, box->down);
    }
}

int fz_html_find_reflow_anchor(fz_context* ctx, fz_html* html, const char* text, int* page, fz_rect* rect) {
    reflow_anchor_state st = {0};
    target_rects_state rs = {0};
    const char* p = text;
    int i, j;
    if (!html || !p || !page || !rect) return 0;
    while (*p) {
        int c;
        p += fz_chartorune(&c, p);
        if (reflow_anchor_skip(c)) continue;
        if (st.len == 80) return 0;
        st.needle[st.len++] = c;
    }
    if (st.len < 4) return 0;
    for (i = 1, j = 0; i < st.len; i++) {
        while (j && st.needle[i] != st.needle[j]) j = st.prefix[j - 1];
        if (st.needle[i] == st.needle[j]) j++;
        st.prefix[i] = j;
    }
    find_reflow_anchor_box(&st, html->tree.root);
    if (!st.hit) return 0;
    rs.html = html;
    rs.pages = page;
    rs.rects = rect;
    rs.max = 1;
    add_flow_rect(&rs, st.hit);
    return rs.n;
}

static fz_html_flow* make_flow_bookmark(fz_context* ctx, fz_html_flow* flow, float y, fz_html_flow** candidate) {
    while (flow) {
        *candidate = flow;
        if (flow->y >= y) return flow;
        flow = flow->next;
    }
    return NULL;
}

static fz_html_flow *
make_box_bookmark(fz_context *ctx, fz_html_box *box, float y, fz_html_flow **candidate)
{
	fz_html_flow *mark;
	fz_html_flow *dummy = NULL;
	if (candidate == NULL)
		candidate = &dummy;
	while (box)
	{
		if (box->type == BOX_FLOW)
		{
			if (box->s.layout.y >= y)
			{
				mark = make_flow_bookmark(ctx, box->u.flow.head, y, candidate);
				if (mark)
					return mark;
			}
			else
				*candidate = make_flow_bookmark(ctx, box->u.flow.head, y, candidate);
		}
		else
		{
			mark = make_box_bookmark(ctx, box->down, y, candidate);
			if (mark)
				return mark;
		}
		box = box->next;
	}
	return *candidate;
}

fz_bookmark
fz_make_html_bookmark(fz_context *ctx, fz_html *html, int page)
{
	float y0 = 0;
	fz_html_page_box(html, page, &y0, NULL, NULL, NULL, NULL, NULL, NULL);
	return (fz_bookmark)make_box_bookmark(ctx, html->tree.root, y0, NULL);
}

static int
lookup_flow_bookmark(fz_context *ctx, fz_html_flow *flow, fz_html_flow *mark)
{
	while (flow)
	{
		if (flow == mark)
			return 1;
		flow = flow->next;
	}
	return 0;
}

static int
lookup_box_bookmark(fz_context *ctx, fz_html_box *box, fz_html_flow *mark)
{
	while (box)
	{
		if (box->type == BOX_FLOW)
		{
			if (lookup_flow_bookmark(ctx, box->u.flow.head, mark))
				return 1;
		}
		else
		{
			if (lookup_box_bookmark(ctx, box->down, mark))
				return 1;
		}
		box = box->next;
	}
	return 0;
}

int
fz_lookup_html_bookmark(fz_context *ctx, fz_html *html, fz_bookmark mark)
{
	fz_html_flow *flow = (fz_html_flow*)mark;
	if (flow && lookup_box_bookmark(ctx, html->tree.root, flow))
		return fz_html_page_number_at_y(html, flow->y);
	return -1;
}

struct outline_parser
{
	fz_html *html;
	fz_buffer *cat;
	fz_outline *head;
	fz_outline **tail[6];
	fz_outline **down[6];
	int level[6];
	int current;
	int id;
};

static void
cat_html_flow(fz_context *ctx, fz_buffer *cat, fz_html_flow *flow)
{
	while (flow)
	{
		switch (flow->type)
		{
		case FLOW_WORD:
			fz_append_string(ctx, cat, flow->content.text);
			break;
		case FLOW_SPACE:
		case FLOW_BREAK:
			fz_append_byte(ctx, cat, ' ');
			break;
		default:
			break;
		}
		flow = flow->next;
	}
}

static void
cat_html_box(fz_context *ctx, fz_buffer *cat, fz_html_box *box)
{
	while (box)
	{
		if (box->type == BOX_FLOW)
			cat_html_flow(ctx, cat, box->u.flow.head);
		cat_html_box(ctx, cat, box->down);
		box = box->next;
	}
}

static const char *
cat_html_text(fz_context *ctx, struct outline_parser *x, fz_html_box *box)
{
	if (!x->cat)
		x->cat = fz_new_buffer(ctx, 1024);
	else
		fz_clear_buffer(ctx, x->cat);

	cat_html_flow(ctx, x->cat, box->u.flow.head);
	cat_html_box(ctx, x->cat, box->down);

	return fz_string_from_buffer(ctx, x->cat);
}

static void
add_html_outline(fz_context *ctx, struct outline_parser *x, fz_html_box *box)
{
	fz_outline *node;
	char buf[100];
	int heading;

	node = fz_new_outline(ctx);
	fz_try(ctx)
	{
		node->title = Memento_label(fz_strdup(ctx, cat_html_text(ctx, x, box)), "outline_title");
		if (!box->id)
		{
			fz_snprintf(buf, sizeof buf, "'%d", x->id++);
			box->id = Memento_label(fz_pool_strdup(ctx, x->html->tree.pool, buf), "box_id");
		}
		node->uri = Memento_label(fz_asprintf(ctx, "#%s", box->id), "outline_uri");
		node->is_open = 1;
	}
	fz_catch(ctx)
	{
		fz_free(ctx, node);
		fz_rethrow(ctx);
	}

	heading = box->heading;
	if (x->level[x->current] < heading && x->current < 5)
	{
		x->tail[x->current+1] = x->down[x->current];
		x->current += 1;
	}
	else
	{
		while (x->current > 0 && x->level[x->current] > heading)
		{
			x->current -= 1;
		}
	}
	x->level[x->current] = heading;

	*(x->tail[x->current]) = node;
	x->tail[x->current] = &node->next;
	x->down[x->current] = &node->down;
}

static void
load_html_outline(fz_context *ctx, struct outline_parser *x, fz_html_box *box)
{
	while (box)
	{
		int heading = box->heading;
		if (heading)
			add_html_outline(ctx, x, box);
		if (box->down)
			load_html_outline(ctx, x, box->down);
		box = box->next;
	}
}

fz_outline *
fz_load_html_outline(fz_context *ctx, fz_html *html)
{
	struct outline_parser state;
	state.html = html;
	state.cat = NULL;
	state.head = NULL;
	state.tail[0] = &state.head;
	state.down[0] = NULL;
	state.level[0] = 99;
	state.current = 0;
	state.id = 1;
	fz_try(ctx)
		load_html_outline(ctx, &state, html->tree.root);
	fz_always(ctx)
		fz_drop_buffer(ctx, state.cat);
	fz_catch(ctx)
	{
		fz_drop_outline(ctx, state.head);
		state.head = NULL;
	}
	return state.head;
}
