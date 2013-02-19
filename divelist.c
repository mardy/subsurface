/* divelist.c */
/* this creates the UI for the dive list -
 * controlled through the following interfaces:
 *
 * void flush_divelist(struct dive *dive)
 * GtkWidget dive_list_create(void)
 * void dive_list_update_dives(void)
 * void update_dive_list_units(void)
 * void set_divelist_font(const char *font)
 * void mark_divelist_changed(int changed)
 * int unsaved_changes()
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <glib/gi18n.h>
#include <assert.h>

#include "divelist.h"
#include "dive.h"
#include "display.h"
#include "display-gtk.h"

#include <gdk-pixbuf/gdk-pixdata.h>
#include "satellite.h"

struct DiveList {
	GtkWidget    *tree_view;
	GtkWidget    *container_widget;
	GtkTreeStore *model, *listmodel, *treemodel;
	GtkTreeViewColumn *nr, *date, *stars, *depth, *duration, *location;
	GtkTreeViewColumn *temperature, *cylinder, *totalweight, *suit, *nitrox, *sac, *otu, *maxcns;
	int changed;
};

static struct DiveList dive_list;
#define MODEL(_dl) GTK_TREE_MODEL((_dl).model)
#define TREEMODEL(_dl) GTK_TREE_MODEL((_dl).treemodel)
#define LISTMODEL(_dl) GTK_TREE_MODEL((_dl).listmodel)
#define STORE(_dl) GTK_TREE_STORE((_dl).model)
#define TREESTORE(_dl) GTK_TREE_STORE((_dl).treemodel)
#define LISTSTORE(_dl) GTK_TREE_STORE((_dl).listmodel)

dive_trip_t *dive_trip_list;
gboolean autogroup = FALSE;

/*
 * The dive list has the dive data in both string format (for showing)
 * and in "raw" format (for sorting purposes)
 */
enum {
	DIVE_INDEX = 0,
	DIVE_NR,		/* int: dive->nr */
	DIVE_DATE,		/* timestamp_t: dive->when */
	DIVE_RATING,		/* int: 0-5 stars */
	DIVE_DEPTH,		/* int: dive->maxdepth in mm */
	DIVE_DURATION,		/* int: in seconds */
	DIVE_TEMPERATURE,	/* int: in mkelvin */
	DIVE_TOTALWEIGHT,	/* int: in grams */
	DIVE_SUIT,		/* "wet, 3mm" */
	DIVE_CYLINDER,
	DIVE_NITROX,		/* int: dummy */
	DIVE_SAC,		/* int: in ml/min */
	DIVE_OTU,		/* int: in OTUs */
	DIVE_MAXCNS,		/* int: in % */
	DIVE_LOCATION,		/* "2nd Cathedral, Lanai" */
	DIVE_LOC_ICON,		/* pixbuf for gps icon */
	DIVELIST_COLUMNS
};

static void turn_dive_into_trip(GtkTreePath *path);
static void merge_dive_into_trip_above_cb(GtkWidget *menuitem, GtkTreePath *path);

#ifdef DEBUG_MODEL
static gboolean dump_model_entry(GtkTreeModel *model, GtkTreePath *path,
				GtkTreeIter *iter, gpointer data)
{
	char *location;
	int idx, nr, duration;
	struct dive *dive;
	timestamp_t when;
	struct tm tm;

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_NR, &nr, DIVE_DATE, &when,
			DIVE_DURATION, &duration, DIVE_LOCATION, &location, -1);
	utc_mkdate(when, &tm);
	printf("iter %x:%x entry #%d : nr %d @ %04d-%02d-%02d %02d:%02d:%02d duration %d location %s ",
		iter->stamp, iter->user_data, idx, nr,
		tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec,
		duration, location);
	dive = get_dive(idx);
	if (dive)
		printf("tripflag %d\n", dive->tripflag);
	else
		printf("without matching dive\n");

	free(location);

	return FALSE;
}

static void dump_model(GtkListStore *store)
{
	gtk_tree_model_foreach(GTK_TREE_MODEL(store), dump_model_entry, NULL);
	printf("\n---\n\n");
}
#endif

#if DEBUG_SELECTION_TRACKING
void dump_selection(void)
{
	int i;
	struct dive *dive;

	printf("currently selected are %u dives:", amount_selected);
	for_each_dive(i, dive) {
		if (dive->selected)
			printf(" %d", i);
	}
	printf("\n");
}
#endif

/* when subsurface starts we want to have the last dive selected. So we simply
   walk to the first leaf (and skip the summary entries - which have negative
   DIVE_INDEX) */
static void first_leaf(GtkTreeModel *model, GtkTreeIter *iter, int *diveidx)
{
	GtkTreeIter parent;
	GtkTreePath *tpath;

	while (*diveidx < 0) {
		memcpy(&parent, iter, sizeof(parent));
		tpath = gtk_tree_model_get_path(model, &parent);
		if (!gtk_tree_model_iter_children(model, iter, &parent)) {
			/* we should never have a parent without child */
			gtk_tree_path_free(tpath);
			return;
		}
		if (!gtk_tree_view_row_expanded(GTK_TREE_VIEW(dive_list.tree_view), tpath))
			gtk_tree_view_expand_row(GTK_TREE_VIEW(dive_list.tree_view), tpath, FALSE);
		gtk_tree_path_free(tpath);
		gtk_tree_model_get(model, iter, DIVE_INDEX, diveidx, -1);
	}
}

static struct dive *dive_from_path(GtkTreePath *path)
{
	GtkTreeIter iter;
	int idx;

	if (gtk_tree_model_get_iter(MODEL(dive_list), &iter, path)) {
		gtk_tree_model_get(MODEL(dive_list), &iter, DIVE_INDEX, &idx, -1);
		return get_dive(idx);
	} else {
		return NULL;
	}

}

/* make sure that if we expand a summary row that is selected, the children show
   up as selected, too */
static void row_expanded_cb(GtkTreeView *tree_view, GtkTreeIter *iter, GtkTreePath *path, gpointer data)
{
	GtkTreeIter child;
	GtkTreeModel *model = MODEL(dive_list);
	GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));

	if (!gtk_tree_model_iter_children(model, &child, iter))
		return;

	do {
		int idx;
		struct dive *dive;

		gtk_tree_model_get(model, &child, DIVE_INDEX, &idx, -1);
		dive = get_dive(idx);

		if (dive->selected)
			gtk_tree_selection_select_iter(selection, &child);
		else
			gtk_tree_selection_unselect_iter(selection, &child);
	} while (gtk_tree_model_iter_next(model, &child));
}

static int selected_children(GtkTreeModel *model, GtkTreeIter *iter)
{
	GtkTreeIter child;

	if (!gtk_tree_model_iter_children(model, &child, iter))
		return FALSE;

	do {
		int idx;
		struct dive *dive;

		gtk_tree_model_get(model, &child, DIVE_INDEX, &idx, -1);
		dive = get_dive(idx);

		if (dive->selected)
			return TRUE;
	} while (gtk_tree_model_iter_next(model, &child));
	return FALSE;
}

/* Make sure that if we collapse a summary row with any selected children, the row
   shows up as selected too */
static void row_collapsed_cb(GtkTreeView *tree_view, GtkTreeIter *iter, GtkTreePath *path, gpointer data)
{
	GtkTreeModel *model = MODEL(dive_list);
	GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));

	if (selected_children(model, iter))
		gtk_tree_selection_select_iter(selection, iter);
}

const char *star_strings[] = {
	ZERO_STARS,
	ONE_STARS,
	TWO_STARS,
	THREE_STARS,
	FOUR_STARS,
	FIVE_STARS
};

static void star_data_func(GtkTreeViewColumn *col,
			   GtkCellRenderer *renderer,
			   GtkTreeModel *model,
			   GtkTreeIter *iter,
			   gpointer data)
{
	int nr_stars, idx;
	char buffer[40];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_RATING, &nr_stars, -1);
	if (idx < 0) {
		*buffer = '\0';
	} else {
		if (nr_stars < 0 || nr_stars > 5)
			nr_stars = 0;
		snprintf(buffer, sizeof(buffer), "%s", star_strings[nr_stars]);
	}
	g_object_set(renderer, "text", buffer, NULL);
}

static void date_data_func(GtkTreeViewColumn *col,
			   GtkCellRenderer *renderer,
			   GtkTreeModel *model,
			   GtkTreeIter *iter,
			   gpointer data)
{
	int idx, nr;
	struct tm tm;
	timestamp_t when;
	/* this should be enought for most languages. if not increase the value. */
	char buffer[256];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_DATE, &when, -1);
	nr = gtk_tree_model_iter_n_children(model, iter);

	utc_mkdate(when, &tm);
	if (idx < 0) {
		snprintf(buffer, sizeof(buffer),
			/*++GETTEXT 60 char buffer weekday, monthname, day of month, year, nr dives */
			ngettext("Trip %1$s, %2$s %3$d, %4$d (%5$d dive)",
				"Trip %1$s, %2$s %3$d, %4$d (%5$d dives)", nr),
			weekday(tm.tm_wday),
			monthname(tm.tm_mon),
			tm.tm_mday, tm.tm_year + 1900,
			nr);
	} else {
		snprintf(buffer, sizeof(buffer),
			/*++GETTEXT 60 char buffer weekday, monthname, day of month, year, hour:min */
			_("%1$s, %2$s %3$d, %4$d %5$02d:%6$02d"),
			weekday(tm.tm_wday),
			monthname(tm.tm_mon),
			tm.tm_mday, tm.tm_year + 1900,
			tm.tm_hour, tm.tm_min);
	}
	g_object_set(renderer, "text", buffer, NULL);
}

static void depth_data_func(GtkTreeViewColumn *col,
			    GtkCellRenderer *renderer,
			    GtkTreeModel *model,
			    GtkTreeIter *iter,
			    gpointer data)
{
	int depth, integer, frac, len, idx;
	char buffer[40];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_DEPTH, &depth, -1);

	if (idx < 0) {
		*buffer = '\0';
	} else {
		switch (prefs.units.length) {
		case METERS:
			/* To tenths of meters */
			depth = (depth + 49) / 100;
			integer = depth / 10;
			frac = depth % 10;
			if (integer < 20)
				break;
			if (frac >= 5)
				integer++;
			frac = -1;
			break;
		case FEET:
			integer = mm_to_feet(depth) + 0.5;
			frac = -1;
			break;
		default:
			return;
		}
		len = snprintf(buffer, sizeof(buffer), "%d", integer);
		if (frac >= 0)
			len += snprintf(buffer+len, sizeof(buffer)-len, ".%d", frac);
	}
	g_object_set(renderer, "text", buffer, NULL);
}

static void duration_data_func(GtkTreeViewColumn *col,
			       GtkCellRenderer *renderer,
			       GtkTreeModel *model,
			       GtkTreeIter *iter,
			       gpointer data)
{
	unsigned int sec;
	int idx;
	char buffer[16];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_DURATION, &sec, -1);
	if (idx < 0)
		*buffer = '\0';
	else
		snprintf(buffer, sizeof(buffer), "%d:%02d", sec / 60, sec % 60);

	g_object_set(renderer, "text", buffer, NULL);
}

static void temperature_data_func(GtkTreeViewColumn *col,
				  GtkCellRenderer *renderer,
				  GtkTreeModel *model,
				  GtkTreeIter *iter,
				  gpointer data)
{
	int value, idx;
	char buffer[80];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_TEMPERATURE, &value, -1);

	*buffer = 0;
	if (idx >= 0 && value) {
		double deg;
		switch (prefs.units.temperature) {
		case CELSIUS:
			deg = mkelvin_to_C(value);
			break;
		case FAHRENHEIT:
			deg = mkelvin_to_F(value);
			break;
		default:
			return;
		}
		snprintf(buffer, sizeof(buffer), "%.1f", deg);
	}

	g_object_set(renderer, "text", buffer, NULL);
}

static void gpsicon_data_func(GtkTreeViewColumn *col,
			   GtkCellRenderer *renderer,
			   GtkTreeModel *model,
			   GtkTreeIter *iter,
			   gpointer data)
{
	int idx;
	GdkPixbuf *icon;

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_LOC_ICON, &icon, -1);
	g_object_set(renderer, "pixbuf", icon, NULL);
}

static void nr_data_func(GtkTreeViewColumn *col,
			   GtkCellRenderer *renderer,
			   GtkTreeModel *model,
			   GtkTreeIter *iter,
			   gpointer data)
{
	int idx, nr;
	char buffer[40];
	struct dive *dive;

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_NR, &nr, -1);
	if (idx < 0) {
		*buffer = '\0';
	} else {
		/* make dives that are not in trips stand out */
		dive = get_dive(idx);
		if (!DIVE_IN_TRIP(dive))
			snprintf(buffer, sizeof(buffer), "<b>%d</b>", nr);
		else
			snprintf(buffer, sizeof(buffer), "%d", nr);
	}
	g_object_set(renderer, "markup", buffer, NULL);
}

/*
 * Get "maximal" dive gas for a dive.
 * Rules:
 *  - Trimix trumps nitrox (highest He wins, O2 breaks ties)
 *  - Nitrox trumps air (even if hypoxic)
 * These are the same rules as the inter-dive sorting rules.
 */
static void get_dive_gas(struct dive *dive, int *o2_p, int *he_p, int *o2low_p)
{
	int i;
	int maxo2 = -1, maxhe = -1, mino2 = 1000;

	for (i = 0; i < MAX_CYLINDERS; i++) {
		cylinder_t *cyl = dive->cylinder + i;
		struct gasmix *mix = &cyl->gasmix;
		int o2 = mix->o2.permille;
		int he = mix->he.permille;

		if (cylinder_none(cyl))
			continue;
		if (!o2)
			o2 = O2_IN_AIR;
		if (o2 < mino2)
			mino2 = o2;
		if (he > maxhe)
			goto newmax;
		if (he < maxhe)
			continue;
		if (o2 <= maxo2)
			continue;
newmax:
		maxhe = he;
		maxo2 = o2;
	}
	/* All air? Show/sort as "air"/zero */
	if (!maxhe && maxo2 == O2_IN_AIR && mino2 == maxo2)
		maxo2 = mino2 = 0;
	*o2_p = maxo2;
	*he_p = maxhe;
	*o2low_p = mino2;
}

int total_weight(struct dive *dive)
{
	int i, total_grams = 0;

	if (dive)
		for (i=0; i< MAX_WEIGHTSYSTEMS; i++)
			total_grams += dive->weightsystem[i].weight.grams;
	return total_grams;
}

static void weight_data_func(GtkTreeViewColumn *col,
			     GtkCellRenderer *renderer,
			     GtkTreeModel *model,
			     GtkTreeIter *iter,
			     gpointer data)
{
	int indx, decimals;
	double value;
	char buffer[80];
	struct dive *dive;

	gtk_tree_model_get(model, iter, DIVE_INDEX, &indx, -1);
	dive = get_dive(indx);
	value = get_weight_units(total_weight(dive), &decimals, NULL);
	if (value == 0.0)
		*buffer = '\0';
	else
		snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);

	g_object_set(renderer, "text", buffer, NULL);
}

static gint nitrox_sort_func(GtkTreeModel *model,
	GtkTreeIter *iter_a,
	GtkTreeIter *iter_b,
	gpointer user_data)
{
	int index_a, index_b;
	struct dive *a, *b;
	int a_o2, b_o2;
	int a_he, b_he;
	int a_o2low, b_o2low;

	gtk_tree_model_get(model, iter_a, DIVE_INDEX, &index_a, -1);
	gtk_tree_model_get(model, iter_b, DIVE_INDEX, &index_b, -1);
	a = get_dive(index_a);
	b = get_dive(index_b);
	get_dive_gas(a, &a_o2, &a_he, &a_o2low);
	get_dive_gas(b, &b_o2, &b_he, &b_o2low);

	/* Sort by Helium first, O2 second */
	if (a_he == b_he) {
		if (a_o2 == b_o2)
			return a_o2low - b_o2low;
		return a_o2 - b_o2;
	}
	return a_he - b_he;
}

#define UTF8_ELLIPSIS "\xE2\x80\xA6"

static void nitrox_data_func(GtkTreeViewColumn *col,
			     GtkCellRenderer *renderer,
			     GtkTreeModel *model,
			     GtkTreeIter *iter,
			     gpointer data)
{
	int idx, o2, he, o2low;
	char buffer[80];
	struct dive *dive;

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, -1);
	if (idx < 0) {
		*buffer = '\0';
		goto exit;
	}
	dive = get_dive(idx);
	get_dive_gas(dive, &o2, &he, &o2low);
	o2 = (o2 + 5) / 10;
	he = (he + 5) / 10;
	o2low = (o2low + 5) / 10;

	if (he)
		snprintf(buffer, sizeof(buffer), "%d/%d", o2, he);
	else if (o2)
		if (o2 == o2low)
			snprintf(buffer, sizeof(buffer), "%d", o2);
		else
			snprintf(buffer, sizeof(buffer), "%d" UTF8_ELLIPSIS "%d", o2low, o2);
	else
		strcpy(buffer, _("air"));
exit:
	g_object_set(renderer, "text", buffer, NULL);
}

/* Render the SAC data (integer value of "ml / min") */
static void sac_data_func(GtkTreeViewColumn *col,
			  GtkCellRenderer *renderer,
			  GtkTreeModel *model,
			  GtkTreeIter *iter,
			  gpointer data)
{
	int value, idx;
	const char *fmt;
	char buffer[16];
	double sac;

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_SAC, &value, -1);

	if (idx < 0 || !value) {
		*buffer = '\0';
		goto exit;
	}

	sac = value / 1000.0;
	switch (prefs.units.volume) {
	case LITER:
		fmt = "%4.1f";
		break;
	case CUFT:
		fmt = "%4.2f";
		sac = ml_to_cuft(sac * 1000);
		break;
	}
	snprintf(buffer, sizeof(buffer), fmt, sac);
exit:
	g_object_set(renderer, "text", buffer, NULL);
}

/* Render the OTU data (integer value of "OTU") */
static void otu_data_func(GtkTreeViewColumn *col,
			  GtkCellRenderer *renderer,
			  GtkTreeModel *model,
			  GtkTreeIter *iter,
			  gpointer data)
{
	int value, idx;
	char buffer[16];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_OTU, &value, -1);

	if (idx < 0 || !value)
		*buffer = '\0';
	else
		snprintf(buffer, sizeof(buffer), "%d", value);

	g_object_set(renderer, "text", buffer, NULL);
}

/* Render the CNS data (in full %) */
static void cns_data_func(GtkTreeViewColumn *col,
			  GtkCellRenderer *renderer,
			  GtkTreeModel *model,
			  GtkTreeIter *iter,
			  gpointer data)
{
	int value, idx;
	char buffer[16];

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, DIVE_MAXCNS, &value, -1);

	if (idx < 0 || !value)
		*buffer = '\0';
	else
		snprintf(buffer, sizeof(buffer), "%d%%", value);

	g_object_set(renderer, "text", buffer, NULL);
}

static int active_o2(struct dive *dive, struct divecomputer *dc, duration_t time)
{
	int o2permille = dive->cylinder[0].gasmix.o2.permille;
	struct event *event = dc->events;

	if (!o2permille)
		o2permille = O2_IN_AIR;

	for (event = dc->events; event; event = event->next) {
		if (event->time.seconds > time.seconds)
			break;
		if (strcmp(event->name, "gaschange"))
			continue;
		o2permille = 10*(event->value & 0xffff);
	}
	return o2permille;
}

/* calculate OTU for a dive - this only takes the first diveomputer into account */
static int calculate_otu(struct dive *dive)
{
	int i;
	double otu = 0.0;
	struct divecomputer *dc = &dive->dc;

	for (i = 1; i < dc->samples; i++) {
		int t;
		int po2;
		struct sample *sample = dc->sample + i;
		struct sample *psample = sample - 1;
		t = sample->time.seconds - psample->time.seconds;
		if (sample->po2) {
			po2 = sample->po2;
		} else {
			int o2 = active_o2(dive, dc, sample->time);
			po2 = o2 / 1000.0 * depth_to_mbar(sample->depth.mm, dive);
		}
		if (po2 >= 500)
			otu += pow((po2 - 500) / 1000.0, 0.83) * t / 30.0;
	}
	return otu + 0.5;
}
/*
 * Return air usage (in liters).
 */
static double calculate_airuse(struct dive *dive)
{
	double airuse = 0;
	int i;

	for (i = 0; i < MAX_CYLINDERS; i++) {
		pressure_t start, end;
		cylinder_t *cyl = dive->cylinder + i;
		int size = cyl->type.size.mliter;
		double kilo_atm;

		if (!size)
			continue;

		start = cyl->start.mbar ? cyl->start : cyl->sample_start;
		end = cyl->end.mbar ? cyl->end : cyl->sample_end;
		kilo_atm = (to_ATM(start) - to_ATM(end)) / 1000.0;

		/* Liters of air at 1 atm == milliliters at 1k atm*/
		airuse += kilo_atm * size;
	}
	return airuse;
}

/* this only uses the first divecomputer to calculate the SAC rate */
static int calculate_sac(struct dive *dive)
{
	struct divecomputer *dc = &dive->dc;
	double airuse, pressure, sac;
	int duration, i;

	airuse = calculate_airuse(dive);
	if (!airuse)
		return 0;
	duration = dc->duration.seconds;
	if (!duration)
		return 0;

	/* find and eliminate long surface intervals */

	for (i = 0; i < dc->samples; i++) {
		if (dc->sample[i].depth.mm < 100) { /* less than 10cm */
			int end = i + 1;
			while (end < dc->samples && dc->sample[end].depth.mm < 100)
				end++;
			/* we only want the actual surface time during a dive */
			if (end < dc->samples) {
				end--;
				duration -= dc->sample[end].time.seconds -
						dc->sample[i].time.seconds;
				i = end + 1;
			}
		}
	}
	/* Mean pressure in bar (SAC calculations are in bar*l/min) */
	pressure = depth_to_mbar(dc->meandepth.mm, dive) / 1000.0;
	sac = airuse / pressure * 60 / duration;

	/* milliliters per minute.. */
	return sac * 1000;
}

/* for now we do this based on the first divecomputer */
static void add_dive_to_deco(struct dive *dive)
{
	struct divecomputer *dc = &dive->dc;
	int i;

	if (!dc)
		return;
	for (i = 1; i < dc->samples; i++) {
		struct sample *psample = dc->sample + i - 1;
		struct sample *sample = dc->sample + i;
		int t0 = psample->time.seconds;
		int t1 = sample->time.seconds;
		int j;

		for (j = t0; j < t1; j++) {
			int depth = interpolate(psample->depth.mm, sample->depth.mm, j - t0, t1 - t0);
			(void) add_segment(depth_to_mbar(depth, dive) / 1000.0,
					   &dive->cylinder[sample->sensor].gasmix, 1, sample->po2, dive);
		}
	}
}

static int get_divenr(struct dive *dive)
{
	int divenr = -1;
	while (++divenr < dive_table.nr && get_dive(divenr) != dive)
		;
	return divenr;
}

static struct gasmix air = { .o2.permille = O2_IN_AIR };

/* take into account previous dives until there is a 48h gap between dives */
double init_decompression(struct dive *dive)
{
	int i, divenr = -1;
	unsigned int surface_time;
	timestamp_t when, lasttime = 0;
	gboolean deco_init = FALSE;
	double tissue_tolerance, surface_pressure;

	if (!dive)
		return 0.0;
	divenr = get_divenr(dive);
	when = dive->when;
	i = divenr;
	while (i && --i) {
		struct dive* pdive = get_dive(i);
		/* we don't want to mix dives from different trips as we keep looking
		 * for how far back we need to go */
		if (dive->divetrip && pdive->divetrip != dive->divetrip)
			continue;
		if (!pdive || pdive->when > when || pdive->when + pdive->duration.seconds + 48 * 60 * 60 < when)
			break;
		when = pdive->when;
		lasttime = when + pdive->duration.seconds;
	}
	while (++i < divenr) {
		struct dive* pdive = get_dive(i);
		/* again skip dives from different trips */
		if (dive->divetrip && dive->divetrip != pdive->divetrip)
			continue;
		surface_pressure = get_surface_pressure_in_mbar(pdive, TRUE) / 1000.0;
		if (!deco_init) {
			clear_deco(surface_pressure);
			deco_init = TRUE;
#if DECO_CALC_DEBUG & 2
			dump_tissues();
#endif
		}
		add_dive_to_deco(pdive);
#if DECO_CALC_DEBUG & 2
		printf("added dive #%d\n", pdive->number);
		dump_tissues();
#endif
		if (pdive->when > lasttime) {
			surface_time = pdive->when - lasttime;
			lasttime = pdive->when + pdive->duration.seconds;
			tissue_tolerance = add_segment(surface_pressure, &air, surface_time, 0, dive);
#if DECO_CALC_DEBUG & 2
			printf("after surface intervall of %d:%02u\n", FRACTION(surface_time,60));
			dump_tissues();
#endif
		}
	}
	/* add the final surface time */
	if (lasttime && dive->when > lasttime) {
		surface_time = dive->when - lasttime;
		surface_pressure = get_surface_pressure_in_mbar(dive, TRUE) / 1000.0;
		tissue_tolerance = add_segment(surface_pressure, &air, surface_time, 0, dive);
#if DECO_CALC_DEBUG & 2
		printf("after surface intervall of %d:%02u\n", FRACTION(surface_time,60));
		dump_tissues();
#endif
	}
	if (!deco_init) {
		double surface_pressure = get_surface_pressure_in_mbar(dive, TRUE) / 1000.0;
		clear_deco(surface_pressure);
#if DECO_CALC_DEBUG & 2
		printf("no previous dive\n");
		dump_tissues();
#endif
	}
	return tissue_tolerance;
}

void update_cylinder_related_info(struct dive *dive)
{
	if (dive != NULL) {
		dive->sac = calculate_sac(dive);
		dive->otu = calculate_otu(dive);
	}
}

static void get_string(char **str, const char *s)
{
	int len;
	char *n;

	if (!s)
		s = "";
	len = g_utf8_strlen(s, -1);
	if (len > 60)
		len = 60;
	n = malloc(len * sizeof(gunichar) + 1);
	g_utf8_strncpy(n, s, len);
	*str = n;
}

static void get_location(struct dive *dive, char **str)
{
	get_string(str, dive->location);
}

static void get_cylinder(struct dive *dive, char **str)
{
	get_string(str, dive->cylinder[0].type.description);
}

static void get_suit(struct dive *dive, char **str)
{
	get_string(str, dive->suit);
}

GdkPixbuf *get_gps_icon(void)
{
	return gdk_pixbuf_from_pixdata(&satellite_pixbuf, TRUE, NULL);
}

static GdkPixbuf *get_gps_icon_for_dive(struct dive *dive)
{
	if (dive_has_gps_location(dive))
		return get_gps_icon();
	else
		return NULL;
}

/*
 * Set up anything that could have changed due to editing
 * of dive information; we need to do this for both models,
 * so we simply call set_one_dive again with the non-current model
 */
/* forward declaration for recursion */
static gboolean set_one_dive(GtkTreeModel *model,
			     GtkTreePath *path,
			     GtkTreeIter *iter,
			     gpointer data);

static void fill_one_dive(struct dive *dive,
			  GtkTreeModel *model,
			  GtkTreeIter *iter)
{
	char *location, *cylinder, *suit;
	GtkTreeModel *othermodel;
	GdkPixbuf *icon;

	get_cylinder(dive, &cylinder);
	get_location(dive, &location);
	get_suit(dive, &suit);
	icon = get_gps_icon_for_dive(dive);
	gtk_tree_store_set(GTK_TREE_STORE(model), iter,
		DIVE_NR, dive->number,
		DIVE_LOCATION, location,
		DIVE_LOC_ICON, icon,
		DIVE_CYLINDER, cylinder,
		DIVE_RATING, dive->rating,
		DIVE_SAC, dive->sac,
		DIVE_OTU, dive->otu,
		DIVE_MAXCNS, dive->maxcns,
		DIVE_TOTALWEIGHT, total_weight(dive),
		DIVE_SUIT, suit,
		-1);

	if (icon)
		g_object_unref(icon);
	free(location);
	free(cylinder);
	free(suit);

	if (model == TREEMODEL(dive_list))
		othermodel = LISTMODEL(dive_list);
	else
		othermodel = TREEMODEL(dive_list);
	if (othermodel != MODEL(dive_list))
		/* recursive call */
		gtk_tree_model_foreach(othermodel, set_one_dive, dive);
}

static gboolean set_one_dive(GtkTreeModel *model,
			     GtkTreePath *path,
			     GtkTreeIter *iter,
			     gpointer data)
{
	int idx;
	struct dive *dive;

	/* Get the dive number */
	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, -1);
	if (idx < 0)
		return FALSE;
	dive = get_dive(idx);
	if (!dive)
		return TRUE;
	if (data && dive != data)
		return FALSE;

	fill_one_dive(dive, model, iter);
	return dive == data;
}

void flush_divelist(struct dive *dive)
{
	GtkTreeModel *model = MODEL(dive_list);

	gtk_tree_model_foreach(model, set_one_dive, dive);
}

void set_divelist_font(const char *font)
{
	PangoFontDescription *font_desc = pango_font_description_from_string(font);
	gtk_widget_modify_font(dive_list.tree_view, font_desc);
	pango_font_description_free(font_desc);
}

void update_dive_list_units(void)
{
	const char *unit;
	GtkTreeModel *model = MODEL(dive_list);

	(void) get_depth_units(0, NULL, &unit);
	gtk_tree_view_column_set_title(dive_list.depth, unit);

	(void) get_temp_units(0, &unit);
	gtk_tree_view_column_set_title(dive_list.temperature, unit);

	(void) get_weight_units(0, NULL, &unit);
	gtk_tree_view_column_set_title(dive_list.totalweight, unit);

	gtk_tree_model_foreach(model, set_one_dive, NULL);
}

void update_dive_list_col_visibility(void)
{
	gtk_tree_view_column_set_visible(dive_list.cylinder, prefs.visible_cols.cylinder);
	gtk_tree_view_column_set_visible(dive_list.temperature, prefs.visible_cols.temperature);
	gtk_tree_view_column_set_visible(dive_list.totalweight, prefs.visible_cols.totalweight);
	gtk_tree_view_column_set_visible(dive_list.suit, prefs.visible_cols.suit);
	gtk_tree_view_column_set_visible(dive_list.nitrox, prefs.visible_cols.nitrox);
	gtk_tree_view_column_set_visible(dive_list.sac, prefs.visible_cols.sac);
	gtk_tree_view_column_set_visible(dive_list.otu, prefs.visible_cols.otu);
	gtk_tree_view_column_set_visible(dive_list.maxcns, prefs.visible_cols.maxcns);
	return;
}

/*
 * helper functions for dive_trip handling
 */

#ifdef DEBUG_TRIP
static void dump_trip_list(void)
{
	dive_trip_t *trip;
	int i=0;
	timestamp_t last_time = 0;

	for (trip = dive_trip_list; trip; trip = trip->next) {
		struct tm tm;
		utc_mkdate(trip->when, &tm);
		if (trip->when < last_time)
			printf("\n\ndive_trip_list OUT OF ORDER!!!\n\n\n");
		printf("%s trip %d to \"%s\" on %04u-%02u-%02u %02u:%02u:%02u (%d dives - %p)\n",
			trip->autogen ? "autogen " : "",
			++i, trip->location,
			tm.tm_year + 1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
			trip->nrdives, trip);
		last_time = trip->when;
	}
	printf("-----\n");
}
#endif

static dive_trip_t *find_trip_by_idx(int idx)
{
	dive_trip_t *trip = dive_trip_list;

	if (idx >= 0)
		return NULL;

	while (trip) {
		if (trip->index == idx)
			return trip;
		trip = trip->next;
	}
	return NULL;
}

/* this finds the last trip that at or before the time given */
static dive_trip_t *find_matching_trip(timestamp_t when)
{
	dive_trip_t *trip = dive_trip_list;

	if (!trip || trip->when > when) {
#ifdef DEBUG_TRIP
		printf("no matching trip\n");
#endif
		return NULL;
	}
	while (trip->next && trip->next->when <= when)
		trip = trip->next;
#ifdef DEBUG_TRIP
	{
		struct tm tm;
		utc_mkdate(trip->when, &tm);
		printf("found trip %p @ %04d-%02d-%02d %02d:%02d:%02d\n",
			trip,
			tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec);
	}
#endif
	return trip;
}

/* insert the trip into the dive_trip_list - but ensure you don't have
 * two trips for the same date; but if you have, make sure you don't
 * keep the one with less information */
void insert_trip(dive_trip_t **dive_trip_p)
{
	dive_trip_t *dive_trip = *dive_trip_p;
	dive_trip_t **p = &dive_trip_list;
	dive_trip_t *trip;
	struct dive *divep;

	/* Walk the dive trip list looking for the right location.. */
	while ((trip = *p) != NULL && trip->when < dive_trip->when)
		p = &trip->next;

	if (trip && trip->when == dive_trip->when) {
		if (! trip->location)
			trip->location = dive_trip->location;
		if (! trip->notes)
			trip->notes = dive_trip->notes;
		divep = dive_trip->dives;
		while (divep) {
			add_dive_to_trip(divep, trip);
			divep = divep->next;
		}
		*dive_trip_p = trip;
	} else {
		dive_trip->next = trip;
		*p = dive_trip;
	}
#ifdef DEBUG_TRIP
	dump_trip_list();
#endif
}

static void delete_trip(dive_trip_t *trip)
{
	dive_trip_t **p, *tmp;

	assert(!trip->dives);

	/* Remove the trip from the list of trips */
	p = &dive_trip_list;
	while ((tmp = *p) != NULL) {
		if (tmp == trip) {
			*p = trip->next;
			break;
		}
		p = &tmp->next;
	}

	/* .. and free it */
	if (trip->location)
		free(trip->location);
	if (trip->notes)
		free(trip->notes);
	free(trip);
}

static void find_new_trip_start_time(dive_trip_t *trip)
{
	struct dive *dive = trip->dives;
	timestamp_t when = dive->when;

	while ((dive = dive->next) != NULL) {
		if (dive->when < when)
			when = dive->when;
	}
	trip->when = when;
}

static void remove_dive_from_trip(struct dive *dive)
{
	struct dive *next, **pprev;
	dive_trip_t *trip = dive->divetrip;

	if (!trip)
		return;

	/* Remove the dive from the trip's list of dives */
	next = dive->next;
	pprev = dive->pprev;
	*pprev = next;
	if (next)
		next->pprev = pprev;

	dive->divetrip = NULL;
	dive->tripflag = NO_TRIP;
	assert(trip->nrdives > 0);
	if (!--trip->nrdives)
		delete_trip(trip);
	else if (trip->when == dive->when)
		find_new_trip_start_time(trip);
}

void add_dive_to_trip(struct dive *dive, dive_trip_t *trip)
{
	if (dive->divetrip == trip)
		return;
	assert(trip->when);
	remove_dive_from_trip(dive);
	trip->nrdives++;
	dive->divetrip = trip;
	dive->tripflag = ASSIGNED_TRIP;

	/* Add it to the trip's list of dives*/
	dive->next = trip->dives;
	if (dive->next)
		dive->next->pprev = &dive->next;
	trip->dives = dive;
	dive->pprev = &trip->dives;

	if (dive->when && trip->when > dive->when)
		trip->when = dive->when;
}

static dive_trip_t *create_and_hookup_trip_from_dive(struct dive *dive)
{
	dive_trip_t *dive_trip = calloc(sizeof(dive_trip_t),1);
	dive_trip->when = dive->when;
	if (dive->location)
		dive_trip->location = strdup(dive->location);
	insert_trip(&dive_trip);

	dive->tripflag = IN_TRIP;
	add_dive_to_trip(dive, dive_trip);
	return dive_trip;
}

/*
 * Walk the dives from the oldest dive, and see if we can autogroup them
 */
static void autogroup_dives(void)
{
	int i;
	struct dive *dive, *lastdive = NULL;

	for_each_dive(i, dive) {
		dive_trip_t *trip;

		if (dive->divetrip) {
			lastdive = dive;
			continue;
		}

		if (!DIVE_NEEDS_TRIP(dive)) {
			lastdive = NULL;
			continue;
		}

		/* Do we have a trip we can combine this into? */
		if (lastdive && dive->when < lastdive->when + TRIP_THRESHOLD) {
			dive_trip_t *trip = lastdive->divetrip;
			add_dive_to_trip(dive, trip);
			if (dive->location && !trip->location)
				trip->location = strdup(dive->location);
			lastdive = dive;
			continue;
		}

		lastdive = dive;
		trip = create_and_hookup_trip_from_dive(dive);
		trip->autogen = 1;
	}

#ifdef DEBUG_TRIP
	dump_trip_list();
#endif
}

static void clear_trip_indexes(void)
{
	dive_trip_t *trip;

	for (trip = dive_trip_list; trip != NULL; trip = trip->next)
		trip->index = 0;
}

/* Select the iter asked for, and set the keyboard focus on it */
static void go_to_iter(GtkTreeSelection *selection, GtkTreeIter *iter);
static void fill_dive_list(void)
{
	int i, trip_index = 0;
	GtkTreeIter iter, parent_iter, lookup, *parent_ptr = NULL;
	GtkTreeStore *liststore, *treestore;
	GdkPixbuf *icon;

	/* Do we need to create any dive groups automatically? */
	if (autogroup)
		autogroup_dives();

	treestore = TREESTORE(dive_list);
	liststore = LISTSTORE(dive_list);

	clear_trip_indexes();

	i = dive_table.nr;
	while (--i >= 0) {
		struct dive *dive = get_dive(i);
		dive_trip_t *trip = dive->divetrip;

		if (!trip) {
			parent_ptr = NULL;
		} else if (!trip->index) {
			trip->index = ++trip_index;

			/* Create new trip entry */
			gtk_tree_store_append(treestore, &parent_iter, NULL);
			parent_ptr = &parent_iter;

			/* a duration of 0 (and negative index) identifies a group */
			gtk_tree_store_set(treestore, parent_ptr,
					DIVE_INDEX, -trip_index,
					DIVE_DATE, trip->when,
					DIVE_LOCATION, trip->location,
					DIVE_DURATION, 0,
					-1);
		} else {
			int idx, ok;
			GtkTreeModel *model = TREEMODEL(dive_list);

			parent_ptr = NULL;
			ok = gtk_tree_model_get_iter_first(model, &lookup);
			while (ok) {
				gtk_tree_model_get(model, &lookup, DIVE_INDEX, &idx, -1);
				if (idx == -trip->index) {
					parent_ptr = &lookup;
					break;
				}
				ok = gtk_tree_model_iter_next(model, &lookup);
			}
		}

		/* store dive */
		update_cylinder_related_info(dive);
		gtk_tree_store_append(treestore, &iter, parent_ptr);
		icon = get_gps_icon_for_dive(dive);
		gtk_tree_store_set(treestore, &iter,
			DIVE_INDEX, i,
			DIVE_NR, dive->number,
			DIVE_DATE, dive->when,
			DIVE_DEPTH, dive->maxdepth,
			DIVE_DURATION, dive->duration.seconds,
			DIVE_LOCATION, dive->location,
			DIVE_LOC_ICON, icon,
			DIVE_RATING, dive->rating,
			DIVE_TEMPERATURE, dive->watertemp.mkelvin,
			DIVE_SAC, 0,
			-1);
		if (icon)
			g_object_unref(icon);
		gtk_tree_store_append(liststore, &iter, NULL);
		gtk_tree_store_set(liststore, &iter,
			DIVE_INDEX, i,
			DIVE_NR, dive->number,
			DIVE_DATE, dive->when,
			DIVE_DEPTH, dive->maxdepth,
			DIVE_DURATION, dive->duration.seconds,
			DIVE_LOCATION, dive->location,
			DIVE_LOC_ICON, icon,
			DIVE_RATING, dive->rating,
			DIVE_TEMPERATURE, dive->watertemp.mkelvin,
			DIVE_TOTALWEIGHT, 0,
			DIVE_SUIT, dive->suit,
			DIVE_SAC, 0,
			-1);
	}

	update_dive_list_units();
	if (amount_selected == 0 && gtk_tree_model_get_iter_first(MODEL(dive_list), &iter)) {
		GtkTreeSelection *selection;

		/* select the last dive (and make sure it's an actual dive that is selected) */
		gtk_tree_model_get(MODEL(dive_list), &iter, DIVE_INDEX, &selected_dive, -1);
		first_leaf(MODEL(dive_list), &iter, &selected_dive);
		selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));
		go_to_iter(selection, &iter);
	}
}

void dive_list_update_dives(void)
{
	dive_table.preexisting = dive_table.nr;
	gtk_tree_store_clear(TREESTORE(dive_list));
	gtk_tree_store_clear(LISTSTORE(dive_list));
	fill_dive_list();
	repaint_dive();
}

static gint dive_nr_sort(GtkTreeModel *model,
	GtkTreeIter *iter_a,
	GtkTreeIter *iter_b,
	gpointer user_data)
{
	int idx_a, idx_b;
	timestamp_t when_a, when_b;
	struct dive *a, *b;
	dive_trip_t *tripa = NULL, *tripb = NULL;

	gtk_tree_model_get(model, iter_a, DIVE_INDEX, &idx_a, DIVE_DATE, &when_a, -1);
	gtk_tree_model_get(model, iter_b, DIVE_INDEX, &idx_b, DIVE_DATE, &when_b, -1);

	if (idx_a < 0) {
		a = NULL;
		tripa = find_trip_by_idx(idx_a);
	} else {
		a = get_dive(idx_a);
		if (a)
			tripa = a->divetrip;
	}

	if (idx_b < 0) {
		b = NULL;
		tripb = find_trip_by_idx(idx_b);
	} else {
		b = get_dive(idx_b);
		if (b)
			tripb = b->divetrip;
	}

	/*
	 * Compare dive dates within the same trip (or when there
	 * are no trips involved at all). But if we have two
	 * different trips use the trip dates for comparison
	 */
	if (tripa != tripb) {
		if (tripa)
			when_a = tripa->when;
		if (tripb)
			when_b = tripb->when;
	}
	return when_a - when_b;
}


static struct divelist_column {
	const char *header;
	data_func_t data;
	sort_func_t sort;
	unsigned int flags;
	int *visible;
} dl_column[] = {
	[DIVE_NR] = { "#", nr_data_func, dive_nr_sort, ALIGN_RIGHT },
	[DIVE_DATE] = { N_("Date"), date_data_func, NULL, ALIGN_LEFT },
	[DIVE_RATING] = { UTF8_BLACKSTAR, star_data_func, NULL, ALIGN_LEFT },
	[DIVE_DEPTH] = { N_("ft"), depth_data_func, NULL, ALIGN_RIGHT },
	[DIVE_DURATION] = { N_("min"), duration_data_func, NULL, ALIGN_RIGHT },
	[DIVE_TEMPERATURE] = { UTF8_DEGREE "F", temperature_data_func, NULL, ALIGN_RIGHT, &prefs.visible_cols.temperature },
	[DIVE_TOTALWEIGHT] = { N_("lbs"), weight_data_func, NULL, ALIGN_RIGHT, &prefs.visible_cols.totalweight },
	[DIVE_SUIT] = { N_("Suit"), NULL, NULL, ALIGN_LEFT, &prefs.visible_cols.suit },
	[DIVE_CYLINDER] = { N_("Cyl"), NULL, NULL, 0, &prefs.visible_cols.cylinder },
	[DIVE_NITROX] = { "O" UTF8_SUBSCRIPT_2 "%", nitrox_data_func, nitrox_sort_func, 0, &prefs.visible_cols.nitrox },
	[DIVE_SAC] = { N_("SAC"), sac_data_func, NULL, 0, &prefs.visible_cols.sac },
	[DIVE_OTU] = { N_("OTU"), otu_data_func, NULL, 0, &prefs.visible_cols.otu },
	[DIVE_MAXCNS] = { N_("maxCNS"), cns_data_func, NULL, 0, &prefs.visible_cols.maxcns },
	[DIVE_LOCATION] = { N_("Location"), NULL, NULL, ALIGN_LEFT },
};


static GtkTreeViewColumn *divelist_column(struct DiveList *dl, struct divelist_column *col)
{
	int index = col - &dl_column[0];
	const char *title = _(col->header);
	data_func_t data_func = col->data;
	sort_func_t sort_func = col->sort;
	unsigned int flags = col->flags;
	int *visible = col->visible;
	GtkWidget *tree_view = dl->tree_view;
	GtkTreeStore *treemodel = dl->treemodel;
	GtkTreeStore *listmodel = dl->listmodel;
	GtkTreeViewColumn *ret;

	if (visible && !*visible)
		flags |= INVISIBLE;
	ret = tree_view_column(tree_view, index, title, data_func, flags);
	if (sort_func) {
		/* the sort functions are needed in the corresponding models */
		if (index == DIVE_NR)
			gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(treemodel), index, sort_func, NULL, NULL);
		else
			gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(listmodel), index, sort_func, NULL, NULL);
	}
	return ret;
}

/*
 * This is some crazy crap. The only way to get default focus seems
 * to be to grab focus as the widget is being shown the first time.
 */
static void realize_cb(GtkWidget *tree_view, gpointer userdata)
{
	gtk_widget_grab_focus(tree_view);
}

/*
 * Double-clicking on a group entry will expand a collapsed group
 * and vice versa.
 */
static void collapse_expand(GtkTreeView *tree_view, GtkTreePath *path)
{
	if (!gtk_tree_view_row_expanded(tree_view, path))
		gtk_tree_view_expand_row(tree_view, path, FALSE);
	else
		gtk_tree_view_collapse_row(tree_view, path);

}

/* Double-click on a dive list */
static void row_activated_cb(GtkTreeView *tree_view,
			GtkTreePath *path,
			GtkTreeViewColumn *column,
			gpointer userdata)
{
	int index;
	GtkTreeIter iter;

	if (!gtk_tree_model_get_iter(MODEL(dive_list), &iter, path))
		return;

	gtk_tree_model_get(MODEL(dive_list), &iter, DIVE_INDEX, &index, -1);
	/* a negative index is special for the "group by date" entries */
	if (index < 0) {
		collapse_expand(tree_view, path);
		return;
	}
	edit_dive_info(get_dive(index), FALSE);
}

void add_dive_cb(GtkWidget *menuitem, gpointer data)
{
	struct dive *dive;

	dive = alloc_dive();
	if (add_new_dive(dive)) {
		record_dive(dive);
		report_dives(TRUE, FALSE);
		return;
	}
	free(dive);
}

static void edit_trip_cb(GtkWidget *menuitem, GtkTreePath *path)
{
	int idx;
	GtkTreeIter iter;
	dive_trip_t *dive_trip;

	gtk_tree_model_get_iter(MODEL(dive_list), &iter, path);
	gtk_tree_model_get(MODEL(dive_list), &iter, DIVE_INDEX, &idx, -1);
	dive_trip = find_trip_by_idx(idx);
	if (edit_trip(dive_trip))
		gtk_tree_store_set(STORE(dive_list), &iter, DIVE_LOCATION, dive_trip->location, -1);
}

static void edit_selected_dives_cb(GtkWidget *menuitem, gpointer data)
{
	edit_multi_dive_info(NULL);
}

static void edit_dive_from_path_cb(GtkWidget *menuitem, GtkTreePath *path)
{
	struct dive *dive = dive_from_path(path);

	edit_multi_dive_info(dive);
}

static void edit_dive_when_cb(GtkWidget *menuitem, struct dive *dive)
{
	GtkWidget *dialog, *cal, *h, *m;
	timestamp_t when;

	guint yval, mval, dval;
	int success;
	struct tm tm;

	if (!dive)
		return;

	when = dive->when;
	utc_mkdate(when, &tm);
	dialog = create_date_time_widget(&tm, &cal, &h, &m);

	gtk_widget_show_all(dialog);
	success = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
	if (!success) {
		gtk_widget_destroy(dialog);
		return;
	}
	memset(&tm, 0, sizeof(tm));
	gtk_calendar_get_date(GTK_CALENDAR(cal), &yval, &mval, &dval);
	tm.tm_year = yval;
	tm.tm_mon = mval;
	tm.tm_mday = dval;
	tm.tm_hour = gtk_spin_button_get_value(GTK_SPIN_BUTTON(h));
	tm.tm_min = gtk_spin_button_get_value(GTK_SPIN_BUTTON(m));

	gtk_widget_destroy(dialog);
	when = utc_mktime(&tm);
	if (dive->when != when) {
		/* if this is the only dive in the trip, just change the trip time */
		if (dive->divetrip && dive->divetrip->nrdives == 1)
			dive->divetrip->when = when;
		/* if this is suddenly before the start of the trip, remove it from the trip */
		else if (dive->divetrip && dive->divetrip->when > when)
			remove_dive_from_trip(dive);
		else if (find_matching_trip(when) != dive->divetrip)
			remove_dive_from_trip(dive);
		dive->when = when;
		mark_divelist_changed(TRUE);
		remember_tree_state();
		report_dives(FALSE, FALSE);
		dive_list_update_dives();
		restore_tree_state();
	}
}

#if HAVE_OSM_GPS_MAP
static void show_gps_location_cb(GtkWidget *menuitem, struct dive *dive)
{
	show_gps_location(dive, NULL);
}
#endif

gboolean icon_click_cb(GtkWidget *w, GdkEventButton *event, gpointer data)
{
#if HAVE_OSM_GPS_MAP
	GtkTreePath *path = NULL;
	GtkTreeIter iter;
	GtkTreeViewColumn *col;
	int idx;
	struct dive *dive;

	/* left click ? */
	if (event->button == 1 &&
	    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(dive_list.tree_view), event->x, event->y, &path, &col, NULL, NULL)) {
		/* is it the icon column ? (we passed the correct column in when registering the callback) */
		if (col == data) {
			gtk_tree_model_get_iter(MODEL(dive_list), &iter, path);
			gtk_tree_model_get(MODEL(dive_list), &iter, DIVE_INDEX, &idx, -1);
			dive = get_dive(idx);
			if (dive && dive_has_gps_location(dive))
				show_gps_location(dive, NULL);
		}
		if (path)
			gtk_tree_path_free(path);
	}
#endif
	/* keep processing the click */
	return FALSE;
}

static void save_as_cb(GtkWidget *menuitem, struct dive *dive)
{
	GtkWidget *dialog;
	char *filename = NULL;

	dialog = gtk_file_chooser_dialog_new(_("Save File As"),
		GTK_WINDOW(main_window),
		GTK_FILE_CHOOSER_ACTION_SAVE,
		GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
		GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
		NULL);
	gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
	}
	gtk_widget_destroy(dialog);

	if (filename){
		set_filename(filename, TRUE);
		save_dives_logic(filename, TRUE);
		g_free(filename);
	}
}

static void expand_all_cb(GtkWidget *menuitem, GtkTreeView *tree_view)
{
	gtk_tree_view_expand_all(tree_view);
}

static void collapse_all_cb(GtkWidget *menuitem, GtkTreeView *tree_view)
{
	gtk_tree_view_collapse_all(tree_view);
}

/* copy the node and return the index */
static int copy_tree_node(GtkTreeIter *a, GtkTreeIter *b)
{
	struct dive store_dive;
	int totalweight, idx;
	char *cylinder_text;
	GdkPixbuf *icon;

	gtk_tree_model_get(MODEL(dive_list), a,
		DIVE_INDEX, &idx,
		DIVE_NR, &store_dive.number,
		DIVE_DATE, &store_dive.when,
		DIVE_RATING, &store_dive.rating,
		DIVE_DEPTH, &store_dive.maxdepth,
		DIVE_DURATION, &store_dive.duration,
		DIVE_TEMPERATURE, &store_dive.watertemp.mkelvin,
		DIVE_TOTALWEIGHT, &totalweight,
		DIVE_SUIT, &store_dive.suit,
		DIVE_CYLINDER, &cylinder_text,
		DIVE_SAC, &store_dive.sac,
		DIVE_OTU, &store_dive.otu,
		DIVE_MAXCNS, &store_dive.maxcns,
		DIVE_LOCATION, &store_dive.location,
		DIVE_LOC_ICON, &icon,
		-1);
	gtk_tree_store_set(STORE(dive_list), b,
		DIVE_INDEX, idx,
		DIVE_NR, store_dive.number,
		DIVE_DATE, store_dive.when,
		DIVE_RATING, store_dive.rating,
		DIVE_DEPTH, store_dive.maxdepth,
		DIVE_DURATION, store_dive.duration,
		DIVE_TEMPERATURE, store_dive.watertemp.mkelvin,
		DIVE_TOTALWEIGHT, totalweight,
		DIVE_SUIT, store_dive.suit,
		DIVE_CYLINDER, cylinder_text,
		DIVE_SAC, store_dive.sac,
		DIVE_OTU, store_dive.otu,
		DIVE_MAXCNS, store_dive.maxcns,
		DIVE_LOCATION, store_dive.location,
		DIVE_LOC_ICON, icon,
		-1);
	free(cylinder_text);
	free(store_dive.location);
	free(store_dive.suit);
	return idx;
}

/* to avoid complicated special cases based on ordering or number of children,
   we always take the first and last child and pick the smaller timestamp_t (which
   works regardless of ordering and also with just one child) */
static void update_trip_timestamp(GtkTreeIter *parent, dive_trip_t *divetrip)
{
	GtkTreeIter first_child, last_child;
	int nr;
	timestamp_t t1, t2, tnew;

	if (gtk_tree_store_iter_depth(STORE(dive_list), parent) != 0 ||
		gtk_tree_model_iter_n_children(MODEL(dive_list), parent) == 0)
		return;
	nr = gtk_tree_model_iter_n_children(MODEL(dive_list), parent);
	gtk_tree_model_iter_nth_child(MODEL(dive_list), &first_child, parent, 0);
	gtk_tree_model_get(MODEL(dive_list), &first_child, DIVE_DATE, &t1, -1);
	gtk_tree_model_iter_nth_child(MODEL(dive_list), &last_child, parent, nr - 1);
	gtk_tree_model_get(MODEL(dive_list), &last_child, DIVE_DATE, &t2, -1);
	tnew = MIN(t1, t2);
	gtk_tree_store_set(STORE(dive_list), parent, DIVE_DATE, tnew, -1);
	if (divetrip)
		divetrip->when = tnew;
}

/* move dive_iter, which is a child of old_trip (could be NULL) to new_trip (could be NULL);
 * either of the trips being NULL means that this was (or will be) a dive without a trip;
 * update the dive trips (especially the starting times) accordingly
 * maintain the selected status of the dive
 * IMPORTANT - the move needs to keep the tree consistant - so no out of order moving... */
static GtkTreeIter *move_dive_between_trips(GtkTreeIter *dive_iter, GtkTreeIter *old_trip, GtkTreeIter *new_trip,
					GtkTreeIter *sibling, gboolean before)
{
	int idx;
	timestamp_t old_when, new_when;
	struct dive *dive;
	dive_trip_t *old_divetrip, *new_divetrip;
	GtkTreeIter *new_iter = malloc(sizeof(GtkTreeIter));

	if (before)
		gtk_tree_store_insert_before(STORE(dive_list), new_iter, new_trip, sibling);
	else
		gtk_tree_store_insert_after(STORE(dive_list), new_iter, new_trip, sibling);
	idx = copy_tree_node(dive_iter, new_iter);
	gtk_tree_model_get(MODEL(dive_list), new_iter, DIVE_INDEX, &idx, -1);
	dive = get_dive(idx);
	gtk_tree_store_remove(STORE(dive_list), dive_iter);
	if (old_trip) {
		gtk_tree_model_get(MODEL(dive_list), old_trip, DIVE_DATE, &old_when, -1);
		old_divetrip = find_matching_trip(old_when);
		update_trip_timestamp(old_trip, old_divetrip);
	}
	if (new_trip) {
		gtk_tree_model_get(MODEL(dive_list), new_trip, DIVE_DATE, &new_when, -1);
		new_divetrip = dive->divetrip;
		update_trip_timestamp(new_trip, new_divetrip);
	}
	if (dive->selected) {
		GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));
		gtk_tree_selection_select_iter(selection, new_iter);
	}
	return new_iter;
}

/* this gets called when we are on a top level dive and we know that the previous
 * top level node is a trip; if multiple consecutive dives are selected, they are
 * all merged into the previous trip*/
static void merge_dive_into_trip_above_cb(GtkWidget *menuitem, GtkTreePath *path)
{
	int idx;
	GtkTreeIter dive_iter, trip_iter, prev_iter;
	GtkTreePath *trip_path;
	struct dive *dive, *prev_dive;

	/* get the path and iter for the trip and the last dive of that trip */
	trip_path = gtk_tree_path_copy(path);
	(void)gtk_tree_path_prev(trip_path);
	gtk_tree_model_get_iter(MODEL(dive_list), &trip_iter, trip_path);
	gtk_tree_model_get_iter(MODEL(dive_list), &dive_iter, path);
	gtk_tree_model_iter_nth_child(MODEL(dive_list), &prev_iter, &trip_iter,
				gtk_tree_model_iter_n_children(MODEL(dive_list), &trip_iter) - 1);
	gtk_tree_model_get(MODEL(dive_list), &dive_iter, DIVE_INDEX, &idx, -1);
	dive = get_dive(idx);
	gtk_tree_model_get(MODEL(dive_list), &prev_iter, DIVE_INDEX, &idx, -1);
	prev_dive = get_dive(idx);
	/* add the dive to the trip */
	for (;;) {
		add_dive_to_trip(dive, prev_dive->divetrip);
		/* we intentionally changed the dive_trip, so update the time
		 * stamp that we fall back to when toggling autogroup */
		dive->tripflag = IN_TRIP;
		free(move_dive_between_trips(&dive_iter, NULL, &trip_iter, NULL, TRUE));
		prev_dive = dive;
		/* by merging the dive into the trip above the path now points to the next
		   top level entry. If that iter exists, it's also a dive and both this dive
		   and that next dive are selected, continue merging dives into the trip */
		if (!gtk_tree_model_get_iter(MODEL(dive_list), &dive_iter, path))
			break;
		gtk_tree_model_get(MODEL(dive_list), &dive_iter, DIVE_INDEX, &idx, -1);
		if (idx < 0)
			break;
		dive = get_dive(idx);
		if (!dive->selected || !prev_dive->selected)
			break;
	}
	mark_divelist_changed(TRUE);
}

static void turn_dive_into_trip(GtkTreePath *path)
{
	GtkTreeIter iter, *newiter, newparent;
	GtkTreePath *treepath;
	timestamp_t when;
	char *location;
	int idx;
	struct dive *dive;

	/* this is a dive on the top level, insert trip AFTER it, populate its date / location, and
	 * then move the dive below that trip */
	gtk_tree_model_get_iter(MODEL(dive_list), &iter, path);
	gtk_tree_store_insert_after(STORE(dive_list), &newparent, NULL, &iter);
	gtk_tree_model_get(MODEL(dive_list), &iter,
			DIVE_INDEX, &idx, DIVE_DATE, &when, DIVE_LOCATION, &location, -1);
	gtk_tree_store_set(STORE(dive_list), &newparent,
			DIVE_INDEX, -1, DIVE_DATE, when, DIVE_LOCATION, location, -1);
	free(location);
	newiter = move_dive_between_trips(&iter, NULL, &newparent, NULL, FALSE);
	treepath = gtk_tree_model_get_path(MODEL(dive_list), newiter);
	gtk_tree_view_expand_to_path(GTK_TREE_VIEW(dive_list.tree_view), treepath);
	gtk_tree_path_free(treepath);
	dive = get_dive(idx);
	create_and_hookup_trip_from_dive(dive);
	free(newiter);
}

/* we know that path is pointing at a dive in a trip and are asked to split this trip into two */
static void insert_trip_before(GtkTreePath *path)
{
	GtkTreeIter iter, prev_iter, parent, newparent, nextsibling;
	GtkTreePath *treepath, *prev_path;
	struct dive *dive, *prev_dive;
	dive_trip_t *new_divetrip;
	int idx, nr, i;

	gtk_tree_model_get_iter(MODEL(dive_list), &iter, path);
	prev_path = gtk_tree_path_copy(path);
	if (!gtk_tree_path_prev(prev_path) ||
	    !gtk_tree_model_iter_parent(MODEL(dive_list), &parent, &iter))
		return;
	gtk_tree_model_get_iter(MODEL(dive_list), &prev_iter, prev_path);
	gtk_tree_model_get(MODEL(dive_list), &prev_iter, DIVE_INDEX, &idx, -1);
	prev_dive = get_dive(idx);
	gtk_tree_store_insert_after(STORE(dive_list), &newparent, NULL, &parent);
	copy_tree_node(&parent, &newparent);
	gtk_tree_model_get(MODEL(dive_list), &iter, DIVE_INDEX, &idx, -1);
	dive = get_dive(idx);
	/* make sure that the timestamp_t of the previous divetrip is correct before
	 * inserting a new one */
	if (dive->when < prev_dive->when)
		if (prev_dive->divetrip && prev_dive->divetrip->when < prev_dive->when)
			prev_dive->divetrip->when = prev_dive->when;
	new_divetrip = create_and_hookup_trip_from_dive(dive);

	/* in order for the data structures to stay consistent we need to walk from
	 * the last child backwards to this one. The easiest way seems to be to do
	 * this with the nth iterator API */
	nr = gtk_tree_model_iter_n_children(MODEL(dive_list), &parent);
	for (i = nr - 1; i >= 0; i--) {
		gtk_tree_model_iter_nth_child(MODEL(dive_list), &nextsibling, &parent, i);
		treepath = gtk_tree_model_get_path(MODEL(dive_list), &nextsibling);
		gtk_tree_model_get(MODEL(dive_list), &nextsibling, DIVE_INDEX, &idx, -1);
		dive = get_dive(idx);
		add_dive_to_trip(dive, new_divetrip);
		free(move_dive_between_trips(&nextsibling, &parent, &newparent, NULL, FALSE));
		if (gtk_tree_path_compare(path, treepath) == 0) {
			/* we copied the dive we were called with; we are done */
			gtk_tree_path_free(treepath);
			break;
		}
		gtk_tree_path_free(treepath);
	}
	/* treat this divetrip as if it had been read from a file */
	treepath = gtk_tree_model_get_path(MODEL(dive_list), &newparent);
	gtk_tree_view_expand_to_path(GTK_TREE_VIEW(dive_list.tree_view), treepath);
	gtk_tree_path_free(treepath);
#ifdef DEBUG_TRIP
	dump_trip_list();
#endif
}

static void insert_trip_before_cb(GtkWidget *menuitem, GtkTreePath *path)
{
	/* is this splitting a trip or turning a dive into a trip? */
	if (gtk_tree_path_get_depth(path) == 2) {
		insert_trip_before(path);
	} else { /* this is a top level dive */
		struct dive *dive, *next_dive;
		GtkTreePath *next_path;

		dive = dive_from_path(path);
		if (dive->selected) {
			next_path = gtk_tree_path_copy(path);
			for (;;) {
				/* let's find the first dive in a block of selected dives */
				if (gtk_tree_path_prev(next_path)) {
					next_dive = dive_from_path(next_path);
					if (next_dive && next_dive->selected) {
						path = gtk_tree_path_copy(next_path);
						continue;
					}
				}
				break;
			}
		}
		/* now path points at the first selected dive in a consecutive block */
		turn_dive_into_trip(path);
		/* if the dive was selected and the next dive was selected, too,
		 * then all of them should be part of the new trip */
		if (dive->selected) {
			next_path = gtk_tree_path_copy(path);
			gtk_tree_path_next(next_path);
			next_dive = dive_from_path(next_path);
			if (next_dive && next_dive->selected)
				merge_dive_into_trip_above_cb(menuitem, next_path);
		}
	}
	mark_divelist_changed(TRUE);
}

static int get_path_index(GtkTreePath *path)
{
	GtkTreeIter iter;
	int idx;

	gtk_tree_model_get_iter(MODEL(dive_list), &iter, path);
	gtk_tree_model_get(MODEL(dive_list), &iter, DIVE_INDEX, &idx, -1);
	return idx;
}

static void remove_from_trip_cb(GtkWidget *menuitem, GtkTreePath *path)
{
	struct dive *dive;
	int idx;

	idx = get_path_index(path);
	if (idx < 0)
		return;
	dive = get_dive(idx);

	remember_tree_state();
	if (dive->selected) {
		/* remove all the selected dives */
		for_each_dive(idx, dive) {
			if (!dive->selected)
				continue;
			remove_dive_from_trip(dive);
		}
	} else {
		/* just remove the dive the mouse pointer is on */
		remove_dive_from_trip(dive);
	}
	dive_list_update_dives();
	restore_tree_state();
	mark_divelist_changed(TRUE);
}

static void remove_trip(GtkTreePath *trippath)
{
	int idx, i;
	dive_trip_t *trip;
	struct dive *dive;

	idx = get_path_index(trippath);
	trip = find_trip_by_idx(idx);
	if (!trip)
		return;

	remember_tree_state();
	for_each_dive(i, dive) {
		if (dive->divetrip != trip)
			continue;
		remove_dive_from_trip(dive);
	}

	dive_list_update_dives();
	restore_tree_state();

#ifdef DEBUG_TRIP
	dump_trip_list();
#endif
}

static void remove_trip_cb(GtkWidget *menuitem, GtkTreePath *trippath)
{
	int success;
	GtkWidget *dialog;

	dialog = gtk_dialog_new_with_buttons(_("Remove Trip"),
		GTK_WINDOW(main_window),
		GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
		GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
		NULL);

	gtk_widget_show_all(dialog);
	success = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
	gtk_widget_destroy(dialog);
	if (!success)
		return;

	remove_trip(trippath);
	mark_divelist_changed(TRUE);
}

static void merge_trips_cb(GtkWidget *menuitem, GtkTreePath *trippath)
{
	GtkTreePath *prevpath;
	GtkTreeIter thistripiter, prevtripiter;
	GtkTreeModel *tm = MODEL(dive_list);
	dive_trip_t *thistrip, *prevtrip;
	timestamp_t when;

	/* this only gets called when we are on a trip and there is another trip right before */
	prevpath = gtk_tree_path_copy(trippath);
	gtk_tree_path_prev(prevpath);
	gtk_tree_model_get_iter(tm, &thistripiter, trippath);
	gtk_tree_model_get(tm, &thistripiter, DIVE_DATE, &when, -1);
	thistrip = find_matching_trip(when);
	gtk_tree_model_get_iter(tm, &prevtripiter, prevpath);
	gtk_tree_model_get(tm, &prevtripiter, DIVE_DATE, &when, -1);
	prevtrip = find_matching_trip(when);
	remember_tree_state();
	/* move dives from trip */
	assert(thistrip != prevtrip);
	while (thistrip->dives)
		add_dive_to_trip(thistrip->dives, prevtrip);
	dive_list_update_dives();
	restore_tree_state();
	mark_divelist_changed(TRUE);
}

/* this implements the mechanics of removing the dive from the table,
 * but doesn't deal with updating dive trips, etc */
void delete_single_dive(int idx)
{
	int i;
	struct dive *dive = get_dive(idx);
	if (!dive)
		return; /* this should never happen */
	remove_dive_from_trip(dive);
	for (i = idx; i < dive_table.nr - 1; i++)
		dive_table.dives[i] = dive_table.dives[i+1];
	dive_table.dives[--dive_table.nr] = NULL;
	if (dive->selected)
		amount_selected--;
	/* free all allocations */
	free(dive->dc.sample);
	if (dive->location)
		free((void *)dive->location);
	if (dive->notes)
		free((void *)dive->notes);
	if (dive->divemaster)
		free((void *)dive->divemaster);
	if (dive->buddy)
		free((void *)dive->buddy);
	if (dive->suit)
		free((void *)dive->suit);
	free(dive);
}

void add_single_dive(int idx, struct dive *dive)
{
	int i;
	dive_table.nr++;
	if (dive->selected)
		amount_selected++;
	for (i = idx; i < dive_table.nr ; i++) {
		struct dive *tmp = dive_table.dives[i];
		dive_table.dives[i] = dive;
		dive = tmp;
	}
}

/* remember expanded state */
void remember_tree_state()
{
	dive_trip_t *trip;
	GtkTreeIter iter;
	if (!gtk_tree_model_get_iter_first(TREEMODEL(dive_list), &iter))
		return;
	do {
		int idx;
		GtkTreePath *path;

		gtk_tree_model_get(TREEMODEL(dive_list), &iter, DIVE_INDEX, &idx, -1);
		if (idx >= 0)
			continue;
		path = gtk_tree_model_get_path(TREEMODEL(dive_list), &iter);
		if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(dive_list.tree_view), path)) {
			trip = find_trip_by_idx(idx);
			if (trip)
				trip->expanded = TRUE;
		}
		gtk_tree_path_free(path);
	} while (gtk_tree_model_iter_next(TREEMODEL(dive_list), &iter));
}

static gboolean restore_node_state(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	int idx;
	struct dive *dive;
	dive_trip_t *trip;
	GtkTreeView *tree_view = GTK_TREE_VIEW(dive_list.tree_view);
	GtkTreeSelection *selection = gtk_tree_view_get_selection(tree_view);

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, -1);
	if (idx < 0) {
		trip = find_trip_by_idx(idx);
		if (trip && trip->expanded)
			gtk_tree_view_expand_row(tree_view, path, FALSE);
		if (trip && trip->selected)
			gtk_tree_selection_select_iter(selection, iter);
	} else {
		dive = get_dive(idx);
		if (dive && dive->selected)
			gtk_tree_selection_select_iter(selection, iter);
	}
	/* continue foreach */
	return FALSE;
}

/* restore expanded and selected state */
void restore_tree_state()
{
	gtk_tree_model_foreach(MODEL(dive_list), restore_node_state, NULL);
}

/* called when multiple dives are selected and one of these is right-clicked for delete */
static void delete_selected_dives_cb(GtkWidget *menuitem, GtkTreePath *path)
{
	int i;
	struct dive *dive;
	int success;
	GtkWidget *dialog;
	char *dialog_title;

	if (!amount_selected)
		return;
	if (amount_selected == 1)
		dialog_title = _("Delete dive");
	else
		dialog_title = _("Delete dives");

	dialog = gtk_dialog_new_with_buttons(dialog_title,
		GTK_WINDOW(main_window),
		GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
		GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
		NULL);

	gtk_widget_show_all(dialog);
	success = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
	gtk_widget_destroy(dialog);
	if (!success)
		return;

	remember_tree_state();
	/* walk the dive list in chronological order */
	for (i = 0; i < dive_table.nr; i++) {
		dive = get_dive(i);
		if (!dive)
			continue;
		if (!dive->selected)
			continue;
		/* now remove the dive from the table and free it. also move the iterator back,
		 * so that we don't skip a dive */
		delete_single_dive(i);
		i--;
	}
	dive_list_update_dives();
	restore_tree_state();

	/* if no dives are selected at this point clear the display widgets */
	if (!amount_selected) {
		selected_dive = 0;
		process_selected_dives();
		clear_stats_widgets();
		clear_equipment_widgets();
		show_dive_info(NULL);
	}
	mark_divelist_changed(TRUE);
}

/* this gets called with path pointing to a dive, either in the top level
 * or as part of a trip */
static void delete_dive_cb(GtkWidget *menuitem, GtkTreePath *path)
{
	int idx;
	GtkTreeIter iter;
	int success;
	GtkWidget *dialog;

	dialog = gtk_dialog_new_with_buttons(_("Delete dive"),
		GTK_WINDOW(main_window),
		GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
		GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
		NULL);

	gtk_widget_show_all(dialog);
	success = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
	gtk_widget_destroy(dialog);
	if (!success)
		return;

	remember_tree_state();
	if (!gtk_tree_model_get_iter(MODEL(dive_list), &iter, path))
		return;
	gtk_tree_model_get(MODEL(dive_list), &iter, DIVE_INDEX, &idx, -1);
	delete_single_dive(idx);
	dive_list_update_dives();
	restore_tree_state();
	mark_divelist_changed(TRUE);
}

static void merge_dive_index(int i, struct dive *a)
{
	struct dive *b = get_dive(i+1);
	struct dive *res;

	res = merge_dives(a, b, b->when - a->when, FALSE);
	if (!res)
		return;

	remember_tree_state();
	add_single_dive(i, res);
	delete_single_dive(i+1);
	delete_single_dive(i+1);

	dive_list_update_dives();
	restore_tree_state();
	mark_divelist_changed(TRUE);
}

static void merge_dives_cb(GtkWidget *menuitem, void *unused)
{
	int i;
	struct dive *dive;

	for_each_dive(i, dive) {
		if (dive->selected) {
			merge_dive_index(i, dive);
			return;
		}
	}
}

/* Called if there are exactly two selected dives and the dive at idx is one of them */
static void add_dive_merge_label(int idx, GtkMenuShell *menu)
{
	struct dive *a, *b;
	GtkWidget *menuitem;

	/* The other selected dive must be next to it.. */
	a = get_dive(idx);
	b = get_dive(idx+1);
	if (!b || !b->selected) {
		b = a;
		a = get_dive(idx-1);
		if (!a || !a->selected)
			return;
	}

	/* .. and they had better be in the same dive trip */
	if (a->divetrip != b->divetrip)
		return;

	/* .. and if the surface interval is excessive, you must be kidding us */
	if (b->when > a->when + a->duration.seconds + 30*60)
		return;

	/* If so, we can add a "merge dive" menu entry */
	menuitem = gtk_menu_item_new_with_label(_("Merge dives"));
	g_signal_connect(menuitem, "activate", G_CALLBACK(merge_dives_cb), NULL);
	gtk_menu_shell_append(menu, menuitem);
}

static void popup_divelist_menu(GtkTreeView *tree_view, GtkTreeModel *model, int button, GdkEventButton *event)
{
	GtkWidget *menu, *menuitem, *image;
	char editplurallabel[] = N_("Edit dives");
	char editsinglelabel[] = N_("Edit dive");
	char *editlabel;
	char deleteplurallabel[] = N_("Delete dives");
	char deletesinglelabel[] = N_("Delete dive");
	char *deletelabel;
	GtkTreePath *path, *prevpath, *nextpath;
	GtkTreeIter iter, previter, nextiter;
	int idx, previdx, nextidx;
	struct dive *dive;

	if (!event || !gtk_tree_view_get_path_at_pos(tree_view, event->x, event->y, &path, NULL, NULL, NULL))
		return;
	gtk_tree_model_get_iter(MODEL(dive_list), &iter, path);
	gtk_tree_model_get(MODEL(dive_list), &iter, DIVE_INDEX, &idx, -1);

	menu = gtk_menu_new();
	menuitem = gtk_image_menu_item_new_with_label(_("Add dive"));
	image = gtk_image_new_from_stock(GTK_STOCK_ADD, GTK_ICON_SIZE_MENU);
	gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(menuitem), image);
	g_signal_connect(menuitem, "activate", G_CALLBACK(add_dive_cb), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

	if (idx < 0) {
		/* mouse pointer is on a trip summary entry */
		menuitem = gtk_menu_item_new_with_label(_("Edit Trip Summary"));
		g_signal_connect(menuitem, "activate", G_CALLBACK(edit_trip_cb), path);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
		prevpath = gtk_tree_path_copy(path);
		if (gtk_tree_path_prev(prevpath) &&
		    gtk_tree_model_get_iter(MODEL(dive_list), &previter, prevpath)) {
			gtk_tree_model_get(MODEL(dive_list), &previter, DIVE_INDEX, &previdx, -1);
			if (previdx < 0) {
				menuitem = gtk_menu_item_new_with_label(_("Merge trip with trip above"));
				g_signal_connect(menuitem, "activate", G_CALLBACK(merge_trips_cb), path);
				gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
			}
		}
		nextpath = gtk_tree_path_copy(path);
		gtk_tree_path_next(nextpath);
		if (gtk_tree_model_get_iter(MODEL(dive_list), &nextiter, nextpath)) {
			gtk_tree_model_get(MODEL(dive_list), &nextiter, DIVE_INDEX, &nextidx, -1);
			if (nextidx < 0) {
				menuitem = gtk_menu_item_new_with_label(_("Merge trip with trip below"));
				g_signal_connect(menuitem, "activate", G_CALLBACK(merge_trips_cb), nextpath);
				gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
			}
		}
		menuitem = gtk_menu_item_new_with_label(_("Remove Trip"));
		g_signal_connect(menuitem, "activate", G_CALLBACK(remove_trip_cb), path);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
	} else {
		dive = get_dive(idx);
		/* if we right click on selected dive(s), edit or delete those */
		if (dive->selected) {
			if (amount_selected == 1) {
				deletelabel = _(deletesinglelabel);
				editlabel = _(editsinglelabel);
				menuitem = gtk_menu_item_new_with_label(_("Edit dive date/time"));
				g_signal_connect(menuitem, "activate", G_CALLBACK(edit_dive_when_cb), dive);
				gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
			} else {
				deletelabel = _(deleteplurallabel);
				editlabel = _(editplurallabel);
			}
			menuitem = gtk_menu_item_new_with_label(_("Save as"));
			g_signal_connect(menuitem, "activate", G_CALLBACK(save_as_cb), dive);
			gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

			menuitem = gtk_menu_item_new_with_label(deletelabel);
			g_signal_connect(menuitem, "activate", G_CALLBACK(delete_selected_dives_cb), path);
			gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

			menuitem = gtk_menu_item_new_with_label(editlabel);
			g_signal_connect(menuitem, "activate", G_CALLBACK(edit_selected_dives_cb), NULL);
			gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

			/* Two contiguous selected dives? */
			if (amount_selected == 2)
				add_dive_merge_label(idx, GTK_MENU_SHELL(menu));
		} else {
			menuitem = gtk_menu_item_new_with_label(_("Edit dive date/time"));
			g_signal_connect(menuitem, "activate", G_CALLBACK(edit_dive_when_cb), dive);
			gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

			deletelabel = _(deletesinglelabel);
			menuitem = gtk_menu_item_new_with_label(deletelabel);
			g_signal_connect(menuitem, "activate", G_CALLBACK(delete_dive_cb), path);
			gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

			editlabel = _(editsinglelabel);
			menuitem = gtk_menu_item_new_with_label(editlabel);
			g_signal_connect(menuitem, "activate", G_CALLBACK(edit_dive_from_path_cb), path);
			gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
		}
#if HAVE_OSM_GPS_MAP
		/* Only offer to show on map if it has a location. */
		if (dive_has_gps_location(dive)) {
			menuitem = gtk_menu_item_new_with_label(_("Show in map"));
			g_signal_connect(menuitem, "activate", G_CALLBACK(show_gps_location_cb), dive);
			gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
		}
#endif
		/* only offer trip editing options when we are displaying the tree model */
		if (dive_list.model == dive_list.treemodel) {
			int depth = gtk_tree_path_get_depth(path);
			int *indices = gtk_tree_path_get_indices(path);
			/* top level dive or child dive that is not the first child */
			if (depth == 1 || indices[1] > 0) {
				menuitem = gtk_menu_item_new_with_label(_("Create new trip above"));
				g_signal_connect(menuitem, "activate", G_CALLBACK(insert_trip_before_cb), path);
				gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
			}
			prevpath = gtk_tree_path_copy(path);
			/* top level dive with a trip right before it */
			if (depth == 1 &&
			    gtk_tree_path_prev(prevpath) &&
			    gtk_tree_model_get_iter(MODEL(dive_list), &previter, prevpath) &&
			    gtk_tree_model_iter_n_children(model, &previter)) {
				menuitem = gtk_menu_item_new_with_label(_("Add to trip above"));
				g_signal_connect(menuitem, "activate", G_CALLBACK(merge_dive_into_trip_above_cb), path);
				gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
			}
			if (DIVE_IN_TRIP(dive)) {
				if (dive->selected && amount_selected > 1)
					menuitem = gtk_menu_item_new_with_label(_("Remove selected dives from trip"));
				else
					menuitem = gtk_menu_item_new_with_label(_("Remove dive from trip"));
				g_signal_connect(menuitem, "activate", G_CALLBACK(remove_from_trip_cb), path);
				gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
			}
		}
	}
	menuitem = gtk_menu_item_new_with_label(_("Expand all"));
	g_signal_connect(menuitem, "activate", G_CALLBACK(expand_all_cb), tree_view);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

	menuitem = gtk_menu_item_new_with_label(_("Collapse all"));
	g_signal_connect(menuitem, "activate", G_CALLBACK(collapse_all_cb), tree_view);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

	gtk_widget_show_all(menu);

	gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL,
		button, gtk_get_current_event_time());
}

static void popup_menu_cb(GtkTreeView *tree_view, gpointer userdata)
{
	popup_divelist_menu(tree_view, MODEL(dive_list), 0, NULL);
}

static gboolean button_press_cb(GtkWidget *treeview, GdkEventButton *event, gpointer userdata)
{
	/* Right-click? Bring up the menu */
	if (event->type == GDK_BUTTON_PRESS  &&  event->button == 3) {
		popup_divelist_menu(GTK_TREE_VIEW(treeview), MODEL(dive_list), 3, event);
		return TRUE;
	}
	return FALSE;
}

static void scroll_to_path(GtkTreePath *path)
{
	gtk_tree_view_expand_to_path(GTK_TREE_VIEW(dive_list.tree_view), path);
	gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(dive_list.tree_view), path, NULL, FALSE, 0, 0);
	gtk_tree_view_set_cursor(GTK_TREE_VIEW(dive_list.tree_view), path, NULL, FALSE);
}

/* we need to have a temporary copy of the selected dives while
   switching model as the selection_cb function keeps getting called
   when gtk_tree_selection_select_path is called.  We also need to
   keep copies of the sort order so we can restore that as well after
   switching models. */
static gboolean second_call = FALSE;
static GtkSortType sortorder[] = { [0 ... DIVELIST_COLUMNS - 1] = GTK_SORT_DESCENDING, };
static int lastcol = DIVE_NR;

/* Check if this dive was selected previously and select it again in the new model;
 * This is used after we switch models to maintain consistent selections.
 * We always return FALSE to iterate through all dives */
static gboolean set_selected(GtkTreeModel *model, GtkTreePath *path,
				GtkTreeIter *iter, gpointer data)
{
	GtkTreeSelection *selection = GTK_TREE_SELECTION(data);
	int idx, selected;
	struct dive *dive;

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, -1);
	if (idx < 0) {
		GtkTreeIter child;
		if (gtk_tree_model_iter_children(model, &child, iter))
			gtk_tree_model_get(model, &child, DIVE_INDEX, &idx, -1);
	}
	dive = get_dive(idx);
	selected = dive && dive->selected;
	if (selected) {
		gtk_tree_view_expand_to_path(GTK_TREE_VIEW(dive_list.tree_view), path);
		gtk_tree_selection_select_path(selection, path);
	}
	return FALSE;

}

static gboolean scroll_to_this(GtkTreeModel *model, GtkTreePath *path,
				GtkTreeIter *iter, gpointer data)
{
	int idx;
	struct dive *dive;

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, -1);
	dive = get_dive(idx);
	if (dive == current_dive) {
		scroll_to_path(path);
		return TRUE;
	}
	return FALSE;
}

static void scroll_to_current(GtkTreeModel *model)
{
	if (current_dive)
		gtk_tree_model_foreach(model, scroll_to_this, current_dive);
}

static void update_column_and_order(int colid)
{
	/* Careful: the index into treecolumns is off by one as we don't have a
	   tree_view column for DIVE_INDEX */
	GtkTreeViewColumn **treecolumns = &dive_list.nr;

	/* this will trigger a second call into sort_column_change_cb,
	   so make sure we don't start an infinite recursion... */
	second_call = TRUE;
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(dive_list.model), colid, sortorder[colid]);
	gtk_tree_view_column_set_sort_order(treecolumns[colid - 1], sortorder[colid]);
	second_call = FALSE;
	scroll_to_current(GTK_TREE_MODEL(dive_list.model));
}

/* If the sort column is nr (default), show the tree model.
   For every other sort column only show the list model.
   If the model changed, inform the new model of the chosen sort column and make
   sure the same dives are still selected.

   The challenge with this function is that once we change the model
   we also need to change the sort column again (as it was changed in
   the other model) and that causes this function to be called
   recursively - so we need to catch that.
*/
static void sort_column_change_cb(GtkTreeSortable *treeview, gpointer data)
{
	int colid;
	GtkSortType order;
	GtkTreeStore *currentmodel = dive_list.model;

	if (second_call)
		return;

	gtk_tree_sortable_get_sort_column_id(treeview, &colid, &order);
	if (colid == lastcol) {
		/* we just changed sort order */
		sortorder[colid] = order;
		return;
	} else {
		lastcol = colid;
	}
	if (colid == DIVE_NR)
		dive_list.model = dive_list.treemodel;
	else
		dive_list.model = dive_list.listmodel;
	if (dive_list.model != currentmodel) {
		GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));

		gtk_tree_view_set_model(GTK_TREE_VIEW(dive_list.tree_view), MODEL(dive_list));
		update_column_and_order(colid);
		gtk_tree_model_foreach(MODEL(dive_list), set_selected, selection);
	} else {
		if (order != sortorder[colid]) {
			update_column_and_order(colid);
		}
	}
}

static void select_dive(int idx)
{
	struct dive *dive = get_dive(idx);
	if (dive && !dive->selected) {
		dive->selected = 1;
		amount_selected++;
		selected_dive = idx;
	}
}

static void deselect_dive(int idx)
{
	struct dive *dive = get_dive(idx);
	if (dive && dive->selected) {
		dive->selected = 0;
		amount_selected--;
		if (selected_dive == idx && amount_selected > 0) {
			/* pick a different dive as selected */
			while (--selected_dive >= 0) {
				dive = get_dive(selected_dive);
				if (dive && dive->selected)
					return;
			}
			selected_dive = idx;
			while (++selected_dive < dive_table.nr) {
				dive = get_dive(selected_dive);
				if (dive && dive->selected)
					return;
			}
		}
		if (amount_selected == 0)
			selected_dive = -1;
	}
}

static gboolean modify_selection_cb(GtkTreeSelection *selection, GtkTreeModel *model,
				GtkTreePath *path, gboolean was_selected, gpointer userdata)
{
	int idx;
	GtkTreeIter iter;

	if (!was_selected)
		return TRUE;
	gtk_tree_model_get_iter(model, &iter, path);
	gtk_tree_model_get(model, &iter, DIVE_INDEX, &idx, -1);
	if (idx < 0) {
		int i;
		struct dive *dive;
		dive_trip_t *trip = find_trip_by_idx(idx);
		if (!trip)
			return TRUE;

		trip->selected = 0;
		/* If this is expanded, let the gtk selection happen for each dive under it */
		if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(dive_list.tree_view), path))
			return TRUE;
		/* Otherwise, consider each dive under it deselected */
		for_each_dive(i, dive) {
			if (dive->divetrip == trip)
				deselect_dive(i);
		}
	} else {
		deselect_dive(idx);
	}
	return TRUE;
}

/* This gets called for each selected entry after a selection has changed */
static void entry_selected(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	int idx;

	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, -1);
	if (idx < 0) {
		int i;
		struct dive *dive;
		dive_trip_t *trip = find_trip_by_idx(idx);

		if (!trip)
			return;
		trip->selected = 1;

		/* If this is expanded, let the gtk selection happen for each dive under it */
		if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(dive_list.tree_view), path)) {
			trip->fixup = 1;
			return;
		}

		/* Otherwise, consider each dive under it selected */
		for_each_dive(i, dive) {
			if (dive->divetrip == trip)
				select_dive(i);
		}
		trip->fixup = 0;
	} else {
		select_dive(idx);
	}
}

static void update_gtk_selection(GtkTreeSelection *selection, GtkTreeModel *model)
{
	GtkTreeIter iter;

	if (!gtk_tree_model_get_iter_first(model, &iter))
		return;
	do {
		GtkTreeIter child;

		if (!gtk_tree_model_iter_children(model, &child, &iter))
			continue;

		do {
			int idx;
			struct dive *dive;
			dive_trip_t *trip;

			gtk_tree_model_get(model, &child, DIVE_INDEX, &idx, -1);
			dive = get_dive(idx);
			if (!dive || !dive->selected)
				break;
			trip = dive->divetrip;
			if (!trip)
				break;
			gtk_tree_selection_select_iter(selection, &child);
		} while (gtk_tree_model_iter_next(model, &child));
	} while (gtk_tree_model_iter_next(model, &iter));
}

/* this is called when gtk thinks that the selection has changed */
static void selection_cb(GtkTreeSelection *selection, GtkTreeModel *model)
{
	int i, fixup;
	struct dive *dive;

	gtk_tree_selection_selected_foreach(selection, entry_selected, model);

	/*
	 * Go through all the dives, if there is a trip that is selected but no
	 * dives under it are selected, force-select all the dives
	 */

	/* First, clear "fixup" for any trip that has selected dives */
	for_each_dive(i, dive) {
		dive_trip_t *trip = dive->divetrip;
		if (!trip || !trip->fixup)
			continue;
		if (dive->selected || !trip->selected)
			trip->fixup = 0;
	}

	/*
	 * Ok, not fixup is only set for trips that are selected
	 * but have no selected dives in them. Select all dives
	 * for such trips.
	 */
	fixup = 0;
	for_each_dive(i, dive) {
		dive_trip_t *trip = dive->divetrip;
		if (!trip || !trip->fixup)
			continue;
		fixup = 1;
		select_dive(i);
	}

	/*
	 * Ok, we did a forced selection of dives, now we need to update the gtk
	 * view of what is selected too..
	 */
	if (fixup)
		update_gtk_selection(selection, model);

#if DEBUG_SELECTION_TRACKING
	dump_selection();
#endif

	process_selected_dives();
	repaint_dive();
}

GtkWidget *dive_list_create(void)
{
	GtkTreeSelection  *selection;

	dive_list.listmodel = gtk_tree_store_new(DIVELIST_COLUMNS,
				G_TYPE_INT,			/* index */
				G_TYPE_INT,			/* nr */
				G_TYPE_INT64,			/* Date */
				G_TYPE_INT,			/* Star rating */
				G_TYPE_INT, 			/* Depth */
				G_TYPE_INT,			/* Duration */
				G_TYPE_INT,			/* Temperature */
				G_TYPE_INT,			/* Total weight */
				G_TYPE_STRING,			/* Suit */
				G_TYPE_STRING,			/* Cylinder */
				G_TYPE_INT,			/* Nitrox */
				G_TYPE_INT,			/* SAC */
				G_TYPE_INT,			/* OTU */
				G_TYPE_INT,			/* MAXCNS */
				G_TYPE_STRING,			/* Location */
				GDK_TYPE_PIXBUF			/* GPS icon */
				);
	dive_list.treemodel = gtk_tree_store_new(DIVELIST_COLUMNS,
				G_TYPE_INT,			/* index */
				G_TYPE_INT,			/* nr */
				G_TYPE_INT64,			/* Date */
				G_TYPE_INT,			/* Star rating */
				G_TYPE_INT, 			/* Depth */
				G_TYPE_INT,			/* Duration */
				G_TYPE_INT,			/* Temperature */
				G_TYPE_INT,			/* Total weight */
				G_TYPE_STRING,			/* Suit */
				G_TYPE_STRING,			/* Cylinder */
				G_TYPE_INT,			/* Nitrox */
				G_TYPE_INT,			/* SAC */
				G_TYPE_INT,			/* OTU */
				G_TYPE_INT,			/* MAXCNS */
				G_TYPE_STRING,			/* Location */
				GDK_TYPE_PIXBUF			/* GPS icon */
				);
	dive_list.model = dive_list.treemodel;
	dive_list.tree_view = gtk_tree_view_new_with_model(TREEMODEL(dive_list));
	set_divelist_font(prefs.divelist_font);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));

	gtk_tree_selection_set_mode(GTK_TREE_SELECTION(selection), GTK_SELECTION_MULTIPLE);
	gtk_widget_set_size_request(dive_list.tree_view, 200, 200);

	/* check if utf8 stars are available as a default OS feature */
	if (!subsurface_os_feature_available(UTF8_FONT_WITH_STARS))
		dl_column[3].header = "*";

	dive_list.nr = divelist_column(&dive_list, dl_column + DIVE_NR);
	dive_list.date = divelist_column(&dive_list, dl_column + DIVE_DATE);
	dive_list.stars = divelist_column(&dive_list, dl_column + DIVE_RATING);
	dive_list.depth = divelist_column(&dive_list, dl_column + DIVE_DEPTH);
	dive_list.duration = divelist_column(&dive_list, dl_column + DIVE_DURATION);
	dive_list.temperature = divelist_column(&dive_list, dl_column + DIVE_TEMPERATURE);
	dive_list.totalweight = divelist_column(&dive_list, dl_column + DIVE_TOTALWEIGHT);
	dive_list.suit = divelist_column(&dive_list, dl_column + DIVE_SUIT);
	dive_list.cylinder = divelist_column(&dive_list, dl_column + DIVE_CYLINDER);
	dive_list.nitrox = divelist_column(&dive_list, dl_column + DIVE_NITROX);
	dive_list.sac = divelist_column(&dive_list, dl_column + DIVE_SAC);
	dive_list.otu = divelist_column(&dive_list, dl_column + DIVE_OTU);
	dive_list.maxcns = divelist_column(&dive_list, dl_column + DIVE_MAXCNS);
	dive_list.location = divelist_column(&dive_list, dl_column + DIVE_LOCATION);
	/* now add the GPS icon to the location column */
	tree_view_column_add_pixbuf(dive_list.tree_view, gpsicon_data_func, dive_list.location);

	fill_dive_list();

	g_object_set(G_OBJECT(dive_list.tree_view), "headers-visible", TRUE,
					  "search-column", DIVE_LOCATION,
					  "rules-hint", TRUE,
					  NULL);

	g_signal_connect_after(dive_list.tree_view, "realize", G_CALLBACK(realize_cb), NULL);
	g_signal_connect(dive_list.tree_view, "row-activated", G_CALLBACK(row_activated_cb), NULL);
	g_signal_connect(dive_list.tree_view, "row-expanded", G_CALLBACK(row_expanded_cb), NULL);
	g_signal_connect(dive_list.tree_view, "row-collapsed", G_CALLBACK(row_collapsed_cb), NULL);
	g_signal_connect(dive_list.tree_view, "button-press-event", G_CALLBACK(button_press_cb), NULL);
	g_signal_connect(dive_list.tree_view, "popup-menu", G_CALLBACK(popup_menu_cb), NULL);
	g_signal_connect(selection, "changed", G_CALLBACK(selection_cb), dive_list.model);
	g_signal_connect(dive_list.listmodel, "sort-column-changed", G_CALLBACK(sort_column_change_cb), NULL);
	g_signal_connect(dive_list.treemodel, "sort-column-changed", G_CALLBACK(sort_column_change_cb), NULL);

	gtk_tree_selection_set_select_function(selection, modify_selection_cb, NULL, NULL);

	dive_list.container_widget = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(dive_list.container_widget),
			       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(dive_list.container_widget), dive_list.tree_view);

	dive_list.changed = 0;

	return dive_list.container_widget;
}

void dive_list_destroy(void)
{
	gtk_widget_destroy(dive_list.tree_view);
}

void mark_divelist_changed(int changed)
{
	dive_list.changed = changed;
}

int unsaved_changes()
{
	return dive_list.changed;
}

void remove_autogen_trips()
{
	int i;
	struct dive *dive;

	for_each_dive(i, dive) {
		dive_trip_t *trip = dive->divetrip;

		if (trip && trip->autogen)
			remove_dive_from_trip(dive);
	}
}

struct iteridx {
	int idx;
	GtkTreeIter *iter;
};

static gboolean iter_has_idx(GtkTreeModel *model, GtkTreePath *path,
			     GtkTreeIter *iter, gpointer _data)
{
	struct iteridx *iteridx = _data;
	int idx;
	/* Get the dive number */
	gtk_tree_model_get(model, iter, DIVE_INDEX, &idx, -1);
	if (idx == iteridx->idx) {
		iteridx->iter = gtk_tree_iter_copy(iter);
		return TRUE; /* end foreach */
	}
	return FALSE;
}

static GtkTreeIter *get_iter_from_idx(int idx)
{
	struct iteridx iteridx = {idx, };
	gtk_tree_model_foreach(MODEL(dive_list), iter_has_idx, &iteridx);
	return iteridx.iter;
}

static void scroll_to_selected(GtkTreeIter *iter)
{
	GtkTreePath *treepath;
	treepath = gtk_tree_model_get_path(MODEL(dive_list), iter);
	scroll_to_path(treepath);
	gtk_tree_path_free(treepath);
}

static void go_to_iter(GtkTreeSelection *selection, GtkTreeIter *iter)
{
	gtk_tree_selection_unselect_all(selection);
	gtk_tree_selection_select_iter(selection, iter);
	scroll_to_selected(iter);
}

void show_and_select_dive(struct dive *dive)
{
	GtkTreeSelection *selection;
	GtkTreeIter *iter;
	struct dive *odive;
	int i, divenr;

	divenr = get_divenr(dive);
	if (divenr < 0)
		/* we failed to find the dive */
		return;
	iter = get_iter_from_idx(divenr);
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));
	for_each_dive(i, odive)
		odive->selected = FALSE;
	amount_selected = 1;
	selected_dive = divenr;
	dive->selected = TRUE;
	go_to_iter(selection, iter);
	gtk_tree_iter_free(iter);
}

void select_next_dive(void)
{
	GtkTreeIter *nextiter, *parent = NULL;
	GtkTreeIter *iter = get_iter_from_idx(selected_dive);
	GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));
	int idx;

	if (!iter)
		return;
	nextiter = gtk_tree_iter_copy(iter);
	if (!gtk_tree_model_iter_next(MODEL(dive_list), nextiter)) {
		if (!gtk_tree_model_iter_parent(MODEL(dive_list), nextiter, iter)) {
			/* we're at the last top level node */
			return;
		}
		if (!gtk_tree_model_iter_next(MODEL(dive_list), nextiter)) {
			/* last trip */
			return;
		}
	}
	gtk_tree_model_get(MODEL(dive_list), nextiter, DIVE_INDEX, &idx, -1);
	if (idx < 0) {
		/* need the first child */
		parent = gtk_tree_iter_copy(nextiter);
		if (! gtk_tree_model_iter_children(MODEL(dive_list), nextiter, parent))
			return;
	}
	go_to_iter(selection, nextiter);
	if (parent)
		gtk_tree_iter_free(parent);
	gtk_tree_iter_free(iter);
}

void select_prev_dive(void)
{
	GtkTreeIter previter, *parent = NULL;
	GtkTreeIter *iter = get_iter_from_idx(selected_dive);
	GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(dive_list.tree_view));
	GtkTreePath *treepath;
	int idx;

	if (!iter)
		return;
	treepath = gtk_tree_model_get_path(MODEL(dive_list), iter);
	if (!gtk_tree_path_prev(treepath)) {
		if (!gtk_tree_model_iter_parent(MODEL(dive_list), &previter, iter))
			/* we're at the last top level node */
			goto free_path;
		gtk_tree_path_free(treepath);
		treepath = gtk_tree_model_get_path(MODEL(dive_list), &previter);
		if (!gtk_tree_path_prev(treepath))
			/* first trip */
			goto free_path;
		if (!gtk_tree_model_get_iter(MODEL(dive_list), &previter, treepath))
			goto free_path;
	}
	if (!gtk_tree_model_get_iter(MODEL(dive_list), &previter, treepath))
		goto free_path;
	gtk_tree_model_get(MODEL(dive_list), &previter, DIVE_INDEX, &idx, -1);
	if (idx < 0) {
		/* need the last child */
		parent = gtk_tree_iter_copy(&previter);
		if (! gtk_tree_model_iter_nth_child(MODEL(dive_list), &previter, parent,
				gtk_tree_model_iter_n_children(MODEL(dive_list), parent) - 1))
			goto free_path;
	}
	go_to_iter(selection, &previter);
free_path:
	gtk_tree_path_free(treepath);
	if (parent)
		gtk_tree_iter_free(parent);
	gtk_tree_iter_free(iter);
}
