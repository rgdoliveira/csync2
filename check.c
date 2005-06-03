/*
 *  csync2 - cluster synchronisation tool, 2nd generation
 *  LINBIT Information Technologies GmbH <http://www.linbit.com>
 *  Copyright (C) 2004  Clifford Wolf <clifford@clifford.at>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "csync2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

void csync_hint(const char *file, int recursive)
{
	SQL("Adding Hint",
		"INSERT INTO hint (filename, recursive) "
		"VALUES ('%s', %d)", url_encode(file), recursive);
}

void csync_mark(const char *file, const char *thispeer)
{
	struct peer *pl = csync_find_peers(file, thispeer);
	int pl_idx;

	csync_schedule_commands(file, thispeer == 0);

	if ( ! pl ) {
		csync_debug(2, "Not in one of my groups: %s\n", file);
		return;
	}

	csync_debug(1, "Marking file as dirty: %s\n", file);
	for (pl_idx=0; pl[pl_idx].peername; pl_idx++)
		SQL("Marking File Dirty",
			"INSERT INTO dirty (filename, force, myname, peername) "
			"VALUES ('%s', 0, '%s', '%s')", url_encode(file),
			url_encode(pl[pl_idx].myname),
			url_encode(pl[pl_idx].peername));

	free(pl);
}

/* return 0 if path does not contain any symlinks */
int csync_check_pure(const char *filename)
{
#ifdef __CYGWIN__
	// For some reason or another does this function __kills__
	// the performance when using large directories with cygwin.
	// And there are no symlinks in windows anyways..
	return 0;
#else

	struct stat sbuf;
	int i=0;

	while (filename[i]) i++;

	{ /* new block for myfilename[] */
		char myfilename[i+1];
		memcpy(myfilename, filename, i+1);
		while (1) {
			while (myfilename[i] != '/')
				if (--i <= 0) return 0;
			myfilename[i]=0;
			if ( lstat(myfilename, &sbuf) || S_ISLNK(sbuf.st_mode) ) return 1;
		}
	}
#endif
}

void csync_check_del(const char *file, int recursive, int init_run)
{
	char *where_rec = "";
	struct textlist *tl = 0, *t;
	struct stat st;

	if ( recursive ) {
		if ( !strcmp(file, "/") )
			asprintf(&where_rec, "or 1");
		else
			asprintf(&where_rec, "or (filename > '%s/' "
					"and filename < '%s0')",
					url_encode(file), url_encode(file));
	}

	SQL_BEGIN("Checking for removed files",
			"SELECT filename from file where "
			"filename = '%s' %s ORDER BY filename", url_encode(file), where_rec)
	{
		const char *filename = url_decode(SQL_V[0]);
		if ( lstat(filename, &st) != 0 || csync_check_pure(filename) )
			textlist_add(&tl, filename, 0);
	} SQL_END;

	for (t = tl; t != 0; t = t->next) {
		if (!init_run) csync_mark(t->value, 0);
		SQL("Removing file from DB. It isn't with us anymore.",
		    "DELETE FROM file WHERE filename = '%s'",
		    url_encode(t->value));
	}

	textlist_free(tl);

	if ( recursive )
		free(where_rec);
}

void csync_check_mod(const char * file, int recursive, int ignnoent, int init_run)
{
	int check_type = csync_match_file(file);
	struct dirent **namelist;
	int n, this_is_dirty = 0;
	const char * checktxt;
	struct stat st;

	if ( check_type>0 && lstat(file, &st) != 0 ) {
		if ( ignnoent ) return;
		csync_fatal("This should not happen: "
				"Can't stat %s.\n", file);
	}

	switch ( check_type )
	{
	case 2:
		csync_debug(2, "Checking %s.\n", file);
		checktxt = csync_genchecktxt(&st, file, 0);

		SQL_BEGIN("Checking File",
			"SELECT checktxt FROM file WHERE "
			"filename = '%s'", url_encode(file))
		{
			if ( !csync_cmpchecktxt(checktxt,
						url_decode(SQL_V[0])) ) {
				csync_debug(2, "File has changed: %s\n", file);
				this_is_dirty = 1;
			}
		} SQL_FIN {
			if ( SQL_COUNT == 0 ) {
				csync_debug(2, "New file: %s\n", file);
				this_is_dirty = 1;
			}
		} SQL_END;

		if ( this_is_dirty ) {
			SQL("Adding or updating file entry",
			    "INSERT INTO file (filename, checktxt) "
			    "VALUES ('%s', '%s')",
			    url_encode(file), url_encode(checktxt));
			if (!init_run) csync_mark(file, 0);
		}
		/* fall thru */
	case 1:
		if ( !recursive ) break;
		if ( !S_ISDIR(st.st_mode) ) break;
		csync_debug(2, "Checking %s/* ..\n", file);
		n = scandir(file, &namelist, 0, alphasort);
		if (n < 0)
			perror("scandir");
		else {
			while(n--) {
			  if ( strcmp(namelist[n]->d_name, ".") &&
					strcmp(namelist[n]->d_name, "..") ) {
				char fn[strlen(file)+
					strlen(namelist[n]->d_name)+2];
				sprintf(fn, "%s/%s",
					!strcmp(file, "/") ? "" : file,
					namelist[n]->d_name);
				csync_check_mod(fn, recursive, 0, init_run);
			  }
			  free(namelist[n]);
			}
			free(namelist);
		}
	  	break;
	default:
		csync_debug(2, "Don't check at all: %s\n", file);
		break;
	}
}

void csync_check(const char * filename, int recursive, int init_run)
{
#if __CYGWIN__
	if (!strcmp(filename, "/")) {
		filename = "/cygdrive";
	}
#endif
	csync_debug(2, "Running%s check for %s ...\n",
			recursive ? " recursive" : "", filename);
	csync_check_del(filename, recursive, init_run);
	csync_check_mod(filename, recursive, 1, init_run);
}

