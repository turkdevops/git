/*
 * "Ostensibly Recursive's Twin" merge strategy, or "ort" for short.  Meant
 * as a drop-in replacement for the "recursive" merge strategy, allowing one
 * to replace
 *
 *   git merge [-s recursive]
 *
 * with
 *
 *   git merge -s ort
 *
 * Note: git's parser allows the space between '-s' and its argument to be
 * missing.  (Should I have backronymed "ham", "alsa", "kip", "nap, "alvo",
 * "cale", "peedy", or "ins" instead of "ort"?)
 */

#include "cache.h"
#include "merge-ort.h"

#include "alloc.h"
#include "blob.h"
#include "cache-tree.h"
#include "commit.h"
#include "commit-reach.h"
#include "diff.h"
#include "diffcore.h"
#include "dir.h"
#include "object-store.h"
#include "strmap.h"
#include "tree.h"
#include "unpack-trees.h"
#include "xdiff-interface.h"

/*
 * We have many arrays of size 3.  Whenever we have such an array, the
 * indices refer to one of the sides of the three-way merge.  This is so
 * pervasive that the constants 0, 1, and 2 are used in many places in the
 * code (especially in arithmetic operations to find the other side's index
 * or to compute a relevant mask), but sometimes these enum names are used
 * to aid code clarity.
 *
 * See also 'filemask' and 'dirmask' in struct conflict_info; the "ith side"
 * referred to there is one of these three sides.
 */
enum merge_side {
	MERGE_BASE = 0,
	MERGE_SIDE1 = 1,
	MERGE_SIDE2 = 2
};

struct merge_options_internal {
	/*
	 * paths: primary data structure in all of merge ort.
	 *
	 * The keys of paths:
	 *   * are full relative paths from the toplevel of the repository
	 *     (e.g. "drivers/firmware/raspberrypi.c").
	 *   * store all relevant paths in the repo, both directories and
	 *     files (e.g. drivers, drivers/firmware would also be included)
	 *   * these keys serve to intern all the path strings, which allows
	 *     us to do pointer comparison on directory names instead of
	 *     strcmp; we just have to be careful to use the interned strings.
	 *     (Technically paths_to_free may track some strings that were
	 *      removed from froms paths.)
	 *
	 * The values of paths:
	 *   * either a pointer to a merged_info, or a conflict_info struct
	 *   * merged_info contains all relevant information for a
	 *     non-conflicted entry.
	 *   * conflict_info contains a merged_info, plus any additional
	 *     information about a conflict such as the higher orders stages
	 *     involved and the names of the paths those came from (handy
	 *     once renames get involved).
	 *   * a path may start "conflicted" (i.e. point to a conflict_info)
	 *     and then a later step (e.g. three-way content merge) determines
	 *     it can be cleanly merged, at which point it'll be marked clean
	 *     and the algorithm will ignore any data outside the contained
	 *     merged_info for that entry
	 *   * If an entry remains conflicted, the merged_info portion of a
	 *     conflict_info will later be filled with whatever version of
	 *     the file should be placed in the working directory (e.g. an
	 *     as-merged-as-possible variation that contains conflict markers).
	 */
	struct strmap paths;

	/*
	 * conflicted: a subset of keys->values from "paths"
	 *
	 * conflicted is basically an optimization between process_entries()
	 * and record_conflicted_index_entries(); the latter could loop over
	 * ALL the entries in paths AGAIN and look for the ones that are
	 * still conflicted, but since process_entries() has to loop over
	 * all of them, it saves the ones it couldn't resolve in this strmap
	 * so that record_conflicted_index_entries() can iterate just the
	 * relevant entries.
	 */
	struct strmap conflicted;

	/*
	 * paths_to_free: additional list of strings to free
	 *
	 * If keys are removed from "paths", they are added to paths_to_free
	 * to ensure they are later freed.  We avoid free'ing immediately since
	 * other places (e.g. conflict_info.pathnames[]) may still be
	 * referencing these paths.
	 */
	struct string_list paths_to_free;

	/*
	 * output: special messages and conflict notices for various paths
	 *
	 * This is a map of pathnames (a subset of the keys in "paths" above)
	 * to strbufs.  It gathers various warning/conflict/notice messages
	 * for later processing.
	 */
	struct strmap output;

	/*
	 * current_dir_name: temporary var used in collect_merge_info_callback()
	 *
	 * Used to set merged_info.directory_name; see documentation for that
	 * variable and the requirements placed on that field.
	 */
	const char *current_dir_name;

	/* call_depth: recursion level counter for merging merge bases */
	int call_depth;
};

struct version_info {
	struct object_id oid;
	unsigned short mode;
};

struct merged_info {
	/* if is_null, ignore result.  otherwise result has oid & mode */
	struct version_info result;
	unsigned is_null:1;

	/*
	 * clean: whether the path in question is cleanly merged.
	 *
	 * see conflict_info.merged for more details.
	 */
	unsigned clean:1;

	/*
	 * basename_offset: offset of basename of path.
	 *
	 * perf optimization to avoid recomputing offset of final '/'
	 * character in pathname (0 if no '/' in pathname).
	 */
	size_t basename_offset;

	 /*
	  * directory_name: containing directory name.
	  *
	  * Note that we assume directory_name is constructed such that
	  *    strcmp(dir1_name, dir2_name) == 0 iff dir1_name == dir2_name,
	  * i.e. string equality is equivalent to pointer equality.  For this
	  * to hold, we have to be careful setting directory_name.
	  */
	const char *directory_name;
};

struct conflict_info {
	/*
	 * merged: the version of the path that will be written to working tree
	 *
	 * WARNING: It is critical to check merged.clean and ensure it is 0
	 * before reading any conflict_info fields outside of merged.
	 * Allocated merge_info structs will always have clean set to 1.
	 * Allocated conflict_info structs will have merged.clean set to 0
	 * initially.  The merged.clean field is how we know if it is safe
	 * to access other parts of conflict_info besides merged; if a
	 * conflict_info's merged.clean is changed to 1, the rest of the
	 * algorithm is not allowed to look at anything outside of the
	 * merged member anymore.
	 */
	struct merged_info merged;

	/* oids & modes from each of the three trees for this path */
	struct version_info stages[3];

	/* pathnames for each stage; may differ due to rename detection */
	const char *pathnames[3];

	/* Whether this path is/was involved in a directory/file conflict */
	unsigned df_conflict:1;

	/*
	 * Whether this path is/was involved in a non-content conflict other
	 * than a directory/file conflict (e.g. rename/rename, rename/delete,
	 * file location based on possible directory rename).
	 */
	unsigned path_conflict:1;

	/*
	 * For filemask and dirmask, the ith bit corresponds to whether the
	 * ith entry is a file (filemask) or a directory (dirmask).  Thus,
	 * filemask & dirmask is always zero, and filemask | dirmask is at
	 * most 7 but can be less when a path does not appear as either a
	 * file or a directory on at least one side of history.
	 *
	 * Note that these masks are related to enum merge_side, as the ith
	 * entry corresponds to side i.
	 *
	 * These values come from a traverse_trees() call; more info may be
	 * found looking at tree-walk.h's struct traverse_info,
	 * particularly the documentation above the "fn" member (note that
	 * filemask = mask & ~dirmask from that documentation).
	 */
	unsigned filemask:3;
	unsigned dirmask:3;

	/*
	 * Optimization to track which stages match, to avoid the need to
	 * recompute it in multiple steps. Either 0 or at least 2 bits are
	 * set; if at least 2 bits are set, their corresponding stages match.
	 */
	unsigned match_mask:3;
};

/*** Function Grouping: various utility functions ***/

/*
 * For the next three macros, see warning for conflict_info.merged.
 *
 * In each of the below, mi is a struct merged_info*, and ci was defined
 * as a struct conflict_info* (but we need to verify ci isn't actually
 * pointed at a struct merged_info*).
 *
 * INITIALIZE_CI: Assign ci to mi but only if it's safe; set to NULL otherwise.
 * VERIFY_CI: Ensure that something we assigned to a conflict_info* is one.
 * ASSIGN_AND_VERIFY_CI: Similar to VERIFY_CI but do assignment first.
 */
#define INITIALIZE_CI(ci, mi) do {                                           \
	(ci) = (!(mi) || (mi)->clean) ? NULL : (struct conflict_info *)(mi); \
} while (0)
#define VERIFY_CI(ci) assert(ci && !ci->merged.clean);
#define ASSIGN_AND_VERIFY_CI(ci, mi) do {    \
	(ci) = (struct conflict_info *)(mi);  \
	assert((ci) && !(mi)->clean);        \
} while (0)

static void free_strmap_strings(struct strmap *map)
{
	struct hashmap_iter iter;
	struct strmap_entry *entry;

	strmap_for_each_entry(map, &iter, entry) {
		free((char*)entry->key);
	}
}

static void clear_or_reinit_internal_opts(struct merge_options_internal *opti,
					  int reinitialize)
{
	void (*strmap_func)(struct strmap *, int) =
		reinitialize ? strmap_partial_clear : strmap_clear;

	/*
	 * We marked opti->paths with strdup_strings = 0, so that we
	 * wouldn't have to make another copy of the fullpath created by
	 * make_traverse_path from setup_path_info().  But, now that we've
	 * used it and have no other references to these strings, it is time
	 * to deallocate them.
	 */
	free_strmap_strings(&opti->paths);
	strmap_func(&opti->paths, 1);

	/*
	 * All keys and values in opti->conflicted are a subset of those in
	 * opti->paths.  We don't want to deallocate anything twice, so we
	 * don't free the keys and we pass 0 for free_values.
	 */
	strmap_func(&opti->conflicted, 0);

	/*
	 * opti->paths_to_free is similar to opti->paths; we created it with
	 * strdup_strings = 0 to avoid making _another_ copy of the fullpath
	 * but now that we've used it and have no other references to these
	 * strings, it is time to deallocate them.  We do so by temporarily
	 * setting strdup_strings to 1.
	 */
	opti->paths_to_free.strdup_strings = 1;
	string_list_clear(&opti->paths_to_free, 0);
	opti->paths_to_free.strdup_strings = 0;

	if (!reinitialize) {
		struct hashmap_iter iter;
		struct strmap_entry *e;

		/* Release and free each strbuf found in output */
		strmap_for_each_entry(&opti->output, &iter, e) {
			struct strbuf *sb = e->value;
			strbuf_release(sb);
			/*
			 * While strictly speaking we don't need to free(sb)
			 * here because we could pass free_values=1 when
			 * calling strmap_clear() on opti->output, that would
			 * require strmap_clear to do another
			 * strmap_for_each_entry() loop, so we just free it
			 * while we're iterating anyway.
			 */
			free(sb);
		}
		strmap_clear(&opti->output, 0);
	}
}

static int err(struct merge_options *opt, const char *err, ...)
{
	va_list params;
	struct strbuf sb = STRBUF_INIT;

	strbuf_addstr(&sb, "error: ");
	va_start(params, err);
	strbuf_vaddf(&sb, err, params);
	va_end(params);

	error("%s", sb.buf);
	strbuf_release(&sb);

	return -1;
}

__attribute__((format (printf, 4, 5)))
static void path_msg(struct merge_options *opt,
		     const char *path,
		     int omittable_hint, /* skippable under --remerge-diff */
		     const char *fmt, ...)
{
	va_list ap;
	struct strbuf *sb = strmap_get(&opt->priv->output, path);
	if (!sb) {
		sb = xmalloc(sizeof(*sb));
		strbuf_init(sb, 0);
		strmap_put(&opt->priv->output, path, sb);
	}

	va_start(ap, fmt);
	strbuf_vaddf(sb, fmt, ap);
	va_end(ap);

	strbuf_addch(sb, '\n');
}

/*** Function Grouping: functions related to collect_merge_info() ***/

static void setup_path_info(struct merge_options *opt,
			    struct string_list_item *result,
			    const char *current_dir_name,
			    int current_dir_name_len,
			    char *fullpath, /* we'll take over ownership */
			    struct name_entry *names,
			    struct name_entry *merged_version,
			    unsigned is_null,     /* boolean */
			    unsigned df_conflict, /* boolean */
			    unsigned filemask,
			    unsigned dirmask,
			    int resolved          /* boolean */)
{
	/* result->util is void*, so mi is a convenience typed variable */
	struct merged_info *mi;

	assert(!is_null || resolved);
	assert(!df_conflict || !resolved); /* df_conflict implies !resolved */
	assert(resolved == (merged_version != NULL));

	mi = xcalloc(1, resolved ? sizeof(struct merged_info) :
				   sizeof(struct conflict_info));
	mi->directory_name = current_dir_name;
	mi->basename_offset = current_dir_name_len;
	mi->clean = !!resolved;
	if (resolved) {
		mi->result.mode = merged_version->mode;
		oidcpy(&mi->result.oid, &merged_version->oid);
		mi->is_null = !!is_null;
	} else {
		int i;
		struct conflict_info *ci;

		ASSIGN_AND_VERIFY_CI(ci, mi);
		for (i = MERGE_BASE; i <= MERGE_SIDE2; i++) {
			ci->pathnames[i] = fullpath;
			ci->stages[i].mode = names[i].mode;
			oidcpy(&ci->stages[i].oid, &names[i].oid);
		}
		ci->filemask = filemask;
		ci->dirmask = dirmask;
		ci->df_conflict = !!df_conflict;
		if (dirmask)
			/*
			 * Assume is_null for now, but if we have entries
			 * under the directory then when it is complete in
			 * write_completed_directory() it'll update this.
			 * Also, for D/F conflicts, we have to handle the
			 * directory first, then clear this bit and process
			 * the file to see how it is handled -- that occurs
			 * near the top of process_entry().
			 */
			mi->is_null = 1;
	}
	strmap_put(&opt->priv->paths, fullpath, mi);
	result->string = fullpath;
	result->util = mi;
}

static int collect_merge_info_callback(int n,
				       unsigned long mask,
				       unsigned long dirmask,
				       struct name_entry *names,
				       struct traverse_info *info)
{
	/*
	 * n is 3.  Always.
	 * common ancestor (mbase) has mask 1, and stored in index 0 of names
	 * head of side 1  (side1) has mask 2, and stored in index 1 of names
	 * head of side 2  (side2) has mask 4, and stored in index 2 of names
	 */
	struct merge_options *opt = info->data;
	struct merge_options_internal *opti = opt->priv;
	struct string_list_item pi;  /* Path Info */
	struct conflict_info *ci; /* typed alias to pi.util (which is void*) */
	struct name_entry *p;
	size_t len;
	char *fullpath;
	const char *dirname = opti->current_dir_name;
	unsigned filemask = mask & ~dirmask;
	unsigned match_mask = 0; /* will be updated below */
	unsigned mbase_null = !(mask & 1);
	unsigned side1_null = !(mask & 2);
	unsigned side2_null = !(mask & 4);
	unsigned side1_matches_mbase = (!side1_null && !mbase_null &&
					names[0].mode == names[1].mode &&
					oideq(&names[0].oid, &names[1].oid));
	unsigned side2_matches_mbase = (!side2_null && !mbase_null &&
					names[0].mode == names[2].mode &&
					oideq(&names[0].oid, &names[2].oid));
	unsigned sides_match = (!side1_null && !side2_null &&
				names[1].mode == names[2].mode &&
				oideq(&names[1].oid, &names[2].oid));

	/*
	 * Note: When a path is a file on one side of history and a directory
	 * in another, we have a directory/file conflict.  In such cases, if
	 * the conflict doesn't resolve from renames and deletions, then we
	 * always leave directories where they are and move files out of the
	 * way.  Thus, while struct conflict_info has a df_conflict field to
	 * track such conflicts, we ignore that field for any directories at
	 * a path and only pay attention to it for files at the given path.
	 * The fact that we leave directories were they are also means that
	 * we do not need to worry about getting additional df_conflict
	 * information propagated from parent directories down to children
	 * (unlike, say traverse_trees_recursive() in unpack-trees.c, which
	 * sets a newinfo.df_conflicts field specifically to propagate it).
	 */
	unsigned df_conflict = (filemask != 0) && (dirmask != 0);

	/* n = 3 is a fundamental assumption. */
	if (n != 3)
		BUG("Called collect_merge_info_callback wrong");

	/*
	 * A bunch of sanity checks verifying that traverse_trees() calls
	 * us the way I expect.  Could just remove these at some point,
	 * though maybe they are helpful to future code readers.
	 */
	assert(mbase_null == is_null_oid(&names[0].oid));
	assert(side1_null == is_null_oid(&names[1].oid));
	assert(side2_null == is_null_oid(&names[2].oid));
	assert(!mbase_null || !side1_null || !side2_null);
	assert(mask > 0 && mask < 8);

	/* Determine match_mask */
	if (side1_matches_mbase)
		match_mask = (side2_matches_mbase ? 7 : 3);
	else if (side2_matches_mbase)
		match_mask = 5;
	else if (sides_match)
		match_mask = 6;

	/*
	 * Get the name of the relevant filepath, which we'll pass to
	 * setup_path_info() for tracking.
	 */
	p = names;
	while (!p->mode)
		p++;
	len = traverse_path_len(info, p->pathlen);

	/* +1 in both of the following lines to include the NUL byte */
	fullpath = xmalloc(len + 1);
	make_traverse_path(fullpath, len + 1, info, p->path, p->pathlen);

	/*
	 * If mbase, side1, and side2 all match, we can resolve early.  Even
	 * if these are trees, there will be no renames or anything
	 * underneath.
	 */
	if (side1_matches_mbase && side2_matches_mbase) {
		/* mbase, side1, & side2 all match; use mbase as resolution */
		setup_path_info(opt, &pi, dirname, info->pathlen, fullpath,
				names, names+0, mbase_null, 0,
				filemask, dirmask, 1);
		return mask;
	}

	/*
	 * Record information about the path so we can resolve later in
	 * process_entries.
	 */
	setup_path_info(opt, &pi, dirname, info->pathlen, fullpath,
			names, NULL, 0, df_conflict, filemask, dirmask, 0);

	ci = pi.util;
	VERIFY_CI(ci);
	ci->match_mask = match_mask;

	/* If dirmask, recurse into subdirectories */
	if (dirmask) {
		struct traverse_info newinfo;
		struct tree_desc t[3];
		void *buf[3] = {NULL, NULL, NULL};
		const char *original_dir_name;
		int i, ret;

		ci->match_mask &= filemask;
		newinfo = *info;
		newinfo.prev = info;
		newinfo.name = p->path;
		newinfo.namelen = p->pathlen;
		newinfo.pathlen = st_add3(newinfo.pathlen, p->pathlen, 1);
		/*
		 * If this directory we are about to recurse into cared about
		 * its parent directory (the current directory) having a D/F
		 * conflict, then we'd propagate the masks in this way:
		 *    newinfo.df_conflicts |= (mask & ~dirmask);
		 * But we don't worry about propagating D/F conflicts.  (See
		 * comment near setting of local df_conflict variable near
		 * the beginning of this function).
		 */

		for (i = MERGE_BASE; i <= MERGE_SIDE2; i++) {
			if (i == 1 && side1_matches_mbase)
				t[1] = t[0];
			else if (i == 2 && side2_matches_mbase)
				t[2] = t[0];
			else if (i == 2 && sides_match)
				t[2] = t[1];
			else {
				const struct object_id *oid = NULL;
				if (dirmask & 1)
					oid = &names[i].oid;
				buf[i] = fill_tree_descriptor(opt->repo,
							      t + i, oid);
			}
			dirmask >>= 1;
		}

		original_dir_name = opti->current_dir_name;
		opti->current_dir_name = pi.string;
		ret = traverse_trees(NULL, 3, t, &newinfo);
		opti->current_dir_name = original_dir_name;

		for (i = MERGE_BASE; i <= MERGE_SIDE2; i++)
			free(buf[i]);

		if (ret < 0)
			return -1;
	}

	return mask;
}

static int collect_merge_info(struct merge_options *opt,
			      struct tree *merge_base,
			      struct tree *side1,
			      struct tree *side2)
{
	int ret;
	struct tree_desc t[3];
	struct traverse_info info;
	const char *toplevel_dir_placeholder = "";

	opt->priv->current_dir_name = toplevel_dir_placeholder;
	setup_traverse_info(&info, toplevel_dir_placeholder);
	info.fn = collect_merge_info_callback;
	info.data = opt;
	info.show_all_errors = 1;

	parse_tree(merge_base);
	parse_tree(side1);
	parse_tree(side2);
	init_tree_desc(t + 0, merge_base->buffer, merge_base->size);
	init_tree_desc(t + 1, side1->buffer, side1->size);
	init_tree_desc(t + 2, side2->buffer, side2->size);

	ret = traverse_trees(NULL, 3, t, &info);

	return ret;
}

/*** Function Grouping: functions related to threeway content merges ***/

static int handle_content_merge(struct merge_options *opt,
				const char *path,
				const struct version_info *o,
				const struct version_info *a,
				const struct version_info *b,
				const char *pathnames[3],
				const int extra_marker_size,
				struct version_info *result)
{
	die("Not yet implemented");
}

/*** Function Grouping: functions related to detect_and_process_renames(), ***
 *** which are split into directory and regular rename detection sections. ***/

/*** Function Grouping: functions related to directory rename detection ***/

/*** Function Grouping: functions related to regular rename detection ***/

static int detect_and_process_renames(struct merge_options *opt,
				      struct tree *merge_base,
				      struct tree *side1,
				      struct tree *side2)
{
	int clean = 1;

	/*
	 * Rename detection works by detecting file similarity.  Here we use
	 * a really easy-to-implement scheme: files are similar IFF they have
	 * the same filename.  Therefore, by this scheme, there are no renames.
	 *
	 * TODO: Actually implement a real rename detection scheme.
	 */
	return clean;
}

/*** Function Grouping: functions related to process_entries() ***/

static int string_list_df_name_compare(const char *one, const char *two)
{
	int onelen = strlen(one);
	int twolen = strlen(two);
	/*
	 * Here we only care that entries for D/F conflicts are
	 * adjacent, in particular with the file of the D/F conflict
	 * appearing before files below the corresponding directory.
	 * The order of the rest of the list is irrelevant for us.
	 *
	 * To achieve this, we sort with df_name_compare and provide
	 * the mode S_IFDIR so that D/F conflicts will sort correctly.
	 * We use the mode S_IFDIR for everything else for simplicity,
	 * since in other cases any changes in their order due to
	 * sorting cause no problems for us.
	 */
	int cmp = df_name_compare(one, onelen, S_IFDIR,
				  two, twolen, S_IFDIR);
	/*
	 * Now that 'foo' and 'foo/bar' compare equal, we have to make sure
	 * that 'foo' comes before 'foo/bar'.
	 */
	if (cmp)
		return cmp;
	return onelen - twolen;
}

struct directory_versions {
	/*
	 * versions: list of (basename -> version_info)
	 *
	 * The basenames are in reverse lexicographic order of full pathnames,
	 * as processed in process_entries().  This puts all entries within
	 * a directory together, and covers the directory itself after
	 * everything within it, allowing us to write subtrees before needing
	 * to record information for the tree itself.
	 */
	struct string_list versions;

	/*
	 * offsets: list of (full relative path directories -> integer offsets)
	 *
	 * Since versions contains basenames from files in multiple different
	 * directories, we need to know which entries in versions correspond
	 * to which directories.  Values of e.g.
	 *     ""             0
	 *     src            2
	 *     src/moduleA    5
	 * Would mean that entries 0-1 of versions are files in the toplevel
	 * directory, entries 2-4 are files under src/, and the remaining
	 * entries starting at index 5 are files under src/moduleA/.
	 */
	struct string_list offsets;

	/*
	 * last_directory: directory that previously processed file found in
	 *
	 * last_directory starts NULL, but records the directory in which the
	 * previous file was found within.  As soon as
	 *    directory(current_file) != last_directory
	 * then we need to start updating accounting in versions & offsets.
	 * Note that last_directory is always the last path in "offsets" (or
	 * NULL if "offsets" is empty) so this exists just for quick access.
	 */
	const char *last_directory;

	/* last_directory_len: cached computation of strlen(last_directory) */
	unsigned last_directory_len;
};

static int tree_entry_order(const void *a_, const void *b_)
{
	const struct string_list_item *a = a_;
	const struct string_list_item *b = b_;

	const struct merged_info *ami = a->util;
	const struct merged_info *bmi = b->util;
	return base_name_compare(a->string, strlen(a->string), ami->result.mode,
				 b->string, strlen(b->string), bmi->result.mode);
}

static void write_tree(struct object_id *result_oid,
		       struct string_list *versions,
		       unsigned int offset,
		       size_t hash_size)
{
	size_t maxlen = 0, extra;
	unsigned int nr = versions->nr - offset;
	struct strbuf buf = STRBUF_INIT;
	struct string_list relevant_entries = STRING_LIST_INIT_NODUP;
	int i;

	/*
	 * We want to sort the last (versions->nr-offset) entries in versions.
	 * Do so by abusing the string_list API a bit: make another string_list
	 * that contains just those entries and then sort them.
	 *
	 * We won't use relevant_entries again and will let it just pop off the
	 * stack, so there won't be allocation worries or anything.
	 */
	relevant_entries.items = versions->items + offset;
	relevant_entries.nr = versions->nr - offset;
	QSORT(relevant_entries.items, relevant_entries.nr, tree_entry_order);

	/* Pre-allocate some space in buf */
	extra = hash_size + 8; /* 8: 6 for mode, 1 for space, 1 for NUL char */
	for (i = 0; i < nr; i++) {
		maxlen += strlen(versions->items[offset+i].string) + extra;
	}
	strbuf_grow(&buf, maxlen);

	/* Write each entry out to buf */
	for (i = 0; i < nr; i++) {
		struct merged_info *mi = versions->items[offset+i].util;
		struct version_info *ri = &mi->result;
		strbuf_addf(&buf, "%o %s%c",
			    ri->mode,
			    versions->items[offset+i].string, '\0');
		strbuf_add(&buf, ri->oid.hash, hash_size);
	}

	/* Write this object file out, and record in result_oid */
	write_object_file(buf.buf, buf.len, tree_type, result_oid);
	strbuf_release(&buf);
}

static void record_entry_for_tree(struct directory_versions *dir_metadata,
				  const char *path,
				  struct merged_info *mi)
{
	const char *basename;

	if (mi->is_null)
		/* nothing to record */
		return;

	basename = path + mi->basename_offset;
	assert(strchr(basename, '/') == NULL);
	string_list_append(&dir_metadata->versions,
			   basename)->util = &mi->result;
}

static void write_completed_directory(struct merge_options *opt,
				      const char *new_directory_name,
				      struct directory_versions *info)
{
	const char *prev_dir;
	struct merged_info *dir_info = NULL;
	unsigned int offset;

	/*
	 * Some explanation of info->versions and info->offsets...
	 *
	 * process_entries() iterates over all relevant files AND
	 * directories in reverse lexicographic order, and calls this
	 * function.  Thus, an example of the paths that process_entries()
	 * could operate on (along with the directories for those paths
	 * being shown) is:
	 *
	 *     xtract.c             ""
	 *     tokens.txt           ""
	 *     src/moduleB/umm.c    src/moduleB
	 *     src/moduleB/stuff.h  src/moduleB
	 *     src/moduleB/baz.c    src/moduleB
	 *     src/moduleB          src
	 *     src/moduleA/foo.c    src/moduleA
	 *     src/moduleA/bar.c    src/moduleA
	 *     src/moduleA          src
	 *     src                  ""
	 *     Makefile             ""
	 *
	 * info->versions:
	 *
	 *     always contains the unprocessed entries and their
	 *     version_info information.  For example, after the first five
	 *     entries above, info->versions would be:
	 *
	 *     	   xtract.c     <xtract.c's version_info>
	 *     	   token.txt    <token.txt's version_info>
	 *     	   umm.c        <src/moduleB/umm.c's version_info>
	 *     	   stuff.h      <src/moduleB/stuff.h's version_info>
	 *     	   baz.c        <src/moduleB/baz.c's version_info>
	 *
	 *     Once a subdirectory is completed we remove the entries in
	 *     that subdirectory from info->versions, writing it as a tree
	 *     (write_tree()).  Thus, as soon as we get to src/moduleB,
	 *     info->versions would be updated to
	 *
	 *     	   xtract.c     <xtract.c's version_info>
	 *     	   token.txt    <token.txt's version_info>
	 *     	   moduleB      <src/moduleB's version_info>
	 *
	 * info->offsets:
	 *
	 *     helps us track which entries in info->versions correspond to
	 *     which directories.  When we are N directories deep (e.g. 4
	 *     for src/modA/submod/subdir/), we have up to N+1 unprocessed
	 *     directories (+1 because of toplevel dir).  Corresponding to
	 *     the info->versions example above, after processing five entries
	 *     info->offsets will be:
	 *
	 *     	   ""           0
	 *     	   src/moduleB  2
	 *
	 *     which is used to know that xtract.c & token.txt are from the
	 *     toplevel dirctory, while umm.c & stuff.h & baz.c are from the
	 *     src/moduleB directory.  Again, following the example above,
	 *     once we need to process src/moduleB, then info->offsets is
	 *     updated to
	 *
	 *     	   ""           0
	 *     	   src          2
	 *
	 *     which says that moduleB (and only moduleB so far) is in the
	 *     src directory.
	 *
	 *     One unique thing to note about info->offsets here is that
	 *     "src" was not added to info->offsets until there was a path
	 *     (a file OR directory) immediately below src/ that got
	 *     processed.
	 *
	 * Since process_entry() just appends new entries to info->versions,
	 * write_completed_directory() only needs to do work if the next path
	 * is in a directory that is different than the last directory found
	 * in info->offsets.
	 */

	/*
	 * If we are working with the same directory as the last entry, there
	 * is no work to do.  (See comments above the directory_name member of
	 * struct merged_info for why we can use pointer comparison instead of
	 * strcmp here.)
	 */
	if (new_directory_name == info->last_directory)
		return;

	/*
	 * If we are just starting (last_directory is NULL), or last_directory
	 * is a prefix of the current directory, then we can just update
	 * info->offsets to record the offset where we started this directory
	 * and update last_directory to have quick access to it.
	 */
	if (info->last_directory == NULL ||
	    !strncmp(new_directory_name, info->last_directory,
		     info->last_directory_len)) {
		uintptr_t offset = info->versions.nr;

		info->last_directory = new_directory_name;
		info->last_directory_len = strlen(info->last_directory);
		/*
		 * Record the offset into info->versions where we will
		 * start recording basenames of paths found within
		 * new_directory_name.
		 */
		string_list_append(&info->offsets,
				   info->last_directory)->util = (void*)offset;
		return;
	}

	/*
	 * The next entry that will be processed will be within
	 * new_directory_name.  Since at this point we know that
	 * new_directory_name is within a different directory than
	 * info->last_directory, we have all entries for info->last_directory
	 * in info->versions and we need to create a tree object for them.
	 */
	dir_info = strmap_get(&opt->priv->paths, info->last_directory);
	assert(dir_info);
	offset = (uintptr_t)info->offsets.items[info->offsets.nr-1].util;
	if (offset == info->versions.nr) {
		/*
		 * Actually, we don't need to create a tree object in this
		 * case.  Whenever all files within a directory disappear
		 * during the merge (e.g. unmodified on one side and
		 * deleted on the other, or files were renamed elsewhere),
		 * then we get here and the directory itself needs to be
		 * omitted from its parent tree as well.
		 */
		dir_info->is_null = 1;
	} else {
		/*
		 * Write out the tree to the git object directory, and also
		 * record the mode and oid in dir_info->result.
		 */
		dir_info->is_null = 0;
		dir_info->result.mode = S_IFDIR;
		write_tree(&dir_info->result.oid, &info->versions, offset,
			   opt->repo->hash_algo->rawsz);
	}

	/*
	 * We've now used several entries from info->versions and one entry
	 * from info->offsets, so we get rid of those values.
	 */
	info->offsets.nr--;
	info->versions.nr = offset;

	/*
	 * Now we've taken care of the completed directory, but we need to
	 * prepare things since future entries will be in
	 * new_directory_name.  (In particular, process_entry() will be
	 * appending new entries to info->versions.)  So, we need to make
	 * sure new_directory_name is the last entry in info->offsets.
	 */
	prev_dir = info->offsets.nr == 0 ? NULL :
		   info->offsets.items[info->offsets.nr-1].string;
	if (new_directory_name != prev_dir) {
		uintptr_t c = info->versions.nr;
		string_list_append(&info->offsets,
				   new_directory_name)->util = (void*)c;
	}

	/* And, of course, we need to update last_directory to match. */
	info->last_directory = new_directory_name;
	info->last_directory_len = strlen(info->last_directory);
}

/* Per entry merge function */
static void process_entry(struct merge_options *opt,
			  const char *path,
			  struct conflict_info *ci,
			  struct directory_versions *dir_metadata)
{
	VERIFY_CI(ci);
	assert(ci->filemask >= 0 && ci->filemask <= 7);
	/* ci->match_mask == 7 was handled in collect_merge_info_callback() */
	assert(ci->match_mask == 0 || ci->match_mask == 3 ||
	       ci->match_mask == 5 || ci->match_mask == 6);

	if (ci->dirmask) {
		record_entry_for_tree(dir_metadata, path, &ci->merged);
		if (ci->filemask == 0)
			/* nothing else to handle */
			return;
		assert(ci->df_conflict);
	}

	if (ci->df_conflict) {
		die("Not yet implemented.");
	}

	/*
	 * NOTE: Below there is a long switch-like if-elseif-elseif... block
	 *       which the code goes through even for the df_conflict cases
	 *       above.  Well, it will once we don't die-not-implemented above.
	 */
	if (ci->match_mask) {
		ci->merged.clean = 1;
		if (ci->match_mask == 6) {
			/* stages[1] == stages[2] */
			ci->merged.result.mode = ci->stages[1].mode;
			oidcpy(&ci->merged.result.oid, &ci->stages[1].oid);
		} else {
			/* determine the mask of the side that didn't match */
			unsigned int othermask = 7 & ~ci->match_mask;
			int side = (othermask == 4) ? 2 : 1;

			ci->merged.result.mode = ci->stages[side].mode;
			ci->merged.is_null = !ci->merged.result.mode;
			oidcpy(&ci->merged.result.oid, &ci->stages[side].oid);

			assert(othermask == 2 || othermask == 4);
			assert(ci->merged.is_null ==
			       (ci->filemask == ci->match_mask));
		}
	} else if (ci->filemask >= 6 &&
		   (S_IFMT & ci->stages[1].mode) !=
		   (S_IFMT & ci->stages[2].mode)) {
		/*
		 * Two different items from (file/submodule/symlink)
		 */
		die("Not yet implemented.");
	} else if (ci->filemask >= 6) {
		/*
		 * TODO: Needs a two-way or three-way content merge, but we're
		 * just being lazy and copying the version from HEAD and
		 * leaving it as conflicted.
		 */
		ci->merged.clean = 0;
		ci->merged.result.mode = ci->stages[1].mode;
		oidcpy(&ci->merged.result.oid, &ci->stages[1].oid);
		/* When we fix above, we'll call handle_content_merge() */
		(void)handle_content_merge;
	} else if (ci->filemask == 3 || ci->filemask == 5) {
		/* Modify/delete */
		const char *modify_branch, *delete_branch;
		int side = (ci->filemask == 5) ? 2 : 1;
		int index = opt->priv->call_depth ? 0 : side;

		ci->merged.result.mode = ci->stages[index].mode;
		oidcpy(&ci->merged.result.oid, &ci->stages[index].oid);
		ci->merged.clean = 0;

		modify_branch = (side == 1) ? opt->branch1 : opt->branch2;
		delete_branch = (side == 1) ? opt->branch2 : opt->branch1;

		path_msg(opt, path, 0,
			 _("CONFLICT (modify/delete): %s deleted in %s "
			   "and modified in %s.  Version %s of %s left "
			   "in tree."),
			 path, delete_branch, modify_branch,
			 modify_branch, path);
	} else if (ci->filemask == 2 || ci->filemask == 4) {
		/* Added on one side */
		int side = (ci->filemask == 4) ? 2 : 1;
		ci->merged.result.mode = ci->stages[side].mode;
		oidcpy(&ci->merged.result.oid, &ci->stages[side].oid);
		ci->merged.clean = !ci->df_conflict;
	} else if (ci->filemask == 1) {
		/* Deleted on both sides */
		ci->merged.is_null = 1;
		ci->merged.result.mode = 0;
		oidcpy(&ci->merged.result.oid, &null_oid);
		ci->merged.clean = 1;
	}

	/*
	 * If still conflicted, record it separately.  This allows us to later
	 * iterate over just conflicted entries when updating the index instead
	 * of iterating over all entries.
	 */
	if (!ci->merged.clean)
		strmap_put(&opt->priv->conflicted, path, ci);
	record_entry_for_tree(dir_metadata, path, &ci->merged);
}

static void process_entries(struct merge_options *opt,
			    struct object_id *result_oid)
{
	struct hashmap_iter iter;
	struct strmap_entry *e;
	struct string_list plist = STRING_LIST_INIT_NODUP;
	struct string_list_item *entry;
	struct directory_versions dir_metadata = { STRING_LIST_INIT_NODUP,
						   STRING_LIST_INIT_NODUP,
						   NULL, 0 };

	if (strmap_empty(&opt->priv->paths)) {
		oidcpy(result_oid, opt->repo->hash_algo->empty_tree);
		return;
	}

	/* Hack to pre-allocate plist to the desired size */
	ALLOC_GROW(plist.items, strmap_get_size(&opt->priv->paths), plist.alloc);

	/* Put every entry from paths into plist, then sort */
	strmap_for_each_entry(&opt->priv->paths, &iter, e) {
		string_list_append(&plist, e->key)->util = e->value;
	}
	plist.cmp = string_list_df_name_compare;
	string_list_sort(&plist);

	/*
	 * Iterate over the items in reverse order, so we can handle paths
	 * below a directory before needing to handle the directory itself.
	 *
	 * This allows us to write subtrees before we need to write trees,
	 * and it also enables sane handling of directory/file conflicts
	 * (because it allows us to know whether the directory is still in
	 * the way when it is time to process the file at the same path).
	 */
	for (entry = &plist.items[plist.nr-1]; entry >= plist.items; --entry) {
		char *path = entry->string;
		/*
		 * NOTE: mi may actually be a pointer to a conflict_info, but
		 * we have to check mi->clean first to see if it's safe to
		 * reassign to such a pointer type.
		 */
		struct merged_info *mi = entry->util;

		write_completed_directory(opt, mi->directory_name,
					  &dir_metadata);
		if (mi->clean)
			record_entry_for_tree(&dir_metadata, path, mi);
		else {
			struct conflict_info *ci = (struct conflict_info *)mi;
			process_entry(opt, path, ci, &dir_metadata);
		}
	}

	if (dir_metadata.offsets.nr != 1 ||
	    (uintptr_t)dir_metadata.offsets.items[0].util != 0) {
		printf("dir_metadata.offsets.nr = %d (should be 1)\n",
		       dir_metadata.offsets.nr);
		printf("dir_metadata.offsets.items[0].util = %u (should be 0)\n",
		       (unsigned)(uintptr_t)dir_metadata.offsets.items[0].util);
		fflush(stdout);
		BUG("dir_metadata accounting completely off; shouldn't happen");
	}
	write_tree(result_oid, &dir_metadata.versions, 0,
		   opt->repo->hash_algo->rawsz);
	string_list_clear(&plist, 0);
	string_list_clear(&dir_metadata.versions, 0);
	string_list_clear(&dir_metadata.offsets, 0);
}

/*** Function Grouping: functions related to merge_switch_to_result() ***/

static int checkout(struct merge_options *opt,
		    struct tree *prev,
		    struct tree *next)
{
	/* Switch the index/working copy from old to new */
	int ret;
	struct tree_desc trees[2];
	struct unpack_trees_options unpack_opts;

	memset(&unpack_opts, 0, sizeof(unpack_opts));
	unpack_opts.head_idx = -1;
	unpack_opts.src_index = opt->repo->index;
	unpack_opts.dst_index = opt->repo->index;

	setup_unpack_trees_porcelain(&unpack_opts, "merge");

	/*
	 * NOTE: if this were just "git checkout" code, we would probably
	 * read or refresh the cache and check for a conflicted index, but
	 * builtin/merge.c or sequencer.c really needs to read the index
	 * and check for conflicted entries before starting merging for a
	 * good user experience (no sense waiting for merges/rebases before
	 * erroring out), so there's no reason to duplicate that work here.
	 */

	/* 2-way merge to the new branch */
	unpack_opts.update = 1;
	unpack_opts.merge = 1;
	unpack_opts.quiet = 0; /* FIXME: sequencer might want quiet? */
	unpack_opts.verbose_update = (opt->verbosity > 2);
	unpack_opts.fn = twoway_merge;
	if (1/* FIXME: opts->overwrite_ignore*/) {
		unpack_opts.dir = xcalloc(1, sizeof(*unpack_opts.dir));
		unpack_opts.dir->flags |= DIR_SHOW_IGNORED;
		setup_standard_excludes(unpack_opts.dir);
	}
	parse_tree(prev);
	init_tree_desc(&trees[0], prev->buffer, prev->size);
	parse_tree(next);
	init_tree_desc(&trees[1], next->buffer, next->size);

	ret = unpack_trees(2, trees, &unpack_opts);
	clear_unpack_trees_porcelain(&unpack_opts);
	dir_clear(unpack_opts.dir);
	FREE_AND_NULL(unpack_opts.dir);
	return ret;
}

static int record_conflicted_index_entries(struct merge_options *opt,
					   struct index_state *index,
					   struct strmap *paths,
					   struct strmap *conflicted)
{
	struct hashmap_iter iter;
	struct strmap_entry *e;
	int errs = 0;
	int original_cache_nr;

	if (strmap_empty(conflicted))
		return 0;

	original_cache_nr = index->cache_nr;

	/* Put every entry from paths into plist, then sort */
	strmap_for_each_entry(conflicted, &iter, e) {
		const char *path = e->key;
		struct conflict_info *ci = e->value;
		int pos;
		struct cache_entry *ce;
		int i;

		VERIFY_CI(ci);

		/*
		 * The index will already have a stage=0 entry for this path,
		 * because we created an as-merged-as-possible version of the
		 * file and checkout() moved the working copy and index over
		 * to that version.
		 *
		 * However, previous iterations through this loop will have
		 * added unstaged entries to the end of the cache which
		 * ignore the standard alphabetical ordering of cache
		 * entries and break invariants needed for index_name_pos()
		 * to work.  However, we know the entry we want is before
		 * those appended cache entries, so do a temporary swap on
		 * cache_nr to only look through entries of interest.
		 */
		SWAP(index->cache_nr, original_cache_nr);
		pos = index_name_pos(index, path, strlen(path));
		SWAP(index->cache_nr, original_cache_nr);
		if (pos < 0) {
			if (ci->filemask != 1)
				BUG("Conflicted %s but nothing in basic working tree or index; this shouldn't happen", path);
			cache_tree_invalidate_path(index, path);
		} else {
			ce = index->cache[pos];

			/*
			 * Clean paths with CE_SKIP_WORKTREE set will not be
			 * written to the working tree by the unpack_trees()
			 * call in checkout().  Our conflicted entries would
			 * have appeared clean to that code since we ignored
			 * the higher order stages.  Thus, we need override
			 * the CE_SKIP_WORKTREE bit and manually write those
			 * files to the working disk here.
			 *
			 * TODO: Implement this CE_SKIP_WORKTREE fixup.
			 */

			/*
			 * Mark this cache entry for removal and instead add
			 * new stage>0 entries corresponding to the
			 * conflicts.  If there are many conflicted entries, we
			 * want to avoid memmove'ing O(NM) entries by
			 * inserting the new entries one at a time.  So,
			 * instead, we just add the new cache entries to the
			 * end (ignoring normal index requirements on sort
			 * order) and sort the index once we're all done.
			 */
			ce->ce_flags |= CE_REMOVE;
		}

		for (i = MERGE_BASE; i <= MERGE_SIDE2; i++) {
			struct version_info *vi;
			if (!(ci->filemask & (1ul << i)))
				continue;
			vi = &ci->stages[i];
			ce = make_cache_entry(index, vi->mode, &vi->oid,
					      path, i+1, 0);
			add_index_entry(index, ce, ADD_CACHE_JUST_APPEND);
		}
	}

	/*
	 * Remove the unused cache entries (and invalidate the relevant
	 * cache-trees), then sort the index entries to get the conflicted
	 * entries we added to the end into their right locations.
	 */
	remove_marked_cache_entries(index, 1);
	QSORT(index->cache, index->cache_nr, cmp_cache_name_compare);

	return errs;
}

void merge_switch_to_result(struct merge_options *opt,
			    struct tree *head,
			    struct merge_result *result,
			    int update_worktree_and_index,
			    int display_update_msgs)
{
	assert(opt->priv == NULL);
	if (result->clean >= 0 && update_worktree_and_index) {
		struct merge_options_internal *opti = result->priv;

		if (checkout(opt, head, result->tree)) {
			/* failure to function */
			result->clean = -1;
			return;
		}

		if (record_conflicted_index_entries(opt, opt->repo->index,
						    &opti->paths,
						    &opti->conflicted)) {
			/* failure to function */
			result->clean = -1;
			return;
		}
	}

	if (display_update_msgs) {
		struct merge_options_internal *opti = result->priv;
		struct hashmap_iter iter;
		struct strmap_entry *e;
		struct string_list olist = STRING_LIST_INIT_NODUP;
		int i;

		/* Hack to pre-allocate olist to the desired size */
		ALLOC_GROW(olist.items, strmap_get_size(&opti->output),
			   olist.alloc);

		/* Put every entry from output into olist, then sort */
		strmap_for_each_entry(&opti->output, &iter, e) {
			string_list_append(&olist, e->key)->util = e->value;
		}
		string_list_sort(&olist);

		/* Iterate over the items, printing them */
		for (i = 0; i < olist.nr; ++i) {
			struct strbuf *sb = olist.items[i].util;

			printf("%s", sb->buf);
		}
		string_list_clear(&olist, 0);
	}

	merge_finalize(opt, result);
}

void merge_finalize(struct merge_options *opt,
		    struct merge_result *result)
{
	struct merge_options_internal *opti = result->priv;

	assert(opt->priv == NULL);

	clear_or_reinit_internal_opts(opti, 0);
	FREE_AND_NULL(opti);
}

/*** Function Grouping: helper functions for merge_incore_*() ***/

static inline void set_commit_tree(struct commit *c, struct tree *t)
{
	c->maybe_tree = t;
}

static struct commit *make_virtual_commit(struct repository *repo,
					  struct tree *tree,
					  const char *comment)
{
	struct commit *commit = alloc_commit_node(repo);

	set_merge_remote_desc(commit, comment, (struct object *)commit);
	set_commit_tree(commit, tree);
	commit->object.parsed = 1;
	return commit;
}

static void merge_start(struct merge_options *opt, struct merge_result *result)
{
	/* Sanity checks on opt */
	assert(opt->repo);

	assert(opt->branch1 && opt->branch2);

	assert(opt->detect_directory_renames >= MERGE_DIRECTORY_RENAMES_NONE &&
	       opt->detect_directory_renames <= MERGE_DIRECTORY_RENAMES_TRUE);
	assert(opt->rename_limit >= -1);
	assert(opt->rename_score >= 0 && opt->rename_score <= MAX_SCORE);
	assert(opt->show_rename_progress >= 0 && opt->show_rename_progress <= 1);

	assert(opt->xdl_opts >= 0);
	assert(opt->recursive_variant >= MERGE_VARIANT_NORMAL &&
	       opt->recursive_variant <= MERGE_VARIANT_THEIRS);

	/*
	 * detect_renames, verbosity, buffer_output, and obuf are ignored
	 * fields that were used by "recursive" rather than "ort" -- but
	 * sanity check them anyway.
	 */
	assert(opt->detect_renames >= -1 &&
	       opt->detect_renames <= DIFF_DETECT_COPY);
	assert(opt->verbosity >= 0 && opt->verbosity <= 5);
	assert(opt->buffer_output <= 2);
	assert(opt->obuf.len == 0);

	assert(opt->priv == NULL);

	/* Default to histogram diff.  Actually, just hardcode it...for now. */
	opt->xdl_opts = DIFF_WITH_ALG(opt, HISTOGRAM_DIFF);

	/* Initialization of opt->priv, our internal merge data */
	opt->priv = xcalloc(1, sizeof(*opt->priv));

	/*
	 * Although we initialize opt->priv->paths with strdup_strings=0,
	 * that's just to avoid making yet another copy of an allocated
	 * string.  Putting the entry into paths means we are taking
	 * ownership, so we will later free it.  paths_to_free is similar.
	 *
	 * In contrast, conflicted just has a subset of keys from paths, so
	 * we don't want to free those (it'd be a duplicate free).
	 */
	strmap_init_with_options(&opt->priv->paths, NULL, 0);
	strmap_init_with_options(&opt->priv->conflicted, NULL, 0);
	string_list_init(&opt->priv->paths_to_free, 0);

	/*
	 * keys & strbufs in output will sometimes need to outlive "paths",
	 * so it will have a copy of relevant keys.  It's probably a small
	 * subset of the overall paths that have special output.
	 */
	strmap_init(&opt->priv->output);
}

/*** Function Grouping: merge_incore_*() and their internal variants ***/

/*
 * Originally from merge_trees_internal(); heavily adapted, though.
 */
static void merge_ort_nonrecursive_internal(struct merge_options *opt,
					    struct tree *merge_base,
					    struct tree *side1,
					    struct tree *side2,
					    struct merge_result *result)
{
	struct object_id working_tree_oid;

	if (collect_merge_info(opt, merge_base, side1, side2) != 0) {
		/*
		 * TRANSLATORS: The %s arguments are: 1) tree hash of a merge
		 * base, and 2-3) the trees for the two trees we're merging.
		 */
		err(opt, _("collecting merge info failed for trees %s, %s, %s"),
		    oid_to_hex(&merge_base->object.oid),
		    oid_to_hex(&side1->object.oid),
		    oid_to_hex(&side2->object.oid));
		result->clean = -1;
		return;
	}

	result->clean = detect_and_process_renames(opt, merge_base,
						   side1, side2);
	process_entries(opt, &working_tree_oid);

	/* Set return values */
	result->tree = parse_tree_indirect(&working_tree_oid);
	/* existence of conflicted entries implies unclean */
	result->clean &= strmap_empty(&opt->priv->conflicted);
	if (!opt->priv->call_depth) {
		result->priv = opt->priv;
		opt->priv = NULL;
	}
}

/*
 * Originally from merge_recursive_internal(); somewhat adapted, though.
 */
static void merge_ort_internal(struct merge_options *opt,
			       struct commit_list *merge_bases,
			       struct commit *h1,
			       struct commit *h2,
			       struct merge_result *result)
{
	struct commit_list *iter;
	struct commit *merged_merge_bases;
	const char *ancestor_name;
	struct strbuf merge_base_abbrev = STRBUF_INIT;

	if (!merge_bases) {
		merge_bases = get_merge_bases(h1, h2);
		/* See merge-ort.h:merge_incore_recursive() declaration NOTE */
		merge_bases = reverse_commit_list(merge_bases);
	}

	merged_merge_bases = pop_commit(&merge_bases);
	if (merged_merge_bases == NULL) {
		/* if there is no common ancestor, use an empty tree */
		struct tree *tree;

		tree = lookup_tree(opt->repo, opt->repo->hash_algo->empty_tree);
		merged_merge_bases = make_virtual_commit(opt->repo, tree,
							 "ancestor");
		ancestor_name = "empty tree";
	} else if (merge_bases) {
		ancestor_name = "merged common ancestors";
	} else {
		strbuf_add_unique_abbrev(&merge_base_abbrev,
					 &merged_merge_bases->object.oid,
					 DEFAULT_ABBREV);
		ancestor_name = merge_base_abbrev.buf;
	}

	for (iter = merge_bases; iter; iter = iter->next) {
		const char *saved_b1, *saved_b2;
		struct commit *prev = merged_merge_bases;

		opt->priv->call_depth++;
		/*
		 * When the merge fails, the result contains files
		 * with conflict markers. The cleanness flag is
		 * ignored (unless indicating an error), it was never
		 * actually used, as result of merge_trees has always
		 * overwritten it: the committed "conflicts" were
		 * already resolved.
		 */
		saved_b1 = opt->branch1;
		saved_b2 = opt->branch2;
		opt->branch1 = "Temporary merge branch 1";
		opt->branch2 = "Temporary merge branch 2";
		merge_ort_internal(opt, NULL, prev, iter->item, result);
		if (result->clean < 0)
			return;
		opt->branch1 = saved_b1;
		opt->branch2 = saved_b2;
		opt->priv->call_depth--;

		merged_merge_bases = make_virtual_commit(opt->repo,
							 result->tree,
							 "merged tree");
		commit_list_insert(prev, &merged_merge_bases->parents);
		commit_list_insert(iter->item,
				   &merged_merge_bases->parents->next);

		clear_or_reinit_internal_opts(opt->priv, 1);
	}

	opt->ancestor = ancestor_name;
	merge_ort_nonrecursive_internal(opt,
					repo_get_commit_tree(opt->repo,
							     merged_merge_bases),
					repo_get_commit_tree(opt->repo, h1),
					repo_get_commit_tree(opt->repo, h2),
					result);
	strbuf_release(&merge_base_abbrev);
	opt->ancestor = NULL;  /* avoid accidental re-use of opt->ancestor */
}

void merge_incore_nonrecursive(struct merge_options *opt,
			       struct tree *merge_base,
			       struct tree *side1,
			       struct tree *side2,
			       struct merge_result *result)
{
	assert(opt->ancestor != NULL);
	merge_start(opt, result);
	merge_ort_nonrecursive_internal(opt, merge_base, side1, side2, result);
}

void merge_incore_recursive(struct merge_options *opt,
			    struct commit_list *merge_bases,
			    struct commit *side1,
			    struct commit *side2,
			    struct merge_result *result)
{
	/* We set the ancestor label based on the merge_bases */
	assert(opt->ancestor == NULL);

	merge_start(opt, result);
	merge_ort_internal(opt, merge_bases, side1, side2, result);
}
